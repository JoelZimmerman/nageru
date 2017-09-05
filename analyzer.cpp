#include "analyzer.h"

#include <QDialogButtonBox>
#include <QMouseEvent>
#include <QPen>
#include <QSurface>
#include <QTimer>

#include <movit/resource_pool.h>
#include <movit/util.h>

#include "context.h"
#include "flags.h"
#include "mixer.h"
#include "ui_analyzer.h"

// QCustomPlot includes qopenglfunctions.h, which #undefs all of the epoxy
// definitions (ugh) and doesn't put back any others (ugh). Add the ones we
// need back.

#define glBindBuffer epoxy_glBindBuffer
#define glBindFramebuffer epoxy_glBindFramebuffer
#define glBufferData epoxy_glBufferData
#define glDeleteBuffers epoxy_glDeleteBuffers
#define glDisable epoxy_glDisable
#define glGenBuffers epoxy_glGenBuffers
#define glGetError epoxy_glGetError
#define glReadPixels epoxy_glReadPixels
#define glUnmapBuffer epoxy_glUnmapBuffer
#define glWaitSync epoxy_glWaitSync

using namespace std;

Analyzer::Analyzer()
	: ui(new Ui::Analyzer),
	  grabbed_image(global_flags.width, global_flags.height, QImage::Format_ARGB32_Premultiplied)
{
	ui->setupUi(this);

	surface = create_surface(QSurfaceFormat::defaultFormat());
	context = create_context(surface);
	if (!make_current(context, surface)) {
		printf("oops\n");
		exit(1);
	}

	grab_timer.setSingleShot(true);
	connect(&grab_timer, &QTimer::timeout, bind(&Analyzer::grab_clicked, this));

	ui->input_box->addItem("Live", Mixer::OUTPUT_LIVE);
	ui->input_box->addItem("Preview", Mixer::OUTPUT_PREVIEW);
	unsigned num_channels = global_mixer->get_num_channels();
	for (unsigned channel_idx = 0; channel_idx < num_channels; ++channel_idx) {
		Mixer::Output channel = static_cast<Mixer::Output>(Mixer::OUTPUT_INPUT0 + channel_idx);	
		string name = global_mixer->get_channel_name(channel);
		ui->input_box->addItem(QString::fromStdString(name), channel);
	}

	ui->grab_frequency_box->addItem("Never", 0);
	ui->grab_frequency_box->addItem("100 ms", 100);
	ui->grab_frequency_box->addItem("1 sec", 1000);
	ui->grab_frequency_box->addItem("10 sec", 10000);
	ui->grab_frequency_box->setCurrentIndex(2);

	connect(ui->grab_btn, &QPushButton::clicked, bind(&Analyzer::grab_clicked, this));
	connect(ui->input_box, static_cast<void(QComboBox::*)(int)>(&QComboBox::currentIndexChanged), bind(&Analyzer::signal_changed, this));
	signal_changed();
	ui->grabbed_frame_label->installEventFilter(this);

        glGenBuffers(1, &pbo);
        glBindBuffer(GL_PIXEL_PACK_BUFFER_ARB, pbo);
        glBufferData(GL_PIXEL_PACK_BUFFER_ARB, global_flags.width * global_flags.height * 4, nullptr, GL_STREAM_READ);

	ui->histogram->xAxis->setVisible(true);
	ui->histogram->yAxis->setVisible(false);
	ui->histogram->xAxis->setRange(0, 255);
}

Analyzer::~Analyzer()
{
	delete_context(context);
	delete surface;
}

void Analyzer::update_channel_name(Mixer::Output output, const string &name)
{
	if (output >= Mixer::OUTPUT_INPUT0) {
		int index = (output - Mixer::OUTPUT_INPUT0) + 2;
		ui->input_box->setItemText(index, QString::fromStdString(name));
	}
}

void Analyzer::mixer_shutting_down()
{
	ui->display->shutdown();

	if (!make_current(context, surface)) {
		printf("oops\n");
		exit(1);
	}
	glDeleteBuffers(1, &pbo);
	check_error();
	if (resource_pool != nullptr) {
		resource_pool->clean_context();
	}
}

