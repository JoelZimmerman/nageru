#ifndef CORRELATION_METER_H
#define CORRELATION_METER_H

#include <mutex>

#include <QPixmap>
#include <QString>
#include <QWidget>

class QObject;
class QPaintEvent;
class QResizeEvent;

class CorrelationMeter : public QWidget
{
	Q_OBJECT

public:
	CorrelationMeter(QWidget *parent);

	void set_correlation(float correlation) {
		std::unique_lock<std::mutex> lock(correlation_mutex);
		this->correlation = correlation;
		QMetaObject::invokeMethod(this, "update", Qt::AutoConnection);
	}

private:
	void resizeEvent(QResizeEvent *event) override;
	void paintEvent(QPaintEvent *event) override;

	std::mutex correlation_mutex;
	float correlation = 0.0f;

	QPixmap on_pixmap, off_pixmap;
};

#endif
