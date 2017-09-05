#include "x264_speed_control.h"

#include <dlfcn.h>
#include <math.h>
#include <stdio.h>
#include <x264.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <ratio>
#include <type_traits>

#include "flags.h"
#include "metrics.h"

using namespace std;
using namespace std::chrono;

#define SC_PRESETS 25

X264SpeedControl::X264SpeedControl(x264_t *x264, float f_speed, int i_buffer_size, float f_buffer_init)
	: dyn(load_x264_for_bit_depth(global_flags.x264_bit_depth)),
	  x264(x264), f_speed(f_speed)
{
	x264_param_t param;
	dyn.x264_encoder_parameters(x264, &param);

	float fps = (float)param.i_fps_num / param.i_fps_den;
	uspf = 1e6 / fps;
	set_buffer_size(i_buffer_size);
	buffer_fill = buffer_size * f_buffer_init;
	buffer_fill = max<int64_t>(buffer_fill, uspf);
	buffer_fill = min(buffer_fill, buffer_size);
	timestamp = steady_clock::now();
	preset = -1;
	cplx_num = 3e3; //FIXME estimate initial complexity
	cplx_den = .1;
	stat.min_buffer = buffer_size;
	stat.max_buffer = 0;
	stat.avg_preset = 0.0;
	stat.den = 0;

	metric_x264_speedcontrol_buffer_available_seconds = buffer_fill * 1e-6;
	metric_x264_speedcontrol_buffer_size_seconds = buffer_size * 1e-6;
	metric_x264_speedcontrol_preset_used_frames.init_uniform(SC_PRESETS);
	global_metrics.add("x264_speedcontrol_preset_used_frames", &metric_x264_speedcontrol_preset_used_frames);
	global_metrics.add("x264_speedcontrol_buffer_available_seconds", &metric_x264_speedcontrol_buffer_available_seconds, Metrics::TYPE_GAUGE);
	global_metrics.add("x264_speedcontrol_buffer_size_seconds", &metric_x264_speedcontrol_buffer_size_seconds, Metrics::TYPE_GAUGE);
	global_metrics.add("x264_speedcontrol_idle_frames", &metric_x264_speedcontrol_idle_frames);
	global_metrics.add("x264_speedcontrol_late_frames", &metric_x264_speedcontrol_late_frames);
}

X264SpeedControl::~X264SpeedControl()
{
	fprintf(stderr, "speedcontrol: avg preset=%.3f  buffer min=%.3f max=%.3f\n",
		stat.avg_preset / stat.den,
		(float)stat.min_buffer / buffer_size,
		(float)stat.max_buffer / buffer_size );
	//  x264_log( x264, X264_LOG_INFO, "speedcontrol: avg cplx=%.5f\n", cplx_num / cplx_den );
	if (dyn.handle) {
		dlclose(dyn.handle);
	}
}

typedef struct
{
	float time; // relative encoding time, compared to the other presets
	int subme;
	int me;
	int refs;
	int mix;
	int trellis;
	int partitions;
	int badapt;
	int bframes;
	int direct;
	int merange;
} sc_preset_t;

