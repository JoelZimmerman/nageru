#include "aboutdialog.h"

#include <QDialogButtonBox>

#include "ui_aboutdialog.h"

using namespace std;

AboutDialog::AboutDialog()
	: ui(new Ui::AboutDialog)
{
	ui->setupUi(this);

	connect(ui->button_box, &QDialogButtonBox::accepted, [this]{ this->close(); });
}

