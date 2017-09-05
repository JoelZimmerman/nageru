// The x264 speed control tries to encode video at maximum possible quality
// without skipping frames (at the expense of higher encoding latency and
// less even output rates, although VBV is still respected). It does this
// by continuously (every frame) changing the x264 quality settings such that
// it uses maximum amount of CPU, but no more.
//
// Speed control works by maintaining a queue of frames, with the confusing
// nomenclature “full” meaning that there are no queues in the frame.
// (Conversely, if the queue is “empty” and a new frame comes in, we need to
// drop that frame.) It tries to keep the buffer 3/4 “full” by using a table
// of measured relative speeds for the different presets, and choosing one that it
// thinks will return the buffer to that state over time. However, since
// different frames take different times to encode regardless of preset, it
// also tries to maintain a running average of how long the typical frame will
// take to encode at the fastest preset (the so-called “complexity”), by dividing
// the actual time by the relative time for the preset used.
//
// Frame timings is a complex topic in its own sright, since usually, multiple
// frames are encoded in parallel. X264SpeedControl only supports the timing
// method that the original patch calls “alternate timing”; one simply measures
// the time the last x264_encoder_encode() call took. (The other alternative given
// is to measure the time between successive x264_encoder_encode() calls.)
// Unless using the zerocopy presets (which activate slice threading), the function
// actually returns not when the given frame is done encoding, but when one a few
// frames back is done encoding. So it doesn't actually measure the time of any
// given one frame, but it measures something correlated to it, at least as long as
// you are near 100% CPU utilization (ie., the encoded frame doesn't linger in the
// buffers already when x264_encoder_encode() is called).
//
// The code has a long history; it was originally part of Avail Media's x264
// branch, used in their encoder appliances, and then a snapshot of that was
// released. (Given that x264 is licensed under GPLv2 or newer, this means that
// we can also treat the patch as GPLv2 or newer if we want, which we do.
// As far as I know, it is copyright Avail Media, although no specific copyright
// notice was posted on the patch.)
//
// From there, it was incorporated in OBE's x264 tree (x264-obe) and some bugs
// were fixed. I started working on it for the purposes of Nageru, fixing various
// issues, adding VFR support and redoing the timings entirely based on more
// modern presets (the patch was made before several important x264 features,
// such as weighted P-frames). Finally, I took it out of x264 and put it into
// Nageru (it does not actually use any hooks into the codec itself), so that
// one does not need to patch x264 to use it in Nageru. It still could do with
// some cleanup, but it's much, much better than just using a static preset.

#include <stdint.h>
#include <atomic>
#include <chrono>
#include <functional>

extern "C" {
#include <x264.h>
}

#include "metrics.h"
#include "x264_dynamic.h"

class X264SpeedControl {
public:
	// x264: Encoding object we are using; must be opened. Assumed to be
	//    set to the "faster" preset, and with 16 reference frames.
	// f_speed: Relative encoding speed, usually 1.0.
	// i_buffer_size: Number of frames in the buffer.
	// f_buffer_init: Relative fullness of buffer at start
	//    (0.0 = assumed to be <i_buffer_size> frames in buffer,
	//     1.0 = no frames in buffer)
	X264SpeedControl(x264_t *x264, float f_speed, int i_buffer_size, float f_buffer_init);
	~X264SpeedControl();

	// You need to call before_frame() immediately before each call to
	// x264_encoder_encode(), and after_frame() immediately after.
	//
	// new_buffer_fill: Buffer fullness, in microseconds (_not_ a relative
	//   number, unlike f_buffer_init in the constructor).
	// new_buffer_size: If > 0, new number of frames in the buffer,
	//   ie. the buffer size has changed. (It is harmless to set this
	//   even if the buffer hasn't actually changed.)
	// f_uspf: If > 0, new microseconds per frame, ie. the frame rate has
	//   changed. (Of course, with VFR, it can be impossible to truly know
	//   the frame rate of the coming frames, but it is a reasonable
	//   assumption that the next second or so is likely to be the same
	//   frame rate as the last frame.)
	void before_frame(float new_buffer_fill, int new_buffer_size, float f_uspf);
	void after_frame();

	// x264 seemingly has an issue where x264_encoder_reconfig() is not reflected
	// immediately in x264_encoder_parameters(). Since speed control keeps calling
	// those two all the time, any changes you make outside X264SpeedControl
	// could be overridden. Thus, to make changes to encoder parameters, you should
	// instead set a function here, which will be called every time parameters
	// are modified.
	void set_config_override_function(std::function<void(x264_param_t *)> override_func)
	{
		this->override_func = override_func;
	}

private:
	void set_buffer_size(int new_buffer_size);
	int dither_preset(float f);
	void apply_preset(int new_preset);

	X264Dynamic dyn;

	// Not owned by us.
	x264_t *x264;

	float f_speed;

	// all times that are not std::chrono::* are in usec
	std::chrono::steady_clock::time_point timestamp;   // when was speedcontrol last invoked
	std::chrono::steady_clock::duration cpu_time_last_frame{std::chrono::seconds{0}};   // time spent encoding the previous frame
	int64_t buffer_size; // assumed application-side buffer of frames to be streamed (measured in microseconds),
	int64_t buffer_fill; //   where full = we don't have to hurry
	int64_t compensation_period; // how quickly we try to return to the target buffer fullness
	float uspf;          // microseconds per frame
	int preset = -1;     // which setting was used in the previous frame
	float cplx_num = 3e3;  // rolling average of estimated spf for preset #0. FIXME estimate initial complexity
	float cplx_den = .1;
	float cplx_decay;
	float dither = 0.0f;

	bool first = true;

	struct
	{
		int64_t min_buffer, max_buffer;
		double avg_preset;
		int den;
	} stat;

	std::function<void(x264_param_t *)> override_func = nullptr;

	// Metrics.
	Histogram metric_x264_speedcontrol_preset_used_frames;
	std::atomic<double> metric_x264_speedcontrol_buffer_available_seconds{0.0};
	std::atomic<double> metric_x264_speedcontrol_buffer_size_seconds{0.0};
	std::atomic<int64_t> metric_x264_speedcontrol_idle_frames{0};
	std::atomic<int64_t> metric_x264_speedcontrol_late_frames{0};
};
