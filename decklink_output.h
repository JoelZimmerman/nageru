#ifndef _DECKLINK_OUTPUT_H
#define _DECKLINK_OUTPUT_H 1

#include <epoxy/gl.h>
#include <movit/image_format.h>
#include <stdint.h>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

#include "DeckLinkAPI.h"
#include "DeckLinkAPITypes.h"
#include "LinuxCOM.h"

#include "context.h"
#include "print_latency.h"
#include "quittable_sleeper.h"
#include "ref_counted_frame.h"
#include "ref_counted_gl_sync.h"

namespace movit {

class ResourcePool;

}  // namespace movit

class ChromaSubsampler;
class IDeckLink;
class IDeckLinkOutput;
class QSurface;

class DeckLinkOutput : public IDeckLinkVideoOutputCallback {
public:
	DeckLinkOutput(movit::ResourcePool *resource_pool, QSurface *surface, unsigned width, unsigned height, unsigned card_index);

	void set_device(IDeckLink *output);
	void start_output(uint32_t mode, int64_t base_pts);  // Mode comes from get_available_video_modes().
	void end_output();

	void send_frame(GLuint y_tex, GLuint cbcr_tex, movit::YCbCrLumaCoefficients ycbcr_coefficients, const std::vector<RefCountedFrame> &input_frames, int64_t pts, int64_t duration);
	void send_audio(int64_t pts, const std::vector<float> &samples);

	// NOTE: The returned timestamp is undefined for preroll.
	// Otherwise, it is the timestamp of the output frame as it should have been,
	// even if we're overshooting. E.g. at 50 fps (0.02 spf), assuming the
	// last frame was at t=0.980:
	//
	//   If we're at t=0.999, we wait until t=1.000 and return that.
	//   If we're at t=1.001, we return t=1.000 immediately (small overshoot).
	//   If we're at t=1.055, we drop two frames and return t=1.040 immediately.
	void wait_for_frame(int64_t pts, int *dropped_frames, int64_t *frame_duration, bool *is_preroll, std::chrono::steady_clock::time_point *frame_timestamp);

	// Analogous to CaptureInterface. Will only return modes that have the right width/height.
	std::map<uint32_t, bmusb::VideoMode> get_available_video_modes() const { return video_modes; }

	// If the given mode is supported, return it. If not, pick some “best” valid mode.
	uint32_t pick_video_mode(uint32_t mode) const;

	// Desired Y'CbCr coefficients for the current mode. Undefined before start_output().
	movit::YCbCrLumaCoefficients preferred_ycbcr_coefficients() const;

	// IUnknown.
	HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, LPVOID *ppv) override;
	ULONG STDMETHODCALLTYPE AddRef() override;
	ULONG STDMETHODCALLTYPE Release() override;

	// IDeckLinkVideoOutputCallback.
	HRESULT ScheduledFrameCompleted(/* in */ IDeckLinkVideoFrame *completedFrame, /* in */ BMDOutputFrameCompletionResult result) override;
	HRESULT ScheduledPlaybackHasStopped() override;

private:
	struct Frame : public IDeckLinkVideoFrame {
	public:
		~Frame();

		// IUnknown.
		HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, LPVOID *ppv) override;
		ULONG STDMETHODCALLTYPE AddRef() override;
		ULONG STDMETHODCALLTYPE Release() override;

		// IDeckLinkVideoFrame.
		long GetWidth() override;
		long GetHeight() override;
		long GetRowBytes() override;
		BMDPixelFormat GetPixelFormat() override;
		BMDFrameFlags GetFlags() override;
		HRESULT GetBytes(/* out */ void **buffer) override;

		HRESULT GetTimecode(/* in */ BMDTimecodeFormat format, /* out */ IDeckLinkTimecode **timecode) override;
		HRESULT GetAncillaryData(/* out */ IDeckLinkVideoFrameAncillary **ancillary) override;

	private:
		std::atomic<int> refcount{1};
		RefCountedGLsync fence;  // Needs to be waited on before uyvy_ptr can be read from.
		std::vector<RefCountedFrame> input_frames;  // Cannot be released before we are done rendering (ie., <fence> is asserted).
		ReceivedTimestamps received_ts;
		int64_t pts, duration;
		movit::ResourcePool *resource_pool;

		// These members are persistently allocated, and reused when the frame object is.
		GLuint uyvy_tex;  // Owned by <resource_pool>. Can also hold v210 data.
		GLuint pbo;
		uint8_t *uyvy_ptr;  // Persistent mapping into the PBO.

		// Current Blackmagic drivers (January 2017) have a bug where sending a PBO
		// pointer to the driver causes a kernel oops. Thus, we do an extra copy into
		// this pointer before giving the data to the driver. (We don't do a get
		// directly into this pointer, because e.g. Intel/Mesa hits a slow path when
		// you do readback into something that's not a PBO.) When Blackmagic fixes
		// the bug, we should drop this.
		std::unique_ptr<uint8_t[]> uyvy_ptr_local;

		friend class DeckLinkOutput;
	};
	std::unique_ptr<Frame> get_frame();
	void create_uyvy(GLuint y_tex, GLuint cbcr_tex, GLuint dst_tex);

	void present_thread_func();

	std::atomic<int> refcount{1};

	std::unique_ptr<ChromaSubsampler> chroma_subsampler;
	std::map<uint32_t, bmusb::VideoMode> video_modes;

	std::thread present_thread;
	QuittableSleeper should_quit;

	std::mutex frame_queue_mutex;
	std::queue<std::unique_ptr<Frame>> pending_video_frames;  // Under <frame_queue_mutex>.
	std::queue<std::unique_ptr<Frame>> frame_freelist;  // Under <frame_queue_mutex>.
	int num_frames_in_flight = 0;  // Number of frames allocated but not on the freelist. Under <frame_queue_mutex>.
	std::condition_variable frame_queues_changed;
	bool playback_initiated = false, playback_started = false;
	int64_t base_pts, frame_duration;
	BMDDisplayModeFlags current_mode_flags = 0;
	bool last_frame_had_mode_mismatch = false;

	movit::ResourcePool *resource_pool;
	IDeckLinkOutput *output = nullptr;
	BMDVideoConnection video_connection;
	QSurface *surface;
	unsigned width, height;
	unsigned card_index;

	GLuint uyvy_vbo;  // Holds position and texcoord data.
	GLuint uyvy_program_num;  // Owned by <resource_pool>.
	GLuint uyvy_position_attribute_index, uyvy_texcoord_attribute_index;
};

#endif  // !defined(_DECKLINK_OUTPUT_H)
