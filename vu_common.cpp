#include "vu_common.h"

#include <QColor>
#include <QPainter>
#include <algorithm>
#include <cmath>

using namespace std;

double lufs_to_pos(float level_lu, int height, float min_level, float max_level)
{
	// Note: “max” is the loudest level, but y=0 is top of screen.

	// Handle -inf.
	if (level_lu < min_level) {
		return height - 1;
	}

	double y = height * (level_lu - max_level) / (min_level - max_level);
	y = max<double>(y, 0);
	y = min<double>(y, height - 1);

	// If we are big enough, snap to pixel grid instead of antialiasing
	// the edges; the unevenness will be less noticeable than the blurriness.
	double height_per_level = height / (max_level - min_level) - 2.0;
	if (height_per_level >= 10.0) {
		y = round(y);
	}

	return y;
}

void draw_vu_meter(QPainter &painter, int width, int height, int horizontal_margin, double segment_margin, bool is_on, float min_level, float max_level, bool flip, int y_offset)
{
	painter.fillRect(horizontal_margin, y_offset, width - 2 * horizontal_margin, height, Qt::black);

	for (int y = 0; y < height; ++y) {
		// Find coverage of “on” rectangles in this pixel row.
		double coverage = 0.0;
		for (int level = floor(min_level); level <= ceil(max_level); ++level) {
			double min_y = lufs_to_pos(level + 1.0, height, min_level, max_level) + segment_margin * 0.5;
			double max_y = lufs_to_pos(level, height, min_level, max_level) - segment_margin * 0.5;
			min_y = std::max<double>(min_y, y);
			min_y = std::min<double>(min_y, y + 1);
			max_y = std::max<double>(max_y, y);
			max_y = std::min<double>(max_y, y + 1);
			coverage += max_y - min_y;
		}

		double on_r, on_g, on_b;
		if (is_on) {
			double t = double(y) / height;
			if (t <= 0.5) {
				on_r = 1.0;
				on_g = 2.0 * t;
				on_b = 0.0;
			} else {
				on_r = 1.0 - 2.0 * (t - 0.5);
				on_g = 1.0;
				on_b = 0.0;
			}
		} else {
			on_r = on_g = on_b = 0.05;
		}

		// Correct for coverage and do a simple gamma correction.
		int r = lrintf(255 * pow(on_r * coverage, 1.0 / 2.2));
		int g = lrintf(255 * pow(on_g * coverage, 1.0 / 2.2));
		int b = lrintf(255 * pow(on_b * coverage, 1.0 / 2.2));
		int draw_y = flip ? (height - y - 1) : y;
		painter.fillRect(horizontal_margin, draw_y + y_offset, width - 2 * horizontal_margin, 1, QColor(r, g, b));
	}
}
