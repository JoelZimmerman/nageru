#include "input_mapping.h"

#include <assert.h>
#include <fcntl.h>
#include <google/protobuf/io/zero_copy_stream_impl.h>
#include <google/protobuf/text_format.h>
#include <stdio.h>
#include <set>
#include <utility>

#include "audio_mixer.h" 
#include "state.pb.h"

using namespace std;
using namespace google::protobuf;

bool save_input_mapping_to_file(const map<DeviceSpec, DeviceInfo> &devices, const InputMapping &input_mapping, const string &filename)
{
	InputMappingProto mapping_proto;
	{
		map<DeviceSpec, unsigned> used_devices;
		for (const InputMapping::Bus &bus : input_mapping.buses) {
			if (!used_devices.count(bus.device)) {
				used_devices.emplace(bus.device, used_devices.size());
				global_audio_mixer->serialize_device(bus.device, mapping_proto.add_device());
			}

			BusProto *bus_proto = mapping_proto.add_bus();
			bus_proto->set_name(bus.name);
			bus_proto->set_device_index(used_devices[bus.device]);
			bus_proto->set_source_channel_left(bus.source_channel[0]);
			bus_proto->set_source_channel_right(bus.source_channel[1]);
		}
	}

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

bool load_input_mapping_from_file(const map<DeviceSpec, DeviceInfo> &devices, const string &filename, InputMapping *new_mapping)
{
	// Read and parse the protobuf from disk.
	int fd = open(filename.c_str(), O_RDONLY);
	if (fd == -1) {
		perror(filename.c_str());
		return false;
	}
	io::FileInputStream input(fd);  // Takes ownership of fd.
	InputMappingProto mapping_proto;
	if (!TextFormat::Parse(&input, &mapping_proto)) {
		input.Close();
		return false;
	}
	input.Close();

	// Map devices in the proto to our current ones:

	// Get a list of all active devices.
	set<DeviceSpec> remaining_devices;
	for (const auto &device_spec_and_info : devices) {
		remaining_devices.insert(device_spec_and_info.first);
	}

	// Now look at every device in the serialized protobuf and try to map
	// it to one device we haven't taken yet. This isn't a full maximal matching,
	// but it's good enough for our uses.
	vector<DeviceSpec> device_mapping;
	for (unsigned device_index = 0; device_index < unsigned(mapping_proto.device_size()); ++device_index) {
		const DeviceSpecProto &device_proto = mapping_proto.device(device_index);
		switch (device_proto.type()) {
		case DeviceSpecProto::SILENCE:
			device_mapping.push_back(DeviceSpec{InputSourceType::SILENCE, 0});
			break;
		case DeviceSpecProto::CAPTURE_CARD: {
			// First see if there's a card that matches on both index and name.
			DeviceSpec spec{InputSourceType::CAPTURE_CARD, unsigned(device_proto.index())};
			assert(devices.count(spec));
			const DeviceInfo &dev = devices.find(spec)->second;
			if (remaining_devices.count(spec) &&
			    dev.display_name == device_proto.display_name()) {
				device_mapping.push_back(spec);
				remaining_devices.erase(spec);
				goto found_capture_card;
			}

			// Scan and see if there's a match on name alone.
			for (const DeviceSpec &spec : remaining_devices) {
				if (spec.type == InputSourceType::CAPTURE_CARD &&
				    dev.display_name == device_proto.display_name()) {
					device_mapping.push_back(spec);
					remaining_devices.erase(spec);
					goto found_capture_card;
				}
			}

			// OK, see if at least the index is free.
			if (remaining_devices.count(spec)) {
				device_mapping.push_back(spec);
				remaining_devices.erase(spec);
				goto found_capture_card;
			}

			// Give up.
			device_mapping.push_back(DeviceSpec{InputSourceType::SILENCE, 0});
found_capture_card:
			break;
		}
		case DeviceSpecProto::ALSA_INPUT: {
			// For ALSA, we don't really care about index, but we can use address
			// in its place.

			// First see if there's a card that matches on name, num_channels and address.
			for (const DeviceSpec &spec : remaining_devices) {
				assert(devices.count(spec));
				const DeviceInfo &dev = devices.find(spec)->second;
				if (spec.type == InputSourceType::ALSA_INPUT &&
				    dev.alsa_name == device_proto.alsa_name() &&
				    dev.alsa_info == device_proto.alsa_info() &&
				    int(dev.num_channels) == device_proto.num_channels() &&
				    dev.alsa_address == device_proto.address()) {
					device_mapping.push_back(spec);
					remaining_devices.erase(spec);
					goto found_alsa_input;
				}
			}

			// Looser check: Ignore the address.
			for (const DeviceSpec &spec : remaining_devices) {
				assert(devices.count(spec));
				const DeviceInfo &dev = devices.find(spec)->second;
				if (spec.type == InputSourceType::ALSA_INPUT &&
				    dev.alsa_name == device_proto.alsa_name() &&
				    dev.alsa_info == device_proto.alsa_info() &&
				    int(dev.num_channels) == device_proto.num_channels()) {
					device_mapping.push_back(spec);
					remaining_devices.erase(spec);
					goto found_alsa_input;
				}
			}

			// OK, so we couldn't map this to a device, but perhaps one is added
			// at some point in the future through hotplug. Create a dead card
			// matching this one; right now, it will give only silence,
			// but it could be replaced with something later.
			//
			// NOTE: There's a potential race condition here, if the card
			// gets inserted while we're doing the device remapping
			// (or perhaps more realistically, while we're reading the
			// input mapping from disk).
			DeviceSpec dead_card_spec;
			dead_card_spec = global_audio_mixer->create_dead_card(
				device_proto.alsa_name(), device_proto.alsa_info(), device_proto.num_channels());
			device_mapping.push_back(dead_card_spec);

found_alsa_input:
			break;
		}
		default:
			assert(false);
		}
	}

	for (const BusProto &bus_proto : mapping_proto.bus()) {
		if (bus_proto.device_index() < 0 || unsigned(bus_proto.device_index()) >= device_mapping.size()) {
			return false;
		}
		InputMapping::Bus bus;
		bus.name = bus_proto.name();
		bus.device = device_mapping[bus_proto.device_index()];
		bus.source_channel[0] = bus_proto.source_channel_left();
		bus.source_channel[1] = bus_proto.source_channel_right();
		new_mapping->buses.push_back(bus);
	}

	return true;
}
