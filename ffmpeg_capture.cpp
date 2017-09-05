#include "ffmpeg_capture.h"

#include <assert.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/error.h>
#include <libavutil/frame.h>
#include <libavutil/imgutils.h>
#include <libavutil/mem.h>
#include <libavutil/pixfmt.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>
}

#include <chrono>
#include <cstdint>
#include <utility>
#include <vector>

#include "bmusb/bmusb.h"
#include "ffmpeg_raii.h"
#include "ffmpeg_util.h"
#include "flags.h"
#include "image_input.h"
#include "ref_counted_frame.h"
#include "timebase.h"

#define FRAME_SIZE (8 << 20)  // 8 MB.

using namespace std;
using namespace std::chrono;
using namespace bmusb;
using namespace movit;

namespace {

steady_clock::time_point compute_frame_start(int64_t frame_pts, int64_t pts_origin, const AVRational &video_timebase, const steady_clock::time_point &origin, double rate)
{
	const duration<double> pts((frame_pts - pts_origin) * double(video_timebase.num) / double(video_timebase.den));
	return origin + duration_cast<steady_clock::duration>(pts / rate);
}

bool changed_since(const std::string &pathname, const timespec &ts)
{
	if (ts.tv_sec < 0) {
		return false;
	}
	struct stat buf;
	if (stat(pathname.c_str(), &buf) != 0) {
		fprintf(stderr, "%s: Couldn't check for new version, leaving the old in place.\n", pathname.c_str());
		return false;
	}
	return (buf.st_mtim.tv_sec != ts.tv_sec || buf.st_mtim.tv_nsec != ts.tv_nsec);
}

bool is_full_range(const AVPixFmtDescriptor *desc)
{
	// This is horrible, but there's no better way that I know of.
	return (strchr(desc->name, 'j') != nullptr);
}

AVPixelFormat decide_dst_format(AVPixelFormat src_format, bmusb::PixelFormat dst_format_type)
{
	if (dst_format_type == bmusb::PixelFormat_8BitBGRA) {
		return AV_PIX_FMT_BGRA;
	}
	if (dst_format_type == FFmpegCapture::PixelFormat_NV12) {
		return AV_PIX_FMT_NV12;
	}

	assert(dst_format_type == bmusb::PixelFormat_8BitYCbCrPlanar);

	// If this is a non-Y'CbCr format, just convert to 4:4:4 Y'CbCr
	// and be done with it. It's too strange to spend a lot of time on.
	// (Let's hope there's no alpha.)
	const AVPixFmtDescriptor *src_desc = av_pix_fmt_desc_get(src_format);
	if (src_desc == nullptr ||
	    src_desc->nb_components != 3 ||
	    (src_desc->flags & AV_PIX_FMT_FLAG_RGB)) {
		return AV_PIX_FMT_YUV444P;
	}

	// The best for us would be Cb and Cr together if possible,
	// but FFmpeg doesn't support that except in the special case of
	// NV12, so we need to go to planar even for the case of NV12.
	// Thus, look for the closest (but no worse) 8-bit planar Y'CbCr format
	// that matches in color range. (This will also include the case of
	// the source format already being acceptable.)
	bool src_full_range = is_full_range(src_desc);
	const char *best_format = "yuv444p";
	unsigned best_score = numeric_limits<unsigned>::max();
	for (const AVPixFmtDescriptor *desc = av_pix_fmt_desc_next(nullptr);
	     desc;
	     desc = av_pix_fmt_desc_next(desc)) {
		// Find planar Y'CbCr formats only.
		if (desc->nb_components != 3) continue;
		if (desc->flags & AV_PIX_FMT_FLAG_RGB) continue;
		if (!(desc->flags & AV_PIX_FMT_FLAG_PLANAR)) continue;
		if (desc->comp[0].plane != 0 ||
		    desc->comp[1].plane != 1 ||
		    desc->comp[2].plane != 2) continue;

		// 8-bit formats only.
		if (desc->flags & AV_PIX_FMT_FLAG_BE) continue;
		if (desc->comp[0].depth != 8) continue;

		// Same or better chroma resolution only.
		int chroma_w_diff = desc->log2_chroma_w - src_desc->log2_chroma_w;
		int chroma_h_diff = desc->log2_chroma_h - src_desc->log2_chroma_h;
		if (chroma_w_diff < 0 || chroma_h_diff < 0)
			continue;

		// Matching full/limited range only.
		if (is_full_range(desc) != src_full_range)
			continue;

		// Pick something with as little excess chroma resolution as possible.
		unsigned score = (1 << (chroma_w_diff)) << chroma_h_diff;
		if (score < best_score) {
			best_score = score;
			best_format = desc->name;
		}
	}
	return av_get_pix_fmt(best_format);
}

YCbCrFormat decode_ycbcr_format(const AVPixFmtDescriptor *desc, const AVFrame *frame)
{
	YCbCrFormat format;
	AVColorSpace colorspace = av_frame_get_colorspace(frame);
	switch (colorspace) {
	case AVCOL_SPC_BT709:
		format.luma_coefficients = YCBCR_REC_709;
		break;
	case AVCOL_SPC_BT470BG:
	case AVCOL_SPC_SMPTE170M:
	case AVCOL_SPC_SMPTE240M:
		format.luma_coefficients = YCBCR_REC_601;
		break;
	case AVCOL_SPC_BT2020_NCL:
		format.luma_coefficients = YCBCR_REC_2020;
		break;
	case AVCOL_SPC_UNSPECIFIED:
		format.luma_coefficients = (frame->height >= 720 ? YCBCR_REC_709 : YCBCR_REC_601);
		break;
	default:
		fprintf(stderr, "Unknown Y'CbCr coefficient enum %d from FFmpeg; choosing Rec. 709.\n",
			colorspace);
		format.luma_coefficients = YCBCR_REC_709;
		break;
	}

	format.full_range = is_full_range(desc);
	format.num_levels = 1 << desc->comp[0].depth;
	format.chroma_subsampling_x = 1 << desc->log2_chroma_w;
	format.chroma_subsampling_y = 1 << desc->log2_chroma_h;

	switch (frame->chroma_location) {
	case AVCHROMA_LOC_LEFT:
		format.cb_x_position = 0.0;
		format.cb_y_position = 0.5;
		break;
	case AVCHROMA_LOC_CENTER:
		format.cb_x_position = 0.5;
		format.cb_y_position = 0.5;
		break;
	case AVCHROMA_LOC_TOPLEFT:
		format.cb_x_position = 0.0;
		format.cb_y_position = 0.0;
		break;
	case AVCHROMA_LOC_TOP:
		format.cb_x_position = 0.5;
		format.cb_y_position = 0.0;
		break;
	case AVCHROMA_LOC_BOTTOMLEFT:
		format.cb_x_position = 0.0;
		format.cb_y_position = 1.0;
		break;
	case AVCHROMA_LOC_BOTTOM:
		format.cb_x_position = 0.5;
		format.cb_y_position = 1.0;
		break;
	default:
		fprintf(stderr, "Unknown chroma location coefficient enum %d from FFmpeg; choosing Rec. 709.\n",
			frame->chroma_location);
		format.cb_x_position = 0.5;
		format.cb_y_position = 0.5;
		break;
	}

	format.cr_x_position = format.cb_x_position;
	format.cr_y_position = format.cb_y_position;
	return format;
}

}  // namespace