// The actual presets, including the equivalent commandline options. Note that
// all presets are benchmarked with --weightp 1 --mbtree --rc-lookahead 20
// on top of the given settings (equivalent settings to the "faster" preset).
// Timings and SSIM measurements were done on a quadcore Haswell i5 3.2 GHz
// on the first 1000 frames of "Elephants Dream" in 1080p.
// See experiments/measure-x264.pl for a way to reproduce.
//
// Note that the two first and the two last are also used for extrapolation
// should the desired time be outside the range. Thus, it is disadvantageous if
// they are chosen so that the timings are too close to each other.
static const sc_preset_t presets[SC_PRESETS] = {
#define I4 X264_ANALYSE_I4x4
#define I8 X264_ANALYSE_I8x8
#define P4 X264_ANALYSE_PSUB8x8
#define P8 X264_ANALYSE_PSUB16x16
#define B8 X264_ANALYSE_BSUB16x16
	// Preset 0: 16.583db, --preset superfast --b-adapt 0 --bframes 0
	{ .time= 1.000, .subme=1, .me=X264_ME_DIA, .refs=1, .mix=0, .trellis=0, .partitions=I8|I4, .badapt=0, .bframes=0, .direct=0, .merange=16 },

	// Preset 1: 17.386db, --preset superfast
	{ .time= 1.288, .subme=1, .me=X264_ME_DIA, .refs=1, .mix=0, .trellis=0, .partitions=I8|I4, .badapt=1, .bframes=3, .direct=1, .merange=16 },

	// Preset 2: 17.919db, --preset superfast --subme 2
	{ .time= 2.231, .subme=2, .me=X264_ME_DIA, .refs=1, .mix=0, .trellis=0, .partitions=I8|I4, .badapt=1, .bframes=3, .direct=1, .merange=16 },

	// Preset 3: 18.051db, --preset veryfast
	{ .time= 2.403, .subme=2, .me=X264_ME_HEX, .refs=1, .mix=0, .trellis=0, .partitions=I8|I4|P8|B8, .badapt=1, .bframes=3, .direct=1, .merange=16 },

	// Preset 4: 18.422db, --preset veryfast --subme 3
	{ .time= 2.636, .subme=3, .me=X264_ME_HEX, .refs=1, .mix=0, .trellis=0, .partitions=I8|I4|P8|B8, .badapt=1, .bframes=3, .direct=1, .merange=16 },

	// Preset 5: 18.514db, --preset veryfast --subme 3 --ref 2
	{ .time= 2.844, .subme=3, .me=X264_ME_HEX, .refs=2, .mix=0, .trellis=0, .partitions=I8|I4|P8|B8, .badapt=1, .bframes=3, .direct=1, .merange=16 },

	// Preset 6: 18.564db, --preset veryfast --subme 4 --ref 2
	{ .time= 3.366, .subme=4, .me=X264_ME_HEX, .refs=2, .mix=0, .trellis=0, .partitions=I8|I4|P8|B8, .badapt=1, .bframes=3, .direct=1, .merange=16 },

	// Preset 7: 18.411db, --preset faster
	{ .time= 3.450, .subme=4, .me=X264_ME_HEX, .refs=2, .mix=0, .trellis=1, .partitions=I8|I4|P8|B8, .badapt=1, .bframes=3, .direct=1, .merange=16 },

	// Preset 8: 18.429db, --preset faster --mixed-refs
	{ .time= 3.701, .subme=4, .me=X264_ME_HEX, .refs=2, .mix=1, .trellis=1, .partitions=I8|I4|P8|B8, .badapt=1, .bframes=3, .direct=1, .merange=16 },

	// Preset 9: 18.454db, --preset faster --mixed-refs --subme 5
	{ .time= 4.297, .subme=5, .me=X264_ME_HEX, .refs=2, .mix=1, .trellis=1, .partitions=I8|I4|P8|B8, .badapt=1, .bframes=3, .direct=1, .merange=16 },

	// Preset 10: 18.528db, --preset fast
	{ .time= 5.181, .subme=6, .me=X264_ME_HEX, .refs=2, .mix=1, .trellis=1, .partitions=I8|I4|P8|B8, .badapt=1, .bframes=3, .direct=1, .merange=16 },

	// Preset 11: 18.762db, --preset fast --subme 7
	{ .time= 5.357, .subme=7, .me=X264_ME_HEX, .refs=2, .mix=1, .trellis=1, .partitions=I8|I4|P8|B8, .badapt=1, .bframes=3, .direct=1, .merange=16 },

	// Preset 12: 18.819db, --preset medium
	{ .time= 6.040, .subme=7, .me=X264_ME_HEX, .refs=3, .mix=1, .trellis=1, .partitions=I8|I4|P8|B8, .badapt=1, .bframes=3, .direct=1, .merange=16 },

	// Preset 13: 18.889db, --preset medium --subme 8
	{ .time= 7.408, .subme=8, .me=X264_ME_HEX, .refs=3, .mix=1, .trellis=1, .partitions=I8|I4|P8|B8, .badapt=1, .bframes=3, .direct=1, .merange=16 },

	// Preset 14: 19.127db, --preset medium --subme 8 --trellis 2
	{ .time=10.124, .subme=8, .me=X264_ME_HEX, .refs=3, .mix=1, .trellis=2, .partitions=I8|I4|P8|B8, .badapt=1, .bframes=3, .direct=1, .merange=16 },

	// Preset 15: 19.118db, --preset medium --subme 8 --trellis 2 --direct auto
	{ .time=10.144, .subme=8, .me=X264_ME_HEX, .refs=3, .mix=1, .trellis=2, .partitions=I8|I4|P8|B8, .badapt=1, .bframes=3, .direct=3, .merange=16 },

	// Preset 16: 19.172db, --preset slow
	{ .time=11.142, .subme=8, .me=X264_ME_HEX, .refs=5, .mix=1, .trellis=2, .partitions=I8|I4|P8|B8, .badapt=1, .bframes=3, .direct=3, .merange=16 },

	// Preset 17: 19.309db, --preset slow --b-adapt 2 --subme 9
	{ .time=11.168, .subme=9, .me=X264_ME_HEX, .refs=5, .mix=1, .trellis=2, .partitions=I8|I4|P8|B8, .badapt=2, .bframes=3, .direct=3, .merange=16 },

	// Preset 18: 19.316db, --preset slow --b-adapt 2 --subme 9 --me umh
	{ .time=12.942, .subme=9, .me=X264_ME_UMH, .refs=5, .mix=1, .trellis=2, .partitions=I8|I4|P8|B8, .badapt=2, .bframes=3, .direct=3, .merange=16 },

	// Preset 19: 19.342db, --preset slow --b-adapt 2 --subme 9 --me umh --ref 6
	{ .time=14.302, .subme=9, .me=X264_ME_UMH, .refs=6, .mix=1, .trellis=2, .partitions=I8|I4|P8|B8, .badapt=2, .bframes=3, .direct=3, .merange=16 },

	// Preset 20: 19.365db, --preset slow --b-adapt 2 --subme 9 --me umh --ref 7
	{ .time=15.554, .subme=9, .me=X264_ME_UMH, .refs=7, .mix=1, .trellis=2, .partitions=I8|I4|P8|B8, .badapt=2, .bframes=3, .direct=3, .merange=16 },

	// Preset 21: 19.396db, --preset slower
	{ .time=17.551, .subme=9, .me=X264_ME_UMH, .refs=8, .mix=1, .trellis=2, .partitions=I8|I4|P8|B8|P4, .badapt=2, .bframes=3, .direct=3, .merange=16 },

	// Preset 22: 19.491db, --preset slower --subme 10
	{ .time=21.321, .subme=10, .me=X264_ME_UMH, .refs=8, .mix=1, .trellis=2, .partitions=I8|I4|P8|B8|P4, .badapt=2, .bframes=3, .direct=3, .merange=16 },

	// Preset 23: 19.764db, --preset slower --subme 10 --bframes 8
	{ .time=23.200, .subme=10, .me=X264_ME_UMH, .refs=8, .mix=1, .trellis=2, .partitions=I8|I4|P8|B8|P4, .badapt=2, .bframes=8, .direct=3, .merange=16 },

	// Preset 24: 19.807db, --preset veryslow
	{ .time=36.922, .subme=10, .me=X264_ME_UMH, .refs=16, .mix=1, .trellis=2, .partitions=I8|I4|P8|B8|P4, .badapt=2, .bframes=8, .direct=3, .merange=24 },
#undef I4
#undef I8
#undef P4
#undef P8
#undef B8
};

