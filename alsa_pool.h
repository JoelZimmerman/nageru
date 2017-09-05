#ifndef _ALSA_POOL_H
#define _ALSA_POOL_H 1

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

class ALSAInput;
class DeviceSpecProto;

// The class dealing with the collective of all ALSA cards in the system.
// In particular, it deals with enumeration of cards, and hotplug of new ones.
class ALSAPool {
public:
	ALSAPool();
	~ALSAPool();

	struct Device {
		enum class State {
			// There is no card here. (There probably used to be one,
			// but it got removed.) We don't insert a card before
			// we've actually probed it, ie., we know whether it
			// can be captured from at all, and what its name is.
			EMPTY,

			// This card is ready for capture, as far as we know.
			// (It could still be used by someone else; we don't know
			// until we try to open it.)
			READY,

			// We are trying to start capture from this card, but we are not
			// streaming yet. Note that this could in theory go on forever,
			// if the card is in use by some other process; in the UI,
			// we will show this state as “(busy)”.
			STARTING,

			// The card is capturing and sending data. If there's a fatal error,
			// it could go back to STARTING, or it could go to DEAD
			// (depending on the error).
			RUNNING,

			// The card is gone (e.g., unplugged). However, since there's
			// still a bus using it, we can't just remove the entry.
			// If the card comes back (ie., a new card is plugged in,
			// and we believe it has the same configuration), it could be
			// installed in place of this card, and then presumably be put
			// back into STARTING or RUNNING.
			DEAD
		} state = State::EMPTY;

		std::string address;  // E.g. “hw:0,0”.
		std::string name, info;
		unsigned num_channels;
		ALSAInput *input = nullptr;  // nullptr iff EMPTY or DEAD.

		// Whether the AudioMixer is interested in this card or not.
		// “Interested” could mean either of two things: Either it is part of
		// a bus mapping, or it is in the process of enumerating devices
		// (to show to the user). A card that is _not_ held can disappear
		// at any given time as a result of an error or hotplug event;
		// a card that is held will go to the DEAD state instead.
		bool held = false;

		std::string display_name() const { return name + " (" + info + ")"; }
	};

	void init();

	// Get the list of all current devices. Note that this will implicitly mark
	// all of the returned devices as held, since the input mapping UI needs
	// some kind of stability when the user is to choose. Thus, when you are done
	// with the list and have set a new mapping, you must go through all the devices
	// you don't want and release them using release_device().
	std::vector<Device> get_devices();

	void hold_device(unsigned index);
	void release_device(unsigned index);  // Note: index is allowed to go out of bounds.

	// If device is held, start or restart capture. If device is not held,
	// stop capture if it isn't already.
	void reset_device(unsigned index);

	// Note: The card must be held. Returns OUTPUT_FREQUENCY if the card is in EMPTY or DEAD.
	unsigned get_capture_frequency(unsigned index);

	// Note: The card must be held.
	Device::State get_card_state(unsigned index);

	// Only for ALSAInput.
	void set_card_state(unsigned index, Device::State state);

	// Just a short form for taking <mu> and then moving the card to
	// EMPTY or DEAD state. Only for ALSAInput and for internal use.
	void free_card(unsigned index);

	// Create a new card, mark it immediately as DEAD and hold it.
	// Returns the new index.
	unsigned create_dead_card(const std::string &name, const std::string &info, unsigned num_channels);

	// Make a protobuf representation of the given card, so that it can be
	// matched against at a later stage. For AudioMixer only.
	// The given card must be held.
	void serialize_device(unsigned index, DeviceSpecProto *serialized);

private:
	mutable std::mutex mu;
	std::vector<Device> devices;  // Under mu.
	std::vector<std::unique_ptr<ALSAInput>> inputs;  // Under mu, corresponds 1:1 to devices.

	// Keyed on device address (e.g. “hw:0,0”). If there's an entry here,
	// it means we already have a thread doing retries, so we shouldn't
	// start a new one.
	std::unordered_map<std::string, unsigned> add_device_tries_left;  // Under add_device_mutex.
	std::mutex add_device_mutex;

	static constexpr int num_retries = 10;

	void inotify_thread_func();
	void enumerate_devices();

	// Try to add an input at the given card/device. If it succeeds, return
	// synchronously. If not, fire off a background thread to try up to
	// <num_retries> times.
	void probe_device_with_retry(unsigned card_index, unsigned dev_index);
	void probe_device_retry_thread_func(unsigned card_index, unsigned dev_index);

	enum class ProbeResult {
		SUCCESS,
		DEFER,
		FAILURE
	};
	ProbeResult probe_device_once(unsigned card_index, unsigned dev_index);

	void unplug_device(unsigned card_index, unsigned dev_index);

	// Must be called with <mu> held. Will allocate a new entry if needed.
	// The returned entry will be set to READY state.
	unsigned find_free_device_index(const std::string &name,
	                                const std::string &info,
	                                unsigned num_channels,
	                                const std::string &address);

	std::atomic<bool> should_quit{false};
	int should_quit_fd;
	std::thread inotify_thread;
	std::atomic<int> retry_threads_running{0};

	friend class ALSAInput;
};

#endif  // !defined(_ALSA_POOL_H)
