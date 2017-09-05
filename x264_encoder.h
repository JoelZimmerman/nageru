// A wrapper around x264, to encode video in higher quality than Quick Sync
// can give us. We maintain a queue of uncompressed Y'CbCr frames (of 50 frames,
// so a little under 100 MB at 720p), then have a separate thread pull out
// those threads as fast as we can to give it to x264 for encoding.
//
// The encoding threads are niced down because mixing is more important than
// encoding; if we lose frames in mixing, we'll lose frames to disk _and_
// to the stream, as where if we lose frames in encoding, we'll lose frames
// to the stream only, so the latter is strictly better. More importantly,
// this allows speedcontrol to do its thing without disturbing the mixer.

#ifndef _X264ENCODE_H
#define _X264ENCODE_H 1

#include <sched.h>
#include <stdint.h>
#include <x264.h>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

extern "C" {
#include <libavformat/avformat.h>
}

#include <movit/image_format.h>

#include "defs.h"
#include "metrics.h"
#include "print_latency.h"
#include "x264_dynamic.h"

class Mux;
class X264SpeedControl;

class X264Encoder {
public:
	X264Encoder(AVOutputFormat *oformat);  // Does not take ownership.

	// Called after the last frame. Will block; once this returns,
	// the last data is flushed.
	~X264Encoder();

	// Must be called before first frame. Does not take ownership.
	void add_mux(Mux *mux) { muxes.push_back(mux); }

	// <data> is taken to be raw NV12 data of WIDTHxHEIGHT resolution.
	// Does not block.
	void add_frame(int64_t pts, int64_t duration, movit::YCbCrLumaCoefficients ycbcr_coefficients, const uint8_t *data, const ReceivedTimestamps &received_ts);

	std::string get_global_headers() const {
		while (!x264_init_done) {
			sched_yield();
		}
		return global_headers;
	}

	void change_bitrate(unsigned rate_kbit) {
		new_bitrate_kbit = rate_kbit;
	}

private:
	struct QueuedFrame {
		int64_t pts, duration;
		movit::YCbCrLumaCoefficients ycbcr_coefficients;
		uint8_t *data;
		ReceivedTimestamps received_ts;
	};
	void encoder_thread_func();
	void init_x264();
	void encode_frame(QueuedFrame qf);

	// One big memory chunk of all 50 (or whatever) frames, allocated in
	// the constructor. All data functions just use pointers into this
	// pool.
	std::unique_ptr<uint8_t[]> frame_pool;

	std::vector<Mux *> muxes;
	bool wants_global_headers;

	std::string global_headers;
	std::string buffered_sei;  // Will be output before first frame, if any.

	std::thread encoder_thread;
	std::atomic<bool> x264_init_done{false};
	std::atomic<bool> should_quit{false};
	X264Dynamic dyn;
	x264_t *x264;
	std::unique_ptr<X264SpeedControl> speed_control;

	std::function<void(x264_param_t *)> bitrate_override_func;

	std::atomic<unsigned> new_bitrate_kbit{0};  // 0 for no change.

	// Protects everything below it.
	std::mutex mu;

	// Frames that are not being encoded or waiting to be encoded,
	// so that add_frame() can use new ones.
	std::queue<uint8_t *> free_frames;

	// Frames that are waiting to be encoded (ie., add_frame() has been
	// called, but they are not picked up for encoding yet).
	std::queue<QueuedFrame> queued_frames;

	// Whenever the state of <queued_frames> changes.
	std::condition_variable queued_frames_nonempty;

	// Key is the pts of the frame.
	std::unordered_map<int64_t, ReceivedTimestamps> frames_being_encoded;
};

#endif  // !defined(_X264ENCODE_H)
