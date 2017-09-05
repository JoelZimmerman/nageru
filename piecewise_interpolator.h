#ifndef _PIECEWISE_INTERPOLATOR_H
#define _PIECEWISE_INTERPOLATOR_H

// A class to do piecewise linear interpolation of one scale to another
// (and back). Typically used to implement nonlinear dB mappings for sliders
// or meters, thus the nomenclature.

#include <vector>

class PiecewiseInterpolator {
public:
	// Both dB and fraction values must go from high to low.
	struct ControlPoint {
		double db_value;
		double fraction;
	};
	PiecewiseInterpolator(const std::vector<ControlPoint> &control_points)
		: control_points(control_points) {}

	double fraction_to_db(double db) const;
	double db_to_fraction(double x) const;

private:
	const std::vector<ControlPoint> control_points;
};

#endif  // !defined(_PIECEWISE_INTERPOLATOR_H)
