#include "pbo_frame_allocator.h"

#include <bmusb/bmusb.h>
#include <movit/util.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <cstddef>

#include "flags.h"
#include "v210_converter.h"

using namespace std;

namespace {

void set_clamp_to_edge()
{
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	check_error();
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	check_error();
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	check_error();
}

}  // namespace

PBOFrameAllocator::PBOFrameAllocator(bmusb::PixelFormat pixel_format, size_t frame_size, GLuint width, GLuint height, size_t num_queued_frames, GLenum buffer, GLenum permissions, GLenum map_bits)
        : pixel_format(pixel_format), buffer(buffer)
{
	userdata.reset(new Userdata[num_queued_frames]);
	for (size_t i = 0; i < num_queued_frames; ++i) {
		GLuint pbo;
		glGenBuffers(1, &pbo);
		check_error();
		glBindBuffer(buffer, pbo);
		check_error();
		glBufferStorage(buffer, frame_size, nullptr, permissions | GL_MAP_PERSISTENT_BIT);
		check_error();

		Frame frame;
		frame.data = (uint8_t *)glMapBufferRange(buffer, 0, frame_size, permissions | map_bits | GL_MAP_PERSISTENT_BIT);
		frame.data2 = frame.data + frame_size / 2;
		check_error();
		frame.size = frame_size;
		frame.userdata = &userdata[i];
		userdata[i].pbo = pbo;
		userdata[i].pixel_format = pixel_format;
		frame.owner = this;

		// For 8-bit non-planar Y'CbCr, we ask the driver to split Y' and Cb/Cr
		// into separate textures. For 10-bit, the input format (v210)
		// is complicated enough that we need to interpolate up to 4:4:4,
		// which we do in a compute shader ourselves. For BGRA, the data
		// is already 4:4:4:4.
		frame.interleaved = (pixel_format == bmusb::PixelFormat_8BitYCbCr);

		// Create textures. We don't allocate any data for the second field at this point
		// (just create the texture state with the samplers), since our default assumed
		// resolution is progressive.
		switch (pixel_format) {
		case bmusb::PixelFormat_8BitYCbCr:
			glGenTextures(2, userdata[i].tex_y);
			check_error();
			glGenTextures(2, userdata[i].tex_cbcr);
			check_error();
			break;
		case bmusb::PixelFormat_10BitYCbCr:
			glGenTextures(2, userdata[i].tex_v210);
			check_error();
			glGenTextures(2, userdata[i].tex_444);
			check_error();
			break;
		case bmusb::PixelFormat_8BitBGRA:
			glGenTextures(2, userdata[i].tex_rgba);
			check_error();
			break;
		case bmusb::PixelFormat_8BitYCbCrPlanar:
			glGenTextures(2, userdata[i].tex_y);
			check_error();
			glGenTextures(2, userdata[i].tex_cb);
			check_error();
			glGenTextures(2, userdata[i].tex_cr);
			check_error();
			break;
		default:
			assert(false);
		}

		userdata[i].last_width[0] = width;
		userdata[i].last_height[0] = height;
		userdata[i].last_cbcr_width[0] = width / 2;
		userdata[i].last_cbcr_height[0] = height;
		userdata[i].last_v210_width[0] = 0;

		userdata[i].last_width[1] = 0;
		userdata[i].last_height[1] = 0;
		userdata[i].last_cbcr_width[1] = 0;
		userdata[i].last_cbcr_height[1] = 0;
		userdata[i].last_v210_width[1] = 0;

		userdata[i].last_interlaced = false;
		userdata[i].last_has_signal = false;
		userdata[i].last_is_connected = false;
		for (unsigned field = 0; field < 2; ++field) {
			switch (pixel_format) {
			case bmusb::PixelFormat_10BitYCbCr: {
				const size_t v210_width = v210Converter::get_minimum_v210_texture_width(width);

				// Seemingly we need to set the minification filter even though
				// shader image loads don't use them, or NVIDIA will just give us
				// zero back.
				glBindTexture(GL_TEXTURE_2D, userdata[i].tex_v210[field]);
				check_error();
				glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
				check_error();
				if (field == 0) {
					userdata[i].last_v210_width[0] = v210_width;
					glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB10_A2, v210_width, height, 0, GL_RGBA, GL_UNSIGNED_INT_2_10_10_10_REV, nullptr);
					check_error();
				}

				glBindTexture(GL_TEXTURE_2D, userdata[i].tex_444[field]);
				check_error();
				set_clamp_to_edge();
				if (field == 0) {
					glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB10_A2, width, height, 0, GL_RGBA, GL_UNSIGNED_INT_2_10_10_10_REV, nullptr);
					check_error();
				}
				break;
			}
			case bmusb::PixelFormat_8BitYCbCr:
				glBindTexture(GL_TEXTURE_2D, userdata[i].tex_y[field]);
				check_error();
				set_clamp_to_edge();
				if (field == 0) {
					glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, width, height, 0, GL_RED, GL_UNSIGNED_BYTE, nullptr);
					check_error();
				}

				glBindTexture(GL_TEXTURE_2D, userdata[i].tex_cbcr[field]);
				check_error();
				set_clamp_to_edge();
				if (field == 0) {
					glTexImage2D(GL_TEXTURE_2D, 0, GL_RG8, width / 2, height, 0, GL_RG, GL_UNSIGNED_BYTE, nullptr);
					check_error();
				}
				break;
			case bmusb::PixelFormat_8BitBGRA:
				glBindTexture(GL_TEXTURE_2D, userdata[i].tex_rgba[field]);
				check_error();
				set_clamp_to_edge();
				if (field == 0) {
					if (global_flags.can_disable_srgb_decoder) {  // See the comments in tweaked_inputs.h.
						glTexImage2D(GL_TEXTURE_2D, 0, GL_SRGB8_ALPHA8, width, height, 0, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, nullptr);
					} else {
						glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, nullptr);
					}
					check_error();
				}
				break;
			case bmusb::PixelFormat_8BitYCbCrPlanar:
				glBindTexture(GL_TEXTURE_2D, userdata[i].tex_y[field]);
				check_error();
				set_clamp_to_edge();
				if (field == 0) {
					glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, width, height, 0, GL_RED, GL_UNSIGNED_BYTE, nullptr);
					check_error();
				}

				glBindTexture(GL_TEXTURE_2D, userdata[i].tex_cb[field]);
				check_error();
				set_clamp_to_edge();
				if (field == 0) {
					glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, width / 2, height, 0, GL_RED, GL_UNSIGNED_BYTE, nullptr);
					check_error();
				}

				glBindTexture(GL_TEXTURE_2D, userdata[i].tex_cr[field]);
				check_error();
				set_clamp_to_edge();
				if (field == 0) {
					glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, width / 2, height, 0, GL_RED, GL_UNSIGNED_BYTE, nullptr);
					check_error();
				}
				break;
			default:
				assert(false);
			}
		}

		freelist.push(frame);
	}
	glBindBuffer(buffer, 0);
	check_error();
	glBindTexture(GL_TEXTURE_2D, 0);
	check_error();
}

