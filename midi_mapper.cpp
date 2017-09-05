#include "midi_mapper.h"

#include <alsa/asoundlib.h>
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <google/protobuf/descriptor.h>
#include <google/protobuf/io/zero_copy_stream_impl.h>
#include <google/protobuf/message.h>
#include <google/protobuf/text_format.h>
#include <pthread.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/eventfd.h>
#include <unistd.h>
#include <algorithm>
#include <functional>
#include <thread>

#include "audio_mixer.h"
#include "midi_mapping.pb.h"

using namespace google::protobuf;
using namespace std;
using namespace std::placeholders;

namespace {

double map_controller_to_float(int val)
{
	// Slightly hackish mapping so that we can represent exactly 0.0, 0.5 and 1.0.
	if (val <= 0) {
		return 0.0;
	} else if (val >= 127) {
		return 1.0;
	} else {
		return (val + 0.5) / 127.0;
	}
}

}  // namespace

MIDIMapper::MIDIMapper(ControllerReceiver *receiver)
	: receiver(receiver), mapping_proto(new MIDIMappingProto)
{
	should_quit_fd = eventfd(/*initval=*/0, /*flags=*/0);
	assert(should_quit_fd != -1);
}

MIDIMapper::~MIDIMapper()
{
	should_quit = true;
	const uint64_t one = 1;
	if (write(should_quit_fd, &one, sizeof(one)) != sizeof(one)) {
		perror("write(should_quit_fd)");
		exit(1);
	}
	midi_thread.join();
	close(should_quit_fd);
}

bool load_midi_mapping_from_file(const string &filename, MIDIMappingProto *new_mapping)
{
	// Read and parse the protobuf from disk.
	int fd = open(filename.c_str(), O_RDONLY);
	if (fd == -1) {
		perror(filename.c_str());
		return false;
	}
	io::FileInputStream input(fd);  // Takes ownership of fd.
	if (!TextFormat::Parse(&input, new_mapping)) {
		input.Close();
		return false;
	}
	input.Close();
	return true;
}

bool save_midi_mapping_to_file(const MIDIMappingProto &mapping_proto, const string &filename)
{
	// Save to disk. We use the text format because it's friendlier
	// for a user to look at and edit.
	int fd = open(filename.c_str(), O_WRONLY | O_TRUNC | O_CREAT, 0666);
	if (fd == -1) {
		perror(filename.c_str());
		return false;
	}
	io::FileOutputStream output(fd);  // Takes ownership of fd.
	if (!TextFormat::Print(mapping_proto, &output)) {
		// TODO: Don't overwrite the old file (if any) on error.
		output.Close();
		return false;
	}

	output.Close();
	return true;
}

void MIDIMapper::set_midi_mapping(const MIDIMappingProto &new_mapping)
{
	lock_guard<mutex> lock(mu);
	if (mapping_proto) {
		mapping_proto->CopyFrom(new_mapping);
	} else {
		mapping_proto.reset(new MIDIMappingProto(new_mapping));
	}

	num_controller_banks = min(max(mapping_proto->num_controller_banks(), 1), 5);
        current_controller_bank = 0;

	receiver->clear_all_highlights();
	update_highlights();
}

void MIDIMapper::start_thread()
{
	midi_thread = thread(&MIDIMapper::thread_func, this);
}

const MIDIMappingProto &MIDIMapper::get_current_mapping() const
{
	lock_guard<mutex> lock(mu);
	return *mapping_proto;
}

ControllerReceiver *MIDIMapper::set_receiver(ControllerReceiver *new_receiver)
{
	lock_guard<mutex> lock(mu);
	swap(receiver, new_receiver);
	return new_receiver;  // Now old receiver.
}

#define RETURN_ON_ERROR(msg, expr) do {                            \
	int err = (expr);                                          \
	if (err < 0) {                                             \
		fprintf(stderr, msg ": %s\n", snd_strerror(err));  \
		return;                                            \
	}                                                          \
} while (false)

#define WARN_ON_ERROR(msg, expr) do {                              \
	int err = (expr);                                          \
	if (err < 0) {                                             \
		fprintf(stderr, msg ": %s\n", snd_strerror(err));  \
	}                                                          \
} while (false)


