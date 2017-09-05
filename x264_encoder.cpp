#include "x264_encoder.h"

#include <assert.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <x264.h>
#include <atomic>
#include <cstdint>
#include <mutex>

#include "defs.h"
#include "flags.h"
#include "metrics.h"
#include "mux.h"
#include "print_latency.h"
#include "timebase.h"
#include "x264_dynamic.h"
#include "x264_speed_control.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
}

using namespace movit;
using namespace std;
using namespace std::chrono;

namespace {

// X264Encoder can be restarted if --record-x264-video is set, so make these
// metrics global.
atomic<int64_t> metric_x264_queued_frames{0};
atomic<int64_t> metric_x264_max_queued_frames{X264_QUEUE_LENGTH};
atomic<int64_t> metric_x264_dropped_frames{0};
atomic<int64_t> metric_x264_output_frames_i{0};
atomic<int64_t> metric_x264_output_frames_p{0};
atomic<int64_t> metric_x264_output_frames_b{0};
Histogram metric_x264_crf;
LatencyHistogram x264_latency_histogram;
once_flag x264_metrics_inited;

void update_vbv_settings(x264_param_t *param)
{
	if (global_flags.x264_bitrate == -1) {
		return;
	}
	if (global_flags.x264_vbv_buffer_size < 0) {
		param->rc.i_vbv_buffer_size = param->rc.i_bitrate;  // One-second VBV.
	} else {
		param->rc.i_vbv_buffer_size = global_flags.x264_vbv_buffer_size;
	}
	if (global_flags.x264_vbv_max_bitrate < 0) {
		param->rc.i_vbv_max_bitrate = param->rc.i_bitrate;  // CBR.
	} else {
		param->rc.i_vbv_max_bitrate = global_flags.x264_vbv_max_bitrate;
	}
}

}  // namespace

X264Encoder::X264Encoder(AVOutputFormat *oformat)
	: wants_global_headers(oformat->flags & AVFMT_GLOBALHEADER),
	  dyn(load_x264_for_bit_depth(global_flags.x264_bit_depth))
{
	call_once(x264_metrics_inited, [](){
		global_metrics.add("x264_queued_frames", &metric_x264_queued_frames, Metrics::TYPE_GAUGE);
		global_metrics.add("x264_max_queued_frames", &metric_x264_max_queued_frames, Metrics::TYPE_GAUGE);
		global_metrics.add("x264_dropped_frames", &metric_x264_dropped_frames);
		global_metrics.add("x264_output_frames", {{ "type", "i" }}, &metric_x264_output_frames_i);
		global_metrics.add("x264_output_frames", {{ "type", "p" }}, &metric_x264_output_frames_p);
		global_metrics.add("x264_output_frames", {{ "type", "b" }}, &metric_x264_output_frames_b);

		metric_x264_crf.init_uniform(50);
		global_metrics.add("x264_crf", &metric_x264_crf);
		x264_latency_histogram.init("x264");
	});

	size_t bytes_per_pixel = global_flags.x264_bit_depth > 8 ? 2 : 1;
	frame_pool.reset(new uint8_t[global_flags.width * global_flags.height * 2 * bytes_per_pixel * X264_QUEUE_LENGTH]);
	for (unsigned i = 0; i < X264_QUEUE_LENGTH; ++i) {
		free_frames.push(frame_pool.get() + i * (global_flags.width * global_flags.height * 2 * bytes_per_pixel));
	}
	encoder_thread = thread(&X264Encoder::encoder_thread_func, this);
}

X264Encoder::~X264Encoder()
{
	should_quit = true;
	queued_frames_nonempty.notify_all();
	encoder_thread.join();
	if (dyn.handle) {
		dlclose(dyn.handle);
	}
}

