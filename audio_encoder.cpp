#include "audio_encoder.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavresample/avresample.h>
#include <libavutil/channel_layout.h>
#include <libavutil/error.h>
#include <libavutil/frame.h>
#include <libavutil/mem.h>
#include <libavutil/opt.h>
#include <libavutil/rational.h>
#include <libavutil/samplefmt.h>
}

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <memory>
#include <string>
#include <vector>

#include "defs.h"
#include "mux.h"
#include "timebase.h"

using namespace std;

AudioEncoder::AudioEncoder(const string &codec_name, int bit_rate, const AVOutputFormat *oformat)
{
	AVCodec *codec = avcodec_find_encoder_by_name(codec_name.c_str());
	if (codec == nullptr) {
		fprintf(stderr, "ERROR: Could not find codec '%s'\n", codec_name.c_str());
		exit(1);
	}

	ctx = avcodec_alloc_context3(codec);
	ctx->bit_rate = bit_rate;
	ctx->sample_rate = OUTPUT_FREQUENCY;
	ctx->sample_fmt = codec->sample_fmts[0];
	ctx->channels = 2;
	ctx->channel_layout = AV_CH_LAYOUT_STEREO;
	ctx->time_base = AVRational{1, TIMEBASE};
	if (oformat->flags & AVFMT_GLOBALHEADER) {
		ctx->flags |= CODEC_FLAG_GLOBAL_HEADER;
	}
	if (avcodec_open2(ctx, codec, NULL) < 0) {
		fprintf(stderr, "Could not open codec '%s'\n", codec_name.c_str());
		exit(1);
	}

	resampler = avresample_alloc_context();
	if (resampler == nullptr) {
		fprintf(stderr, "Allocating resampler failed.\n");
		exit(1);
	}

	av_opt_set_int(resampler, "in_channel_layout",  AV_CH_LAYOUT_STEREO,       0);
	av_opt_set_int(resampler, "out_channel_layout", AV_CH_LAYOUT_STEREO,       0);
	av_opt_set_int(resampler, "in_sample_rate",     OUTPUT_FREQUENCY,          0);
	av_opt_set_int(resampler, "out_sample_rate",    OUTPUT_FREQUENCY,          0);
	av_opt_set_int(resampler, "in_sample_fmt",      AV_SAMPLE_FMT_FLT,         0);
	av_opt_set_int(resampler, "out_sample_fmt",     ctx->sample_fmt, 0);

	if (avresample_open(resampler) < 0) {
		fprintf(stderr, "Could not open resample context.\n");
		exit(1);
	}

	audio_frame = av_frame_alloc();
}

AudioEncoder::~AudioEncoder()
{
	av_frame_free(&audio_frame);
	avresample_free(&resampler);
	avcodec_free_context(&ctx);
}

void AudioEncoder::encode_audio(const vector<float> &audio, int64_t audio_pts)
{
	if (ctx->frame_size == 0) {
		// No queueing needed.
		assert(audio_queue.empty());
		assert(audio.size() % 2 == 0);
		encode_audio_one_frame(&audio[0], audio.size() / 2, audio_pts);
		return;
	}

	int64_t sample_offset = audio_queue.size();

	audio_queue.insert(audio_queue.end(), audio.begin(), audio.end());
	size_t sample_num;
	for (sample_num = 0;
	     sample_num + ctx->frame_size * 2 <= audio_queue.size();
	     sample_num += ctx->frame_size * 2) {
		int64_t adjusted_audio_pts = audio_pts + (int64_t(sample_num) - sample_offset) * TIMEBASE / (OUTPUT_FREQUENCY * 2);
		encode_audio_one_frame(&audio_queue[sample_num],
		                       ctx->frame_size,
		                       adjusted_audio_pts);
	}
	audio_queue.erase(audio_queue.begin(), audio_queue.begin() + sample_num);

	last_pts = audio_pts + audio.size() * TIMEBASE / (OUTPUT_FREQUENCY * 2);
}

void AudioEncoder::encode_audio_one_frame(const float *audio, size_t num_samples, int64_t audio_pts)
{
	audio_frame->pts = audio_pts;
	audio_frame->nb_samples = num_samples;
	audio_frame->channel_layout = AV_CH_LAYOUT_STEREO;
	audio_frame->format = ctx->sample_fmt;
	audio_frame->sample_rate = OUTPUT_FREQUENCY;

	if (av_samples_alloc(audio_frame->data, nullptr, 2, num_samples, ctx->sample_fmt, 0) < 0) {
		fprintf(stderr, "Could not allocate %ld samples.\n", num_samples);
		exit(1);
	}

	if (avresample_convert(resampler, audio_frame->data, 0, num_samples,
	                       (uint8_t **)&audio, 0, num_samples) < 0) {
		fprintf(stderr, "Audio conversion failed.\n");
		exit(1);
	}

	int err = avcodec_send_frame(ctx, audio_frame);
	if (err < 0) {
		fprintf(stderr, "avcodec_send_frame() failed with error %d\n", err);
		exit(1);
	}

	for ( ;; ) {  // Termination condition within loop.
		AVPacket pkt;
		av_init_packet(&pkt);
		pkt.data = nullptr;
		pkt.size = 0;
		int err = avcodec_receive_packet(ctx, &pkt);
		if (err == 0) {
			pkt.stream_index = 1;
			pkt.flags = 0;
			for (Mux *mux : muxes) {
				mux->add_packet(pkt, pkt.pts, pkt.dts);
			}
			av_packet_unref(&pkt);
		} else if (err == AVERROR(EAGAIN)) {
			break;
		} else {
			fprintf(stderr, "avcodec_receive_frame() failed with error %d\n", err);
			exit(1);
		}
	}

	av_freep(&audio_frame->data[0]);
	av_frame_unref(audio_frame);
}

void AudioEncoder::encode_last_audio()
{
	if (!audio_queue.empty()) {
		// Last frame can be whatever size we want.
		assert(audio_queue.size() % 2 == 0);
		encode_audio_one_frame(&audio_queue[0], audio_queue.size() / 2, last_pts);
		audio_queue.clear();
	}

	if (ctx->codec->capabilities & AV_CODEC_CAP_DELAY) {
		// Collect any delayed frames.
		for ( ;; ) {
			AVPacket pkt;
			av_init_packet(&pkt);
			pkt.data = nullptr;
			pkt.size = 0;
			int err = avcodec_receive_packet(ctx, &pkt);
			if (err == 0) {
				pkt.stream_index = 1;
				pkt.flags = 0;
				for (Mux *mux : muxes) {
					mux->add_packet(pkt, pkt.pts, pkt.dts);
				}
				av_packet_unref(&pkt);
			} else if (err == AVERROR_EOF) {
				break;
			} else {
				fprintf(stderr, "avcodec_receive_frame() failed with error %d\n", err);
				exit(1);
			}
		}
	}
}

AVCodecParametersWithDeleter AudioEncoder::get_codec_parameters()
{
	AVCodecParameters *codecpar = avcodec_parameters_alloc();
	avcodec_parameters_from_context(codecpar, ctx);
	return AVCodecParametersWithDeleter(codecpar);
}
