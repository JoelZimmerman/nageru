#include "alsa_pool.h"

#include <alsa/asoundlib.h>
#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/eventfd.h>
#include <sys/inotify.h>
#include <unistd.h>
#include <algorithm>
#include <chrono>
#include <functional>
#include <iterator>
#include <memory>
#include <ratio>

#include "alsa_input.h"
#include "audio_mixer.h"
#include "defs.h"
#include "input_mapping.h"
#include "state.pb.h"

using namespace std;
using namespace std::placeholders;

ALSAPool::ALSAPool()
{
	should_quit_fd = eventfd(/*initval=*/0, /*flags=*/0);
	assert(should_quit_fd != -1);
}

ALSAPool::~ALSAPool()
{
	for (Device &device : devices) {
		if (device.input != nullptr) {
			device.input->stop_capture_thread();
		}
	}
	should_quit = true;
	const uint64_t one = 1;
	if (write(should_quit_fd, &one, sizeof(one)) != sizeof(one)) {
		perror("write(should_quit_fd)");
		exit(1);
	}
	inotify_thread.join();

	while (retry_threads_running > 0) {
		this_thread::sleep_for(std::chrono::milliseconds(100));
	}
}

std::vector<ALSAPool::Device> ALSAPool::get_devices()
{
	lock_guard<mutex> lock(mu);
	for (Device &device : devices) {
		device.held = true;
	}
	return devices;
}

void ALSAPool::hold_device(unsigned index)
{
	lock_guard<mutex> lock(mu);
	assert(index < devices.size());
	devices[index].held = true;
}

void ALSAPool::release_device(unsigned index)
{
	lock_guard<mutex> lock(mu);
	if (index < devices.size()) {
		devices[index].held = false;
	}
}

void ALSAPool::enumerate_devices()
{
	// Enumerate all cards.
	for (int card_index = -1; snd_card_next(&card_index) == 0 && card_index >= 0; ) {
		char address[256];
		snprintf(address, sizeof(address), "hw:%d", card_index);

		snd_ctl_t *ctl;
		int err = snd_ctl_open(&ctl, address, 0);
		if (err < 0) {
			printf("%s: %s\n", address, snd_strerror(err));
			continue;
		}
		unique_ptr<snd_ctl_t, decltype(snd_ctl_close)*> ctl_closer(ctl, snd_ctl_close);

		// Enumerate all devices on this card.
		for (int dev_index = -1; snd_ctl_pcm_next_device(ctl, &dev_index) == 0 && dev_index >= 0; ) {
			probe_device_with_retry(card_index, dev_index);
		}
	}
}

void ALSAPool::probe_device_with_retry(unsigned card_index, unsigned dev_index)
{
	char address[256];
	snprintf(address, sizeof(address), "hw:%d,%d", card_index, dev_index);

	lock_guard<mutex> lock(add_device_mutex);
	if (add_device_tries_left.count(address)) {
		// Some thread is already busy retrying this,
		// so just reset its count.
		add_device_tries_left[address] = num_retries;
		return;
	}

	// Try (while still holding the lock) to add the device synchronously.
	ProbeResult result = probe_device_once(card_index, dev_index);
	if (result == ProbeResult::SUCCESS) {
		return;
	} else if (result == ProbeResult::FAILURE) {
		return;
	}
	assert(result == ProbeResult::DEFER);

	// Add failed for whatever reason (probably just that the device
	// isn't up yet. Set up a count so that nobody else starts a thread,
	// then start it ourselves.
	fprintf(stderr, "Trying %s again in one second...\n", address);
	add_device_tries_left[address] = num_retries;
	++retry_threads_running;
	thread(&ALSAPool::probe_device_retry_thread_func, this, card_index, dev_index).detach();
}

void ALSAPool::probe_device_retry_thread_func(unsigned card_index, unsigned dev_index)
{
	char address[256];
	snprintf(address, sizeof(address), "hw:%d,%d", card_index, dev_index);

	char thread_name[16];
	snprintf(thread_name, sizeof(thread_name), "Reprobe_hw:%d,%d", card_index, dev_index);
	pthread_setname_np(pthread_self(), thread_name);

	for ( ;; ) {  // Termination condition within the loop.
		sleep(1);

		// See if there are any retries left.
		lock_guard<mutex> lock(add_device_mutex);
		if (should_quit ||
		    !add_device_tries_left.count(address) ||
		    add_device_tries_left[address] == 0) {
			add_device_tries_left.erase(address);
			fprintf(stderr, "Giving up probe of %s.\n", address);
			break;
		}

		// Seemingly there were. Give it a try (we still hold the mutex).
		ProbeResult result = probe_device_once(card_index, dev_index);
		if (result == ProbeResult::SUCCESS) {
			add_device_tries_left.erase(address);
			fprintf(stderr, "Probe of %s succeeded.\n", address);
			break;
		} else if (result == ProbeResult::FAILURE || --add_device_tries_left[address] == 0) {
			add_device_tries_left.erase(address);
			fprintf(stderr, "Giving up probe of %s.\n", address);
			break;
		}

		// Failed again.
		assert(result == ProbeResult::DEFER);
		fprintf(stderr, "Trying %s again in one second (%d tries left)...\n",
			address, add_device_tries_left[address]);
	}

	--retry_threads_running;
}

