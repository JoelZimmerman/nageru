
// Needs to be in its own file because Qt and libepoxy seemingly don't coexist well
// within the same file.

class QSurface;
class QOpenGLContext;
class QSurfaceFormat;
class QGLWidget;

extern bool using_egl;
extern QGLWidget *global_share_widget;
QSurface *create_surface(const QSurfaceFormat &format);
QSurface *create_surface_with_same_format(const QSurface *surface);
QOpenGLContext *create_context(const QSurface *surface);
bool make_current(QOpenGLContext *context, QSurface *surface);
void delete_context(QOpenGLContext *context);