void Analyzer::grab_clicked()
{
	Mixer::Output channel = static_cast<Mixer::Output>(ui->input_box->currentData().value<int>());

	if (!make_current(context, surface)) {
		printf("oops\n");
		exit(1);
	}

	Mixer::DisplayFrame frame;
	if (!global_mixer->get_display_frame(channel, &frame)) {
		// Not ready yet.
		return;
	}

	// Set up an FBO to render into.
	if (resource_pool == nullptr) {
		resource_pool = frame.chain->get_resource_pool();
	} else {
		assert(resource_pool == frame.chain->get_resource_pool());
	}
	GLuint fbo_tex = resource_pool->create_2d_texture(GL_RGBA8, global_flags.width, global_flags.height);
	check_error();
	GLuint fbo = resource_pool->create_fbo(fbo_tex);
	check_error();

	glWaitSync(frame.ready_fence.get(), /*flags=*/0, GL_TIMEOUT_IGNORED);
	check_error();
	frame.setup_chain();
	check_error();
	glDisable(GL_FRAMEBUFFER_SRGB);
	check_error();
	frame.chain->render_to_fbo(fbo, global_flags.width, global_flags.height);
	check_error();

	// Read back to memory.
	glBindFramebuffer(GL_FRAMEBUFFER, fbo);
	check_error();
	glBindBuffer(GL_PIXEL_PACK_BUFFER, pbo);
	check_error();
	glReadPixels(0, 0, global_flags.width, global_flags.height, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, BUFFER_OFFSET(0));
	check_error();

	unsigned char *buf = (unsigned char *)glMapBuffer(GL_PIXEL_PACK_BUFFER, GL_READ_ONLY);
	check_error();

	size_t pitch = global_flags.width * 4;
	for (int y = 0; y < global_flags.height; ++y) {
		memcpy(grabbed_image.scanLine(global_flags.height - y - 1), buf + y * pitch, pitch);
	}

	{
		char buf[256];
		snprintf(buf, sizeof(buf), "Grabbed frame (%dx%d)", global_flags.width, global_flags.height);
		ui->grabbed_frame_sublabel->setText(buf);
	}

	QPixmap pixmap;
	pixmap.convertFromImage(grabbed_image);
	ui->grabbed_frame_label->setPixmap(pixmap);

	int r_hist[256] = {0}, g_hist[256] = {0}, b_hist[256] = {0};
	const unsigned char *ptr = buf;
	for (int i = 0; i < global_flags.height * global_flags.width; ++i) {
		uint8_t b = *ptr++;
		uint8_t g = *ptr++;
		uint8_t r = *ptr++;
		++ptr;

		++r_hist[r];
		++g_hist[g];
		++b_hist[b];
	}

	glUnmapBuffer(GL_PIXEL_PACK_BUFFER);
	check_error();
	glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
	check_error();
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	check_error();

	QVector<double> r_vec(256), g_vec(256), b_vec(256), x_vec(256);
	double max = 0.0;
	for (unsigned i = 0; i < 256; ++i) {
		x_vec[i] = i;
		r_vec[i] = log(r_hist[i] + 1.0);
		g_vec[i] = log(g_hist[i] + 1.0);
		b_vec[i] = log(b_hist[i] + 1.0);

		max = std::max(max, r_vec[i]);
		max = std::max(max, g_vec[i]);
		max = std::max(max, b_vec[i]);
	}

	ui->histogram->clearGraphs();
	ui->histogram->addGraph();
	ui->histogram->graph(0)->setData(x_vec, r_vec);
	ui->histogram->graph(0)->setPen(QPen(Qt::red));
	ui->histogram->graph(0)->setBrush(QBrush(QColor(255, 127, 127, 80)));
	ui->histogram->addGraph();
	ui->histogram->graph(1)->setData(x_vec, g_vec);
	ui->histogram->graph(1)->setPen(QPen(Qt::green));
	ui->histogram->graph(1)->setBrush(QBrush(QColor(127, 255, 127, 80)));
	ui->histogram->addGraph();
	ui->histogram->graph(2)->setData(x_vec, b_vec);
	ui->histogram->graph(2)->setPen(QPen(Qt::blue));
	ui->histogram->graph(2)->setBrush(QBrush(QColor(127, 127, 255, 80)));

	ui->histogram->xAxis->setVisible(true);
	ui->histogram->yAxis->setVisible(false);
	ui->histogram->xAxis->setRange(0, 255);
	ui->histogram->yAxis->setRange(0, max);
	ui->histogram->replot();

	resource_pool->release_2d_texture(fbo_tex);
	check_error();
	resource_pool->release_fbo(fbo);
	check_error();

	if (last_x >= 0 && last_y >= 0) {
		grab_pixel(last_x, last_y);
	}

	if (isVisible()) {
		grab_timer.stop();

		// Set up the next autograb if configured.
		int delay = ui->grab_frequency_box->currentData().toInt(nullptr);
		if (delay > 0) {
			grab_timer.start(delay);
		}
	}
}

void Analyzer::signal_changed()
{
	Mixer::Output channel = static_cast<Mixer::Output>(ui->input_box->currentData().value<int>());
	ui->display->set_output(channel);
	grab_clicked();
}

bool Analyzer::eventFilter(QObject *watched, QEvent *event)
{
	if (event->type() == QEvent::MouseMove && watched->isWidgetType()) {
		const QMouseEvent *mouse_event = (QMouseEvent *)event;
		last_x = mouse_event->x();
		last_y = mouse_event->y();
		grab_pixel(mouse_event->x(), mouse_event->y());
	}
	if (event->type() == QEvent::Leave && watched->isWidgetType()) {
		last_x = last_y = -1;
		ui->coord_label->setText("Selected coordinate (x,y): (none)");
		ui->red_label->setText(u8"—");
		ui->green_label->setText(u8"—");
		ui->blue_label->setText(u8"—");
		ui->hex_label->setText(u8"#—");
	}
        return false;
}