ALSAPool::ProbeResult ALSAPool::probe_device_once(unsigned card_index, unsigned dev_index)
{
	char address[256];
	snprintf(address, sizeof(address), "hw:%d", card_index);
	snd_ctl_t *ctl;
	int err = snd_ctl_open(&ctl, address, 0);
	if (err < 0) {
		printf("%s: %s\n", address, snd_strerror(err));
		return ALSAPool::ProbeResult::DEFER;
	}
	unique_ptr<snd_ctl_t, decltype(snd_ctl_close)*> ctl_closer(ctl, snd_ctl_close);

	snd_pcm_info_t *pcm_info;
	snd_pcm_info_alloca(&pcm_info);
	snd_pcm_info_set_device(pcm_info, dev_index);
	snd_pcm_info_set_subdevice(pcm_info, 0);
	snd_pcm_info_set_stream(pcm_info, SND_PCM_STREAM_CAPTURE);
	if (snd_ctl_pcm_info(ctl, pcm_info) < 0) {
		// Not available for capture.
		printf("%s: Not available for capture.\n", address);
		return ALSAPool::ProbeResult::DEFER;
	}

	snprintf(address, sizeof(address), "hw:%d,%d", card_index, dev_index);

	unsigned num_channels = 0;

	// Find all channel maps for this device, and pick out the one
	// with the most channels.
	snd_pcm_chmap_query_t **cmaps = snd_pcm_query_chmaps_from_hw(card_index, dev_index, 0, SND_PCM_STREAM_CAPTURE);
	if (cmaps != nullptr) {
		for (snd_pcm_chmap_query_t **ptr = cmaps; *ptr; ++ptr) {
			num_channels = max(num_channels, (*ptr)->map.channels);
		}
		snd_pcm_free_chmaps(cmaps);
	}
	if (num_channels == 0) {
		// Device had no channel maps. We need to open it to query.
		// TODO: Do this asynchronously.
		snd_pcm_t *pcm_handle;
		int err = snd_pcm_open(&pcm_handle, address, SND_PCM_STREAM_CAPTURE, 0);
		if (err < 0) {
			printf("%s: %s\n", address, snd_strerror(err));
			return ALSAPool::ProbeResult::DEFER;
		}
		snd_pcm_hw_params_t *hw_params;
		snd_pcm_hw_params_alloca(&hw_params);
		unsigned sample_rate;
		if (!ALSAInput::set_base_params(address, pcm_handle, hw_params, &sample_rate)) {
			snd_pcm_close(pcm_handle);
			return ALSAPool::ProbeResult::DEFER;
		}
		err = snd_pcm_hw_params_get_channels_max(hw_params, &num_channels);
		if (err < 0) {
			fprintf(stderr, "[%s] snd_pcm_hw_params_get_channels_max(): %s\n",
				address, snd_strerror(err));
			snd_pcm_close(pcm_handle);
			return ALSAPool::ProbeResult::DEFER;
		}
		snd_pcm_close(pcm_handle);
	}

	if (num_channels == 0) {
		printf("%s: No channel maps with channels\n", address);
		return ALSAPool::ProbeResult::FAILURE;
	}

	snd_ctl_card_info_t *card_info;
	snd_ctl_card_info_alloca(&card_info);
	snd_ctl_card_info(ctl, card_info);

	string name = snd_ctl_card_info_get_name(card_info);
	string info = snd_pcm_info_get_name(pcm_info);

	unsigned internal_dev_index;
	string display_name;
	{
		lock_guard<mutex> lock(mu);
		internal_dev_index = find_free_device_index(name, info, num_channels, address);
		devices[internal_dev_index].address = address;
		devices[internal_dev_index].name = name;
		devices[internal_dev_index].info = info;
		devices[internal_dev_index].num_channels = num_channels;
		// Note: Purposefully does not overwrite held.

		display_name = devices[internal_dev_index].display_name();
	}

	fprintf(stderr, "%s: Probed successfully.\n", address);

	reset_device(internal_dev_index);  // Restarts it if it is held (ie., we just replaced a dead card).

	DeviceSpec spec{InputSourceType::ALSA_INPUT, internal_dev_index};
	global_audio_mixer->set_display_name(spec, display_name);
	global_audio_mixer->trigger_state_changed_callback();

	return ALSAPool::ProbeResult::SUCCESS;
}

