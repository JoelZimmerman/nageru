#include "stereocompressor.h"

#include <assert.h>
#include <algorithm>
#include <cmath>

using namespace std;

namespace {

// Implement a less accurate but faster pow(x, y). We use the standard identity
//
//    x^y = exp(y * ln(x))
//
// with the ranges:
//
//    x in 1..(1/threshold)
//    y in -1..0
//
// Assume threshold goes from 0 to -40 dB. That means 1/threshold = 100,
// so input to ln(x) can be 1..100. Worst case for end accuracy is y=-1.
// To get a good minimax approximation (not the least wrt. continuity
// at x=1), I had to make a piecewise linear function for the two ranges:
//
//   with(numapprox):
//   f1 := minimax(ln, 1..6, [3, 3], x -> 1/x, 'maxerror');
//   f2 := minimax(ln, 6..100, [3, 3], x -> 1/x, 'maxerror');
//   f := x -> piecewise(x < 6, f1(x), f2(x));
//
// (Continuity: Error is down to the 1e-6 range for x=1, difference between
// f1 and f2 range at the crossover point is in the 1e-5 range. The cutoff
// point at x=6 is chosen to get maxerror pretty close between f1 and f2.)
//
// Maximum output of ln(x) here is of course ln(100) ~= 4.605. So we can find
// an approximation for exp over the range -4.605..0, where we care mostly
// about the relative error:
//
//   g := minimax(exp, -ln(100)..0, [3, 3], x -> 1/exp(x), 'maxerror');
//
// We can find the worst-case error in dB from this through a simple plot:
//
//   dbdiff := (x, y) -> abs(20 * log10(x / y));
//   plot(dbdiff(g(-f(x)), 1/x), x=1..100);
//
// which readily shows the error never to be above ~0.001 dB or so
// (actually 0.00119 dB, for the case of x=100). y=-1 remains the worst case,
// it would seem.
//
// If we cared even more about speed, we could probably fuse y into
// the coefficients for ln_nom and postgain into the coefficients for ln_den.
// But if so, we should probably rather just SIMD the entire thing instead.
inline float fastpow(float x, float y)
{
	float ln_nom, ln_den;
	if (x < 6.0f) {
		ln_nom = -0.059237648f + (-0.0165117771f + (0.06818859075f + 0.007560968243f * x) * x) * x;
		ln_den = 0.0202509098f + (0.08419174188f + (0.03647189417f + 0.001642577975f * x) * x) * x;
	} else {
		ln_nom = -0.005430534f + (0.00633589178f + (0.0006319155549f + 0.4789541675e-5f * x) * x) * x;
		ln_den = 0.0064785099f + (0.003219629109f + (0.0001531823694f + 0.6884656640e-6f * x) * x) * x;
	}
	float v = y * ln_nom / ln_den;
	float exp_nom = 0.2195097621f + (0.08546059868f + (0.01208501759f + 0.0006173448113f * v) * v) * v;
	float exp_den = 0.2194980791f + (-0.1343051968f + (0.03556072737f - 0.006174398513f * v) * v) * v;
	return exp_nom / exp_den;
}

inline float compressor_knee(float x, float threshold, float inv_threshold, float inv_ratio_minus_one, float postgain)
{
	assert(inv_ratio_minus_one <= 0.0f);
	if (x > threshold) {
		return postgain * fastpow(x * inv_threshold, inv_ratio_minus_one);
	} else {
		return postgain;
	}
}

}  // namespace

void StereoCompressor::process(float *buf, size_t num_samples, float threshold, float ratio,
	    float attack_time, float release_time, float makeup_gain)
{
	float attack_increment = float(pow(2.0f, 1.0f / (attack_time * sample_rate + 1)));
	if (attack_time == 0.0f) attack_increment = 100000;  // For instant attack reaction.

	const float release_increment = float(pow(2.0f, -1.0f / (release_time * sample_rate + 1)));
	const float peak_increment = float(pow(2.0f, -1.0f / (0.003f * sample_rate + 1)));

	float inv_ratio_minus_one = 1.0f / ratio - 1.0f;
	if (ratio > 63) inv_ratio_minus_one = -1.0f;  // Infinite ratio.
	float inv_threshold = 1.0f / threshold;

	float *left_ptr = buf;
	float *right_ptr = buf + 1;

	if (inv_ratio_minus_one >= 0.0) {
		for (size_t i = 0; i < num_samples; ++i) {
			*left_ptr *= makeup_gain;
			left_ptr += 2;

			*right_ptr *= makeup_gain;
			right_ptr += 2;
		}
		return;
	}

	float peak_level = this->peak_level;
	float compr_level = this->compr_level;

	for (size_t i = 0; i < num_samples; ++i) {
		if (fabs(*left_ptr) > peak_level) peak_level = float(fabs(*left_ptr));
		if (fabs(*right_ptr) > peak_level) peak_level = float(fabs(*right_ptr));

		if (peak_level > compr_level) {
			compr_level = min(compr_level * attack_increment, peak_level);
		} else {
			compr_level = max(compr_level * release_increment, 0.0001f);
		}

		float scalefactor_with_gain = compressor_knee(compr_level, threshold, inv_threshold, inv_ratio_minus_one, makeup_gain);

		*left_ptr *= scalefactor_with_gain;
		left_ptr += 2;

		*right_ptr *= scalefactor_with_gain;
		right_ptr += 2;

		peak_level = max(peak_level * peak_increment, 0.0001f);
	}

	// Store attenuation level for debug/visualization.
	scalefactor = compressor_knee(compr_level, threshold, inv_threshold, inv_ratio_minus_one, 1.0f);

	this->peak_level = peak_level;
	this->compr_level = compr_level;
}

