#include "ffmpeg_util.h"

#include <ctype.h>
#include <fcntl.h>
#include <unistd.h>

#include <string>
#include <vector>

#include "flags.h"

using namespace std;

string search_for_file(const string &filename)
{
	if (!filename.empty() && filename[0] == '/') {
		// Absolute path.
		return filename;
	}

	// See if we match ^[a-z]:/, which is probably a URL of some sort
	// (FFmpeg understands various forms of these).
	for (size_t i = 0; i < filename.size() - 1; ++i) {
		if (filename[i] == ':' && filename[i + 1] == '/') {
			return filename;
		}
		if (!isalpha(filename[i])) {
			break;
		}
	}

	// Look for the file in all theme_dirs until we find one;
	// that will be the permanent resolution of this file, whether
	// it is actually valid or not.
	// We store errors from all the attempts, and show them
	// once we know we can't find any of them.
	vector<string> errors;
	for (const string &dir : global_flags.theme_dirs) {
		string pathname = dir + "/" + filename;
		if (access(pathname.c_str(), O_RDONLY) == 0) {
			return pathname;
		} else {
			char buf[512];
			snprintf(buf, sizeof(buf), "%s: %s", pathname.c_str(), strerror(errno));
			errors.push_back(buf);
		}
	}

	for (const string &error : errors) {
		fprintf(stderr, "%s\n", error.c_str());
	}
	return "";
}

string search_for_file_or_die(const string &filename)
{
	string pathname = search_for_file(filename);
	if (pathname.empty()) {
		fprintf(stderr, "Couldn't find %s in any directory in --theme-dirs, exiting.\n",
			filename.c_str());
		exit(1);
	}
	return pathname;
}

int find_stream_index(AVFormatContext *ctx, AVMediaType media_type)
{
	for (unsigned i = 0; i < ctx->nb_streams; ++i) {
		if (ctx->streams[i]->codecpar->codec_type == media_type) {
			return i;
		}
	}
	return -1;
}

