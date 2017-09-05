#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <algorithm>
#include <complex>

#include "defs.h"

#ifdef __SSE__
#include <mmintrin.h>
#endif

#include "filter.h"

using namespace std;

#ifdef __SSE__

// For SSE, we set the denormals-as-zero flag instead.
#define early_undenormalise(sample) 

#else  // !defined(__SSE__)

union uint_float {
        float f;
        unsigned int i;
};
#define early_undenormalise(sample) { \
	 uint_float uf; \
	 uf.f = float(sample); \
	 if ((uf.i&0x60000000)==0) sample=0.0f; \
}

#endif  // !_defined(__SSE__)

Filter::Filter()
{
	omega = M_PI;
	resonance = 0.01f;
	A = 1.0f;

	init(FILTER_NONE, 1);
	update();
}

void Filter::update()
{
	/*
	  uses coefficients grabbed from
	  RBJs audio eq cookbook:
	  http://www.musicdsp.org/files/Audio-EQ-Cookbook.txt
	*/

	float sn, cs;
	float cutoff_freq = omega;
	cutoff_freq = min(cutoff_freq, (float)M_PI);
	cutoff_freq = max(cutoff_freq, 0.001f);
	calcSinCos(cutoff_freq, &sn, &cs);
	if (resonance <= 0) resonance = 0.001f;

#ifdef __GNUC__
	// Faster version of real_resonance = resonance ^ (1 / order).
	// pow(), at least on current GCC, is pretty slow.
	float real_resonance = resonance;
	switch (filter_order) {
	case 0:
	case 1:
		break;
	case 4:
		real_resonance = sqrt(real_resonance);
		// Fall through.
	case 2:
		real_resonance = sqrt(real_resonance);
		break;
	case 3:
		real_resonance = cbrt(real_resonance);
		break;
	default:
		assert(false);
	}
#else
	float real_resonance = pow(resonance, 1.0f / filter_order);
#endif

	float alpha = float(sn / (2 * real_resonance));
	float a0 = 1 + alpha;
	a1 = -2 * cs;
	a2 = 1 - alpha;
	
	switch (filtertype) {
	case FILTER_NONE:
		a0 = b0 = 1.0f;
		a1 = a2 = b1 = b2 = 0.0; //identity filter
		break;

	case FILTER_LPF:
		b0 = (1 - cs) * 0.5f;
		b1 = 1 - cs;
		b2 = b0;
		// a1 = -2*cs;
		// a2 = 1 - alpha;
		break;

	case FILTER_HPF:
		b0 = (1 + cs) * 0.5f;
		b1 = -(1 + cs);
		b2 = b0;
		// a1 = -2*cs;
		// a2 = 1 - alpha;
		break;

	case FILTER_BPF:
		b0 = alpha;
		b1 = 0.0f;
		b2 = -alpha;
		// a1 = -2*cs;
		// a2 = 1 - alpha;
		break;

	case FILTER_NOTCH:
		b0 = 1.0f;
		b1 = -2*cs;
		b2 = 1.0f;
		// a1 = -2*cs;
		// a2 = 1 - alpha;
		break;

	case FILTER_APF:
		b0 = 1 - alpha;
		b1 = -2*cs;
		b2 = 1.0f;
		// a1 = -2*cs;
		// a2 = 1 - alpha;
		break;

	case FILTER_PEAKING_EQ:
		b0 = 1 + alpha * A;
		b1 = -2*cs;
		b2 = 1 - alpha * A;
		a0 = 1 + alpha / A;
		// a1 = -2*cs;
		a2 = 1 - alpha / A;
		break;

	case FILTER_LOW_SHELF:
		b0 =      A * ((A + 1) - (A - 1)*cs + 2 * sqrt(A) * alpha);
		b1 =  2 * A * ((A - 1) - (A + 1)*cs                      );
		b2 =      A * ((A + 1) - (A - 1)*cs - 2 * sqrt(A) * alpha);
		a0 =           (A + 1) + (A - 1)*cs + 2 * sqrt(A) * alpha ;
		a1 =     -2 * ((A - 1) + (A + 1)*cs                      );
		a2 =           (A + 1) + (A - 1)*cs - 2 * sqrt(A) * alpha ;
		break;

	case FILTER_HIGH_SHELF:
		b0 =      A * ((A + 1) + (A - 1)*cs + 2 * sqrt(A) * alpha);
		b1 = -2 * A * ((A - 1) + (A + 1)*cs                      );
		b2 =      A * ((A + 1) + (A - 1)*cs - 2 * sqrt(A) * alpha);
		a0 =           (A + 1) - (A - 1)*cs + 2 * sqrt(A) * alpha ;
		a1 =      2 * ((A - 1) - (A + 1)*cs                      );
		a2 =           (A + 1) - (A - 1)*cs - 2 * sqrt(A) * alpha ;
		break;

	default:
		//unknown filter type
		assert(false);
		break;
	}

	const float invA0 = 1.0f / a0;
	b0 *= invA0;
	b1 *= invA0;
	b2 *= invA0;
	a1 *= invA0;
	a2 *= invA0;
}

