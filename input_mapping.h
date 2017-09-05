#ifndef _INPUT_MAPPING_H
#define _INPUT_MAPPING_H 1

#include <stdint.h>
#include <map>
#include <string>
#include <vector>

enum class InputSourceType { SILENCE, CAPTURE_CARD, ALSA_INPUT };
struct DeviceSpec {
	InputSourceType type;
	unsigned index;

	bool operator== (const DeviceSpec &other) const {
		return type == other.type && index == other.index;
	}

	bool operator< (const DeviceSpec &other) const {
		if (type != other.type)
			return type < other.type;
		return index < other.index;
	}
};
struct DeviceInfo {
	std::string display_name;
	unsigned num_channels;
	std::string alsa_name, alsa_info, alsa_address;  // ALSA devices only, obviously.
};

static inline uint64_t DeviceSpec_to_key(const DeviceSpec &device_spec)
{
	return (uint64_t(device_spec.type) << 32) | device_spec.index;
}

static inline DeviceSpec key_to_DeviceSpec(uint64_t key)
{
	return DeviceSpec{ InputSourceType(key >> 32), unsigned(key & 0xffffffff) };
}

struct InputMapping {
	struct Bus {
		std::string name;
		DeviceSpec device;
		int source_channel[2] { -1, -1 };  // Left and right. -1 = none.
	};

	std::vector<Bus> buses;
};

bool save_input_mapping_to_file(const std::map<DeviceSpec, DeviceInfo> &devices,
                                const InputMapping &mapping,
                                const std::string &filename);
bool load_input_mapping_from_file(const std::map<DeviceSpec, DeviceInfo> &devices,
                                  const std::string &filename,
                                  InputMapping *mapping);

#endif  // !defined(_INPUT_MAPPING_H)
