#include "image_input.h"

#include <errno.h>
#include <movit/flat_input.h>
#include <movit/image_format.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/error.h>
#include <libavutil/frame.h>
#include <libavutil/imgutils.h>
#include <libavutil/mem.h>
#include <libavutil/pixfmt.h>
#include <libswscale/swscale.h>
}

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <cstddef>
#include <functional>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

#include "ffmpeg_raii.h"
#include "ffmpeg_util.h"
#include "flags.h"

struct SwsContext;

using namespace std;

ImageInput::ImageInput(const string &filename)
	: movit::FlatInput({movit::COLORSPACE_sRGB, movit::GAMMA_sRGB}, movit::FORMAT_RGBA_POSTMULTIPLIED_ALPHA,
	                   GL_UNSIGNED_BYTE, 1280, 720),  // Resolution will be overwritten.
	  filename(filename),
	  pathname(search_for_file_or_die(filename)),
	  current_image(load_image(filename, pathname))
{
	if (current_image == nullptr) {  // Could happen even though search_for_file() returned.
		fprintf(stderr, "Couldn't load image, exiting.\n");
		exit(1);
	}
	set_width(current_image->width);
	set_height(current_image->height);
	set_pixel_data(current_image->pixels.get());
}

void ImageInput::set_gl_state(GLuint glsl_program_num, const string& prefix, unsigned *sampler_num)
{
	// See if the background thread has given us a new version of our image.
	// Note: The old version might still be lying around in other ImageInputs
	// (in fact, it's likely), but at least the total amount of memory used
	// is bounded. Currently we don't even share textures between them,
	// so there's a fair amount of OpenGL memory waste anyway (the cache
	// is mostly there to save startup time, not RAM).
	{
		unique_lock<mutex> lock(all_images_lock);
		if (all_images[pathname] != current_image) {
			current_image = all_images[pathname];
			set_pixel_data(current_image->pixels.get());
		}
	}
	movit::FlatInput::set_gl_state(glsl_program_num, prefix, sampler_num);
}

shared_ptr<const ImageInput::Image> ImageInput::load_image(const string &filename, const string &pathname)
{
	unique_lock<mutex> lock(all_images_lock);  // Held also during loading.
	if (all_images.count(pathname)) {
		return all_images[pathname];
	}

	all_images[pathname] = load_image_raw(pathname);
	timespec first_modified = all_images[pathname]->last_modified;
	update_threads[pathname] =
		thread(bind(update_thread_func, filename, pathname, first_modified));

	return all_images[pathname];
}

