#ifndef _IMAGE_INPUT_H
#define _IMAGE_INPUT_H 1

#include <epoxy/gl.h>
#include <movit/flat_input.h>
#include <stdbool.h>
#include <time.h>
#include <condition_variable>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

// An output that takes its input from a static image, loaded with ffmpeg.
// comes from a single 2D array with chunky pixels. The image is refreshed
// from disk about every second.
class ImageInput : public movit::FlatInput {
public:
	ImageInput(const std::string &filename);

	std::string effect_type_id() const override { return "ImageInput"; }
	void set_gl_state(GLuint glsl_program_num, const std::string& prefix, unsigned *sampler_num) override;
	static void shutdown_updaters();
	
private:
	struct Image {
		unsigned width, height;
		std::unique_ptr<uint8_t[]> pixels;
		timespec last_modified;
	};

	std::string filename, pathname;
	std::shared_ptr<const Image> current_image;

	static std::shared_ptr<const Image> load_image(const std::string &filename, const std::string &pathname);
	static std::shared_ptr<const Image> load_image_raw(const std::string &pathname);
	static void update_thread_func(const std::string &filename, const std::string &pathname, const timespec &first_modified);
	static std::mutex all_images_lock;
	static std::map<std::string, std::shared_ptr<const Image>> all_images;
	static std::map<std::string, std::thread> update_threads;

	static std::mutex threads_should_quit_mu;
	static bool threads_should_quit;  // Under threads_should_quit_mu.
	static std::condition_variable threads_should_quit_modified;  // Signals when threads_should_quit is set.
};

#endif // !defined(_IMAGE_INPUT_H)
