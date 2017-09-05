#ifndef _ALSA_INPUT_H
#define _ALSA_INPUT_H 1

// ALSA sound input, running in a separate thread and sending audio back
// in callbacks.
//
// Note: “frame” here generally refers to the ALSA definition of frame,
// which is a set of samples, exactly one for each channel. The only exception
// is in frame_length, where it means the TIMEBASE length of the buffer
// as a whole, since that's what AudioMixer::add_audio() wants.

#include <alsa/asoundlib.h>
#include <stdint.h>
#include <sys/types.h>
#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <thread>

#include "bmusb/bmusb.h"
#include "quittable_sleeper.h"

class ALSAPool;

class ALSAInput {
public:
	typedef std::function<bool(const uint8_t *data, unsigned num_samples, bmusb::AudioFormat audio_format, int64_t frame_length, std::chrono::steady_clock::time_point ts)> audio_callback_t;

	ALSAInput(const char *device, unsigned sample_rate, unsigned num_channels, audio_callback_t audio_callback, ALSAPool *parent_pool, unsigned internal_dev_index);
	~ALSAInput();

	// If not called before start_capture_thread(), the capture thread
	// will call it until it succeeds.
	bool open_device();

	// Not valid before the device has been successfully opened.
	// NOTE: Might very well be different from the sample rate given to the
	// constructor, since the card might not support the one you wanted.
	unsigned get_sample_rate() const { return sample_rate; }

	void start_capture_thread();
	void stop_capture_thread();

	// Set access, sample rate and format parameters on the given ALSA PCM handle.
	// Returns the computed parameter set and the chosen sample rate. Note that
	// sample_rate is an in/out parameter; you send in the desired rate,
	// and ALSA picks one as close to that as possible.
	static bool set_base_params(const char *device_name, snd_pcm_t *pcm_handle, snd_pcm_hw_params_t *hw_params, unsigned *sample_rate);

private:
	void capture_thread_func();
	int64_t frames_to_pts(uint64_t n) const;

	enum class CaptureEndReason {
		REQUESTED_QUIT,
		DEVICE_GONE,
		OTHER_ERROR
	};
	CaptureEndReason do_capture();

	std::string device;
	unsigned sample_rate, num_channels, num_periods;
	snd_pcm_uframes_t period_size;
	snd_pcm_uframes_t buffer_frames;
	bmusb::AudioFormat audio_format;
	audio_callback_t audio_callback;

	snd_pcm_t *pcm_handle = nullptr;
	std::thread capture_thread;
	QuittableSleeper should_quit;
	std::unique_ptr<uint8_t[]> buffer;
	ALSAPool *parent_pool;
	unsigned internal_dev_index;
};

#endif  // !defined(_ALSA_INPUT_H)
