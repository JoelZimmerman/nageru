#ifndef _ELLIPSIS_LABEL_H
#define _ELLIPSIS_LABEL_H 1

#include <QLabel>

class EllipsisLabel : public QLabel {
	Q_OBJECT

public:
	EllipsisLabel(QWidget *parent) : QLabel(parent) {}

	void setFullText(const QString &s)
	{
		full_text = s;
		updateEllipsisText();
	}

protected:
	void resizeEvent(QResizeEvent *event) override
	{
		QLabel::resizeEvent(event);
		updateEllipsisText();
	}

private:
	void updateEllipsisText()
	{
		QFontMetrics metrics(this->font());
		this->setText(metrics.elidedText(full_text, Qt::ElideRight, this->width()));
	}

	QString full_text;
};

#endif  // !defined(_ELLIPSIS_LABEL_H)
