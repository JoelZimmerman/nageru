#include "alsa_input.h"

#include <alsa/error.h>
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <unistd.h>
#include <cstdint>

#include "alsa_pool.h"
#include "bmusb/bmusb.h"
#include "timebase.h"

using namespace std;
using namespace std::chrono;
using namespace std::placeholders;

#define RETURN_ON_ERROR(msg, expr) do {                                                    \
	int err = (expr);                                                                  \
	if (err < 0) {                                                                     \
		fprintf(stderr, "[%s] " msg ": %s\n", device.c_str(), snd_strerror(err));  \
		if (err == -ENODEV) return CaptureEndReason::DEVICE_GONE;                  \
		return CaptureEndReason::OTHER_ERROR;                                      \
	}                                                                                  \
} while (false)

#define RETURN_FALSE_ON_ERROR(msg, expr) do {                                              \
	int err = (expr);                                                                  \
	if (err < 0) {                                                                     \
		fprintf(stderr, "[%s] " msg ": %s\n", device.c_str(), snd_strerror(err));  \
		return false;                                                              \
	}                                                                                  \
} while (false)

#define WARN_ON_ERROR(msg, expr) do {                                                      \
	int err = (expr);                                                                  \
	if (err < 0) {                                                                     \
		fprintf(stderr, "[%s] " msg ": %s\n", device.c_str(), snd_strerror(err));  \
	}                                                                                  \
} while (false)

ALSAInput::ALSAInput(const char *device, unsigned sample_rate, unsigned num_channels, audio_callback_t audio_callback, ALSAPool *parent_pool, unsigned internal_dev_index)
	: device(device),
	  sample_rate(sample_rate),
	  num_channels(num_channels),
	  audio_callback(audio_callback),
	  parent_pool(parent_pool),
	  internal_dev_index(internal_dev_index)
{
}

bool ALSAInput::open_device()
{
	RETURN_FALSE_ON_ERROR("snd_pcm_open()", snd_pcm_open(&pcm_handle, device.c_str(), SND_PCM_STREAM_CAPTURE, 0));

	// Set format.
	snd_pcm_hw_params_t *hw_params;
	snd_pcm_hw_params_alloca(&hw_params);
	if (!set_base_params(device.c_str(), pcm_handle, hw_params, &sample_rate)) {
		return false;
	}

	RETURN_FALSE_ON_ERROR("snd_pcm_hw_params_set_channels()", snd_pcm_hw_params_set_channels(pcm_handle, hw_params, num_channels));

	// Fragment size of 64 samples (about 1 ms at 48 kHz; a frame at 60
	// fps/48 kHz is 800 samples.) We ask for 64 such periods in our buffer
	// (~85 ms buffer); more than that, and our jitter is probably so high
	// that the resampling queue can't keep up anyway.
	// The entire thing with periods and such is a bit mysterious to me;
	// seemingly I can get 96 frames at a time with no problems even if
	// the period size is 64 frames. And if I set num_periods to e.g. 1,
	// I can't have a big buffer.
	num_periods = 16;
	int dir = 0;
	RETURN_FALSE_ON_ERROR("snd_pcm_hw_params_set_periods_near()", snd_pcm_hw_params_set_periods_near(pcm_handle, hw_params, &num_periods, &dir));
	period_size = 64;
	dir = 0;
	RETURN_FALSE_ON_ERROR("snd_pcm_hw_params_set_period_size_near()", snd_pcm_hw_params_set_period_size_near(pcm_handle, hw_params, &period_size, &dir));
	buffer_frames = 64 * 64;
	RETURN_FALSE_ON_ERROR("snd_pcm_hw_params_set_buffer_size_near()", snd_pcm_hw_params_set_buffer_size_near(pcm_handle, hw_params, &buffer_frames));
	RETURN_FALSE_ON_ERROR("snd_pcm_hw_params()", snd_pcm_hw_params(pcm_handle, hw_params));
	//snd_pcm_hw_params_free(hw_params);

	// Figure out which format the card actually chose.
	RETURN_FALSE_ON_ERROR("snd_pcm_hw_params_current()", snd_pcm_hw_params_current(pcm_handle, hw_params));
	snd_pcm_format_t chosen_format;
	RETURN_FALSE_ON_ERROR("snd_pcm_hw_params_get_format()", snd_pcm_hw_params_get_format(hw_params, &chosen_format));

	audio_format.num_channels = num_channels;
	audio_format.bits_per_sample = 0;
	switch (chosen_format) {
	case SND_PCM_FORMAT_S16_LE:
		audio_format.bits_per_sample = 16;
		break;
	case SND_PCM_FORMAT_S24_LE:
		audio_format.bits_per_sample = 24;
		break;
	case SND_PCM_FORMAT_S32_LE:
		audio_format.bits_per_sample = 32;
		break;
	default:
		assert(false);
	}
	audio_format.sample_rate = sample_rate;
	//printf("num_periods=%u period_size=%u buffer_frames=%u sample_rate=%u bits_per_sample=%d\n",
	//	num_periods, unsigned(period_size), unsigned(buffer_frames), sample_rate, audio_format.bits_per_sample);

	buffer.reset(new uint8_t[buffer_frames * num_channels * audio_format.bits_per_sample / 8]);

	snd_pcm_sw_params_t *sw_params;
	snd_pcm_sw_params_alloca(&sw_params);
	RETURN_FALSE_ON_ERROR("snd_pcm_sw_params_current()", snd_pcm_sw_params_current(pcm_handle, sw_params));
	RETURN_FALSE_ON_ERROR("snd_pcm_sw_params_set_start_threshold", snd_pcm_sw_params_set_start_threshold(pcm_handle, sw_params, num_periods * period_size / 2));
	RETURN_FALSE_ON_ERROR("snd_pcm_sw_params()", snd_pcm_sw_params(pcm_handle, sw_params));

	RETURN_FALSE_ON_ERROR("snd_pcm_nonblock()", snd_pcm_nonblock(pcm_handle, 1));
	RETURN_FALSE_ON_ERROR("snd_pcm_prepare()", snd_pcm_prepare(pcm_handle));
	return true;
}

