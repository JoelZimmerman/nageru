#ifndef _TIMECODE_RENDERER_H
#define _TIMECODE_RENDERER_H 1

#include <memory>
#include <string>

#include <epoxy/gl.h>

// A class to render a simple text string onto the picture using Qt and OpenGL.

namespace movit {

class ResourcePool;

}  // namespace movit

class QImage;

class TimecodeRenderer {
public:
	TimecodeRenderer(movit::ResourcePool *resource_pool, unsigned display_width, unsigned display_height);
	~TimecodeRenderer();

	// Return a string with the current wall clock time and the
	// logical stream time.
	static std::string get_timecode_text(double pts, unsigned frame_num);

	// The FBO is assumed to contain three outputs (Y', Cb/Cr and RGBA).
	void render_timecode(GLuint fbo, const std::string &text);

private:
	void render_string_to_buffer(const std::string &text);
	void render_buffer_to_fbo(GLuint fbo);

	movit::ResourcePool *resource_pool;
	unsigned display_width, display_height, height;

	GLuint vbo;  // Holds position and texcoord data.
	GLuint tex;
	//std::unique_ptr<unsigned char[]> pixel_buffer;
	std::unique_ptr<QImage> image;

	GLuint program_num;  // Owned by <resource_pool>.
	GLuint texture_sampler_uniform;
	GLuint position_attribute_index, texcoord_attribute_index;
};

#endif