PBOFrameAllocator::~PBOFrameAllocator()
{
	while (!freelist.empty()) {
		Frame frame = freelist.front();
		freelist.pop();
		GLuint pbo = ((Userdata *)frame.userdata)->pbo;
		glBindBuffer(buffer, pbo);
		check_error();
		glUnmapBuffer(buffer);
		check_error();
		glBindBuffer(buffer, 0);
		check_error();
		glDeleteBuffers(1, &pbo);
		check_error();
		switch (pixel_format) {
		case bmusb::PixelFormat_10BitYCbCr:
			glDeleteTextures(2, ((Userdata *)frame.userdata)->tex_v210);
			check_error();
			glDeleteTextures(2, ((Userdata *)frame.userdata)->tex_444);
			check_error();
			break;
		case bmusb::PixelFormat_8BitYCbCr:
			glDeleteTextures(2, ((Userdata *)frame.userdata)->tex_y);
			check_error();
			glDeleteTextures(2, ((Userdata *)frame.userdata)->tex_cbcr);
			check_error();
			break;
		case bmusb::PixelFormat_8BitBGRA:
			glDeleteTextures(2, ((Userdata *)frame.userdata)->tex_rgba);
			check_error();
			break;
		case bmusb::PixelFormat_8BitYCbCrPlanar:
			glDeleteTextures(2, ((Userdata *)frame.userdata)->tex_y);
			check_error();
			glDeleteTextures(2, ((Userdata *)frame.userdata)->tex_cb);
			check_error();
			glDeleteTextures(2, ((Userdata *)frame.userdata)->tex_cr);
			check_error();
			break;
		default:
			assert(false);
		}
	}
}
//static int sumsum = 0;

bmusb::FrameAllocator::Frame PBOFrameAllocator::alloc_frame()
{
        Frame vf;

	unique_lock<mutex> lock(freelist_mutex);  // Meh.
	if (freelist.empty()) {
		printf("Frame overrun (no more spare PBO frames), dropping frame!\n");
	} else {
		//fprintf(stderr, "freelist has %d allocated\n", ++sumsum);
		vf = freelist.front();
		freelist.pop();  // Meh.
	}
	vf.len = 0;
	vf.overflow = 0;
	return vf;
}

void PBOFrameAllocator::release_frame(Frame frame)
{
	if (frame.overflow > 0) {
		printf("%d bytes overflow after last (PBO) frame\n", int(frame.overflow));
	}

#if 0
	// Poison the page. (Note that this might be bogus if you don't have an OpenGL context.)
	memset(frame.data, 0, frame.size);
	Userdata *userdata = (Userdata *)frame.userdata;
	for (unsigned field = 0; field < 2; ++field) {
		glBindTexture(GL_TEXTURE_2D, userdata->tex_y[field]);
		check_error();
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		check_error();
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		check_error();
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
		check_error();
		glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, userdata->last_width[field], userdata->last_height[field], 0, GL_RED, GL_UNSIGNED_BYTE, nullptr);
		check_error();

		glBindTexture(GL_TEXTURE_2D, userdata->tex_cbcr[field]);
		check_error();
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		check_error();
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		check_error();
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
		check_error();
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RG8, userdata->last_width[field] / 2, userdata->last_height[field], 0, GL_RED, GL_UNSIGNED_BYTE, nullptr);
		check_error();
	}
#endif

	unique_lock<mutex> lock(freelist_mutex);
	freelist.push(frame);
	//--sumsum;
}