void MIDIMapper::thread_func()
{
	pthread_setname_np(pthread_self(), "MIDIMapper");

	snd_seq_t *seq;
	int err;

	RETURN_ON_ERROR("snd_seq_open", snd_seq_open(&seq, "default", SND_SEQ_OPEN_DUPLEX, 0));
	RETURN_ON_ERROR("snd_seq_nonblock", snd_seq_nonblock(seq, 1));
	RETURN_ON_ERROR("snd_seq_client_name", snd_seq_set_client_name(seq, "nageru"));
	RETURN_ON_ERROR("snd_seq_create_simple_port",
		snd_seq_create_simple_port(seq, "nageru",
			SND_SEQ_PORT_CAP_READ |
				SND_SEQ_PORT_CAP_SUBS_READ |
				SND_SEQ_PORT_CAP_WRITE |
				SND_SEQ_PORT_CAP_SUBS_WRITE,
			SND_SEQ_PORT_TYPE_MIDI_GENERIC |
				SND_SEQ_PORT_TYPE_APPLICATION));

	int queue_id = snd_seq_alloc_queue(seq);
	RETURN_ON_ERROR("snd_seq_create_queue", queue_id);
	RETURN_ON_ERROR("snd_seq_start_queue", snd_seq_start_queue(seq, queue_id, nullptr));

	// The sequencer object is now ready to be used from other threads.
	{
		lock_guard<mutex> lock(mu);
		alsa_seq = seq;
		alsa_queue_id = queue_id;
	}

	// Listen to the announce port (0:1), which will tell us about new ports.
	RETURN_ON_ERROR("snd_seq_connect_from", snd_seq_connect_from(seq, 0, /*client=*/0, /*port=*/1));

	// Now go through all ports and subscribe to them.
	snd_seq_client_info_t *cinfo;
	snd_seq_client_info_alloca(&cinfo);

	snd_seq_client_info_set_client(cinfo, -1);
	while (snd_seq_query_next_client(seq, cinfo) >= 0) {
		int client = snd_seq_client_info_get_client(cinfo);

		snd_seq_port_info_t *pinfo;
		snd_seq_port_info_alloca(&pinfo);

		snd_seq_port_info_set_client(pinfo, client);
		snd_seq_port_info_set_port(pinfo, -1);
		while (snd_seq_query_next_port(seq, pinfo) >= 0) {
			constexpr int mask = SND_SEQ_PORT_CAP_READ | SND_SEQ_PORT_CAP_SUBS_READ;
			if ((snd_seq_port_info_get_capability(pinfo) & mask) == mask) {
				lock_guard<mutex> lock(mu);
				subscribe_to_port_lock_held(seq, *snd_seq_port_info_get_addr(pinfo));
			}
		}
	}

	int num_alsa_fds = snd_seq_poll_descriptors_count(seq, POLLIN);
	unique_ptr<pollfd[]> fds(new pollfd[num_alsa_fds + 1]);

	while (!should_quit) {
		snd_seq_poll_descriptors(seq, fds.get(), num_alsa_fds, POLLIN);
		fds[num_alsa_fds].fd = should_quit_fd;
		fds[num_alsa_fds].events = POLLIN;
		fds[num_alsa_fds].revents = 0;

		err = poll(fds.get(), num_alsa_fds + 1, -1);
		if (err == 0 || (err == -1 && errno == EINTR)) {
			continue;
		}
		if (err == -1) {
			perror("poll");
			break;
		}
		if (fds[num_alsa_fds].revents) {
			// Activity on should_quit_fd.
			break;
		}

		// Seemingly we can get multiple events in a single poll,
		// and if we don't handle them all, poll will _not_ alert us!
		while (!should_quit) {
			snd_seq_event_t *event;
			err = snd_seq_event_input(seq, &event);
			if (err < 0) {
				if (err == -EINTR) continue;
				if (err == -EAGAIN) break;
				if (err == -ENOSPC) {
					fprintf(stderr, "snd_seq_event_input: Some events were lost.\n");
					continue;
				}
				fprintf(stderr, "snd_seq_event_input: %s\n", snd_strerror(err));
				return;
			}
			if (event) {
				handle_event(seq, event);
			}
		}
	}
}

