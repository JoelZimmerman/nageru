#ifndef _PBO_FRAME_ALLOCATOR 
#define _PBO_FRAME_ALLOCATOR 1

#include <epoxy/gl.h>
#include <stdbool.h>
#include <stddef.h>
#include <memory>
#include <mutex>
#include <queue>

#include <movit/ycbcr.h>

#include "bmusb/bmusb.h"

// An allocator that allocates straight into OpenGL pinned memory.
// Meant for video frames only. We use a queue rather than a stack,
// since we want to maximize pipelineability.
class PBOFrameAllocator : public bmusb::FrameAllocator {
public:
	// Note: You need to have an OpenGL context when calling
	// the constructor.
	PBOFrameAllocator(bmusb::PixelFormat pixel_format,
	                  size_t frame_size,
	                  GLuint width, GLuint height,
	                  size_t num_queued_frames = 16,
	                  GLenum buffer = GL_PIXEL_UNPACK_BUFFER_ARB,
	                  GLenum permissions = GL_MAP_WRITE_BIT,
	                  GLenum map_bits = GL_MAP_FLUSH_EXPLICIT_BIT);
	~PBOFrameAllocator() override;
	Frame alloc_frame() override;
	void release_frame(Frame frame) override;

	struct Userdata {
		GLuint pbo;

		// NOTE: These frames typically go into LiveInputWrapper, which is
		// configured to accept one type of frame only. In other words,
		// the existence of a format field doesn't mean you can set it
		// freely at runtime.
		bmusb::PixelFormat pixel_format;

		// Used only for PixelFormat_8BitYCbCrPlanar.
		movit::YCbCrFormat ycbcr_format;

		// The second set is only used for the second field of interlaced inputs.
		GLuint tex_y[2], tex_cbcr[2];  // For PixelFormat_8BitYCbCr.
		GLuint tex_cb[2], tex_cr[2];  // For PixelFormat_8BitYCbCrPlanar (which also uses tex_y).
		GLuint tex_v210[2], tex_444[2];  // For PixelFormat_10BitYCbCr.
		GLuint tex_rgba[2];  // For PixelFormat_8BitBGRA.
		GLuint last_width[2], last_height[2];
		GLuint last_cbcr_width[2], last_cbcr_height[2];
		GLuint last_v210_width[2];  // PixelFormat_10BitYCbCr.
		bool last_interlaced, last_has_signal, last_is_connected;
		unsigned last_frame_rate_nom, last_frame_rate_den;
	};

private:
	bmusb::PixelFormat pixel_format;
	std::mutex freelist_mutex;
	std::queue<Frame> freelist;
	GLenum buffer;
	std::unique_ptr<Userdata[]> userdata;
};

#endif  // !defined(_PBO_FRAME_ALLOCATOR)