void Analyzer::grab_pixel(int x, int y)
{
	const QPixmap *pixmap = ui->grabbed_frame_label->pixmap();
	if (pixmap != nullptr) {
		x = lrint(x * double(pixmap->width()) / ui->grabbed_frame_label->width());
		y = lrint(y * double(pixmap->height()) / ui->grabbed_frame_label->height());
		x = std::min(x, pixmap->width() - 1);
		y = std::min(y, pixmap->height() - 1);

		char buf[256];
		snprintf(buf, sizeof(buf), "Selected coordinate (x,y): (%d,%d)", x, y);
		ui->coord_label->setText(buf);

		QRgb pixel = grabbed_image.pixel(x, y);
		ui->red_label->setText(QString::fromStdString(to_string(qRed(pixel))));
		ui->green_label->setText(QString::fromStdString(to_string(qGreen(pixel))));
		ui->blue_label->setText(QString::fromStdString(to_string(qBlue(pixel))));

		snprintf(buf, sizeof(buf), "#%02x%02x%02x", qRed(pixel), qGreen(pixel), qBlue(pixel));
		ui->hex_label->setText(buf);
	}
}

void Analyzer::resizeEvent(QResizeEvent* event)
{
	QMainWindow::resizeEvent(event);

	// Ask for a relayout, but only after the event loop is done doing relayout
	// on everything else.
	QMetaObject::invokeMethod(this, "relayout", Qt::QueuedConnection);
}

void Analyzer::showEvent(QShowEvent *event)
{
	grab_clicked();
}

void Analyzer::relayout()
{
	double aspect = double(global_flags.width) / global_flags.height;

	// Left pane (2/5 of the width).
	{
		int width = ui->left_pane->geometry().width();
		int height = ui->left_pane->geometry().height();

		// Figure out how much space everything that's non-responsive needs.
		int remaining_height = height - ui->left_pane->spacing() * (ui->left_pane->count() - 1);

		remaining_height -= ui->input_box->geometry().height();
		ui->left_pane->setStretch(2, ui->grab_btn->geometry().height());

		remaining_height -= ui->grab_btn->geometry().height();
		ui->left_pane->setStretch(3, ui->grab_btn->geometry().height());

		remaining_height -= ui->histogram_label->geometry().height();
		ui->left_pane->setStretch(5, ui->histogram_label->geometry().height());

		// The histogram's minimumHeight returns 0, so let's set a reasonable minimum for it.
		int min_histogram_height = 50;
		remaining_height -= min_histogram_height;

		// Allocate so that the display is 16:9, if possible.
		unsigned wanted_display_height = width / aspect;
		unsigned display_height;
		unsigned margin = 0;
		if (remaining_height >= int(wanted_display_height)) {
			display_height = wanted_display_height;
		} else {
			display_height = remaining_height;
			int display_width = lrint(display_height * aspect);
			margin = (width - display_width) / 2;
		}
		ui->left_pane->setStretch(1, display_height);
		ui->display_left_spacer->changeSize(margin, 1);
		ui->display_right_spacer->changeSize(margin, 1);

		remaining_height -= display_height;

		// Figure out if we can do the histogram at 16:9.
		remaining_height += min_histogram_height;
		unsigned histogram_height;
		if (remaining_height >= int(wanted_display_height)) {
			histogram_height = wanted_display_height;
		} else {
			histogram_height = remaining_height;
		}
		remaining_height -= histogram_height;
		ui->left_pane->setStretch(4, histogram_height);

		ui->left_pane->setStretch(0, remaining_height / 2);
		ui->left_pane->setStretch(6, remaining_height / 2);
	}

	// Right pane (remaining 3/5 of the width).
	{
		int width = ui->right_pane->geometry().width();
		int height = ui->right_pane->geometry().height();

		// Figure out how much space everything that's non-responsive needs.
		int remaining_height = height - ui->right_pane->spacing() * (ui->right_pane->count() - 1);
		remaining_height -= ui->grabbed_frame_sublabel->geometry().height();
		remaining_height -= ui->coord_label->geometry().height();
		remaining_height -= ui->color_hbox->geometry().height();

		// Allocate so that the display is 16:9, if possible.
		unsigned wanted_display_height = width / aspect;
		unsigned display_height;
		unsigned margin = 0;
		if (remaining_height >= int(wanted_display_height)) {
			display_height = wanted_display_height;
		} else {
			display_height = remaining_height;
			int display_width = lrint(display_height * aspect);
			margin = (width - display_width) / 2;
		}
		ui->right_pane->setStretch(1, display_height);
		ui->grabbed_frame_left_spacer->changeSize(margin, 1);
		ui->grabbed_frame_right_spacer->changeSize(margin, 1);
		remaining_height -= display_height;

		if (remaining_height < 0) remaining_height = 0;

		ui->right_pane->setStretch(0, remaining_height / 2);
		ui->right_pane->setStretch(5, remaining_height / 2);
	}
}
