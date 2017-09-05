#include "correlation_meter.h"

#include <math.h>
#include <algorithm>

#include <QBrush>
#include <QColor>
#include <QPainter>
#include <QRect>

class QPaintEvent;
class QResizeEvent;

using namespace std;

CorrelationMeter::CorrelationMeter(QWidget *parent)
	: QWidget(parent)
{
}

void CorrelationMeter::resizeEvent(QResizeEvent *event)
{
	on_pixmap = QPixmap(width(), height());
	QPainter on_painter(&on_pixmap);
	QLinearGradient on(0, 0, width(), 0);
	on.setColorAt(0.0f, QColor(255, 0, 0));
	on.setColorAt(0.5f, QColor(255, 255, 0));
	on.setColorAt(0.8f, QColor(0, 255, 0));
	on.setColorAt(0.95f, QColor(255, 255, 0));
	on_painter.fillRect(0, 0, width(), height(), Qt::black);
	on_painter.fillRect(1, 1, width() - 2, height() - 2, on);

	off_pixmap = QPixmap(width(), height());
	QPainter off_painter(&off_pixmap);
	QLinearGradient off(0, 0, width(), 0);
	off.setColorAt(0.0f, QColor(127, 0, 0));
	off.setColorAt(0.5f, QColor(127, 127, 0));
	off.setColorAt(0.8f, QColor(0, 127, 0));
	off.setColorAt(0.95f, QColor(127, 127, 0));
	off_painter.fillRect(0, 0, width(), height(), Qt::black);
	off_painter.fillRect(1, 1, width() - 2, height() - 2, off);
}

void CorrelationMeter::paintEvent(QPaintEvent *event)
{
	QPainter painter(this);

	float correlation;
	{
		unique_lock<mutex> lock(correlation_mutex);
		correlation = this->correlation;
	}

	// Just in case.
	correlation = std::min(std::max(correlation, -1.0f), 1.0f);

	int pos = 3 + lrintf(0.5f * (correlation + 1.0f) * (width() - 6));
	QRect off_rect(0, 0, width(), height());
	QRect on_rect(pos - 2, 0, 5, height());

	painter.drawPixmap(off_rect, off_pixmap, off_rect);
	painter.drawPixmap(on_rect, on_pixmap, on_rect);
}