FFmpegCapture::FFmpegCapture(const string &filename, unsigned width, unsigned height)
	: filename(filename), width(width), height(height), video_timebase{1, 1}
{
	// Not really used for anything.
	description = "Video: " + filename;

	avformat_network_init();  // In case someone wants this.
}

FFmpegCapture::~FFmpegCapture()
{
	if (has_dequeue_callbacks) {
		dequeue_cleanup_callback();
	}
	avresample_free(&resampler);
}

void FFmpegCapture::configure_card()
{
	if (video_frame_allocator == nullptr) {
		owned_video_frame_allocator.reset(new MallocFrameAllocator(FRAME_SIZE, NUM_QUEUED_VIDEO_FRAMES));
		set_video_frame_allocator(owned_video_frame_allocator.get());
	}
	if (audio_frame_allocator == nullptr) {
		// Audio can come out in pretty large chunks, so increase from the default 1 MB.
		owned_audio_frame_allocator.reset(new MallocFrameAllocator(1 << 20, NUM_QUEUED_AUDIO_FRAMES));
		set_audio_frame_allocator(owned_audio_frame_allocator.get());
	}
}

void FFmpegCapture::start_bm_capture()
{
	if (running) {
		return;
	}
	running = true;
	producer_thread_should_quit.unquit();
	producer_thread = thread(&FFmpegCapture::producer_thread_func, this);
}