void MIDIMapper::handle_event(snd_seq_t *seq, snd_seq_event_t *event)
{
	if (event->source.client == snd_seq_client_id(seq)) {
		// Ignore events we sent out ourselves.
		return;
	}

	lock_guard<mutex> lock(mu);
	switch (event->type) {
	case SND_SEQ_EVENT_CONTROLLER: {
		const int controller = event->data.control.param;
		const float value = map_controller_to_float(event->data.control.value);

		receiver->controller_changed(controller);

		// Global controllers.
		match_controller(controller, MIDIMappingBusProto::kLocutFieldNumber, MIDIMappingProto::kLocutBankFieldNumber,
			value, bind(&ControllerReceiver::set_locut, receiver, _2));
		match_controller(controller, MIDIMappingBusProto::kLimiterThresholdFieldNumber, MIDIMappingProto::kLimiterThresholdBankFieldNumber,
			value, bind(&ControllerReceiver::set_limiter_threshold, receiver, _2));
		match_controller(controller, MIDIMappingBusProto::kMakeupGainFieldNumber, MIDIMappingProto::kMakeupGainBankFieldNumber,
			value, bind(&ControllerReceiver::set_makeup_gain, receiver, _2));

		// Bus controllers.
		match_controller(controller, MIDIMappingBusProto::kTrebleFieldNumber, MIDIMappingProto::kTrebleBankFieldNumber,
			value, bind(&ControllerReceiver::set_treble, receiver, _1, _2));
		match_controller(controller, MIDIMappingBusProto::kMidFieldNumber, MIDIMappingProto::kMidBankFieldNumber,
			value, bind(&ControllerReceiver::set_mid, receiver, _1, _2));
		match_controller(controller, MIDIMappingBusProto::kBassFieldNumber, MIDIMappingProto::kBassBankFieldNumber,
			value, bind(&ControllerReceiver::set_bass, receiver, _1, _2));
		match_controller(controller, MIDIMappingBusProto::kGainFieldNumber, MIDIMappingProto::kGainBankFieldNumber,
			value, bind(&ControllerReceiver::set_gain, receiver, _1, _2));
		match_controller(controller, MIDIMappingBusProto::kCompressorThresholdFieldNumber, MIDIMappingProto::kCompressorThresholdBankFieldNumber,
			value, bind(&ControllerReceiver::set_compressor_threshold, receiver, _1, _2));
		match_controller(controller, MIDIMappingBusProto::kFaderFieldNumber, MIDIMappingProto::kFaderBankFieldNumber,
			value, bind(&ControllerReceiver::set_fader, receiver, _1, _2));
		break;
	}
	case SND_SEQ_EVENT_NOTEON: {
		const int note = event->data.note.note;

		receiver->note_on(note);

		for (size_t bus_idx = 0; bus_idx < size_t(mapping_proto->bus_mapping_size()); ++bus_idx) {
			const MIDIMappingBusProto &bus_mapping = mapping_proto->bus_mapping(bus_idx);
			if (bus_mapping.has_prev_bank() &&
			    bus_mapping.prev_bank().note_number() == note) {
				current_controller_bank = (current_controller_bank + num_controller_banks - 1) % num_controller_banks;
				update_highlights();
				update_lights_lock_held();
			}
			if (bus_mapping.has_next_bank() &&
			    bus_mapping.next_bank().note_number() == note) {
				current_controller_bank = (current_controller_bank + 1) % num_controller_banks;
				update_highlights();
				update_lights_lock_held();
			}
			if (bus_mapping.has_select_bank_1() &&
			    bus_mapping.select_bank_1().note_number() == note) {
				current_controller_bank = 0;
				update_highlights();
				update_lights_lock_held();
			}
			if (bus_mapping.has_select_bank_2() &&
			    bus_mapping.select_bank_2().note_number() == note &&
			    num_controller_banks >= 2) {
				current_controller_bank = 1;
				update_highlights();
				update_lights_lock_held();
			}
			if (bus_mapping.has_select_bank_3() &&
			    bus_mapping.select_bank_3().note_number() == note &&
			    num_controller_banks >= 3) {
				current_controller_bank = 2;
				update_highlights();
				update_lights_lock_held();
			}
			if (bus_mapping.has_select_bank_4() &&
			    bus_mapping.select_bank_4().note_number() == note &&
			    num_controller_banks >= 4) {
				current_controller_bank = 3;
				update_highlights();
				update_lights_lock_held();
			}
			if (bus_mapping.has_select_bank_5() &&
			    bus_mapping.select_bank_5().note_number() == note &&
			    num_controller_banks >= 5) {
				current_controller_bank = 4;
				update_highlights();
				update_lights_lock_held();
			}
		}

		match_button(note, MIDIMappingBusProto::kToggleLocutFieldNumber, MIDIMappingProto::kToggleLocutBankFieldNumber,
			bind(&ControllerReceiver::toggle_locut, receiver, _1));
		match_button(note, MIDIMappingBusProto::kToggleAutoGainStagingFieldNumber, MIDIMappingProto::kToggleAutoGainStagingBankFieldNumber,
			bind(&ControllerReceiver::toggle_auto_gain_staging, receiver, _1));
		match_button(note, MIDIMappingBusProto::kToggleCompressorFieldNumber, MIDIMappingProto::kToggleCompressorBankFieldNumber,
			bind(&ControllerReceiver::toggle_compressor, receiver, _1));
		match_button(note, MIDIMappingBusProto::kClearPeakFieldNumber, MIDIMappingProto::kClearPeakBankFieldNumber,
			bind(&ControllerReceiver::clear_peak, receiver, _1));
		match_button(note, MIDIMappingBusProto::kToggleMuteFieldNumber, MIDIMappingProto::kClearPeakBankFieldNumber,
			bind(&ControllerReceiver::toggle_mute, receiver, _1));
		match_button(note, MIDIMappingBusProto::kToggleLimiterFieldNumber, MIDIMappingProto::kToggleLimiterBankFieldNumber,
			bind(&ControllerReceiver::toggle_limiter, receiver));
		match_button(note, MIDIMappingBusProto::kToggleAutoMakeupGainFieldNumber, MIDIMappingProto::kToggleAutoMakeupGainBankFieldNumber,
			bind(&ControllerReceiver::toggle_auto_makeup_gain, receiver));
		break;
	}
	case SND_SEQ_EVENT_PORT_START:
		subscribe_to_port_lock_held(seq, event->data.addr);
		break;
	case SND_SEQ_EVENT_PORT_EXIT:
		printf("MIDI port %d:%d went away.\n", event->data.addr.client, event->data.addr.port);
		break;
	case SND_SEQ_EVENT_PORT_SUBSCRIBED:
		if (event->data.connect.sender.client != 0 &&  // Ignore system senders.
		    event->data.connect.sender.client != snd_seq_client_id(seq) &&
		    event->data.connect.dest.client == snd_seq_client_id(seq)) {
			++num_subscribed_ports;
			update_highlights();
		}
		break;
	case SND_SEQ_EVENT_PORT_UNSUBSCRIBED:
		if (event->data.connect.sender.client != 0 &&  // Ignore system senders.
		    event->data.connect.sender.client != snd_seq_client_id(seq) &&
		    event->data.connect.dest.client == snd_seq_client_id(seq)) {
			--num_subscribed_ports;
			update_highlights();
		}
		break;
	case SND_SEQ_EVENT_NOTEOFF:
	case SND_SEQ_EVENT_CLIENT_START:
	case SND_SEQ_EVENT_CLIENT_EXIT:
	case SND_SEQ_EVENT_CLIENT_CHANGE:
	case SND_SEQ_EVENT_PORT_CHANGE:
		break;
	default:
		printf("Ignoring MIDI event of unknown type %d.\n", event->type);
	}
}

