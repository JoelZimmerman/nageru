extern "C" {
#include <libavformat/avformat.h>
}
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <epoxy/gl.h>  // IWYU pragma: keep
#include <QApplication>
#include <QCoreApplication>
#include <QGL>
#include <QSize>
#include <QSurfaceFormat>
#include <string>

#include "basic_stats.h"
#include "context.h"
#include "flags.h"
#include "image_input.h"
#include "mainwindow.h"
#include "mixer.h"

int main(int argc, char *argv[])
{
	parse_flags(PROGRAM_NAGERU, argc, argv);

	if (global_flags.va_display.empty() ||
	    global_flags.va_display[0] != '/') {
		// We normally use EGL for zerocopy, but if we use VA against DRM
		// instead of against X11, we turn it off, and then don't need EGL.
		setenv("QT_XCB_GL_INTEGRATION", "xcb_egl", 0);
		using_egl = true;
	}
	setlinebuf(stdout);
	av_register_all();

	QCoreApplication::setAttribute(Qt::AA_ShareOpenGLContexts, true);
	QApplication app(argc, argv);

	QSurfaceFormat fmt;
	fmt.setDepthBufferSize(0);
	fmt.setStencilBufferSize(0);
	fmt.setProfile(QSurfaceFormat::CoreProfile);
	fmt.setMajorVersion(3);
	fmt.setMinorVersion(1);

	// Turn off vsync, since Qt generally gives us at most frame rate
	// (display frequency) / (number of QGLWidgets active).
	fmt.setSwapInterval(0);

	QSurfaceFormat::setDefaultFormat(fmt);

	QGLFormat::setDefaultFormat(QGLFormat::fromSurfaceFormat(fmt));

	global_share_widget = new QGLWidget();
	if (!global_share_widget->isValid()) {
		fprintf(stderr, "Failed to initialize OpenGL. Nageru needs at least OpenGL 3.1 to function properly.\n");
		exit(1);
	}

	MainWindow mainWindow;
	mainWindow.resize(QSize(1500, 850));
	mainWindow.show();

	app.installEventFilter(&mainWindow);  // For white balance color picking.

	// Even on an otherwise unloaded system, it would seem writing the recording
	// to disk (potentially terabytes of data as time goes by) causes Nageru
	// to be pushed out of RAM. If we have the right privileges, simply lock us
	// into memory for better realtime behavior.
	if (mlockall(MCL_CURRENT | MCL_FUTURE) == -1) {
		perror("mlockall()");
		fprintf(stderr, "Failed to lock Nageru into RAM. You probably want to\n");
		fprintf(stderr, "increase \"memlock\" for your user in limits.conf\n");
		fprintf(stderr, "for better realtime behavior.\n");
		uses_mlock = false;
	} else {
		uses_mlock = true;
	}

	int rc = app.exec();
	global_mixer->quit();
	mainWindow.mixer_shutting_down();
	delete global_mixer;
	ImageInput::shutdown_updaters();
	return rc;
}
