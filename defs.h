#ifndef _DEFS_H
#define _DEFS_H

#define OUTPUT_FREQUENCY 48000  // Currently needs to be exactly 48000, since bmusb outputs in that.
#define MAX_FPS 60
#define FAKE_FPS 25  // Must be an integer.
#define MAX_VIDEO_CARDS 16
#define MAX_ALSA_CARDS 16
#define MAX_BUSES 256  // Audio buses.

// For deinterlacing. See also comments on InputState.
#define FRAME_HISTORY_LENGTH 5

#define AUDIO_OUTPUT_CODEC_NAME "pcm_s32le"
#define DEFAULT_AUDIO_OUTPUT_BIT_RATE 0
#define DEFAULT_X264_OUTPUT_BIT_RATE 4500  // 5 Mbit after making room for some audio and TCP overhead.

#define LOCAL_DUMP_PREFIX "record-"
#define LOCAL_DUMP_SUFFIX ".nut"
#define DEFAULT_STREAM_MUX_NAME "nut"  // Only for HTTP. Local dump guesses from LOCAL_DUMP_SUFFIX.
#define MUX_OPTS { \
	/* Make seekable .mov files. */ \
	{ "movflags", "empty_moov+frag_keyframe+default_base_moof" }, \
	\
	/* Make for somewhat less bursty stream output when using .mov. */ \
	{ "frag_duration", "125000" }, \
	\
	/* Keep nut muxer from using unlimited amounts of memory. */ \
	{ "write_index", "0" } \
}

// In bytes. Beware, if too small, stream clients will start dropping data.
// For mov, you want this at 10MB or so (for the reason mentioned above),
// but for nut, there's no flushing, so such a large mux buffer would cause
// the output to be very uneven.
#define MUX_BUFFER_SIZE 10485760

// In number of frames. Comes in addition to any internal queues in x264
// (frame threading, lookahead, etc.).
#define X264_QUEUE_LENGTH 50

#define X264_DEFAULT_PRESET "ultrafast"
#define X264_DEFAULT_TUNE "film"

#endif  // !defined(_DEFS_H)