void X264SpeedControl::before_frame(float new_buffer_fill, int new_buffer_size, float new_uspf)
{
	if (new_uspf > 0.0) {
		uspf = new_uspf;
	}
	if (new_buffer_size) {
		set_buffer_size(new_buffer_size);
	}
	buffer_fill = buffer_size * new_buffer_fill;
	metric_x264_speedcontrol_buffer_available_seconds = buffer_fill * 1e-6;

	steady_clock::time_point t;

	// update buffer state after encoding and outputting the previous frame(s)
	if (first) {
		t = timestamp = steady_clock::now();
		first = false;
	} else {
		t = steady_clock::now();
	}

	auto delta_t = t - timestamp;
	timestamp = t;

	// update the time predictor
	if (preset >= 0) {
		int cpu_time = duration_cast<microseconds>(cpu_time_last_frame).count();
		cplx_num *= cplx_decay;
		cplx_den *= cplx_decay;
		cplx_num += cpu_time / presets[preset].time;
		++cplx_den;

		stat.avg_preset += preset;
		++stat.den;
	}

	stat.min_buffer = min(buffer_fill, stat.min_buffer);
	stat.max_buffer = max(buffer_fill, stat.max_buffer);

	if (buffer_fill >= buffer_size) { // oops, cpu was idle
		// not really an error, but we'll warn for debugging purposes
		static int64_t idle_t = 0;
		static steady_clock::time_point print_interval;
		static bool first = false;
		idle_t += buffer_fill - buffer_size;
		if (first || duration<double>(t - print_interval).count() > 0.1) {
			//fprintf(stderr, "speedcontrol idle (%.6f sec)\n", idle_t/1e6);
			print_interval = t;
			idle_t = 0;
			first = false;
		}
		buffer_fill = buffer_size;
		metric_x264_speedcontrol_buffer_available_seconds = buffer_fill * 1e-6;
		++metric_x264_speedcontrol_idle_frames;
	} else if (buffer_fill <= 0) {  // oops, we're late
		// fprintf(stderr, "speedcontrol underflow (%.6f sec)\n", buffer_fill/1e6);
		++metric_x264_speedcontrol_late_frames;
	}

	{
		// Pick the preset that should return the buffer to 3/4-full within a time
		// specified by compensation_period.
		//
		// NOTE: This doesn't actually do that, at least assuming the same target is
		// chosen for every frame; exactly what it does is unclear to me. It seems
		// to consistently undershoot a bit, so it needs to be saved by the second
		// predictor below. However, fixing the formula seems to yield somewhat less
		// stable results in practice; in particular, once the buffer is half-full
		// or so, it would give us a negative target. Perhaps increasing
		// compensation_period would be a good idea, but initial (very brief) tests
		// did not yield good results.
		float target = uspf / f_speed
			* (buffer_fill + compensation_period)
			/ (buffer_size*3/4 + compensation_period);
		float cplx = cplx_num / cplx_den;
		float set, t0, t1;
		float filled = (float) buffer_fill / buffer_size;
		int i;
		t0 = presets[0].time * cplx;
		for (i = 1; ; i++) {
			t1 = presets[i].time * cplx;
			if (t1 >= target || i == SC_PRESETS - 1)
				break;
			t0 = t1;
		}
		// exponential interpolation between states
		set = i-1 + (log(target) - log(t0)) / (log(t1) - log(t0));
		set = max<float>(set, -5);
		set = min<float>(set, (SC_PRESETS-1) + 5);
		// Even if our time estimations in the SC_PRESETS array are off
		// this will push us towards our target fullness
		float s1 = set;
		set += (40 * (filled-0.75));
		float s2 = (40 * (filled-0.75));
		set = min<float>(max<float>(set, 0), SC_PRESETS - 1);
		apply_preset(dither_preset(set));

		if (global_flags.x264_speedcontrol_verbose) {
			static float cpu, wall, tgt, den;
			const float decay = 1-1/100.;
			cpu = cpu*decay + duration_cast<microseconds>(cpu_time_last_frame).count();
			wall = wall*decay + duration_cast<microseconds>(delta_t).count();
			tgt = tgt*decay + target;
			den = den*decay + 1;
			fprintf(stderr, "speed: %.2f+%.2f %d[%.5f] (t/c/w: %6.0f/%6.0f/%6.0f = %.4f) fps=%.2f\r",
					s1, s2, preset, (float)buffer_fill / buffer_size,
					tgt/den, cpu/den, wall/den, cpu/wall, 1e6*den/wall );
		}
	}

}