void FFmpegCapture::stop_dequeue_thread()
{
	if (!running) {
		return;
	}
	running = false;
	producer_thread_should_quit.quit();
	producer_thread.join();
}

std::map<uint32_t, VideoMode> FFmpegCapture::get_available_video_modes() const
{
	// Note: This will never really be shown in the UI.
	VideoMode mode;

	char buf[256];
	snprintf(buf, sizeof(buf), "%ux%u", width, height);
	mode.name = buf;
	
	mode.autodetect = false;
	mode.width = width;
	mode.height = height;
	mode.frame_rate_num = 60;
	mode.frame_rate_den = 1;
	mode.interlaced = false;

	return {{ 0, mode }};
}

void FFmpegCapture::producer_thread_func()
{
	char thread_name[16];
	snprintf(thread_name, sizeof(thread_name), "FFmpeg_C_%d", card_index);
	pthread_setname_np(pthread_self(), thread_name);

	while (!producer_thread_should_quit.should_quit()) {
		string pathname = search_for_file(filename);
		if (filename.empty()) {
			fprintf(stderr, "%s not found, sleeping one second and trying again...\n", filename.c_str());
			send_disconnected_frame();
			producer_thread_should_quit.sleep_for(seconds(1));
			continue;
		}
		if (!play_video(pathname)) {
			// Error.
			fprintf(stderr, "Error when playing %s, sleeping one second and trying again...\n", pathname.c_str());
			send_disconnected_frame();
			producer_thread_should_quit.sleep_for(seconds(1));
			continue;
		}

		// Probably just EOF, will exit the loop above on next test.
	}

	if (has_dequeue_callbacks) {
                dequeue_cleanup_callback();
		has_dequeue_callbacks = false;
        }
}

void FFmpegCapture::send_disconnected_frame()
{
	// Send an empty frame to signal that we have no signal anymore.
	FrameAllocator::Frame video_frame = video_frame_allocator->alloc_frame();
	if (video_frame.data) {
		VideoFormat video_format;
		video_format.width = width;
		video_format.height = height;
		video_format.stride = width * 4;
		video_format.frame_rate_nom = 60;
		video_format.frame_rate_den = 1;
		video_format.is_connected = false;

		video_frame.len = width * height * 4;
		memset(video_frame.data, 0, video_frame.len);

		frame_callback(-1, AVRational{1, TIMEBASE}, -1, AVRational{1, TIMEBASE}, timecode++,
			video_frame, /*video_offset=*/0, video_format,
			FrameAllocator::Frame(), /*audio_offset=*/0, AudioFormat());
	}
}