void X264Encoder::add_frame(int64_t pts, int64_t duration, YCbCrLumaCoefficients ycbcr_coefficients, const uint8_t *data, const ReceivedTimestamps &received_ts)
{
	assert(!should_quit);

	QueuedFrame qf;
	qf.pts = pts;
	qf.duration = duration;
	qf.ycbcr_coefficients = ycbcr_coefficients;
	qf.received_ts = received_ts;

	{
		lock_guard<mutex> lock(mu);
		if (free_frames.empty()) {
			fprintf(stderr, "WARNING: x264 queue full, dropping frame with pts %ld\n", pts);
			++metric_x264_dropped_frames;
			return;
		}

		qf.data = free_frames.front();
		free_frames.pop();
	}

	size_t bytes_per_pixel = global_flags.x264_bit_depth > 8 ? 2 : 1;
	memcpy(qf.data, data, global_flags.width * global_flags.height * 2 * bytes_per_pixel);

	{
		lock_guard<mutex> lock(mu);
		queued_frames.push(qf);
		queued_frames_nonempty.notify_all();
		metric_x264_queued_frames = queued_frames.size();
	}
}
	
void X264Encoder::init_x264()
{
	x264_param_t param;
	dyn.x264_param_default_preset(&param, global_flags.x264_preset.c_str(), global_flags.x264_tune.c_str());

	param.i_width = global_flags.width;
	param.i_height = global_flags.height;
	param.i_csp = X264_CSP_NV12;
	if (global_flags.x264_bit_depth > 8) {
		param.i_csp |= X264_CSP_HIGH_DEPTH;
	}
	param.b_vfr_input = 1;
	param.i_timebase_num = 1;
	param.i_timebase_den = TIMEBASE;
	param.i_keyint_max = 50; // About one second.
	if (global_flags.x264_speedcontrol) {
		param.i_frame_reference = 16;  // Because speedcontrol is never allowed to change this above what we set at start.
	}

	// NOTE: These should be in sync with the ones in quicksync_encoder.cpp (sps_rbsp()).
	param.vui.i_vidformat = 5;  // Unspecified.
	param.vui.b_fullrange = 0;
	param.vui.i_colorprim = 1;  // BT.709.
	param.vui.i_transfer = 13;  // sRGB.
	if (global_flags.ycbcr_rec709_coefficients) {
		param.vui.i_colmatrix = 1;  // BT.709.
	} else {
		param.vui.i_colmatrix = 6;  // BT.601/SMPTE 170M.
	}

	if (!isinf(global_flags.x264_crf)) {
		param.rc.i_rc_method = X264_RC_CRF;
		param.rc.f_rf_constant = global_flags.x264_crf;
	} else {
		param.rc.i_rc_method = X264_RC_ABR;
		param.rc.i_bitrate = global_flags.x264_bitrate;
	}
	update_vbv_settings(&param);
	if (param.rc.i_vbv_max_bitrate > 0) {
		// If the user wants VBV control to cap the max rate, it is
		// also reasonable to assume that they are fine with the stream
		// constantly being around that rate even for very low-complexity
		// content; the obvious and extreme example being a static
		// black picture.
		//
		// One would think it's fine to have low-complexity content use
		// less bitrate, but it seems to cause problems in practice;
		// e.g. VLC seems to often drop the stream (similar to a buffer
		// underrun) in such cases, but only when streaming from Nageru,
		// not when reading a dump of the same stream from disk.
		// I'm not 100% sure whether it's in VLC (possibly some buffering
		// in the HTTP layer), in microhttpd or somewhere in Nageru itself,
		// but it's a typical case of problems that can arise. Similarly,
		// TCP's congestion control is not always fond of the rate staying
		// low for a while and then rising quickly -- a variation on the same
		// problem.
		//
		// We solve this by simply asking x264 to fill in dummy bits
		// in these cases, so that the bitrate stays reasonable constant.
		// It's a waste of bandwidth, but it makes things go much more
		// smoothly in these cases. (We don't do it if VBV control is off
		// in general, not the least because it makes no sense and x264
		// thus ignores the parameter.)
		param.rc.b_filler = 1;
	}

	// Occasionally players have problem with extremely low quantizers;
	// be on the safe side. Shouldn't affect quality in any meaningful way.
	param.rc.i_qp_min = 5;

	for (const string &str : global_flags.x264_extra_param) {
		const size_t pos = str.find(',');
		if (pos == string::npos) {
			if (dyn.x264_param_parse(&param, str.c_str(), nullptr) != 0) {
				fprintf(stderr, "ERROR: x264 rejected parameter '%s'\n", str.c_str());
			}
		} else {
			const string key = str.substr(0, pos);
			const string value = str.substr(pos + 1);
			if (dyn.x264_param_parse(&param, key.c_str(), value.c_str()) != 0) {
				fprintf(stderr, "ERROR: x264 rejected parameter '%s' set to '%s'\n",
					key.c_str(), value.c_str());
			}
		}
	}

	if (global_flags.x264_bit_depth > 8) {
		dyn.x264_param_apply_profile(&param, "high10");
	} else {
		dyn.x264_param_apply_profile(&param, "high");
	}

	param.b_repeat_headers = !wants_global_headers;

	x264 = dyn.x264_encoder_open(&param);
	if (x264 == nullptr) {
		fprintf(stderr, "ERROR: x264 initialization failed.\n");
		exit(1);
	}

	if (global_flags.x264_speedcontrol) {
		speed_control.reset(new X264SpeedControl(x264, /*f_speed=*/1.0f, X264_QUEUE_LENGTH, /*f_buffer_init=*/1.0f));
	}

	if (wants_global_headers) {
		x264_nal_t *nal;
		int num_nal;

		dyn.x264_encoder_headers(x264, &nal, &num_nal);

		for (int i = 0; i < num_nal; ++i) {
			if (nal[i].i_type == NAL_SEI) {
				// Don't put the SEI in extradata; make it part of the first frame instead.
				buffered_sei += string((const char *)nal[i].p_payload, nal[i].i_payload);
			} else {
				global_headers += string((const char *)nal[i].p_payload, nal[i].i_payload);
			}
		}
	}
}

