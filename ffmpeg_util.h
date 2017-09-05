#ifndef _FFMPEG_UTIL_H
#define _FFMPEG_UTIL_H 1

// Some common utilities for the two FFmpeg users (ImageInput and FFmpegCapture).

#include <string>

extern "C" {
#include <libavformat/avformat.h>
}

// Look for the file in all theme_dirs until we find one;
// that will be the permanent resolution of this file, whether
// it is actually valid or not. Returns an empty string on error.
std::string search_for_file(const std::string &filename);

// Same, but exits on error.
std::string search_for_file_or_die(const std::string &filename);

// Returns -1 if not found.
int find_stream_index(AVFormatContext *ctx, AVMediaType media_type);

#endif  // !defined(_FFMPEG_UTIL_H)
