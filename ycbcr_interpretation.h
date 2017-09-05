#ifndef _YCBCR_INTERPRETATION_H
#define _YCBCR_INTERPRETATION_H 1

#include <movit/image_format.h>

struct YCbCrInterpretation {
	bool ycbcr_coefficients_auto = true;
	movit::YCbCrLumaCoefficients ycbcr_coefficients = movit::YCBCR_REC_709;
	bool full_range = false;
};

#endif  // !defined(_YCBCR_INTERPRETATION_H)
