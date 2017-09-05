#include "vumeter.h"

#include <QPainter>
#include <QRect>
#include "vu_common.h"

class QPaintEvent;
class QResizeEvent;

using namespace std;

VUMeter::VUMeter(QWidget *parent)
	: QWidget(parent)
{
}

void VUMeter::resizeEvent(QResizeEvent *event)
{
	recalculate_pixmaps();
}

void VUMeter::paintEvent(QPaintEvent *event)
{
	QPainter painter(this);

	float level_lufs[2], peak_lufs[2];
	{
		unique_lock<mutex> lock(level_mutex);
		level_lufs[0] = this->level_lufs[0];
		level_lufs[1] = this->level_lufs[1];
		peak_lufs[0] = this->peak_lufs[0];
		peak_lufs[1] = this->peak_lufs[1];
	}

	int mid = width() / 2;

	for (unsigned channel = 0; channel < 2; ++channel) {
		int left = (channel == 0) ? 0 : mid;
		int right = (channel == 0) ? mid : width();
		float level_lu = level_lufs[channel] - ref_level_lufs;
		int on_pos = lrint(lufs_to_pos(level_lu, height()));

		QRect off_rect(left, 0, right - left, on_pos);
		QRect on_rect(left, on_pos, right - left, height() - on_pos);

		painter.drawPixmap(off_rect, off_pixmap, off_rect);
		painter.drawPixmap(on_rect, on_pixmap, on_rect);

		float peak_lu = peak_lufs[channel] - ref_level_lufs;
		if (peak_lu >= min_level && peak_lu <= max_level) {
			int peak_pos = lrint(lufs_to_pos(peak_lu, height()));
			QRect peak_rect(left, peak_pos - 1, right, 2);
			painter.drawPixmap(peak_rect, full_on_pixmap, peak_rect);
		}
	}
}

void VUMeter::recalculate_pixmaps()
{
	full_on_pixmap = QPixmap(width(), height());
	QPainter full_on_painter(&full_on_pixmap);
	full_on_painter.fillRect(0, 0, width(), height(), parentWidget()->palette().window());
	draw_vu_meter(full_on_painter, width(), height(), 0, 0.0, true, min_level, max_level, /*flip=*/false);

	float margin = 0.5 * (width() - 20);

	on_pixmap = QPixmap(width(), height());
	QPainter on_painter(&on_pixmap);
	on_painter.fillRect(0, 0, width(), height(), parentWidget()->palette().window());
	draw_vu_meter(on_painter, width(), height(), margin, 2.0, true, min_level, max_level, /*flip=*/false);

	off_pixmap = QPixmap(width(), height());
	QPainter off_painter(&off_pixmap);
	off_painter.fillRect(0, 0, width(), height(), parentWidget()->palette().window());
	draw_vu_meter(off_painter, width(), height(), margin, 2.0, false, min_level, max_level, /*flip=*/false);
}
