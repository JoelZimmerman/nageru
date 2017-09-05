#ifndef GLWIDGET_H
#define GLWIDGET_H

#include <epoxy/gl.h>
#include <QGL>
#include <QString>
#include <string>
#include <vector>

#include "mixer.h"

class QMouseEvent;
class QObject;
class QPoint;
class QWidget;

namespace movit {

class ResourcePool;

}  // namespace movit

// Note: We use the older QGLWidget instead of QOpenGLWidget as it is
// much faster (does not go through a separate offscreen rendering step).
//
// TODO: Consider if QOpenGLWindow could do what we want.
class GLWidget : public QGLWidget
{
	Q_OBJECT

public:
	GLWidget(QWidget *parent = 0);
	~GLWidget();

	void set_output(Mixer::Output output)
	{
		this->output = output;
	}

	void shutdown();

protected:
	void initializeGL() override;
	void resizeGL(int width, int height) override;
	void paintGL() override;
	void mousePressEvent(QMouseEvent *event) override;

signals:
	void clicked();
	void transition_names_updated(std::vector<std::string> transition_names);
	void name_updated(Mixer::Output output, const std::string &name);
	void color_updated(Mixer::Output output, const std::string &color);

private slots:
	void show_context_menu(const QPoint &pos);

private:
	void show_live_context_menu(const QPoint &pos);
	void show_preview_context_menu(unsigned signal_num, const QPoint &pos);

	Mixer::Output output;
	GLuint vao, program_num;
	GLuint position_vbo, texcoord_vbo;
	movit::ResourcePool *resource_pool = nullptr;
	int current_width = 1, current_height = 1;
};

#endif