void X264SpeedControl::after_frame()
{
	cpu_time_last_frame = steady_clock::now() - timestamp;
}

void X264SpeedControl::set_buffer_size(int new_buffer_size)
{
	new_buffer_size = max(3, new_buffer_size);
	buffer_size = new_buffer_size * uspf;
	cplx_decay = 1 - 1./new_buffer_size;
	compensation_period = buffer_size/4;
	metric_x264_speedcontrol_buffer_size_seconds = buffer_size * 1e-6;
}

int X264SpeedControl::dither_preset(float f)
{
	int i = f;
	if (f < 0) {
		i--;
	}
	dither += f - i;
	if (dither >= 1.0) {
		dither--;
		i++;
	}
	return i;
}

void X264SpeedControl::apply_preset(int new_preset)
{
	new_preset = max(new_preset, 0);
	new_preset = min(new_preset, SC_PRESETS - 1);

	const sc_preset_t *s = &presets[new_preset];
	x264_param_t p;
	dyn.x264_encoder_parameters(x264, &p);

	p.i_frame_reference = s->refs;
	p.i_bframe_adaptive = s->badapt;
	p.i_bframe = s->bframes;
	p.analyse.inter = s->partitions;
	p.analyse.i_subpel_refine = s->subme;
	p.analyse.i_me_method = s->me;
	p.analyse.i_trellis = s->trellis;
	p.analyse.b_mixed_references = s->mix;
	p.analyse.i_direct_mv_pred = s->direct;
	p.analyse.i_me_range = s->merange;
	if (override_func) {
		override_func(&p);
	}
	dyn.x264_encoder_reconfig(x264, &p);
	preset = new_preset;

	metric_x264_speedcontrol_preset_used_frames.count_event(new_preset);
}
