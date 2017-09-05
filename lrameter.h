#ifndef LRAMETER_H
#define LRAMETER_H

#include <math.h>
#include <QPixmap>
#include <QString>
#include <QWidget>
#include <mutex>

#include "vu_common.h"

class QObject;
class QPaintEvent;
class QResizeEvent;

class LRAMeter : public QWidget
{
	Q_OBJECT

public:
	LRAMeter(QWidget *parent);

	void set_levels(float level_lufs, float range_low_lufs, float range_high_lufs) {
		std::unique_lock<std::mutex> lock(level_mutex);
		this->level_lufs = level_lufs;
		this->range_low_lufs = range_low_lufs;
		this->range_high_lufs = range_high_lufs;
		QMetaObject::invokeMethod(this, "update", Qt::AutoConnection);
	}

	double lufs_to_pos(float level_lu, int height)
	{
		return ::lufs_to_pos(level_lu, height, min_level, max_level);
	}

	void set_min_level(float min_level)
	{
		this->min_level = min_level;
		recalculate_pixmaps();
	}

	void set_max_level(float max_level)
	{
		this->max_level = max_level;
		recalculate_pixmaps();
	}

	void set_ref_level(float ref_level_lufs)
	{
		this->ref_level_lufs = ref_level_lufs;
	}

private:
	void resizeEvent(QResizeEvent *event) override;
	void paintEvent(QPaintEvent *event) override;
	void recalculate_pixmaps();

	std::mutex level_mutex;
	float level_lufs = -HUGE_VAL;
	float range_low_lufs = -HUGE_VAL;
	float range_high_lufs = -HUGE_VAL;
	float min_level = -18.0f, max_level = 9.0f, ref_level_lufs = -23.0f;

	QPixmap on_pixmap, off_pixmap;
};

#endif
