// A class to orchestrate the concept of video encoding. Will keep track of
// the muxes to stream and disk, the QuickSyncEncoder, and also the X264Encoder
// (for the stream) if there is one.

#ifndef _VIDEO_ENCODER_H
#define _VIDEO_ENCODER_H

#include <epoxy/gl.h>
#include <movit/image_format.h>
#include <stdbool.h>
#include <stdint.h>
#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

extern "C" {
#include <libavformat/avformat.h>
#include <libavformat/avio.h>
}

#include "mux.h"
#include "ref_counted_gl_sync.h"

class AudioEncoder;
class DiskSpaceEstimator;
class HTTPD;
class Mux;
class QSurface;
class QuickSyncEncoder;
class RefCountedFrame;
class X264Encoder;

namespace movit {
class ResourcePool;
}  // namespace movit

class VideoEncoder {
public:
	VideoEncoder(movit::ResourcePool *resource_pool, QSurface *surface, const std::string &va_display, int width, int height, HTTPD *httpd, DiskSpaceEstimator *disk_space_estimator);
	~VideoEncoder();

	void add_audio(int64_t pts, std::vector<float> audio);

	bool is_zerocopy() const;

	// Allocate a frame to render into. The returned two textures
	// are yours to render into (build them into an FBO).
	// Call end_frame() when you're done.
	//
	// The semantics of y_tex and cbcr_tex depend on is_zerocopy():
	//
	//   - If false, the are input parameters, ie., the caller
	//     allocates textures. (The contents are not read before
	//     end_frame() is called.)
	//   - If true, they are output parameters, ie., VideoEncoder
	//     allocates textures and borrow them to you for rendering.
	//     In this case, after end_frame(), you are no longer allowed
	//     to use the textures; they are torn down and given to the
	//     H.264 encoder.
	bool begin_frame(int64_t pts, int64_t duration, movit::YCbCrLumaCoefficients ycbcr_coefficients, const std::vector<RefCountedFrame> &input_frames, GLuint *y_tex, GLuint *cbcr_tex);

	// Call after you are done rendering into the frame; at this point,
	// y_tex and cbcr_tex will be assumed done, and handed over to the
	// encoder. The returned fence is purely a convenience; you do not
	// need to use it for anything, but it's useful if you wanted to set
	// one anyway.
	RefCountedGLsync end_frame();

	// Does a cut of the disk stream immediately ("frame" is used for the filename only).
	void do_cut(int frame);

	void change_x264_bitrate(unsigned rate_kbit);

private:
	void open_output_stream();
	static int write_packet2_thunk(void *opaque, uint8_t *buf, int buf_size, AVIODataMarkerType type, int64_t time);
	int write_packet2(uint8_t *buf, int buf_size, AVIODataMarkerType type, int64_t time);

	AVOutputFormat *oformat;
	mutable std::mutex qs_mu, qs_audio_mu;
	std::unique_ptr<QuickSyncEncoder> quicksync_encoder;  // Under <qs_mu> _and_ <qs_audio_mu>.
	movit::ResourcePool *resource_pool;
	QSurface *surface;
	std::string va_display;
	int width, height;
	HTTPD *httpd;
	DiskSpaceEstimator *disk_space_estimator;

	bool seen_sync_markers = false;

	std::unique_ptr<Mux> stream_mux;  // To HTTP.
	std::unique_ptr<AudioEncoder> stream_audio_encoder;
	std::unique_ptr<X264Encoder> x264_encoder;  // nullptr if not using x264.

	std::string stream_mux_header;
	MuxMetrics stream_mux_metrics;

	std::atomic<int> quicksync_encoders_in_shutdown{0};
	std::atomic<int> overriding_bitrate{0};

	// Encoders that are shutdown, but need to call release_gl_resources()
	// (or be deleted) from some thread with an OpenGL context.
	std::vector<std::unique_ptr<QuickSyncEncoder>> qs_needing_cleanup;  // Under <qs_mu>.
};

#endif
