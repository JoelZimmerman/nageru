#ifndef _NONLINEAR_FADER_H
#define _NONLINEAR_FADER_H 1

#include <QAbstractSlider>
#include <QSlider>
#include <QString>

class QObject;
class QPaintEvent;
class QWidget;

class NonLinearFader : public QSlider {
	Q_OBJECT

public:
	NonLinearFader(QWidget *parent);
	void setDbValue(double db);

signals:
	void dbValueChanged(double db);

protected:
	void paintEvent(QPaintEvent *event) override;
	void sliderChange(SliderChange change) override;

private:
	void update_slider_position();

	bool inhibit_updates = false;
	double db_value = 0.0;
};

#endif  // !defined(_NONLINEAR_FADER_H)
