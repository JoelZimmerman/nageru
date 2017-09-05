#ifndef _ABOUTDIALOG_H
#define _ABOUTDIALOG_H 1

#include <QDialog>
#include <QString>

class QObject;

namespace Ui {
class AboutDialog;
}  // namespace Ui

class AboutDialog : public QDialog
{
	Q_OBJECT

public:
	AboutDialog();

private:
	Ui::AboutDialog *ui;
};

#endif  // !defined(_ABOUTDIALOG_H)
