#include "compression_reduction_meter.h"

#include <QPainter>
#include <QRect>
#include "piecewise_interpolator.h"
#include "vu_common.h"

class QPaintEvent;
class QResizeEvent;

using namespace std;

namespace {

vector<PiecewiseInterpolator::ControlPoint> control_points = {
	{ 60.0f, 6.0f },
	{ 30.0f, 5.0f },
	{ 18.0f, 4.0f },
	{ 12.0f, 3.0f },
	{ 6.0f, 2.0f },
	{ 3.0f, 1.0f },
	{ 0.0f, 0.0f }
};
PiecewiseInterpolator interpolator(control_points);

}  // namespace

CompressionReductionMeter::CompressionReductionMeter(QWidget *parent)
	: QWidget(parent)
{
}

void CompressionReductionMeter::resizeEvent(QResizeEvent *event)
{
	recalculate_pixmaps();
}

void CompressionReductionMeter::paintEvent(QPaintEvent *event)
{
	QPainter painter(this);

	float level_db;
	{
		unique_lock<mutex> lock(level_mutex);
		level_db = this->level_db;
	}

	int on_pos = lrint(db_to_pos(level_db));

	QRect on_rect(0, 0, width(), on_pos);
	QRect off_rect(0, on_pos, width(), height());

	painter.drawPixmap(on_rect, on_pixmap, on_rect);
	painter.drawPixmap(off_rect, off_pixmap, off_rect);
}

void CompressionReductionMeter::recalculate_pixmaps()
{
	constexpr int y_offset = text_box_height / 2;
	constexpr int text_margin = 5;
	float margin = 0.5 * (width() - meter_width);

	on_pixmap = QPixmap(width(), height());
	QPainter on_painter(&on_pixmap);
	on_painter.fillRect(0, 0, width(), height(), parentWidget()->palette().window());
	draw_vu_meter(on_painter, width(), meter_height(), margin, 2.0, true, min_level, max_level, /*flip=*/true, y_offset);
	draw_scale(&on_painter, 0.5 * width() + 0.5 * meter_width + text_margin);

	off_pixmap = QPixmap(width(), height());
	QPainter off_painter(&off_pixmap);
	off_painter.fillRect(0, 0, width(), height(), parentWidget()->palette().window());
	draw_vu_meter(off_painter, width(), meter_height(), margin, 2.0, false, min_level, max_level, /*flip=*/true, y_offset);
	draw_scale(&off_painter, 0.5 * width() + 0.5 * meter_width + text_margin);
}

void CompressionReductionMeter::draw_scale(QPainter *painter, int x_pos)
{
	QFont font;
	font.setPointSize(8);
	painter->setPen(Qt::black);
	painter->setFont(font);
	for (size_t i = 0; i < control_points.size(); ++i) {
		char buf[256];
		snprintf(buf, 256, "%.0f", control_points[i].db_value);
		double y = db_to_pos(control_points[i].db_value);
		painter->drawText(QRect(x_pos, y - text_box_height / 2, text_box_width, text_box_height),
			Qt::AlignCenter | Qt::AlignVCenter, buf);
	}
}

double CompressionReductionMeter::db_to_pos(double level_db) const
{
	float value = interpolator.db_to_fraction(level_db);
	return height() - lufs_to_pos(value, meter_height(), min_level, max_level) - text_box_height / 2;
}

int CompressionReductionMeter::meter_height() const
{
	return height() - text_box_height;
}
