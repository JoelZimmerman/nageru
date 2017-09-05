#include "x264_dynamic.h"

#include <assert.h>
#include <dlfcn.h>
#include <link.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <string>

using namespace std;

X264Dynamic load_x264_for_bit_depth(unsigned depth)
{
	X264Dynamic dyn;
	if (unsigned(x264_bit_depth) >= depth) {
		// Just use the one we are linked to.
		dyn.handle = nullptr;
		dyn.x264_encoder_close = x264_encoder_close;
		dyn.x264_encoder_delayed_frames = x264_encoder_delayed_frames;
		dyn.x264_encoder_encode = x264_encoder_encode;
		dyn.x264_encoder_headers = x264_encoder_headers;
		dyn.x264_encoder_open = x264_encoder_open;
		dyn.x264_encoder_parameters = x264_encoder_parameters;
		dyn.x264_encoder_reconfig = x264_encoder_reconfig;
		dyn.x264_param_apply_profile = x264_param_apply_profile;
		dyn.x264_param_default_preset = x264_param_default_preset;
		dyn.x264_param_parse = x264_param_parse;
		dyn.x264_picture_init = x264_picture_init;
		return dyn;
	}

	// Uh-oh, our currently loaded library doesn't have the required support.
	// Let's try to dynamically load a 10-bit version; in particular, Debian
	// has a version in /usr/lib/x86_64-linux-gnu/x264-10bit/libx264.so.<soname>,
	// so we try to figure out where our libx264 comes from, and modify that path.
	string x264_dir, x264_suffix;
	void *handle = dlopen(nullptr, RTLD_NOW);
	link_map *m;
	int err = dlinfo(handle, RTLD_DI_LINKMAP, &m);
	assert(err != -1);
	for ( ; m != nullptr; m = m->l_next) {
		if (m->l_name == nullptr) {
			continue;
		}
		const char *ptr = strstr(m->l_name, "/libx264.so.");
		if (ptr != nullptr) {
			x264_dir = string(m->l_name, ptr - m->l_name);
			x264_suffix = string(ptr, (m->l_name + strlen(m->l_name)) - ptr);
			break;
		}
        }
	dlclose(handle);

	if (x264_dir.empty()) {
		fprintf(stderr, "ERROR: Requested %d-bit x264, but is not linked to such an x264, and could not find one.\n",
			depth);
		exit(1);
	}

	string x264_10b_string = x264_dir + "/x264-10bit" + x264_suffix;
	void *x264_dlhandle = dlopen(x264_10b_string.c_str(), RTLD_NOW);
	if (x264_dlhandle == nullptr) {
		fprintf(stderr, "ERROR: Requested %d-bit x264, but is not linked to such an x264, and %s would not load.\n",
			depth, x264_10b_string.c_str());
		exit(1);
	}

	dyn.handle = x264_dlhandle;
	dyn.x264_encoder_close = (decltype(::x264_encoder_close) *)dlsym(x264_dlhandle, "x264_encoder_close");
	dyn.x264_encoder_delayed_frames = (decltype(::x264_encoder_delayed_frames) *)dlsym(x264_dlhandle, "x264_encoder_delayed_frames");
	dyn.x264_encoder_encode = (decltype(::x264_encoder_encode) *)dlsym(x264_dlhandle, "x264_encoder_encode");
	dyn.x264_encoder_headers = (decltype(::x264_encoder_headers) *)dlsym(x264_dlhandle, "x264_encoder_headers");
	char x264_encoder_open_symname[256];
	snprintf(x264_encoder_open_symname, sizeof(x264_encoder_open_symname), "x264_encoder_open_%d", X264_BUILD);
	dyn.x264_encoder_open = (decltype(::x264_encoder_open) *)dlsym(x264_dlhandle, x264_encoder_open_symname);
	dyn.x264_encoder_parameters = (decltype(::x264_encoder_parameters) *)dlsym(x264_dlhandle, "x264_encoder_parameters");
	dyn.x264_encoder_reconfig = (decltype(::x264_encoder_reconfig) *)dlsym(x264_dlhandle, "x264_encoder_reconfig");
	dyn.x264_param_apply_profile = (decltype(::x264_param_apply_profile) *)dlsym(x264_dlhandle, "x264_param_apply_profile");
	dyn.x264_param_default_preset = (decltype(::x264_param_default_preset) *)dlsym(x264_dlhandle, "x264_param_default_preset");
	dyn.x264_param_parse = (decltype(::x264_param_parse) *)dlsym(x264_dlhandle, "x264_param_parse");
	dyn.x264_picture_init = (decltype(::x264_picture_init) *)dlsym(x264_dlhandle, "x264_picture_init");
	return dyn;
}
