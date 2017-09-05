#include "piecewise_interpolator.h"

#include <assert.h>

double PiecewiseInterpolator::fraction_to_db(double db) const
{
	if (db >= control_points[0].fraction) {
		return control_points[0].db_value;
	}
	if (db <= control_points.back().fraction) {
		return control_points.back().db_value;
	}
	for (unsigned i = 1; i < control_points.size(); ++i) {
		const double x0 = control_points[i].fraction;
		const double x1 = control_points[i - 1].fraction;
		const double y0 = control_points[i].db_value;
		const double y1 = control_points[i - 1].db_value;
		if (db >= x0 && db <= x1) {
			const double t = (db - x0) / (x1 - x0);
			return y0 + t * (y1 - y0);
		}
	}
	assert(false);
}

double PiecewiseInterpolator::db_to_fraction(double x) const
{
	if (x >= control_points[0].db_value) {
		return control_points[0].fraction;
	}
	if (x <= control_points.back().db_value) {
		return control_points.back().fraction;
	}
	for (unsigned i = 1; i < control_points.size(); ++i) {
		const double x0 = control_points[i].db_value;
		const double x1 = control_points[i - 1].db_value;
		const double y0 = control_points[i].fraction;
		const double y1 = control_points[i - 1].fraction;
		if (x >= x0 && x <= x1) {
			const double t = (x - x0) / (x1 - x0);
			return y0 + t * (y1 - y0);
		}
	}
	assert(false);
}