shared_ptr<const ImageInput::Image> ImageInput::load_image_raw(const string &pathname)
{
	// Note: Call before open, not after; otherwise, there's a race.
	// (There is now, too, but it tips the correct way. We could use fstat()
	// if we had the file descriptor.)
	struct stat buf;
	if (stat(pathname.c_str(), &buf) != 0) {
		fprintf(stderr, "%s: Error stat-ing file\n", pathname.c_str());
		return nullptr;
	}
	timespec last_modified = buf.st_mtim;

	auto format_ctx = avformat_open_input_unique(pathname.c_str(), nullptr, nullptr);
	if (format_ctx == nullptr) {
		fprintf(stderr, "%s: Error opening file\n", pathname.c_str());
		return nullptr;
	}

	if (avformat_find_stream_info(format_ctx.get(), nullptr) < 0) {
		fprintf(stderr, "%s: Error finding stream info\n", pathname.c_str());
		return nullptr;
	}

	int stream_index = find_stream_index(format_ctx.get(), AVMEDIA_TYPE_VIDEO);
	if (stream_index == -1) {
		fprintf(stderr, "%s: No video stream found\n", pathname.c_str());
		return nullptr;
	}

	const AVCodecParameters *codecpar = format_ctx->streams[stream_index]->codecpar;
	AVCodecContextWithDeleter codec_ctx = avcodec_alloc_context3_unique(nullptr);
	if (avcodec_parameters_to_context(codec_ctx.get(), codecpar) < 0) {
		fprintf(stderr, "%s: Cannot fill codec parameters\n", pathname.c_str());
		return nullptr;
	}
	AVCodec *codec = avcodec_find_decoder(codecpar->codec_id);
	if (codec == nullptr) {
		fprintf(stderr, "%s: Cannot find decoder\n", pathname.c_str());
		return nullptr;
	}
	if (avcodec_open2(codec_ctx.get(), codec, nullptr) < 0) {
		fprintf(stderr, "%s: Cannot open decoder\n", pathname.c_str());
		return nullptr;
	}
	unique_ptr<AVCodecContext, decltype(avcodec_close)*> codec_ctx_cleanup(
		codec_ctx.get(), avcodec_close);

	// Read packets until we have a frame or there are none left.
	int frame_finished = 0;
	AVFrameWithDeleter frame = av_frame_alloc_unique();
	bool eof = false;
	do {
		AVPacket pkt;
		unique_ptr<AVPacket, decltype(av_packet_unref)*> pkt_cleanup(
			&pkt, av_packet_unref);
		av_init_packet(&pkt);
		pkt.data = nullptr;
		pkt.size = 0;
		if (av_read_frame(format_ctx.get(), &pkt) == 0) {
			if (pkt.stream_index != stream_index) {
				continue;
			}
			if (avcodec_send_packet(codec_ctx.get(), &pkt) < 0) {
				fprintf(stderr, "%s: Cannot send packet to codec.\n", pathname.c_str());
				return nullptr;
			}
		} else {
			eof = true;  // Or error, but ignore that for the time being.
		}

		int err = avcodec_receive_frame(codec_ctx.get(), frame.get());
		if (err == 0) {
			frame_finished = true;
			break;
		} else if (err != AVERROR(EAGAIN)) {
			fprintf(stderr, "%s: Cannot receive frame from codec.\n", pathname.c_str());
			return nullptr;
		}
	} while (!eof);

	if (!frame_finished) {
		fprintf(stderr, "%s: Decoder did not output frame.\n", pathname.c_str());
		return nullptr;
	}

	uint8_t *pic_data[4] = {nullptr};
	unique_ptr<uint8_t *, decltype(av_freep)*> pic_data_cleanup(
		&pic_data[0], av_freep);
	int linesizes[4];
	if (av_image_alloc(pic_data, linesizes, frame->width, frame->height, AV_PIX_FMT_RGBA, 1) < 0) {
		fprintf(stderr, "%s: Could not allocate picture data\n", pathname.c_str());
		return nullptr;
	}
	unique_ptr<SwsContext, decltype(sws_freeContext)*> sws_ctx(
		sws_getContext(frame->width, frame->height,
			(AVPixelFormat)frame->format, frame->width, frame->height,
			AV_PIX_FMT_RGBA, SWS_BICUBIC, nullptr, nullptr, nullptr),
		sws_freeContext);
	if (sws_ctx == nullptr) {
		fprintf(stderr, "%s: Could not create scaler context\n", pathname.c_str());
		return nullptr;
	}
	sws_scale(sws_ctx.get(), frame->data, frame->linesize, 0, frame->height, pic_data, linesizes);

	size_t len = frame->width * frame->height * 4;
	unique_ptr<uint8_t[]> image_data(new uint8_t[len]);
	av_image_copy_to_buffer(image_data.get(), len, pic_data, linesizes, AV_PIX_FMT_RGBA, frame->width, frame->height, 1);

	shared_ptr<Image> image(new Image{unsigned(frame->width), unsigned(frame->height), move(image_data), last_modified});
	return image;
}

// Fire up a thread to update the image every second.
// We could do inotify, but this is good enough for now.
void ImageInput::update_thread_func(const std::string &filename, const std::string &pathname, const timespec &first_modified)
{
	char thread_name[16];
	snprintf(thread_name, sizeof(thread_name), "Update_%s", filename.c_str());
	pthread_setname_np(pthread_self(), thread_name);

	timespec last_modified = first_modified;
	struct stat buf;
	for ( ;; ) {
		{
			unique_lock<mutex> lock(threads_should_quit_mu);
			threads_should_quit_modified.wait_for(lock, chrono::seconds(1), []() { return threads_should_quit; });
		}

		if (threads_should_quit) {
			return;
		}

		if (stat(pathname.c_str(), &buf) != 0) {
			fprintf(stderr, "%s: Couldn't check for new version, leaving the old in place.\n", pathname.c_str());
			continue;
		}
		if (buf.st_mtim.tv_sec == last_modified.tv_sec &&
		    buf.st_mtim.tv_nsec == last_modified.tv_nsec) {
			// Not changed.
			continue;
		}
		shared_ptr<const Image> image = load_image_raw(pathname);
		if (image == nullptr) {
			fprintf(stderr, "Couldn't load image, leaving the old in place.\n");
			continue;
		}
		fprintf(stderr, "Loaded new version of %s from disk.\n", pathname.c_str());
		unique_lock<mutex> lock(all_images_lock);
		all_images[pathname] = image;
		last_modified = image->last_modified;
	}
}

void ImageInput::shutdown_updaters()
{
	{
		unique_lock<mutex> lock(threads_should_quit_mu);
		threads_should_quit = true;
		threads_should_quit_modified.notify_all();
	}
	for (auto &it : update_threads) {
		it.second.join();
	}
}

mutex ImageInput::all_images_lock;
map<string, shared_ptr<const ImageInput::Image>> ImageInput::all_images;
map<string, thread> ImageInput::update_threads;
mutex ImageInput::threads_should_quit_mu;
bool ImageInput::threads_should_quit = false;
condition_variable ImageInput::threads_should_quit_modified;