void ALSAPool::unplug_device(unsigned card_index, unsigned dev_index)
{
	char address[256];
	snprintf(address, sizeof(address), "hw:%d,%d", card_index, dev_index);
	for (unsigned i = 0; i < devices.size(); ++i) {
		if (devices[i].state != Device::State::EMPTY &&
		    devices[i].state != Device::State::DEAD &&
		    devices[i].address == address) {
			free_card(i);
		}
	}
}

void ALSAPool::init()
{
	inotify_thread = thread(&ALSAPool::inotify_thread_func, this);
	enumerate_devices();
}

void ALSAPool::inotify_thread_func()
{
	pthread_setname_np(pthread_self(), "ALSA_Hotplug");

	int inotify_fd = inotify_init();
	if (inotify_fd == -1) {
		perror("inotify_init()");
		fprintf(stderr, "No hotplug of ALSA devices available.\n");
		return;
	}

	int watch_fd = inotify_add_watch(inotify_fd, "/dev/snd", IN_MOVE | IN_CREATE | IN_DELETE);
	if (watch_fd == -1) {
		perror("inotify_add_watch()");
		fprintf(stderr, "No hotplug of ALSA devices available.\n");
		close(inotify_fd);
		return;
	}

	int size = sizeof(inotify_event) + NAME_MAX + 1;
	unique_ptr<char[]> buf(new char[size]);
	while (!should_quit) {
		pollfd fds[2];
		fds[0].fd = inotify_fd;
		fds[0].events = POLLIN;
		fds[0].revents = 0;
		fds[1].fd = should_quit_fd;
		fds[1].events = POLLIN;
		fds[1].revents = 0;

		int ret = poll(fds, 2, -1);
		if (ret == -1) {
			if (errno == EINTR) {
				continue;
			} else {
				perror("poll(inotify_fd)");
				return;
			}
		}
		if (ret == 0) {
			continue;
		}

		if (fds[1].revents) break;  // should_quit_fd asserted.

		ret = read(inotify_fd, buf.get(), size);
		if (ret == -1) {
			if (errno == EINTR) {
				continue;
			} else {
				perror("read(inotify_fd)");
				close(watch_fd);
				close(inotify_fd);
				return;
			}
		}
		if (ret < int(sizeof(inotify_event))) {
			fprintf(stderr, "inotify read unexpectedly returned %d, giving up hotplug of ALSA devices.\n",
				int(ret));
			close(watch_fd);
			close(inotify_fd);
			return;
		}

		for (int i = 0; i < ret; ) {
			const inotify_event *event = reinterpret_cast<const inotify_event *>(&buf[i]);
			i += sizeof(inotify_event) + event->len;

			if (event->mask & IN_Q_OVERFLOW) {
				fprintf(stderr, "WARNING: inotify overflowed, may lose ALSA hotplug events.\n");
				continue;
			}
			unsigned card, device;
			char type;
			if (sscanf(event->name, "pcmC%uD%u%c", &card, &device, &type) == 3 && type == 'c') {
				if (event->mask & (IN_MOVED_FROM | IN_DELETE)) {
					printf("Deleted capture device: Card %u, device %u\n", card, device);
					unplug_device(card, device);
				}
				if (event->mask & (IN_MOVED_TO | IN_CREATE)) {
					printf("Adding capture device: Card %u, device %u\n", card, device);
					probe_device_with_retry(card, device);
				}
			}
		}
	}
	close(watch_fd);
	close(inotify_fd);
	close(should_quit_fd);
}

void ALSAPool::reset_device(unsigned index)
{
	lock_guard<mutex> lock(mu);
	Device *device = &devices[index];
	if (inputs[index] != nullptr) {
		inputs[index]->stop_capture_thread();
	}
	if (!device->held) {
		inputs[index].reset();
	} else {
		// TODO: Put on a background thread instead of locking?
		auto callback = bind(&AudioMixer::add_audio, global_audio_mixer, DeviceSpec{InputSourceType::ALSA_INPUT, index}, _1, _2, _3, _4, _5);
		inputs[index].reset(new ALSAInput(device->address.c_str(), OUTPUT_FREQUENCY, device->num_channels, callback, this, index));
		inputs[index]->start_capture_thread();
	}
	device->input = inputs[index].get();
}

