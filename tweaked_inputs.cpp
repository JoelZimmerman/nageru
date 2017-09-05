#include <epoxy/gl.h>
#include <movit/flat_input.h>
#include <movit/util.h>

#include "tweaked_inputs.h"

sRGBSwitchingFlatInput::~sRGBSwitchingFlatInput()
{
	if (sampler_obj != 0) {
		glDeleteSamplers(1, &sampler_obj);
	}
}

void sRGBSwitchingFlatInput::set_gl_state(GLuint glsl_program_num, const std::string &prefix, unsigned *sampler_num)
{
	movit::FlatInput::set_gl_state(glsl_program_num, prefix, sampler_num);
	texture_unit = *sampler_num - 1;

	if (sampler_obj == 0) {
		glGenSamplers(1, &sampler_obj);
		check_error();
		glSamplerParameteri(sampler_obj, GL_TEXTURE_MIN_FILTER, needs_mipmaps ? GL_LINEAR_MIPMAP_NEAREST : GL_LINEAR);
		check_error();
		glSamplerParameteri(sampler_obj, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		check_error();
		glSamplerParameteri(sampler_obj, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
		check_error()
		// This needs to be done on a sampler and not a texture parameter,
		// because the texture could be used from multiple different
		// contexts at the same time. This flag is ignored for non-sRGB-uploaded
		// textures, so we can set it without checking can_output_linear_gamma().
		if (output_linear_gamma) {
			glSamplerParameteri(sampler_obj, GL_TEXTURE_SRGB_DECODE_EXT, GL_DECODE_EXT);
		} else {
			glSamplerParameteri(sampler_obj, GL_TEXTURE_SRGB_DECODE_EXT, GL_SKIP_DECODE_EXT);
		}
		check_error();
	}

	glBindSampler(texture_unit, sampler_obj);
	check_error();
}

void sRGBSwitchingFlatInput::clear_gl_state()
{
	glBindSampler(texture_unit, 0);
	check_error();
}