void MIDIMapper::subscribe_to_port_lock_held(snd_seq_t *seq, const snd_seq_addr_t &addr)
{
	// Client 0 (SNDRV_SEQ_CLIENT_SYSTEM) is basically the system; ignore it.
	// MIDI through (SNDRV_SEQ_CLIENT_DUMMY) echoes back what we give it, so ignore that, too.
	if (addr.client == 0 || addr.client == 14) {
		return;
	}

	int err = snd_seq_connect_from(seq, 0, addr.client, addr.port);
	if (err < 0) {
		// Just print out a warning (i.e., don't die); it could
		// very well just be e.g. another application.
		printf("Couldn't subscribe to MIDI port %d:%d (%s).\n",
			addr.client, addr.port, snd_strerror(err));
	} else {
		printf("Subscribed to MIDI port %d:%d.\n", addr.client, addr.port);
	}

	// For sending data back.
	err = snd_seq_connect_to(seq, 0, addr.client, addr.port);
	if (err < 0) {
		printf("Couldn't subscribe MIDI port %d:%d (%s) to us.\n",
			addr.client, addr.port, snd_strerror(err));
	} else {
		printf("Subscribed MIDI port %d:%d to us.\n", addr.client, addr.port);
	}

	current_light_status.clear();  // The current state of the device is unknown.
	update_lights_lock_held();
}

