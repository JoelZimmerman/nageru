#ifndef _TWEAKED_INPUTS_H
#define _TWEAKED_INPUTS_H 1

// Some tweaked variations of Movit inputs.

#include <movit/ycbcr_input.h>

namespace movit {
struct ImageFormat;
struct YCbCrFormat;
}  // namespace movit

class NonBouncingYCbCrInput : public movit::YCbCrInput {
public:
        NonBouncingYCbCrInput(const movit::ImageFormat &image_format,
                              const movit::YCbCrFormat &ycbcr_format,
                              unsigned width, unsigned height,
                              movit::YCbCrInputSplitting ycbcr_input_splitting = movit::YCBCR_INPUT_PLANAR)
            : movit::YCbCrInput(image_format, ycbcr_format, width, height, ycbcr_input_splitting) {}

        bool override_disable_bounce() const override { return true; }
};

// We use FlatInput with RGBA inputs a few places where we can't tell when
// uploading the texture whether it needs to be converted from sRGB to linear
// or not. (FlatInput deals with this if you give it pixels, but we give it
// already uploaded textures.)
//
// If we have GL_EXT_texture_sRGB_decode (very common, as far as I can tell),
// we can just always upload with the sRGB flag turned on, and then turn it off
// if not requested; that's sRGBSwitchingFlatInput. If not, we just need to
// turn off the functionality altogether, which is NonsRGBCapableFlatInput.
//
// If you're using NonsRGBCapableFlatInput, upload with GL_RGBA8.
// If using sRGBSwitchingFlatInput, upload with GL_SRGB8_ALPHA8.

class NonsRGBCapableFlatInput : public movit::FlatInput {
public:
	NonsRGBCapableFlatInput(movit::ImageFormat format, movit::MovitPixelFormat pixel_format, GLenum type, unsigned width, unsigned height)
	    : movit::FlatInput(format, pixel_format, type, width, height) {}

	bool can_output_linear_gamma() const override { return false; }
};

class sRGBSwitchingFlatInput : public movit::FlatInput {
public:
	sRGBSwitchingFlatInput(movit::ImageFormat format, movit::MovitPixelFormat pixel_format, GLenum type, unsigned width, unsigned height)
	    : movit::FlatInput(format, pixel_format, type, width, height) {}

	~sRGBSwitchingFlatInput();
	void set_gl_state(GLuint glsl_program_num, const std::string &prefix, unsigned *sampler_num) override;
	void clear_gl_state() override;

	bool set_int(const std::string &key, int value) override
	{
		if (key == "output_linear_gamma") {
			output_linear_gamma = value;
		}
		if (key == "needs_mipmaps") {
			needs_mipmaps = value;
		}
		return movit::FlatInput::set_int(key, value);
	}

private:
	bool output_linear_gamma = false;
	bool needs_mipmaps = false;
	GLuint sampler_obj = 0;
	GLuint texture_unit;
};


#endif  // !defined(_TWEAKED_INPUTS_H)