bool FFmpegCapture::play_video(const string &pathname)
{
	// Note: Call before open, not after; otherwise, there's a race.
	// (There is now, too, but it tips the correct way. We could use fstat()
	// if we had the file descriptor.)
	timespec last_modified;
	struct stat buf;
	if (stat(pathname.c_str(), &buf) != 0) {
		// Probably some sort of protocol, so can't stat.
		last_modified.tv_sec = -1;
	} else {
		last_modified = buf.st_mtim;
	}

	auto format_ctx = avformat_open_input_unique(pathname.c_str(), nullptr, nullptr);
	if (format_ctx == nullptr) {
		fprintf(stderr, "%s: Error opening file\n", pathname.c_str());
		return false;
	}

	if (avformat_find_stream_info(format_ctx.get(), nullptr) < 0) {
		fprintf(stderr, "%s: Error finding stream info\n", pathname.c_str());
		return false;
	}

	int video_stream_index = find_stream_index(format_ctx.get(), AVMEDIA_TYPE_VIDEO);
	if (video_stream_index == -1) {
		fprintf(stderr, "%s: No video stream found\n", pathname.c_str());
		return false;
	}

	int audio_stream_index = find_stream_index(format_ctx.get(), AVMEDIA_TYPE_AUDIO);

	// Open video decoder.
	const AVCodecParameters *video_codecpar = format_ctx->streams[video_stream_index]->codecpar;
	AVCodec *video_codec = avcodec_find_decoder(video_codecpar->codec_id);
	video_timebase = format_ctx->streams[video_stream_index]->time_base;
	AVCodecContextWithDeleter video_codec_ctx = avcodec_alloc_context3_unique(nullptr);
	if (avcodec_parameters_to_context(video_codec_ctx.get(), video_codecpar) < 0) {
		fprintf(stderr, "%s: Cannot fill video codec parameters\n", pathname.c_str());
		return false;
	}
	if (video_codec == nullptr) {
		fprintf(stderr, "%s: Cannot find video decoder\n", pathname.c_str());
		return false;
	}
	if (avcodec_open2(video_codec_ctx.get(), video_codec, nullptr) < 0) {
		fprintf(stderr, "%s: Cannot open video decoder\n", pathname.c_str());
		return false;
	}
	unique_ptr<AVCodecContext, decltype(avcodec_close)*> video_codec_ctx_cleanup(
		video_codec_ctx.get(), avcodec_close);

	// Open audio decoder, if we have audio.
	AVCodecContextWithDeleter audio_codec_ctx = avcodec_alloc_context3_unique(nullptr);
	if (audio_stream_index != -1) {
		const AVCodecParameters *audio_codecpar = format_ctx->streams[audio_stream_index]->codecpar;
		audio_timebase = format_ctx->streams[audio_stream_index]->time_base;
		if (avcodec_parameters_to_context(audio_codec_ctx.get(), audio_codecpar) < 0) {
			fprintf(stderr, "%s: Cannot fill audio codec parameters\n", pathname.c_str());
			return false;
		}
		AVCodec *audio_codec = avcodec_find_decoder(audio_codecpar->codec_id);
		if (audio_codec == nullptr) {
			fprintf(stderr, "%s: Cannot find audio decoder\n", pathname.c_str());
			return false;
		}
		if (avcodec_open2(audio_codec_ctx.get(), audio_codec, nullptr) < 0) {
			fprintf(stderr, "%s: Cannot open audio decoder\n", pathname.c_str());
			return false;
		}
	}
	unique_ptr<AVCodecContext, decltype(avcodec_close)*> audio_codec_ctx_cleanup(
		audio_codec_ctx.get(), avcodec_close);

	internal_rewind();

	// Main loop.
	while (!producer_thread_should_quit.should_quit()) {
		if (process_queued_commands(format_ctx.get(), pathname, last_modified, /*rewound=*/nullptr)) {
			return true;
		}
		UniqueFrame audio_frame = audio_frame_allocator->alloc_frame();
		AudioFormat audio_format;

		int64_t audio_pts;
		bool error;
		AVFrameWithDeleter frame = decode_frame(format_ctx.get(), video_codec_ctx.get(), audio_codec_ctx.get(),
			pathname, video_stream_index, audio_stream_index, audio_frame.get(), &audio_format, &audio_pts, &error);
		if (error) {
			return false;
		}
		if (frame == nullptr) {
			// EOF. Loop back to the start if we can.
			if (av_seek_frame(format_ctx.get(), /*stream_index=*/-1, /*timestamp=*/0, /*flags=*/0) < 0) {
				fprintf(stderr, "%s: Rewind failed, not looping.\n", pathname.c_str());
				return true;
			}
			if (video_codec_ctx != nullptr) {
				avcodec_flush_buffers(video_codec_ctx.get());
			}
			if (audio_codec_ctx != nullptr) {
				avcodec_flush_buffers(audio_codec_ctx.get());
			}
			// If the file has changed since last time, return to get it reloaded.
			// Note that depending on how you move the file into place, you might
			// end up corrupting the one you're already playing, so this path
			// might not trigger.
			if (changed_since(pathname, last_modified)) {
				return true;
			}
			internal_rewind();
			continue;
		}

		VideoFormat video_format = construct_video_format(frame.get(), video_timebase);
		UniqueFrame video_frame = make_video_frame(frame.get(), pathname, &error);
		if (error) {
			return false;
		}

		for ( ;; ) {
			if (last_pts == 0 && pts_origin == 0) {
				pts_origin = frame->pts;	
			}
			next_frame_start = compute_frame_start(frame->pts, pts_origin, video_timebase, start, rate);
			video_frame->received_timestamp = next_frame_start;
			bool finished_wakeup = producer_thread_should_quit.sleep_until(next_frame_start);
			if (finished_wakeup) {
				if (audio_frame->len > 0) {
					assert(audio_pts != -1);
				}
				frame_callback(frame->pts, video_timebase, audio_pts, audio_timebase, timecode++,
					video_frame.get_and_release(), 0, video_format,
					audio_frame.get_and_release(), 0, audio_format);
				break;
			} else {
				if (producer_thread_should_quit.should_quit()) break;

				bool rewound = false;
				if (process_queued_commands(format_ctx.get(), pathname, last_modified, &rewound)) {
					return true;
				}
				// If we just rewound, drop this frame on the floor and be done.
				if (rewound) {
					break;
				}
				// OK, we didn't, so probably a rate change. Recalculate next_frame_start,
				// but if it's now in the past, we'll reset the origin, so that we don't
				// generate a huge backlog of frames that we need to run through quickly.
				next_frame_start = compute_frame_start(frame->pts, pts_origin, video_timebase, start, rate);
				steady_clock::time_point now = steady_clock::now();
				if (next_frame_start < now) {
					pts_origin = frame->pts;
					start = next_frame_start = now;
				}
			}
		}
		last_pts = frame->pts;
	}
	return true;
}