unsigned ALSAPool::get_capture_frequency(unsigned index)
{
	lock_guard<mutex> lock(mu);
	assert(devices[index].held);
	if (devices[index].input)
		return devices[index].input->get_sample_rate();
	else
		return OUTPUT_FREQUENCY;
}

ALSAPool::Device::State ALSAPool::get_card_state(unsigned index)
{
	lock_guard<mutex> lock(mu);
	assert(devices[index].held);
	return devices[index].state;
}

void ALSAPool::set_card_state(unsigned index, ALSAPool::Device::State state)
{
	{
		lock_guard<mutex> lock(mu);
		devices[index].state = state;
	}

	DeviceSpec spec{InputSourceType::ALSA_INPUT, index};
	bool silence = (state != ALSAPool::Device::State::RUNNING);
	while (!global_audio_mixer->silence_card(spec, silence))
		;
	global_audio_mixer->trigger_state_changed_callback();
}

unsigned ALSAPool::find_free_device_index(const string &name, const string &info, unsigned num_channels, const string &address)
{
	// First try to find an exact match on a dead card.
	for (unsigned i = 0; i < devices.size(); ++i) {
		if (devices[i].state == Device::State::DEAD &&
		    devices[i].address == address &&
		    devices[i].name == name &&
		    devices[i].info == info &&
		    devices[i].num_channels == num_channels) {
			devices[i].state = Device::State::READY;
			return i;
		}
	}

	// Then try to find a match on everything but the address
	// (probably that devices were plugged back in a different order).
	// If we have two cards that are equal, this might get them mixed up,
	// but we don't have anything better.
	for (unsigned i = 0; i < devices.size(); ++i) {
		if (devices[i].state == Device::State::DEAD &&
		    devices[i].name == name &&
		    devices[i].info == info &&
		    devices[i].num_channels == num_channels) {
			devices[i].state = Device::State::READY;
			return i;
		}
	}

	// OK, so we didn't find a match; see if there are any empty slots.
	for (unsigned i = 0; i < devices.size(); ++i) {
		if (devices[i].state == Device::State::EMPTY) {
			devices[i].state = Device::State::READY;
			devices[i].held = false;
			return i;
		}
	}

	// Failing that, we just insert the new device at the end.
	Device new_dev;
	new_dev.state = Device::State::READY;
	new_dev.held = false;
	devices.push_back(new_dev);
	inputs.emplace_back(nullptr);
	return devices.size() - 1;
}

unsigned ALSAPool::create_dead_card(const string &name, const string &info, unsigned num_channels)
{
	lock_guard<mutex> lock(mu);

	// See if there are any empty slots. If not, insert one at the end.
	vector<Device>::iterator free_device =
		find_if(devices.begin(), devices.end(),
			[](const Device &device) { return device.state == Device::State::EMPTY; });
	if (free_device == devices.end()) {
		devices.push_back(Device());
		inputs.emplace_back(nullptr);
		free_device = devices.end() - 1;
	}

	free_device->state = Device::State::DEAD;
	free_device->name = name;
	free_device->info = info;
	free_device->num_channels = num_channels;
	free_device->held = true;

	return distance(devices.begin(), free_device);
}

void ALSAPool::serialize_device(unsigned index, DeviceSpecProto *serialized)
{
	lock_guard<mutex> lock(mu);
	assert(index < devices.size());
	assert(devices[index].held);
	serialized->set_type(DeviceSpecProto::ALSA_INPUT);
	serialized->set_index(index);
	serialized->set_display_name(devices[index].display_name());
	serialized->set_alsa_name(devices[index].name);
	serialized->set_alsa_info(devices[index].info);
	serialized->set_num_channels(devices[index].num_channels);
	serialized->set_address(devices[index].address);
}

void ALSAPool::free_card(unsigned index)
{
	DeviceSpec spec{InputSourceType::ALSA_INPUT, index};
	while (!global_audio_mixer->silence_card(spec, true))
		;

	{
		lock_guard<mutex> lock(mu);
		if (devices[index].held) {
			devices[index].state = Device::State::DEAD;
		} else {
			devices[index].state = Device::State::EMPTY;
			inputs[index].reset();
		}
		while (!devices.empty() && devices.back().state == Device::State::EMPTY) {
			devices.pop_back();
			inputs.pop_back();
		}
	}

	global_audio_mixer->trigger_state_changed_callback();
}