#ifndef NDEBUG
void Filter::debug()
{
	// Feed this to gnuplot to get a graph of the frequency response.
	const float Fs2 = OUTPUT_FREQUENCY * 0.5f;
	printf("set xrange [2:%f]; ", Fs2);
	printf("set yrange [-80:20]; ");
	printf("set log x; ");
	printf("phasor(x) = cos(x*pi/%f)*{1,0} + sin(x*pi/%f)*{0,1}; ", Fs2, Fs2);
	printf("tfunc(x, b0, b1, b2, a0, a1, a2) = (b0 * phasor(x)**2 + b1 * phasor(x) + b2) / (a0 * phasor(x)**2 + a1 * phasor(x) + a2); ");
	printf("db(x) = 20*log10(x); ");
	printf("plot db(abs(tfunc(x, %f, %f, %f, %f, %f, %f))) title \"\"\n", b0, b1, b2, 1.0f, a1, a2);
}
#endif

void Filter::init(FilterType type, int order)
{
	filtertype = type;
	filter_order = order;
	if (filtertype == FILTER_NONE) filter_order = 0;
	if (filter_order == 0) filtertype = FILTER_NONE;

	//reset feedback buffer
	for (unsigned i = 0; i < filter_order; i++) {
		feedback[i].d0 = feedback[i].d1 = 0.0f;
	}
}

#ifdef __SSE__
void Filter::render_chunk(float *inout_buf, unsigned int n_samples)
#else
void Filter::render_chunk(float *inout_buf, unsigned int n_samples, unsigned stride)
#endif
{
#ifdef __SSE__
	const unsigned stride = 1;
	unsigned old_denormals_mode = _MM_GET_FLUSH_ZERO_MODE();
	_MM_SET_FLUSH_ZERO_MODE(_MM_FLUSH_ZERO_ON);
#endif
	assert((n_samples & 3) == 0); // make sure n_samples is divisible by 4

	// Apply the filter FILTER_ORDER times.
	for (unsigned j = 0; j < filter_order; j++) {
		float d0 = feedback[j].d0;
		float d1 = feedback[j].d1;
		float *inout_ptr = inout_buf;

		// Render n_samples mono samples. Unrolling manually by a
		// factor four seemingly helps a lot, perhaps because it
		// lets the CPU overlap arithmetic and memory operations
		// better, or perhaps simply because the loop overhead is
		// high.
		for (unsigned i = n_samples >> 2; i; i--) {
			float in, out;

			in = *inout_ptr;
			out = b0*in + d0;
			*inout_ptr = out;
			d0 = b1*in - a1*out + d1;
			d1 = b2*in - a2*out;
			inout_ptr += stride;

			in = *inout_ptr;
			out = b0*in + d0;
			*inout_ptr = out;
			d0 = b1*in - a1*out + d1;
			d1 = b2*in - a2*out;
			inout_ptr += stride;

			in = *inout_ptr;
			out = b0*in + d0;
			*inout_ptr = out;
			d0 = b1*in - a1*out + d1;
			d1 = b2*in - a2*out;
			inout_ptr += stride;

			in = *inout_ptr;
			out = b0*in + d0;
			*inout_ptr = out;
			d0 = b1*in - a1*out + d1;
			d1 = b2*in - a2*out;
			inout_ptr += stride;
		}
		early_undenormalise(d0); //do denormalization step
		early_undenormalise(d1);
		feedback[j].d0 = d0;
		feedback[j].d1 = d1;
	}

#ifdef __SSE__
	_MM_SET_FLUSH_ZERO_MODE(old_denormals_mode);
#endif
}

void Filter::render(float *inout_buf, unsigned int buf_size, float cutoff, float resonance)
{
	//render buf_size mono samples
#ifdef __SSE__
	assert(buf_size % 4 == 0);
#endif
	if (filter_order == 0)
		return;

	this->set_linear_cutoff(cutoff);
	this->set_resonance(resonance);
	this->update();
	this->render_chunk(inout_buf, buf_size);
}