void FFmpegCapture::internal_rewind()
{				
	pts_origin = last_pts = 0;
	start = next_frame_start = steady_clock::now();
}

bool FFmpegCapture::process_queued_commands(AVFormatContext *format_ctx, const std::string &pathname, timespec last_modified, bool *rewound)
{
	// Process any queued commands from other threads.
	vector<QueuedCommand> commands;
	{
		lock_guard<mutex> lock(queue_mu);
		swap(commands, command_queue);
	}
	for (const QueuedCommand &cmd : commands) {
		switch (cmd.command) {
		case QueuedCommand::REWIND:
			if (av_seek_frame(format_ctx, /*stream_index=*/-1, /*timestamp=*/0, /*flags=*/0) < 0) {
				fprintf(stderr, "%s: Rewind failed, stopping play.\n", pathname.c_str());
			}
			// If the file has changed since last time, return to get it reloaded.
			// Note that depending on how you move the file into place, you might
			// end up corrupting the one you're already playing, so this path
			// might not trigger.
			if (changed_since(pathname, last_modified)) {
				return true;
			}
			internal_rewind();
			if (rewound != nullptr) {
				*rewound = true;
			}
			break;

		case QueuedCommand::CHANGE_RATE:
			// Change the origin to the last played frame.
			start = compute_frame_start(last_pts, pts_origin, video_timebase, start, rate);
			pts_origin = last_pts;
			rate = cmd.new_rate;
			break;
		}
	}
	return false;
}

namespace {

}  // namespace

AVFrameWithDeleter FFmpegCapture::decode_frame(AVFormatContext *format_ctx, AVCodecContext *video_codec_ctx, AVCodecContext *audio_codec_ctx,
	const std::string &pathname, int video_stream_index, int audio_stream_index,
	FrameAllocator::Frame *audio_frame, AudioFormat *audio_format, int64_t *audio_pts, bool *error)
{
	*error = false;

	// Read packets until we have a frame or there are none left.
	bool frame_finished = false;
	AVFrameWithDeleter audio_avframe = av_frame_alloc_unique();
	AVFrameWithDeleter video_avframe = av_frame_alloc_unique();
	bool eof = false;
	*audio_pts = -1;
	do {
		AVPacket pkt;
		unique_ptr<AVPacket, decltype(av_packet_unref)*> pkt_cleanup(
			&pkt, av_packet_unref);
		av_init_packet(&pkt);
		pkt.data = nullptr;
		pkt.size = 0;
		if (av_read_frame(format_ctx, &pkt) == 0) {
			if (pkt.stream_index == audio_stream_index && audio_callback != nullptr) {
				audio_callback(&pkt, format_ctx->streams[audio_stream_index]->time_base);
			}
			if (pkt.stream_index == video_stream_index) {
				if (avcodec_send_packet(video_codec_ctx, &pkt) < 0) {
					fprintf(stderr, "%s: Cannot send packet to video codec.\n", pathname.c_str());
					*error = true;
					return AVFrameWithDeleter(nullptr);
				}
			} else if (pkt.stream_index == audio_stream_index) {
				if (*audio_pts == -1) {
					*audio_pts = pkt.pts;
				}
				if (avcodec_send_packet(audio_codec_ctx, &pkt) < 0) {
					fprintf(stderr, "%s: Cannot send packet to audio codec.\n", pathname.c_str());
					*error = true;
					return AVFrameWithDeleter(nullptr);
				}
			}
		} else {
			eof = true;  // Or error, but ignore that for the time being.
		}

		// Decode audio, if any.
		if (*audio_pts != -1) {
			for ( ;; ) {
				int err = avcodec_receive_frame(audio_codec_ctx, audio_avframe.get());
				if (err == 0) {
					convert_audio(audio_avframe.get(), audio_frame, audio_format);
				} else if (err == AVERROR(EAGAIN)) {
					break;
				} else {
					fprintf(stderr, "%s: Cannot receive frame from audio codec.\n", pathname.c_str());
					*error = true;
					return AVFrameWithDeleter(nullptr);
				}
			}
		}

		// Decode video, if we have a frame.
		int err = avcodec_receive_frame(video_codec_ctx, video_avframe.get());
		if (err == 0) {
			frame_finished = true;
			break;
		} else if (err != AVERROR(EAGAIN)) {
			fprintf(stderr, "%s: Cannot receive frame from video codec.\n", pathname.c_str());
			*error = true;
			return AVFrameWithDeleter(nullptr);
		}
	} while (!eof);

	if (frame_finished)
		return video_avframe;
	else
		return AVFrameWithDeleter(nullptr);
}

