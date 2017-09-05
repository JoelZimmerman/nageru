#ifndef _CHROMA_SUBSAMPLER_H
#define _CHROMA_SUBSAMPLER_H 1

#include <epoxy/gl.h>

namespace movit {

class ResourcePool;

}  // namespace movit

class ChromaSubsampler {
public:
	ChromaSubsampler(movit::ResourcePool *resource_pool);
	~ChromaSubsampler();

	// Subsamples chroma (packed Cb and Cr) 2x2 to yield chroma suitable for
	// NV12 (semiplanar 4:2:0). Chroma positioning is left/center (H.264 convention).
	// width and height are the dimensions (in pixels) of the input texture.
	//
	// You can get two equal copies if you'd like; just set dst2_tex to a texture
	// number and it will receive an exact copy of what goes into dst_tex.
	void subsample_chroma(GLuint cbcr_tex, unsigned width, unsigned height, GLuint dst_tex, GLuint dst2_tex = 0);

	// Subsamples and interleaves luma and chroma to give 4:2:2 packed Y'CbCr (UYVY).
	// Chroma positioning is left (H.264 convention).
	// width and height are the dimensions (in pixels) of the input textures.
	void create_uyvy(GLuint y_tex, GLuint cbcr_tex, unsigned width, unsigned height, GLuint dst_tex);

	// Subsamples and interleaves luma and chroma to give 10-bit 4:2:2
	// packed Y'CbCr (v210); see v210converter.h for more information on
	// the format. Luma and chroma are assumed to be 10-bit data packed
	// into 16-bit textures. Chroma positioning is left (H.264 convention).
	// width and height are the dimensions (in pixels) of the input textures;
	// Requires compute shaders; check v210Converter::has_hardware_support().
	void create_v210(GLuint y_tex, GLuint cbcr_tex, unsigned width, unsigned height, GLuint dst_tex);

private:
	movit::ResourcePool *resource_pool;

	GLuint vbo;  // Holds position and texcoord data.

	GLuint cbcr_program_num;  // Owned by <resource_pool>.
	GLuint cbcr_texture_sampler_uniform;
	GLint cbcr_position_attribute_index, cbcr_texcoord_attribute_index;
	GLuint cbcr_chroma_offset_0_location, cbcr_chroma_offset_1_location;

	GLuint uyvy_program_num;  // Owned by <resource_pool>.
	GLuint uyvy_y_texture_sampler_uniform, uyvy_cbcr_texture_sampler_uniform;
	GLint uyvy_position_attribute_index, uyvy_texcoord_attribute_index;
	GLuint uyvy_luma_offset_0_location, uyvy_luma_offset_1_location;
	GLuint uyvy_chroma_offset_0_location, uyvy_chroma_offset_1_location;

	GLuint v210_program_num;  // Compute shader, so owned by ourselves. Can be 0.
	GLuint v210_in_y_pos, v210_in_cbcr_pos, v210_outbuf_pos;
	GLuint v210_inv_width_pos, v210_inv_height_pos;
};

#endif  // !defined(_CHROMA_SUBSAMPLER_H)