bool ALSAInput::set_base_params(const char *device_name, snd_pcm_t *pcm_handle, snd_pcm_hw_params_t *hw_params, unsigned *sample_rate)
{
	int err;
	err = snd_pcm_hw_params_any(pcm_handle, hw_params);
	if (err < 0) {
		fprintf(stderr, "[%s] snd_pcm_hw_params_any(): %s\n", device_name, snd_strerror(err));
		return false;
	}
	err = snd_pcm_hw_params_set_access(pcm_handle, hw_params, SND_PCM_ACCESS_RW_INTERLEAVED);
	if (err < 0) {
		fprintf(stderr, "[%s] snd_pcm_hw_params_set_access(): %s\n", device_name, snd_strerror(err));
		return false;
	}
	snd_pcm_format_mask_t *format_mask;
	snd_pcm_format_mask_alloca(&format_mask);
	snd_pcm_format_mask_set(format_mask, SND_PCM_FORMAT_S16_LE);
	snd_pcm_format_mask_set(format_mask, SND_PCM_FORMAT_S24_LE);
	snd_pcm_format_mask_set(format_mask, SND_PCM_FORMAT_S32_LE);
	err = snd_pcm_hw_params_set_format_mask(pcm_handle, hw_params, format_mask);
	if (err < 0) {
		fprintf(stderr, "[%s] snd_pcm_hw_params_set_format_mask(): %s\n", device_name, snd_strerror(err));
		return false;
	}
	err = snd_pcm_hw_params_set_rate_near(pcm_handle, hw_params, sample_rate, 0);
	if (err < 0) {
		fprintf(stderr, "[%s] snd_pcm_hw_params_set_rate_near(): %s\n", device_name, snd_strerror(err));
		return false;
	}
	return true;
}

ALSAInput::~ALSAInput()
{
	if (pcm_handle) {
		WARN_ON_ERROR("snd_pcm_close()", snd_pcm_close(pcm_handle));
	}
}