void StereoFilter::init(FilterType type, int new_order)
{
#ifdef __SSE__
	parm_filter.init(type, new_order);
	memset(feedback, 0, sizeof(feedback));
#else
	for (unsigned i = 0; i < 2; ++i) {
		filters[i].init(type, new_order);
	}
#endif
}

void StereoFilter::render(float *inout_left_ptr, unsigned n_samples, float cutoff, float resonance, float dbgain_normalized)
{
#ifdef __SSE__
	if (parm_filter.filtertype == FILTER_NONE || parm_filter.filter_order == 0)
		return;

	unsigned old_denormals_mode = _MM_GET_FLUSH_ZERO_MODE();
	_MM_SET_FLUSH_ZERO_MODE(_MM_FLUSH_ZERO_ON);

	parm_filter.set_linear_cutoff(cutoff);
	parm_filter.set_resonance(resonance);
	parm_filter.set_dbgain_normalized(dbgain_normalized);
	parm_filter.update();

	__m128 b0 = _mm_set1_ps(parm_filter.b0);
	__m128 b1 = _mm_set1_ps(parm_filter.b1);
	__m128 b2 = _mm_set1_ps(parm_filter.b2);
	__m128 a1 = _mm_set1_ps(parm_filter.a1);
	__m128 a2 = _mm_set1_ps(parm_filter.a2);

	// Apply the filter FILTER_ORDER times.
	for (unsigned j = 0; j < parm_filter.filter_order; j++) {
		__m128 d0 = feedback[j].d0;
		__m128 d1 = feedback[j].d1;
		__m64 *inout_ptr = (__m64 *)inout_left_ptr;

		__m128 in = _mm_set1_ps(0.0f), out;
		for (unsigned i = n_samples; i; i--) {
			in = _mm_loadl_pi(in, inout_ptr);
			out = _mm_add_ps(_mm_mul_ps(b0, in), d0);
			_mm_storel_pi(inout_ptr, out);
			d0 = _mm_add_ps(_mm_sub_ps(_mm_mul_ps(b1, in), _mm_mul_ps(a1, out)), d1);
			d1 = _mm_sub_ps(_mm_mul_ps(b2, in), _mm_mul_ps(a2, out));
			++inout_ptr;
		}
		feedback[j].d0 = d0;
		feedback[j].d1 = d1;
	}

	_MM_SET_FLUSH_ZERO_MODE(old_denormals_mode);
#else
	if (filters[0].filtertype == FILTER_NONE || filters[0].filter_order == 0)
		return;

	for (unsigned i = 0; i < 2; ++i) {
		filters[i].set_linear_cutoff(cutoff);
		filters[i].set_resonance(resonance);
		filters[i].update();
		filters[i].render_chunk(inout_left_ptr, n_samples, 2);

		++inout_left_ptr;
	}
#endif
}

/*

  Find the transfer function for an IIR biquad. This is relatively basic signal
  processing, but for completeness, here's the rationale for the function:

  The basic system of an IIR biquad looks like this, for input x[n], output y[n]
  and constant filter coefficients [ab][0-2]:

    a2 y[n-2] + a1 y[n-1] + a0 y[n] = b2 x[n-2] + b1 x[n-1] + b0 x[n]

  Taking the discrete Fourier transform (DFT) of both sides (denoting by convention
  DFT{x[n]} by X[w], where w is the angular frequency, going from 0 to 2pi), yields,
  due to the linearity and shift properties of the DFT:

    a2 e^2jw Y[w] + a1 e^jw Y[w] + a0 Y[w] = b2 e^2jw X[w] + b1 e^jw X[w] + b0 Y[w]

  Simple factorization and reorganization yields

    Y[w] / X[w] = (b1 e^2jw + b1 e^jw + b0) / (a2 e^2jw + a1 e^jw + a0)

  and Y[w] / X[w] is by definition the filter's _transfer function_
  (customarily denoted by H(w)), ie. the complex factor it applies to the
  frequency component w. The absolute value of the transfer function is
  the frequency response, ie. how much frequency w is boosted or weakened.

  (This derivation usually goes via the Z-transform and not the DFT, but the
  idea is exactly the same; the Z-transform is just a bit more general.)

  Sending a signal through first one filter and then through another one
  will naturally be equivalent to a filter with the transfer function equal
  to the pointwise multiplication of the two filters, so for N-order filters
  we need to raise the answer to the Nth power.

*/
complex<double> Filter::evaluate_transfer_function(float omega)
{
	complex<float> z = exp(complex<float>(0.0f, omega));
	complex<float> z2 = z * z;
	return pow((b0 * z2 + b1 * z + b2) / (1.0f * z2 + a1 * z + a2), filter_order);
}
