#ifndef _MUX_H
#define _MUX_H 1

// Wrapper around an AVFormat mux.

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
}

#include <sys/types.h>
#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <string>
#include <utility>
#include <thread>
#include <vector>

#include "timebase.h"

struct MuxMetrics {
	// “written” will usually be equal video + audio + mux overhead,
	// except that there could be buffered packets that count in audio or video
	// but not yet in written.
	std::atomic<int64_t> metric_video_bytes{0}, metric_audio_bytes{0}, metric_written_bytes{0};

	// Registers in global_metrics.
	void init(const std::vector<std::pair<std::string, std::string>> &labels);

	void reset()
	{
		metric_video_bytes = 0;
		metric_audio_bytes = 0;
		metric_written_bytes = 0;
	}
};

class Mux {
public:
	enum Codec {
		CODEC_H264,
		CODEC_NV12,  // Uncompressed 4:2:0.
	};
	enum WriteStrategy {
		// add_packet() will write the packet immediately, unless plugged.
		WRITE_FOREGROUND,

		// All writes will happen on a separate thread, so add_packet()
		// won't block. Use this if writing to a file and you might be
		// holding a mutex (because blocking I/O with a mutex held is
		// not good). Note that this will clone every packet, so it has
		// higher overhead.
		WRITE_BACKGROUND,
	};

	// Takes ownership of avctx. <write_callback> will be called every time
	// a write has been made to the video stream (id 0), with the pts of
	// the just-written frame. (write_callback can be nullptr.)
	// Does not take ownership of <metrics>; elements in there, if any,
	// will be added to.
	Mux(AVFormatContext *avctx, int width, int height, Codec video_codec, const std::string &video_extradata, const AVCodecParameters *audio_codecpar, int time_base, std::function<void(int64_t)> write_callback, WriteStrategy write_strategy, const std::vector<MuxMetrics *> &metrics);
	~Mux();
	void add_packet(const AVPacket &pkt, int64_t pts, int64_t dts, AVRational timebase = { 1, TIMEBASE });

	// As long as the mux is plugged, it will not actually write anything to disk,
	// just queue the packets. Once it is unplugged, the packets are reordered by pts
	// and written. This is primarily useful if you might have two different encoders
	// writing to the mux at the same time (because one is shutting down), so that
	// pts might otherwise come out-of-order.
	//
	// You can plug and unplug multiple times; only when the plug count reaches zero,
	// something will actually happen.
	void plug();
	void unplug();

private:
	// If write_strategy == WRITE_FOREGORUND, Must be called with <mu> held.
	void write_packet_or_die(const AVPacket &pkt, int64_t unscaled_pts);
	void thread_func();

	WriteStrategy write_strategy;

	std::mutex mu;

	// These are only in use if write_strategy == WRITE_BACKGROUND.
	std::atomic<bool> writer_thread_should_quit{false};
	std::thread writer_thread;

	AVFormatContext *avctx;  // Protected by <mu>, iff write_strategy == WRITE_BACKGROUND.
	int plug_count = 0;  // Protected by <mu>.

	// Protected by <mu>. If write_strategy == WRITE_FOREGROUND,
	// this is only in use when plugging.
	struct QueuedPacket {
		AVPacket *pkt;
		int64_t unscaled_pts;
	};
	std::vector<QueuedPacket> packet_queue;
	std::condition_variable packet_queue_ready;

	AVStream *avstream_video, *avstream_audio;

	std::function<void(int64_t)> write_callback;
	std::vector<MuxMetrics *> metrics;

	friend struct PacketBefore;
};

#endif  // !defined(_MUX_H)
