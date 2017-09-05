#include "v210_converter.h"

#include <epoxy/gl.h>
#include <movit/util.h>

using namespace std;

v210Converter::~v210Converter()
{
	for (const auto &shader : shaders) {
		glDeleteProgram(shader.second.glsl_program_num);
		check_error();
	}
}

bool v210Converter::has_hardware_support()
{
	// We don't have a GLES version of this, although GLSL ES 3.1 supports
	// compute shaders. Note that GLSL ES has some extra restrictions,
	// like requiring that the images are allocated with glTexStorage*(),
	// or that binding= is effectively mandatory.
	if (!epoxy_is_desktop_gl()) {
		return false;
	}
	if (epoxy_gl_version() >= 43) {
		// Supports compute shaders natively.
		return true;
	}
	return epoxy_has_gl_extension("GL_ARB_compute_shader") &&
	       epoxy_has_gl_extension("GL_ARB_shader_image_load_store");
}

void v210Converter::precompile_shader(unsigned width)
{
	unsigned num_local_work_groups = (width + 5) / 6;
	if (shaders.count(num_local_work_groups)) {
		// Already exists.
		return;
	}

	char buf[16];
	snprintf(buf, sizeof(buf), "%u", num_local_work_groups);
        string shader_src = R"(#version 150
#extension GL_ARB_compute_shader : enable
#extension GL_ARB_shader_image_load_store : enable
layout(local_size_x = )" + string(buf) + R"() in;
layout(rgb10_a2) uniform restrict readonly image2D inbuf;
layout(rgb10_a2) uniform restrict writeonly image2D outbuf;
uniform int max_cbcr_x;
shared vec2 cbcr[gl_WorkGroupSize.x * 3u];

void main()
{
	int xb = int(gl_LocalInvocationID.x);  // X block.
	int y = int(gl_GlobalInvocationID.y);  // Y (actual line).

	// Load our pixel group, containing data for six pixels.
	vec3 indata[4];
	for (int i = 0; i < 4; ++i) {
		indata[i] = imageLoad(inbuf, ivec2(xb * 4 + i, y)).xyz;
	}

	// Decode Cb and Cr to shared memory, because neighboring blocks need it for interpolation.
	cbcr[xb * 3 + 0] = indata[0].xz;
	cbcr[xb * 3 + 1] = vec2(indata[1].y, indata[2].x);
	cbcr[xb * 3 + 2] = vec2(indata[2].z, indata[3].y);
	memoryBarrierShared();

	float pix_y[6];
	pix_y[0] = indata[0].y;
	pix_y[1] = indata[1].x;
	pix_y[2] = indata[1].z;
	pix_y[3] = indata[2].y;
	pix_y[4] = indata[3].x;
	pix_y[5] = indata[3].z;

	barrier();

	// Interpolate the missing Cb/Cr pixels, taking care not to read past the end of the screen
	// for pixels that we use for interpolation.
	vec2 pix_cbcr[7];
	pix_cbcr[0] = indata[0].xz;
	pix_cbcr[2] = cbcr[min(xb * 3 + 1, max_cbcr_x)];
	pix_cbcr[4] = cbcr[min(xb * 3 + 2, max_cbcr_x)];
	pix_cbcr[6] = cbcr[min(xb * 3 + 3, max_cbcr_x)];
	pix_cbcr[1] = 0.5 * (pix_cbcr[0] + pix_cbcr[2]);
	pix_cbcr[3] = 0.5 * (pix_cbcr[2] + pix_cbcr[4]);
	pix_cbcr[5] = 0.5 * (pix_cbcr[4] + pix_cbcr[6]);

	// Write the decoded pixels to the destination texture.
	for (int i = 0; i < 6; ++i) {
		vec4 outdata = vec4(pix_y[i], pix_cbcr[i].x, pix_cbcr[i].y, 1.0f);
		imageStore(outbuf, ivec2(xb * 6 + i, y), outdata);
	}
}
)";

	Shader shader;

	GLuint shader_num = movit::compile_shader(shader_src, GL_COMPUTE_SHADER);
	check_error();
	shader.glsl_program_num = glCreateProgram();
	check_error();
	glAttachShader(shader.glsl_program_num, shader_num);
	check_error();
	glLinkProgram(shader.glsl_program_num);
	check_error();

	GLint success;
	glGetProgramiv(shader.glsl_program_num, GL_LINK_STATUS, &success);
	check_error();
	if (success == GL_FALSE) {
		GLchar error_log[1024] = {0};
		glGetProgramInfoLog(shader.glsl_program_num, 1024, nullptr, error_log);
		fprintf(stderr, "Error linking program: %s\n", error_log);
		exit(1);
	}

	shader.max_cbcr_x_pos = glGetUniformLocation(shader.glsl_program_num, "max_cbcr_x");
	check_error();
	shader.inbuf_pos = glGetUniformLocation(shader.glsl_program_num, "inbuf");
	check_error();
	shader.outbuf_pos = glGetUniformLocation(shader.glsl_program_num, "outbuf");
	check_error();

	shaders.emplace(num_local_work_groups, shader);
}

void v210Converter::convert(GLuint tex_src, GLuint tex_dst, unsigned width, unsigned height)
{
	precompile_shader(width);
	unsigned num_local_work_groups = (width + 5) / 6;
	const Shader &shader = shaders[num_local_work_groups];

	glUseProgram(shader.glsl_program_num);
	check_error();
	glUniform1i(shader.max_cbcr_x_pos, width / 2 - 1);
	check_error();

	// Bind the textures.
	glUniform1i(shader.inbuf_pos, 0);
	check_error();
	glUniform1i(shader.outbuf_pos, 1);
	check_error();
        glBindImageTexture(0, tex_src, 0, GL_FALSE, 0, GL_READ_ONLY, GL_RGB10_A2);
	check_error();
        glBindImageTexture(1, tex_dst, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGB10_A2);
	check_error();

	// Actually run the shader.
	glDispatchCompute(1, height, 1);
	check_error();

	glUseProgram(0);
	check_error();
}