void MIDIMapper::match_controller(int controller, int field_number, int bank_field_number, float value, function<void(unsigned, float)> func)
{
	if (bank_mismatch(bank_field_number)) {
		return;
	}

	for (size_t bus_idx = 0; bus_idx < size_t(mapping_proto->bus_mapping_size()); ++bus_idx) {
		const MIDIMappingBusProto &bus_mapping = mapping_proto->bus_mapping(bus_idx);

		const FieldDescriptor *descriptor = bus_mapping.GetDescriptor()->FindFieldByNumber(field_number);
		const Reflection *bus_reflection = bus_mapping.GetReflection();
		if (!bus_reflection->HasField(bus_mapping, descriptor)) {
			continue;
		}
		const MIDIControllerProto &controller_proto =
			static_cast<const MIDIControllerProto &>(bus_reflection->GetMessage(bus_mapping, descriptor));
		if (controller_proto.controller_number() == controller) {
			func(bus_idx, value);
		}
	}
}

void MIDIMapper::match_button(int note, int field_number, int bank_field_number, function<void(unsigned)> func)
{
	if (bank_mismatch(bank_field_number)) {
		return;
	}

	for (size_t bus_idx = 0; bus_idx < size_t(mapping_proto->bus_mapping_size()); ++bus_idx) {
		const MIDIMappingBusProto &bus_mapping = mapping_proto->bus_mapping(bus_idx);

		const FieldDescriptor *descriptor = bus_mapping.GetDescriptor()->FindFieldByNumber(field_number);
		const Reflection *bus_reflection = bus_mapping.GetReflection();
		if (!bus_reflection->HasField(bus_mapping, descriptor)) {
			continue;
		}
		const MIDIButtonProto &button_proto =
			static_cast<const MIDIButtonProto &>(bus_reflection->GetMessage(bus_mapping, descriptor));
		if (button_proto.note_number() == note) {
			func(bus_idx);
		}
	}
}

bool MIDIMapper::has_active_controller(unsigned bus_idx, int field_number, int bank_field_number)
{
	if (bank_mismatch(bank_field_number)) {
		return false;
	}

	const MIDIMappingBusProto &bus_mapping = mapping_proto->bus_mapping(bus_idx);
	const FieldDescriptor *descriptor = bus_mapping.GetDescriptor()->FindFieldByNumber(field_number);
	const Reflection *bus_reflection = bus_mapping.GetReflection();
	return bus_reflection->HasField(bus_mapping, descriptor);
}

bool MIDIMapper::bank_mismatch(int bank_field_number)
{
	const FieldDescriptor *bank_descriptor = mapping_proto->GetDescriptor()->FindFieldByNumber(bank_field_number);
	const Reflection *reflection = mapping_proto->GetReflection();
	return (reflection->HasField(*mapping_proto, bank_descriptor) &&
 	        reflection->GetInt32(*mapping_proto, bank_descriptor) != current_controller_bank);
}

void MIDIMapper::refresh_highlights()
{
	receiver->clear_all_highlights();
	update_highlights();
}

void MIDIMapper::refresh_lights()
{
	lock_guard<mutex> lock(mu);
	update_lights_lock_held();
}

