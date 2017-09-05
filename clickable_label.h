#ifndef _CLICKABLE_LABEL_H
#define _CLICKABLE_LABEL_H 1

// Just like a normal QLabel, except that it can also emit a clicked signal.

#include <QLabel>

class QMouseEvent;

class ClickableLabel : public QLabel {
	Q_OBJECT

public:
	ClickableLabel(QWidget *parent) : QLabel(parent) {}

signals:
	void clicked();

protected:
	void mousePressEvent(QMouseEvent *event) override
	{
		emit clicked();
	}
};

#endif  // !defined(_CLICKABLE_LABEL_H)