void FFmpegCapture::convert_audio(const AVFrame *audio_avframe, FrameAllocator::Frame *audio_frame, AudioFormat *audio_format)
{
	// Decide on a format. If there already is one in this audio frame,
	// we're pretty much forced to use it. If not, we try to find an exact match.
	// If that still doesn't work, we default to 32-bit signed chunked
	// (float would be nice, but there's really no way to signal that yet).
	AVSampleFormat dst_format;
	if (audio_format->bits_per_sample == 0) {
		switch (audio_avframe->format) {
		case AV_SAMPLE_FMT_S16:
		case AV_SAMPLE_FMT_S16P:
			audio_format->bits_per_sample = 16;
			dst_format = AV_SAMPLE_FMT_S16;
			break;
		case AV_SAMPLE_FMT_S32:
		case AV_SAMPLE_FMT_S32P:
		default:
			audio_format->bits_per_sample = 32;
			dst_format = AV_SAMPLE_FMT_S32;
			break;
		}
	} else if (audio_format->bits_per_sample == 16) {
		dst_format = AV_SAMPLE_FMT_S16;
	} else if (audio_format->bits_per_sample == 32) {
		dst_format = AV_SAMPLE_FMT_S32;
	} else {
		assert(false);
	}
	audio_format->num_channels = 2;

	if (resampler == nullptr ||
	    audio_avframe->format != last_src_format ||
	    dst_format != last_dst_format ||
	    av_frame_get_channel_layout(audio_avframe) != last_channel_layout ||
	    av_frame_get_sample_rate(audio_avframe) != last_sample_rate) {
		avresample_free(&resampler);
		resampler = avresample_alloc_context();
		if (resampler == nullptr) {
			fprintf(stderr, "Allocating resampler failed.\n");
			exit(1);
		}

		av_opt_set_int(resampler, "in_channel_layout",  av_frame_get_channel_layout(audio_avframe), 0);
		av_opt_set_int(resampler, "out_channel_layout", AV_CH_LAYOUT_STEREO,                        0);
		av_opt_set_int(resampler, "in_sample_rate",     av_frame_get_sample_rate(audio_avframe),    0);
		av_opt_set_int(resampler, "out_sample_rate",    OUTPUT_FREQUENCY,                           0);
		av_opt_set_int(resampler, "in_sample_fmt",      audio_avframe->format,                      0);
		av_opt_set_int(resampler, "out_sample_fmt",     dst_format,                                 0);

		if (avresample_open(resampler) < 0) {
			fprintf(stderr, "Could not open resample context.\n");
			exit(1);
		}

		last_src_format = AVSampleFormat(audio_avframe->format);
		last_dst_format = dst_format;
		last_channel_layout = av_frame_get_channel_layout(audio_avframe);
		last_sample_rate = av_frame_get_sample_rate(audio_avframe);
	}

	size_t bytes_per_sample = (audio_format->bits_per_sample / 8) * 2;
	size_t num_samples_room = (audio_frame->size - audio_frame->len) / bytes_per_sample;

	uint8_t *data = audio_frame->data + audio_frame->len;
	int out_samples = avresample_convert(resampler, &data, 0, num_samples_room,
		audio_avframe->data, audio_avframe->linesize[0], audio_avframe->nb_samples);
	if (out_samples < 0) {
                fprintf(stderr, "Audio conversion failed.\n");
                exit(1);
        }

	audio_frame->len += out_samples * bytes_per_sample;
}