void MIDIMapper::update_highlights()
{
	if (num_subscribed_ports.load() == 0) {
		receiver->clear_all_highlights();
		return;
	}

	// Global controllers.
	bool highlight_locut = false;
	bool highlight_limiter_threshold = false;
	bool highlight_makeup_gain = false;
	bool highlight_toggle_limiter = false;
	bool highlight_toggle_auto_makeup_gain = false;
	for (size_t bus_idx = 0; bus_idx < size_t(mapping_proto->bus_mapping_size()); ++bus_idx) {
		if (has_active_controller(
			bus_idx, MIDIMappingBusProto::kLocutFieldNumber, MIDIMappingProto::kLocutBankFieldNumber)) {
			highlight_locut = true;
		}
		if (has_active_controller(
			bus_idx, MIDIMappingBusProto::kLimiterThresholdFieldNumber, MIDIMappingProto::kLimiterThresholdBankFieldNumber)) {
			highlight_limiter_threshold = true;
		}
		if (has_active_controller(
			bus_idx, MIDIMappingBusProto::kMakeupGainFieldNumber, MIDIMappingProto::kMakeupGainBankFieldNumber)) {
			highlight_makeup_gain = true;
		}
		if (has_active_controller(
			bus_idx, MIDIMappingBusProto::kToggleLimiterFieldNumber, MIDIMappingProto::kToggleLimiterBankFieldNumber)) {
			highlight_toggle_limiter = true;
		}
		if (has_active_controller(
			bus_idx, MIDIMappingBusProto::kToggleAutoMakeupGainFieldNumber, MIDIMappingProto::kToggleAutoMakeupGainBankFieldNumber)) {
			highlight_toggle_auto_makeup_gain = true;
		}
	}
	receiver->highlight_locut(highlight_locut);
	receiver->highlight_limiter_threshold(highlight_limiter_threshold);
	receiver->highlight_makeup_gain(highlight_makeup_gain);
	receiver->highlight_toggle_limiter(highlight_toggle_limiter);
	receiver->highlight_toggle_auto_makeup_gain(highlight_toggle_auto_makeup_gain);

	// Per-bus controllers.
	for (size_t bus_idx = 0; bus_idx < size_t(mapping_proto->bus_mapping_size()); ++bus_idx) {
		receiver->highlight_treble(bus_idx, has_active_controller(
			bus_idx, MIDIMappingBusProto::kTrebleFieldNumber, MIDIMappingProto::kTrebleBankFieldNumber));
		receiver->highlight_mid(bus_idx, has_active_controller(
			bus_idx, MIDIMappingBusProto::kMidFieldNumber, MIDIMappingProto::kMidBankFieldNumber));
		receiver->highlight_bass(bus_idx, has_active_controller(
			bus_idx, MIDIMappingBusProto::kBassFieldNumber, MIDIMappingProto::kBassBankFieldNumber));
		receiver->highlight_gain(bus_idx, has_active_controller(
			bus_idx, MIDIMappingBusProto::kGainFieldNumber, MIDIMappingProto::kGainBankFieldNumber));
		receiver->highlight_compressor_threshold(bus_idx, has_active_controller(
			bus_idx, MIDIMappingBusProto::kCompressorThresholdFieldNumber, MIDIMappingProto::kCompressorThresholdBankFieldNumber));
		receiver->highlight_fader(bus_idx, has_active_controller(
			bus_idx, MIDIMappingBusProto::kFaderFieldNumber, MIDIMappingProto::kFaderBankFieldNumber));
		receiver->highlight_mute(bus_idx, has_active_controller(
			bus_idx, MIDIMappingBusProto::kToggleMuteFieldNumber, MIDIMappingProto::kToggleMuteBankFieldNumber));
		receiver->highlight_toggle_locut(bus_idx, has_active_controller(
			bus_idx, MIDIMappingBusProto::kToggleLocutFieldNumber, MIDIMappingProto::kToggleLocutBankFieldNumber));
		receiver->highlight_toggle_auto_gain_staging(bus_idx, has_active_controller(
			bus_idx, MIDIMappingBusProto::kToggleAutoGainStagingFieldNumber, MIDIMappingProto::kToggleAutoGainStagingBankFieldNumber));
		receiver->highlight_toggle_compressor(bus_idx, has_active_controller(
			bus_idx, MIDIMappingBusProto::kToggleCompressorFieldNumber, MIDIMappingProto::kToggleCompressorBankFieldNumber));
	}
}