void ALSAInput::start_capture_thread()
{
	should_quit.unquit();
	capture_thread = thread(&ALSAInput::capture_thread_func, this);
}

void ALSAInput::stop_capture_thread()
{
	should_quit.quit();
	capture_thread.join();
}

void ALSAInput::capture_thread_func()
{
	parent_pool->set_card_state(internal_dev_index, ALSAPool::Device::State::STARTING);

	// If the device hasn't been opened already, we need to do so
	// before we can capture.
	while (!should_quit.should_quit() && pcm_handle == nullptr) {
		if (!open_device()) {
			fprintf(stderr, "[%s] Waiting one second and trying again...\n",
				device.c_str());
			should_quit.sleep_for(seconds(1));
		}
	}

	if (should_quit.should_quit()) {
		// Don't call free_card(); that would be a deadlock.
		WARN_ON_ERROR("snd_pcm_close()", snd_pcm_close(pcm_handle));
		pcm_handle = nullptr;
		return;
	}

	// Do the actual capture. (Termination condition within loop.)
	for ( ;; ) {
		switch (do_capture()) {
		case CaptureEndReason::REQUESTED_QUIT:
			// Don't call free_card(); that would be a deadlock.
			WARN_ON_ERROR("snd_pcm_close()", snd_pcm_close(pcm_handle));
			pcm_handle = nullptr;
			return;
		case CaptureEndReason::DEVICE_GONE:
			parent_pool->free_card(internal_dev_index);
			WARN_ON_ERROR("snd_pcm_close()", snd_pcm_close(pcm_handle));
			pcm_handle = nullptr;
			return;
		case CaptureEndReason::OTHER_ERROR:
			parent_pool->set_card_state(internal_dev_index, ALSAPool::Device::State::STARTING);
			fprintf(stderr, "[%s] Sleeping one second and restarting capture...\n",
				device.c_str());
			should_quit.sleep_for(seconds(1));
			break;
		}
	}
}

ALSAInput::CaptureEndReason ALSAInput::do_capture()
{
	parent_pool->set_card_state(internal_dev_index, ALSAPool::Device::State::STARTING);
	RETURN_ON_ERROR("snd_pcm_start()", snd_pcm_start(pcm_handle));
	parent_pool->set_card_state(internal_dev_index, ALSAPool::Device::State::RUNNING);

	uint64_t num_frames_output = 0;
	while (!should_quit.should_quit()) {
		int ret = snd_pcm_wait(pcm_handle, /*timeout=*/100);
		if (ret == 0) continue;  // Timeout.
		if (ret == -EPIPE) {
			fprintf(stderr, "[%s] ALSA overrun\n", device.c_str());
			snd_pcm_prepare(pcm_handle);
			snd_pcm_start(pcm_handle);
			continue;
		}
		RETURN_ON_ERROR("snd_pcm_wait()", ret);

		snd_pcm_sframes_t frames = snd_pcm_readi(pcm_handle, buffer.get(), buffer_frames);
		if (frames == -EPIPE) {
			fprintf(stderr, "[%s] ALSA overrun\n", device.c_str());
			snd_pcm_prepare(pcm_handle);
			snd_pcm_start(pcm_handle);
			continue;
		}
		if (frames == 0) {
			fprintf(stderr, "snd_pcm_readi() returned 0\n");
			break;
		}
		RETURN_ON_ERROR("snd_pcm_readi()", frames);

		const int64_t prev_pts = frames_to_pts(num_frames_output);
		const int64_t pts = frames_to_pts(num_frames_output + frames);
		const steady_clock::time_point now = steady_clock::now();
		bool success;
		do {
			if (should_quit.should_quit()) return CaptureEndReason::REQUESTED_QUIT;
			success = audio_callback(buffer.get(), frames, audio_format, pts - prev_pts, now);
		} while (!success);
		num_frames_output += frames;
	}
	return CaptureEndReason::REQUESTED_QUIT;
}

int64_t ALSAInput::frames_to_pts(uint64_t n) const
{
	return (n * TIMEBASE) / sample_rate;
}

