// Filter class:
// a cascaded biquad IIR filter
//
// Special cases for type=LPF/BPF/HPF:
//
//   Butterworth filter:    order=1, resonance=1/sqrt(2)
//   Linkwitz-Riley filter: order=2, resonance=1/2

#ifndef _FILTER_H
#define _FILTER_H 1

#define _USE_MATH_DEFINES
#include <cmath>
#include <complex>

#ifdef __SSE__
#include <xmmintrin.h>
#endif

enum FilterType
{
	FILTER_NONE = 0,
	FILTER_LPF,
	FILTER_HPF,
	FILTER_BPF,
	FILTER_NOTCH,
	FILTER_APF,

	// EQ filters.
	FILTER_PEAKING_EQ,
	FILTER_LOW_SHELF,
	FILTER_HIGH_SHELF,
};

#define FILTER_MAX_ORDER 4

class Filter  
{
	friend class StereoFilter;
	friend class SplittingStereoFilter;
public:
	Filter();
	
	void init(FilterType type, int new_order);

	void update(); //update coefficients
#ifndef NDEBUG
	void debug();
#endif
	std::complex<double> evaluate_transfer_function(float omega);

	FilterType get_type()			{ return filtertype; }
	unsigned get_order()			{ return filter_order; }

	// cutoff is taken to be in the [0..pi> (see set_linear_cutoff, below).
	void render(float *inout_array, unsigned int buf_size, float cutoff, float resonance);

	// Set cutoff, from [0..pi> (where pi is the Nyquist frequency).
	// Overridden by render() if you use that.
	void set_linear_cutoff(float new_omega)
	{
		omega = new_omega;
	}

	void set_resonance(float new_resonance)
	{
		resonance = new_resonance;
	}

	// For EQ filters only.
	void set_dbgain_normalized(float db_gain_div_40)
	{
		A = pow(10.0f, db_gain_div_40);
	}

#ifdef __SSE__
	// We don't need the stride argument for SSE, as StereoFilter
	// has its own SSE implementations.
	void render_chunk(float *inout_buf, unsigned nSamples);
#else
	void render_chunk(float *inout_buf, unsigned nSamples, unsigned stride = 1);
#endif

	FilterType filtertype;
private:
	float omega; //which is 2*Pi*frequency /SAMPLE_RATE
	float resonance;
	float A;  // which is 10^(db_gain / 40)

public:
	unsigned filter_order;
private:
	float b0, b1, b2, a1, a2; //filter coefs

	struct FeedbackBuffer {
		float d0,d1; //feedback buffers
	} feedback[FILTER_MAX_ORDER];

	void calcSinCos(float omega, float *sinVal, float *cosVal)
	{
		*sinVal = (float)sin(omega);
		*cosVal = (float)cos(omega);
	}
};


class StereoFilter
{
public:
	void init(FilterType type, int new_order);
	
	void render(float *inout_left_ptr, unsigned n_samples, float cutoff, float resonance, float dbgain_normalized = 0.0f);
#ifndef NDEBUG
#ifdef __SSE__
	void debug() { parm_filter.debug(); }
#else
	void debug() { filters[0].debug(); }
#endif
#endif
#ifdef __SSE__
	FilterType get_type() { return parm_filter.get_type(); }
#else
	FilterType get_type() { return filters[0].get_type(); }
#endif

private:
#ifdef __SSE__
	// We only use the filter to calculate coefficients; we don't actually
	// use its feedbacks.
	Filter parm_filter;
	struct SIMDFeedbackBuffer {
	        __m128 d0, d1;
	} feedback[FILTER_MAX_ORDER];
#else
	Filter filters[2];
#endif
};

#endif // !defined(_FILTER_H)