void MIDIMapper::update_lights_lock_held()
{
	if (alsa_seq == nullptr || global_audio_mixer == nullptr) {
		return;
	}

	set<unsigned> active_lights;  // Desired state.
	if (current_controller_bank == 0) {
		activate_lights_all_buses(MIDIMappingBusProto::kBank1IsSelectedFieldNumber, &active_lights);
	}
	if (current_controller_bank == 1) {
		activate_lights_all_buses(MIDIMappingBusProto::kBank2IsSelectedFieldNumber, &active_lights);
	}
	if (current_controller_bank == 2) {
		activate_lights_all_buses(MIDIMappingBusProto::kBank3IsSelectedFieldNumber, &active_lights);
	}
	if (current_controller_bank == 3) {
		activate_lights_all_buses(MIDIMappingBusProto::kBank4IsSelectedFieldNumber, &active_lights);
	}
	if (current_controller_bank == 4) {
		activate_lights_all_buses(MIDIMappingBusProto::kBank5IsSelectedFieldNumber, &active_lights);
	}
	if (global_audio_mixer->get_limiter_enabled()) {
		activate_lights_all_buses(MIDIMappingBusProto::kLimiterIsOnFieldNumber, &active_lights);
	}
	if (global_audio_mixer->get_final_makeup_gain_auto()) {
		activate_lights_all_buses(MIDIMappingBusProto::kAutoMakeupGainIsOnFieldNumber, &active_lights);
	}
	unsigned num_buses = min<unsigned>(global_audio_mixer->num_buses(), mapping_proto->bus_mapping_size());
	for (unsigned bus_idx = 0; bus_idx < num_buses; ++bus_idx) {
		if (global_audio_mixer->get_mute(bus_idx)) {
			activate_lights(bus_idx, MIDIMappingBusProto::kIsMutedFieldNumber, &active_lights);
		}
		if (global_audio_mixer->get_locut_enabled(bus_idx)) {
			activate_lights(bus_idx, MIDIMappingBusProto::kLocutIsOnFieldNumber, &active_lights);
		}
		if (global_audio_mixer->get_gain_staging_auto(bus_idx)) {
			activate_lights(bus_idx, MIDIMappingBusProto::kAutoGainStagingIsOnFieldNumber, &active_lights);
		}
		if (global_audio_mixer->get_compressor_enabled(bus_idx)) {
			activate_lights(bus_idx, MIDIMappingBusProto::kCompressorIsOnFieldNumber, &active_lights);
		}
		if (has_peaked[bus_idx]) {
			activate_lights(bus_idx, MIDIMappingBusProto::kHasPeakedFieldNumber, &active_lights);
		}
	}

	unsigned num_events = 0;
	for (unsigned note_num = 1; note_num <= 127; ++note_num) {
		bool active = active_lights.count(note_num);
		if (current_light_status.count(note_num) &&
		    current_light_status[note_num] == active) {
			// Already known to be in the desired state.
			continue;
		}

		snd_seq_event_t ev;
		snd_seq_ev_clear(&ev);

		// Some devices drop events if we throw them onto them
		// too quickly. Add a 1 ms delay for each.
		snd_seq_real_time_t tm{0, num_events++ * 1000000};
		snd_seq_ev_schedule_real(&ev, alsa_queue_id, true, &tm);
		snd_seq_ev_set_source(&ev, 0);
		snd_seq_ev_set_subs(&ev);

		// For some reason, not all devices respond to note off.
		// Use note-on with velocity of 0 (which is equivalent) instead.
		snd_seq_ev_set_noteon(&ev, /*channel=*/0, note_num, active ? 127 : 0);
		WARN_ON_ERROR("snd_seq_event_output", snd_seq_event_output(alsa_seq, &ev));
		current_light_status[note_num] = active;
	}
	WARN_ON_ERROR("snd_seq_drain_output", snd_seq_drain_output(alsa_seq));
}

void MIDIMapper::activate_lights(unsigned bus_idx, int field_number, set<unsigned> *active_lights)
{
	const MIDIMappingBusProto &bus_mapping = mapping_proto->bus_mapping(bus_idx);

	const FieldDescriptor *descriptor = bus_mapping.GetDescriptor()->FindFieldByNumber(field_number);
	const Reflection *bus_reflection = bus_mapping.GetReflection();
	if (!bus_reflection->HasField(bus_mapping, descriptor)) {
		return;
	}
	const MIDILightProto &light_proto =
		static_cast<const MIDILightProto &>(bus_reflection->GetMessage(bus_mapping, descriptor));
	active_lights->insert(light_proto.note_number());
}

void MIDIMapper::activate_lights_all_buses(int field_number, set<unsigned> *active_lights)
{
	for (size_t bus_idx = 0; bus_idx < size_t(mapping_proto->bus_mapping_size()); ++bus_idx) {
		const MIDIMappingBusProto &bus_mapping = mapping_proto->bus_mapping(bus_idx);

		const FieldDescriptor *descriptor = bus_mapping.GetDescriptor()->FindFieldByNumber(field_number);
		const Reflection *bus_reflection = bus_mapping.GetReflection();
		if (!bus_reflection->HasField(bus_mapping, descriptor)) {
			continue;
		}
		const MIDILightProto &light_proto =
			static_cast<const MIDILightProto &>(bus_reflection->GetMessage(bus_mapping, descriptor));
		active_lights->insert(light_proto.note_number());
	}
}