VideoFormat FFmpegCapture::construct_video_format(const AVFrame *frame, AVRational video_timebase)
{
	VideoFormat video_format;
	video_format.width = width;
	video_format.height = height;
	if (pixel_format == bmusb::PixelFormat_8BitBGRA) {
		video_format.stride = width * 4;
	} else if (pixel_format == FFmpegCapture::PixelFormat_NV12) {
		video_format.stride = width;
	} else {
		assert(pixel_format == bmusb::PixelFormat_8BitYCbCrPlanar);
		video_format.stride = width;
	}
	video_format.frame_rate_nom = video_timebase.den;
	video_format.frame_rate_den = av_frame_get_pkt_duration(frame) * video_timebase.num;
	if (video_format.frame_rate_nom == 0 || video_format.frame_rate_den == 0) {
		// Invalid frame rate.
		video_format.frame_rate_nom = 60;
		video_format.frame_rate_den = 1;
	}
	video_format.has_signal = true;
	video_format.is_connected = true;
	return video_format;
}

UniqueFrame FFmpegCapture::make_video_frame(const AVFrame *frame, const string &pathname, bool *error)
{
	*error = false;

	UniqueFrame video_frame(video_frame_allocator->alloc_frame());
	if (video_frame->data == nullptr) {
		return video_frame;
	}

	if (sws_ctx == nullptr ||
	    sws_last_width != frame->width ||
	    sws_last_height != frame->height ||
	    sws_last_src_format != frame->format) {
		sws_dst_format = decide_dst_format(AVPixelFormat(frame->format), pixel_format);
		sws_ctx.reset(
			sws_getContext(frame->width, frame->height, AVPixelFormat(frame->format),
				width, height, sws_dst_format,
				SWS_BICUBIC, nullptr, nullptr, nullptr));
		sws_last_width = frame->width;
		sws_last_height = frame->height;
		sws_last_src_format = frame->format;
	}
	if (sws_ctx == nullptr) {
		fprintf(stderr, "%s: Could not create scaler context\n", pathname.c_str());
		*error = true;
		return video_frame;
	}

	uint8_t *pic_data[4] = { nullptr, nullptr, nullptr, nullptr };
	int linesizes[4] = { 0, 0, 0, 0 };
	if (pixel_format == bmusb::PixelFormat_8BitBGRA) {
		pic_data[0] = video_frame->data;
		linesizes[0] = width * 4;
		video_frame->len = (width * 4) * height;
	} else if (pixel_format == PixelFormat_NV12) {
		pic_data[0] = video_frame->data;
		linesizes[0] = width;

		pic_data[1] = pic_data[0] + width * height;
		linesizes[1] = width;

		video_frame->len = (width * 2) * height;

		const AVPixFmtDescriptor *desc = av_pix_fmt_desc_get(sws_dst_format);
		current_frame_ycbcr_format = decode_ycbcr_format(desc, frame);
	} else {
		assert(pixel_format == bmusb::PixelFormat_8BitYCbCrPlanar);
		const AVPixFmtDescriptor *desc = av_pix_fmt_desc_get(sws_dst_format);

		int chroma_width = AV_CEIL_RSHIFT(int(width), desc->log2_chroma_w);
		int chroma_height = AV_CEIL_RSHIFT(int(height), desc->log2_chroma_h);

		pic_data[0] = video_frame->data;
		linesizes[0] = width;

		pic_data[1] = pic_data[0] + width * height;
		linesizes[1] = chroma_width;

		pic_data[2] = pic_data[1] + chroma_width * chroma_height;
		linesizes[2] = chroma_width;

		video_frame->len = width * height + 2 * chroma_width * chroma_height;

		current_frame_ycbcr_format = decode_ycbcr_format(desc, frame);
	}
	sws_scale(sws_ctx.get(), frame->data, frame->linesize, 0, frame->height, pic_data, linesizes);

	return video_frame;
}
