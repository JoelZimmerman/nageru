#ifndef _V210CONVERTER_H
#define _V210CONVERTER_H 1

// v210 is a 10-bit 4:2:2 interleaved Y'CbCr format, packing three values
// into a 32-bit int (leaving two unused bits at the top) with chroma being
// sub-sited with the left luma sample. Even though this 2:10:10:10-arrangement
// can be sampled from using the GL_RGB10_A2/GL_UNSIGNED_2_10_10_10_REV format,
// the placement of the Y', Cb and Cr parts within these ints is rather
// complicated, and thus hard to get a single Y'CbCr pixel from efficiently,
// especially on a GPU. Six pixels (six Y', three Cb, three Cr) are packed into
// four such ints in the following pattern (see e.g. the DeckLink documentation
// for reference):
//
//   A  B   G   R
// -----------------
//   X Cr0 Y0  Cb0
//   X  Y2 Cb2  Y1
//   X Cb4 Y3  Cr2
//   X  Y5 Cr4  Y4
//
// This patterns repeats for as long as needed, with the additional constraint
// that stride must be divisible by 128 (or equivalently, 32 four-byte ints,
// or eight pixel groups representing 48 pixels in all).
//
// Thus, v210Converter allows you to convert from v210 to a more regular
// 4:4:4 format (upsampling Cb/Cr on the way, using linear interpolation)
// that the GPU supports natively, again in the form of GL_RGB10_A2
// (with Y', Cb, Cr packed as R, G and B, respectively -- the “alpha” channel
// is always 1).
//
// It does this fairly efficiently using a compute shader, which means you'll
// need compute shader support (GL_ARB_compute_shader + GL_ARB_shader_image_load_store,
// or equivalently, OpenGL 4.3 or newer) to use it. There are many possible
// strategies for doing this in a compute shader, but I ended up settling
// a fairly simple one after some benchmarking; each work unit takes in
// a single four-int group and writes six samples, but as the interpolation
// needs the leftmost chroma samples from the work unit at the right, each line
// is put into a local work group. Cb/Cr is first decoded into shared memory
// (OpenGL guarantees at least 32 kB shared memory for the work group, which is
// enough for up to 6K video or so), and then the rest of the shuffling and
// writing happens. Each line can of course be converted entirely
// independently, so we can fire up as many such work groups as we have lines.
//
// On the Haswell GPU where I developed it (with single-channel memory),
// conversion takes about 1.4 ms for a 720p frame, so it should be possible to
// keep up multiple inputs at 720p60, although probably a faster machine is
// needed if we want to run e.g. heavy scaling filters in the same pipeline.
// (1.4 ms equates to about 35% of the theoretical memory bandwidth of
// 12.8 GB/sec, which is pretty good.)

#include <map>

#include <epoxy/gl.h>

class v210Converter {
public:
	~v210Converter();

	// Whether the current hardware and driver supports the compute shader
	// necessary to do this conversion.
	static bool has_hardware_support();

	// Given an image width, returns the minimum number of 32-bit groups
	// needed for each line. This can be used to size the input texture properly.
	static GLuint get_minimum_v210_texture_width(unsigned width)
	{
		unsigned num_local_groups = (width + 5) / 6;
		return 4 * num_local_groups;
	}

	// Given an image width, returns the stride (in bytes) for each line.
	static size_t get_v210_stride(unsigned width)
	{
		return (width + 47) / 48 * 128;
	}

	// Since work groups need to be determined at shader compile time,
	// each width needs potentially a different shader. You can call this
	// function at startup to make sure a shader for the given width
	// has been compiled, making sure you don't need to start an expensive
	// compilation job while video is running if a new resolution comes along.
	// This is not required, but generally recommended.
	void precompile_shader(unsigned width);

	// Do the actual conversion. tex_src is assumed to be a GL_RGB10_A2
	// texture of at least [get_minimum_v210_texture_width(width), height].
	// tex_dst is assumed to be a GL_RGB10_A2 texture of exactly [width, height]
	// (actually, other sizes will work fine, but be nonsensical).
	// No textures will be allocated or deleted.
	void convert(GLuint tex_src, GLuint tex_dst, unsigned width, unsigned height);

private:
	// Key is number of local groups, ie., ceil(width / 6).
	struct Shader {
		GLuint glsl_program_num = -1;

		// Uniform locations.
		GLuint max_cbcr_x_pos = -1, inbuf_pos = -1, outbuf_pos = -1;
	};
	std::map<unsigned, Shader> shaders;
};

#endif  // !defined(_V210CONVERTER_H)
