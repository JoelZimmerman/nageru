#include "mux.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <algorithm>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

extern "C" {
#include <libavformat/avio.h>
#include <libavutil/avutil.h>
#include <libavutil/dict.h>
#include <libavutil/mathematics.h>
#include <libavutil/mem.h>
#include <libavutil/pixfmt.h>
#include <libavutil/rational.h>
}

#include "defs.h"
#include "flags.h"
#include "metrics.h"
#include "timebase.h"

using namespace std;

struct PacketBefore {
	PacketBefore(const AVFormatContext *ctx) : ctx(ctx) {}

	bool operator() (const Mux::QueuedPacket &a_qp, const Mux::QueuedPacket &b_qp) const {
		const AVPacket *a = a_qp.pkt;
		const AVPacket *b = b_qp.pkt;
		int64_t a_dts = (a->dts == AV_NOPTS_VALUE ? a->pts : a->dts);
		int64_t b_dts = (b->dts == AV_NOPTS_VALUE ? b->pts : b->dts);
		AVRational a_timebase = ctx->streams[a->stream_index]->time_base;
		AVRational b_timebase = ctx->streams[b->stream_index]->time_base;
		if (av_compare_ts(a_dts, a_timebase, b_dts, b_timebase) != 0) {
			return av_compare_ts(a_dts, a_timebase, b_dts, b_timebase) < 0;
		} else {
			return av_compare_ts(a->pts, a_timebase, b->pts, b_timebase) < 0;
		}
	}

	const AVFormatContext * const ctx;
};

Mux::Mux(AVFormatContext *avctx, int width, int height, Codec video_codec, const string &video_extradata, const AVCodecParameters *audio_codecpar, int time_base, std::function<void(int64_t)> write_callback, WriteStrategy write_strategy, const vector<MuxMetrics *> &metrics)
	: write_strategy(write_strategy), avctx(avctx), write_callback(write_callback), metrics(metrics)
{
	avstream_video = avformat_new_stream(avctx, nullptr);
	if (avstream_video == nullptr) {
		fprintf(stderr, "avformat_new_stream() failed\n");
		exit(1);
	}
	avstream_video->time_base = AVRational{1, time_base};
	avstream_video->codecpar->codec_type = AVMEDIA_TYPE_VIDEO;
	if (video_codec == CODEC_H264) {
		avstream_video->codecpar->codec_id = AV_CODEC_ID_H264;
	} else {
		assert(video_codec == CODEC_NV12);
		avstream_video->codecpar->codec_id = AV_CODEC_ID_RAWVIDEO;
		avstream_video->codecpar->codec_tag = avcodec_pix_fmt_to_codec_tag(AV_PIX_FMT_NV12);
	}
	avstream_video->codecpar->width = width;
	avstream_video->codecpar->height = height;

	// Colorspace details. Closely correspond to settings in EffectChain_finalize,
	// as noted in each comment.
	// Note that the H.264 stream also contains this information and depending on the
	// mux, this might simply get ignored. See sps_rbsp().
	// Note that there's no way to change this per-frame as the H.264 stream
	// would like to be able to.
	avstream_video->codecpar->color_primaries = AVCOL_PRI_BT709;  // RGB colorspace (inout_format.color_space).
	avstream_video->codecpar->color_trc = AVCOL_TRC_IEC61966_2_1;  // Gamma curve (inout_format.gamma_curve).
	// YUV colorspace (output_ycbcr_format.luma_coefficients).
	if (global_flags.ycbcr_rec709_coefficients) {
		avstream_video->codecpar->color_space = AVCOL_SPC_BT709;
	} else {
		avstream_video->codecpar->color_space = AVCOL_SPC_SMPTE170M;
	}
	avstream_video->codecpar->color_range = AVCOL_RANGE_MPEG;  // Full vs. limited range (output_ycbcr_format.full_range).
	avstream_video->codecpar->chroma_location = AVCHROMA_LOC_LEFT;  // Chroma sample location. See chroma_offset_0[] in Mixer::subsample_chroma().
	avstream_video->codecpar->field_order = AV_FIELD_PROGRESSIVE;

	if (!video_extradata.empty()) {
		avstream_video->codecpar->extradata = (uint8_t *)av_malloc(video_extradata.size());
		avstream_video->codecpar->extradata_size = video_extradata.size();
		memcpy(avstream_video->codecpar->extradata, video_extradata.data(), video_extradata.size());
	}

	avstream_audio = avformat_new_stream(avctx, nullptr);
	if (avstream_audio == nullptr) {
		fprintf(stderr, "avformat_new_stream() failed\n");
		exit(1);
	}
	avstream_audio->time_base = AVRational{1, time_base};
	if (avcodec_parameters_copy(avstream_audio->codecpar, audio_codecpar) < 0) {
		fprintf(stderr, "avcodec_parameters_copy() failed\n");
		exit(1);
	}

	AVDictionary *options = NULL;
	vector<pair<string, string>> opts = MUX_OPTS;
	for (pair<string, string> opt : opts) {
		av_dict_set(&options, opt.first.c_str(), opt.second.c_str(), 0);
	}
	if (avformat_write_header(avctx, &options) < 0) {
		fprintf(stderr, "avformat_write_header() failed\n");
		exit(1);
	}
	for (MuxMetrics *metric : metrics) {
		metric->metric_written_bytes += avctx->pb->pos;
	}

	// Make sure the header is written before the constructor exits.
	avio_flush(avctx->pb);

	if (write_strategy == WRITE_BACKGROUND) {
		writer_thread = thread(&Mux::thread_func, this);
	}
}

