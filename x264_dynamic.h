#ifndef _X264_DYNAMIC_H
#define _X264_DYNAMIC_H 1

// A helper to load 10-bit x264 if needed.

#include <stdint.h>

extern "C" {
#include <x264.h>
}

struct X264Dynamic {
	void *handle;  // If not nullptr, needs to be dlclose()d.
	decltype(::x264_encoder_close) *x264_encoder_close;
	decltype(::x264_encoder_delayed_frames) *x264_encoder_delayed_frames;
	decltype(::x264_encoder_encode) *x264_encoder_encode;
	decltype(::x264_encoder_headers) *x264_encoder_headers;
	decltype(::x264_encoder_open) *x264_encoder_open;
	decltype(::x264_encoder_parameters) *x264_encoder_parameters;
	decltype(::x264_encoder_reconfig) *x264_encoder_reconfig;
	decltype(::x264_param_apply_profile) *x264_param_apply_profile;
	decltype(::x264_param_default_preset) *x264_param_default_preset;
	decltype(::x264_param_parse) *x264_param_parse;
	decltype(::x264_picture_init) *x264_picture_init;
};
X264Dynamic load_x264_for_bit_depth(unsigned depth);

#endif  // !defined(_X264_DYNAMIC_H)
