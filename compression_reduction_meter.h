#ifndef COMPRESSION_REDUCTION_METER_H
#define COMPRESSION_REDUCTION_METER_H

// A meter that goes downwards instead of upwards, and has a non-linear scale.

#include <math.h>
#include <QPixmap>
#include <QString>
#include <QWidget>
#include <mutex>

#include "piecewise_interpolator.h"

class QObject;
class QPaintEvent;
class QResizeEvent;

class CompressionReductionMeter : public QWidget
{
	Q_OBJECT

public:
	CompressionReductionMeter(QWidget *parent);

	void set_reduction_db(float level_db) {
		std::unique_lock<std::mutex> lock(level_mutex);
		this->level_db = level_db;
		QMetaObject::invokeMethod(this, "update", Qt::AutoConnection);
	}

private:
	void resizeEvent(QResizeEvent *event) override;
	void paintEvent(QPaintEvent *event) override;
	void recalculate_pixmaps();
	void draw_scale(QPainter *painter, int x_pos);
	double db_to_pos(double db) const;
	int meter_height() const;

	std::mutex level_mutex;
	float level_db = 0.0f;

	static constexpr float min_level = 0.0f;  // Must match control_points (in the .cpp file).
	static constexpr float max_level = 6.0f;  // Same.
	static constexpr int meter_width = 20;

	// Size of the text box. The meter will be shrunk to make room for the text box
	// (half the height) on both sides.
	static constexpr int text_box_width = 15;
	static constexpr int text_box_height = 10;

	QPixmap on_pixmap, off_pixmap;
};

#endif
