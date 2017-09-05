#ifndef _FFMPEG_RAII_H
#define _FFMPEG_RAII_H 1

// Some helpers to make RAII versions of FFmpeg objects.
// The cleanup functions don't interact all that well with unique_ptr,
// so things get a bit messy and verbose, but overall it's worth it to ensure
// we never leak things by accident in error paths.
//
// This does not cover any of the types that can actually be declared as
// a unique_ptr with no helper functions for deleter.

#include <memory>

struct AVCodec;
struct AVCodecContext;
struct AVCodecParameters;
struct AVDictionary;
struct AVFormatContext;
struct AVFrame;
struct AVInputFormat;
struct SwsContext;

// AVFormatContext
struct avformat_close_input_unique {
	void operator() (AVFormatContext *format_ctx) const;
};

typedef std::unique_ptr<AVFormatContext, avformat_close_input_unique>
	AVFormatContextWithCloser;

AVFormatContextWithCloser avformat_open_input_unique(
	const char *pathname, AVInputFormat *fmt, AVDictionary **options);


// AVCodecContext
struct avcodec_free_context_unique {
	void operator() (AVCodecContext *ctx) const;
};

typedef std::unique_ptr<AVCodecContext, avcodec_free_context_unique>
	AVCodecContextWithDeleter;

AVCodecContextWithDeleter avcodec_alloc_context3_unique(const AVCodec *codec);


// AVCodecParameters
struct avcodec_parameters_free_unique {
	void operator() (AVCodecParameters *codec_par) const;
};

typedef std::unique_ptr<AVCodecParameters, avcodec_parameters_free_unique>
	AVCodecParametersWithDeleter;


// AVFrame
struct av_frame_free_unique {
	void operator() (AVFrame *frame) const;
};

typedef std::unique_ptr<AVFrame, av_frame_free_unique>
	AVFrameWithDeleter;

AVFrameWithDeleter av_frame_alloc_unique();

// SwsContext
struct sws_free_context_unique {
	void operator() (SwsContext *context) const;
};

typedef std::unique_ptr<SwsContext, sws_free_context_unique>
	SwsContextWithDeleter;

#endif  // !defined(_FFMPEG_RAII_H)
