#include "nonlinear_fader.h"

#include <assert.h>
#include <math.h>
#include <QPainter>
#include <QPoint>
#include <QRect>
#include <QStyle>
#include <QStyleOption>
#include <memory>
#include <utility>
#include <vector>

#include "piecewise_interpolator.h"

class QPaintEvent;
class QWidget;

using namespace std;

namespace {

PiecewiseInterpolator interpolator({
	// The main area is from +6 to -12 dB (18 dB), and we use half the slider range for it.
	// Adjust slightly so that the MIDI controller value of 106 becomes exactly 0.0 dB
	// (cf. map_controller_to_float()); otherwise, we'd miss ever so slightly, which is
	// really frustrating.
	{ 6.0, 1.0 },
	{ -12.0, 1.0 - (1.0 - 106.5/127.0) * 3.0 },  // About 0.492.

	// -12 to -21 is half the range (9 dB). Halve.
	{ -21.0, 0.325 },

	// -21 to -30 (9 dB) gets the same range as the previous one.
	{ -30.0, 0.25 },

	// -30 to -48 (18 dB) gets half of half.
	{ -48.0, 0.125 },

	// -48 to -84 (36 dB) gets half of half of half.
	{ -84.0, 0.0 },
});

}  // namespace

NonLinearFader::NonLinearFader(QWidget *parent)
	: QSlider(parent)
{
	update_slider_position();
}

void NonLinearFader::setDbValue(double db)
{
	db_value = db;
	update_slider_position();
	emit dbValueChanged(db);
}

void NonLinearFader::paintEvent(QPaintEvent *event)
{
	QStyleOptionSlider opt;
	this->initStyleOption(&opt);
	QRect gr = this->style()->subControlRect(QStyle::CC_Slider, &opt, QStyle::SC_SliderGroove, this);
	QRect sr = this->style()->subControlRect(QStyle::CC_Slider, &opt, QStyle::SC_SliderHandle, this);

	// FIXME: Where does the slider_length / 2 come from? I can't really find it
	// in the Qt code, but it seems to match up with reality.
	int slider_length = sr.height();
	int slider_max = gr.top() + (slider_length / 2);
	int slider_min = gr.bottom() + (slider_length / 2) - slider_length + 1;

	QPainter p(this);

	// Draw some ticks every 6 dB.
	// FIXME: Find a way to make the slider wider, so that we have more space for tickmarks
	// and some dB numbering.
	int x_margin = 5;
	p.setPen(Qt::darkGray);
	for (int db = -84; db <= 6; db += 6) {
		int y = slider_min + lrint(interpolator.db_to_fraction(db) * (slider_max - slider_min));
		p.drawLine(QPoint(0, y), QPoint(gr.left() - x_margin, y));
		p.drawLine(QPoint(gr.right() + x_margin, y), QPoint(width() - 1, y));
	}

	QSlider::paintEvent(event);
}

void NonLinearFader::sliderChange(SliderChange change)
{
	QSlider::sliderChange(change);
	if (change == QAbstractSlider::SliderValueChange && !inhibit_updates) {
		if (value() == 0) {
			db_value = -HUGE_VAL;
		} else {
			double frac = double(value() - minimum()) / (maximum() - minimum());
			db_value = interpolator.fraction_to_db(frac);
		}
		emit dbValueChanged(db_value);
	}
}

void NonLinearFader::update_slider_position()
{
	inhibit_updates = true;
	double val = interpolator.db_to_fraction(db_value) * (maximum() - minimum()) + minimum();
	setValue(lrint(val));
	inhibit_updates = false;
}