void X264Encoder::encoder_thread_func()
{
	if (nice(5) == -1) {  // Note that x264 further nices some of its threads.
		perror("nice()");
		// No exit; it's not fatal.
	}
	pthread_setname_np(pthread_self(), "x264_encode");
	init_x264();
	x264_init_done = true;

	bool frames_left;

	do {
		QueuedFrame qf;

		// Wait for a queued frame, then dequeue it.
		{
			unique_lock<mutex> lock(mu);
			queued_frames_nonempty.wait(lock, [this]() { return !queued_frames.empty() || should_quit; });
			if (!queued_frames.empty()) {
				qf = queued_frames.front();
				queued_frames.pop();
			} else {
				qf.pts = -1;
				qf.duration = -1;
				qf.data = nullptr;
			}

			metric_x264_queued_frames = queued_frames.size();
			frames_left = !queued_frames.empty();
		}

		encode_frame(qf);
		
		{
			lock_guard<mutex> lock(mu);
			free_frames.push(qf.data);
		}

		// We should quit only if the should_quit flag is set _and_ we have nothing
		// in either queue.
	} while (!should_quit || frames_left || dyn.x264_encoder_delayed_frames(x264) > 0);

	dyn.x264_encoder_close(x264);
}

void X264Encoder::encode_frame(X264Encoder::QueuedFrame qf)
{
	x264_nal_t *nal = nullptr;
	int num_nal = 0;
	x264_picture_t pic;
	x264_picture_t *input_pic = nullptr;

	if (qf.data) {
		dyn.x264_picture_init(&pic);

		pic.i_pts = qf.pts;
		if (global_flags.x264_bit_depth > 8) {
			pic.img.i_csp = X264_CSP_NV12 | X264_CSP_HIGH_DEPTH;
			pic.img.i_plane = 2;
			pic.img.plane[0] = qf.data;
			pic.img.i_stride[0] = global_flags.width * sizeof(uint16_t);
			pic.img.plane[1] = qf.data + global_flags.width * global_flags.height * sizeof(uint16_t);
			pic.img.i_stride[1] = global_flags.width / 2 * sizeof(uint32_t);
		} else {
			pic.img.i_csp = X264_CSP_NV12;
			pic.img.i_plane = 2;
			pic.img.plane[0] = qf.data;
			pic.img.i_stride[0] = global_flags.width;
			pic.img.plane[1] = qf.data + global_flags.width * global_flags.height;
			pic.img.i_stride[1] = global_flags.width / 2 * sizeof(uint16_t);
		}
		pic.opaque = reinterpret_cast<void *>(intptr_t(qf.duration));

		input_pic = &pic;

		frames_being_encoded[qf.pts] = qf.received_ts;
	}

	// See if we have a new bitrate to change to.
	unsigned new_rate = new_bitrate_kbit.exchange(0);  // Read and clear.
	if (new_rate != 0) {
		bitrate_override_func = [new_rate](x264_param_t *param) {
			param->rc.i_bitrate = new_rate;
			update_vbv_settings(param);
		};
	}

	auto ycbcr_coefficients_override_func = [qf](x264_param_t *param) {
		if (qf.ycbcr_coefficients == YCBCR_REC_709) {
			param->vui.i_colmatrix = 1;  // BT.709.
		} else {
			assert(qf.ycbcr_coefficients == YCBCR_REC_601);
			param->vui.i_colmatrix = 6;  // BT.601/SMPTE 170M.
		}
	};

	if (speed_control) {
		speed_control->set_config_override_function([this, ycbcr_coefficients_override_func](x264_param_t *param) {
			if (bitrate_override_func) {
				bitrate_override_func(param);
			}
			ycbcr_coefficients_override_func(param);
		});
	} else {
		x264_param_t param;
		dyn.x264_encoder_parameters(x264, &param);
		if (bitrate_override_func) {
			bitrate_override_func(&param);
		}
		ycbcr_coefficients_override_func(&param);
		dyn.x264_encoder_reconfig(x264, &param);
	}

	if (speed_control) {
		speed_control->before_frame(float(free_frames.size()) / X264_QUEUE_LENGTH, X264_QUEUE_LENGTH, 1e6 * qf.duration / TIMEBASE);
	}
	dyn.x264_encoder_encode(x264, &nal, &num_nal, input_pic, &pic);
	if (speed_control) {
		speed_control->after_frame();
	}

	if (num_nal == 0) return;

	if (IS_X264_TYPE_I(pic.i_type)) {
		++metric_x264_output_frames_i;
	} else if (IS_X264_TYPE_B(pic.i_type)) {
		++metric_x264_output_frames_b;
	} else {
		++metric_x264_output_frames_p;
	}

	metric_x264_crf.count_event(pic.prop.f_crf_avg);

	if (frames_being_encoded.count(pic.i_pts)) {
		ReceivedTimestamps received_ts = frames_being_encoded[pic.i_pts];
		frames_being_encoded.erase(pic.i_pts);

		static int frameno = 0;
		print_latency("Current x264 latency (video inputs → network mux):",
			received_ts, (pic.i_type == X264_TYPE_B || pic.i_type == X264_TYPE_BREF),
			&frameno, &x264_latency_histogram);
	} else {
		assert(false);
	}

	// We really need one AVPacket for the entire frame, it seems,
	// so combine it all.
	size_t num_bytes = buffered_sei.size();
	for (int i = 0; i < num_nal; ++i) {
		num_bytes += nal[i].i_payload;
	}

	unique_ptr<uint8_t[]> data(new uint8_t[num_bytes]);
	uint8_t *ptr = data.get();

	if (!buffered_sei.empty()) {
		memcpy(ptr, buffered_sei.data(), buffered_sei.size());
		ptr += buffered_sei.size();
		buffered_sei.clear();
	}
	for (int i = 0; i < num_nal; ++i) {
		memcpy(ptr, nal[i].p_payload, nal[i].i_payload);
		ptr += nal[i].i_payload;
	}

	AVPacket pkt;
	memset(&pkt, 0, sizeof(pkt));
	pkt.buf = nullptr;
	pkt.data = data.get();
	pkt.size = num_bytes;
	pkt.stream_index = 0;
	if (pic.b_keyframe) {
		pkt.flags = AV_PKT_FLAG_KEY;
	} else {
		pkt.flags = 0;
	}
	pkt.duration = reinterpret_cast<intptr_t>(pic.opaque);

	for (Mux *mux : muxes) {
		mux->add_packet(pkt, pic.i_pts, pic.i_dts);
	}
}
