#include <glob.h>
#include <stdio.h>
extern "C" {
#include <pci/pci.h>
}

#include <string>

#include <QGL>
#include <QOffscreenSurface>
#include <QOpenGLContext>
#include <QSurface>
#include <QSurfaceFormat>

QGLWidget *global_share_widget = nullptr;
bool using_egl = false;

using namespace std;

namespace {

string get_pci_device_name(const char *node_name)
{
	char vendor_path[256];
	snprintf(vendor_path, sizeof(vendor_path), "/sys/class/drm/%s/device/vendor", node_name);
	FILE *vendor_file = fopen(vendor_path, "r");
	if (vendor_file == nullptr) {
		return "could not look up vendor ID";
	}
	int vendor;
	if (fscanf(vendor_file, "%i", &vendor) != 1) {
		fclose(vendor_file);
		return "could not parse vendor ID";
	}
	fclose(vendor_file);

	char device_path[256];
	snprintf(device_path, sizeof(device_path), "/sys/class/drm/%s/device/device", node_name);
	FILE *device_file = fopen(device_path, "r");
	if (device_file == nullptr) {
		return "could not look up device ID";
	}
	int device;
	if (fscanf(device_file, "%i", &device) != 1) {
		fclose(device_file);
		return "could not parse device ID";
	}
	fclose(device_file);

	pci_access *pci = pci_alloc();
	if (pci == nullptr) {
		return "could not init libpci";
	}
	pci_init(pci);

	char buf[256];
	const char *name = pci_lookup_name(pci, buf, sizeof(buf), PCI_LOOKUP_VENDOR | PCI_LOOKUP_DEVICE, vendor, device);
	pci_cleanup(pci);

	if (name == nullptr) {
		snprintf(buf, sizeof(buf), "%04x:%04x", vendor, device);
	}
	return buf;
}

void print_available_drm_nodes()
{
	glob_t g;
	int err = glob("/dev/dri/renderD*", 0, nullptr, &g);  // TODO: Accept /dev/dri/card*, too?
	if (err != 0) {
		fprintf(stderr, "Couldn't list render nodes (%s).\n", strerror(errno));
		return;
	}

	if (g.gl_pathc == 0) {
		fprintf(stderr, "\n");
		fprintf(stderr, "No render nodes found in /dev/dri.\n");
	} else {
		fprintf(stderr, "Available devices (these may or may not support VA-API encoding):\n\n");
		for (size_t i = 0; i < g.gl_pathc; ++i) {
			const char *node_name = basename(g.gl_pathv[i]);
			fprintf(stderr, "  %s (%s)\n", g.gl_pathv[i], get_pci_device_name(node_name).c_str());
		}
	}

	globfree(&g);
}

}  // namespace

QSurface *create_surface(const QSurfaceFormat &format)
{
	QOffscreenSurface *surface = new QOffscreenSurface;
	surface->setFormat(format);
	surface->create();
	if (!surface->isValid()) {
		fprintf(stderr, "ERROR: surface not valid!\n");
		if (using_egl) {
			fprintf(stderr, "\n\n");
			fprintf(stderr, "OpenGL initialization failed. This is most likely because your driver does not\n");
			fprintf(stderr, "support EGL (e.g. NVIDIA drivers). You can turn off EGL by specifying the\n");
			fprintf(stderr, "VA-API path directly, assuming you have another GPU with VA-API support\n");
			fprintf(stderr, "(typically an integrated Intel GPU -- note that it you might need to manually\n");
			fprintf(stderr, "enable it in the BIOS, as it might be turned off when a discrete GPU is detected).\n");
			fprintf(stderr, "\n");
			fprintf(stderr, "Specify the VA-API device using “--va-display /dev/dri/<node>”.\n");
			print_available_drm_nodes();
		}
		exit(1);
	}
	return surface;
}

QSurface *create_surface_with_same_format(const QSurface *surface)
{
	return create_surface(surface->format());
}

QOpenGLContext *create_context(const QSurface *surface)
{
	QOpenGLContext *context = new QOpenGLContext;
	context->setShareContext(global_share_widget->context()->contextHandle());
	context->setFormat(surface->format());
	context->create();
	return context;
}

bool make_current(QOpenGLContext *context, QSurface *surface)
{
	return context->makeCurrent(surface);
}

void delete_context(QOpenGLContext *context)
{
	delete context;
}