Mux::~Mux()
{
	assert(plug_count == 0);
	if (write_strategy == WRITE_BACKGROUND) {
		writer_thread_should_quit = true;
		packet_queue_ready.notify_all();
		writer_thread.join();
	}
	int64_t old_pos = avctx->pb->pos;
	av_write_trailer(avctx);
	for (MuxMetrics *metric : metrics) {
		metric->metric_written_bytes += avctx->pb->pos - old_pos;
	}

	if (!(avctx->oformat->flags & AVFMT_NOFILE) &&
	    !(avctx->flags & AVFMT_FLAG_CUSTOM_IO)) {
		avio_closep(&avctx->pb);
	}
	avformat_free_context(avctx);
}

void Mux::add_packet(const AVPacket &pkt, int64_t pts, int64_t dts, AVRational timebase)
{
	AVPacket pkt_copy;
	if (av_copy_packet(&pkt_copy, &pkt) < 0) {
		fprintf(stderr, "av_copy_packet() failed\n");
		exit(1);
	}
	if (pkt.stream_index == 0) {
		pkt_copy.pts = av_rescale_q(pts, timebase, avstream_video->time_base);
		pkt_copy.dts = av_rescale_q(dts, timebase, avstream_video->time_base);
		pkt_copy.duration = av_rescale_q(pkt.duration, timebase, avstream_video->time_base);
	} else if (pkt.stream_index == 1) {
		pkt_copy.pts = av_rescale_q(pts, timebase, avstream_audio->time_base);
		pkt_copy.dts = av_rescale_q(dts, timebase, avstream_audio->time_base);
		pkt_copy.duration = av_rescale_q(pkt.duration, timebase, avstream_audio->time_base);
	} else {
		assert(false);
	}

	{
		lock_guard<mutex> lock(mu);
		if (write_strategy == WriteStrategy::WRITE_BACKGROUND) {
			packet_queue.push_back(QueuedPacket{ av_packet_clone(&pkt_copy), pts });
			if (plug_count == 0) packet_queue_ready.notify_all();
		} else if (plug_count > 0) {
			packet_queue.push_back(QueuedPacket{ av_packet_clone(&pkt_copy), pts });
		} else {
			write_packet_or_die(pkt_copy, pts);
		}
	}

	av_packet_unref(&pkt_copy);
}

void Mux::write_packet_or_die(const AVPacket &pkt, int64_t unscaled_pts)
{
	for (MuxMetrics *metric : metrics) {
		if (pkt.stream_index == 0) {
			metric->metric_video_bytes += pkt.size;
		} else if (pkt.stream_index == 1) {
			metric->metric_audio_bytes += pkt.size;
		} else {
			assert(false);
		}
	}
	int64_t old_pos = avctx->pb->pos;
	if (av_interleaved_write_frame(avctx, const_cast<AVPacket *>(&pkt)) < 0) {
		fprintf(stderr, "av_interleaved_write_frame() failed\n");
		exit(1);
	}
	avio_flush(avctx->pb);
	for (MuxMetrics *metric : metrics) {
		metric->metric_written_bytes += avctx->pb->pos - old_pos;
	}

	if (pkt.stream_index == 0 && write_callback != nullptr) {
		write_callback(unscaled_pts);
	}
}

void Mux::plug()
{
	lock_guard<mutex> lock(mu);
	++plug_count;
}

void Mux::unplug()
{
	lock_guard<mutex> lock(mu);
	if (--plug_count > 0) {
		return;
	}
	assert(plug_count >= 0);

	sort(packet_queue.begin(), packet_queue.end(), PacketBefore(avctx));

	if (write_strategy == WRITE_BACKGROUND) {
		packet_queue_ready.notify_all();
	} else {
		for (QueuedPacket &qp : packet_queue) {
			write_packet_or_die(*qp.pkt, qp.unscaled_pts);
			av_packet_free(&qp.pkt);
		}
		packet_queue.clear();
	}
}

void Mux::thread_func()
{
	unique_lock<mutex> lock(mu);
	for ( ;; ) {
		packet_queue_ready.wait(lock, [this]() {
			return writer_thread_should_quit || (!packet_queue.empty() && plug_count == 0);
		});
		if (writer_thread_should_quit && packet_queue.empty()) {
			// All done.
			break;
		}

		assert(!packet_queue.empty() && plug_count == 0);
		vector<QueuedPacket> packets;
		swap(packets, packet_queue);

		lock.unlock();
		for (QueuedPacket &qp : packets) {
			write_packet_or_die(*qp.pkt, qp.unscaled_pts);
			av_packet_free(&qp.pkt);
		}
		lock.lock();
	}
}

void MuxMetrics::init(const vector<pair<string, string>> &labels)
{
	vector<pair<string, string>> labels_video = labels;
	labels_video.emplace_back("stream", "video");
	global_metrics.add("mux_stream_bytes", labels_video, &metric_video_bytes);

	vector<pair<string, string>> labels_audio = labels;
	labels_audio.emplace_back("stream", "audio");
	global_metrics.add("mux_stream_bytes", labels_audio, &metric_audio_bytes);

	global_metrics.add("mux_written_bytes", labels, &metric_written_bytes);
}
