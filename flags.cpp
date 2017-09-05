#include "flags.h"

#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <utility>

using namespace std;

Flags global_flags;

// Long options that have no corresponding short option.
enum LongOption {
	OPTION_HELP = 1000,
	OPTION_MULTICHANNEL,
	OPTION_MIDI_MAPPING,
	OPTION_FAKE_CARDS_AUDIO,
	OPTION_HTTP_UNCOMPRESSED_VIDEO,
	OPTION_HTTP_X264_VIDEO,
	OPTION_RECORD_X264_VIDEO,
	OPTION_X264_PRESET,
	OPTION_X264_TUNE,
	OPTION_X264_SPEEDCONTROL,
	OPTION_X264_SPEEDCONTROL_VERBOSE,
	OPTION_X264_BITRATE,
	OPTION_X264_CRF,
	OPTION_X264_VBV_BUFSIZE,
	OPTION_X264_VBV_MAX_BITRATE,
	OPTION_X264_PARAM,
	OPTION_HTTP_MUX,
	OPTION_HTTP_COARSE_TIMEBASE,
	OPTION_HTTP_AUDIO_CODEC,
	OPTION_HTTP_AUDIO_BITRATE,
	OPTION_NO_TRANSCODE_AUDIO,
	OPTION_FLAT_AUDIO,
	OPTION_GAIN_STAGING,
	OPTION_DISABLE_LOCUT,
	OPTION_ENABLE_LOCUT,
	OPTION_DISABLE_GAIN_STAGING_AUTO,
	OPTION_ENABLE_GAIN_STAGING_AUTO,
	OPTION_DISABLE_COMPRESSOR,
	OPTION_ENABLE_COMPRESSOR,
	OPTION_DISABLE_LIMITER,
	OPTION_ENABLE_LIMITER,
	OPTION_DISABLE_MAKEUP_GAIN_AUTO,
	OPTION_ENABLE_MAKEUP_GAIN_AUTO,
	OPTION_DISABLE_ALSA_OUTPUT,
	OPTION_NO_FLUSH_PBOS,
	OPTION_PRINT_VIDEO_LATENCY,
	OPTION_MAX_INPUT_QUEUE_FRAMES,
	OPTION_AUDIO_QUEUE_LENGTH_MS,
	OPTION_OUTPUT_YCBCR_COEFFICIENTS,
	OPTION_OUTPUT_BUFFER_FRAMES,
	OPTION_OUTPUT_SLOP_FRAMES,
	OPTION_TIMECODE_STREAM,
	OPTION_TIMECODE_STDOUT,
	OPTION_10_BIT_INPUT,
	OPTION_10_BIT_OUTPUT,
	OPTION_INPUT_YCBCR_INTERPRETATION,
};

void usage(Program program)
{
	if (program == PROGRAM_KAERU) {
		fprintf(stderr, "Usage: kaeru [OPTION]... SOURCE_URL\n");
	} else {
		fprintf(stderr, "Usage: nageru [OPTION]...\n");
	}
	fprintf(stderr, "\n");
	fprintf(stderr, "      --help                      print usage information\n");
	fprintf(stderr, "  -w, --width                     output width in pixels (default 1280)\n");
	fprintf(stderr, "  -h, --height                    output height in pixels (default 720)\n");
	if (program == PROGRAM_NAGERU) {
		fprintf(stderr, "  -c, --num-cards                 set number of input cards (default 2)\n");
		fprintf(stderr, "  -o, --output-card=CARD          also output signal to the given card (default none)\n");
		fprintf(stderr, "  -t, --theme=FILE                choose theme (default theme.lua)\n");
		fprintf(stderr, "  -I, --theme-dir=DIR             search for theme in this directory (can be given multiple times)\n");
		fprintf(stderr, "  -r, --recording-dir=DIR         where to store disk recording\n");
		fprintf(stderr, "  -v, --va-display=SPEC           VA-API device for H.264 encoding\n");
		fprintf(stderr, "                                    ($DISPLAY spec or /dev/dri/render* path)\n");
		fprintf(stderr, "  -m, --map-signal=SIGNAL,CARD    set a default card mapping (can be given multiple times)\n");
		fprintf(stderr, "  -M, --input-mapping=FILE        start with the given audio input mapping (implies --multichannel)\n");
		fprintf(stderr, "      --multichannel              start in multichannel audio mapping mode\n");
		fprintf(stderr, "      --midi-mapping=FILE         start with the given MIDI controller mapping (implies --multichannel)\n");
		fprintf(stderr, "      --fake-cards-audio          make fake (disconnected) cards output a simple tone\n");
		fprintf(stderr, "      --http-uncompressed-video   send uncompressed NV12 video to HTTP clients\n");
		fprintf(stderr, "      --http-x264-video           send x264-compressed video to HTTP clients\n");
		fprintf(stderr, "      --record-x264-video         store x264-compressed video to disk (implies --http-x264-video,\n");
		fprintf(stderr, "                                    removes the need for working VA-API encoding)\n");
	}
	fprintf(stderr, "      --x264-preset               x264 quality preset (default " X264_DEFAULT_PRESET ")\n");
	fprintf(stderr, "      --x264-tune                 x264 tuning (default " X264_DEFAULT_TUNE ", can be blank)\n");
	fprintf(stderr, "      --x264-speedcontrol         try to match x264 preset to available CPU speed\n");
	fprintf(stderr, "      --x264-speedcontrol-verbose  output speedcontrol debugging statistics\n");
	fprintf(stderr, "      --x264-bitrate              x264 bitrate (in kilobit/sec, default %d)\n",
		DEFAULT_X264_OUTPUT_BIT_RATE);
	fprintf(stderr, "      --x264-crf=VALUE            quality-based VBR (-12 to 51), incompatible with --x264-bitrate and VBV\n");
	fprintf(stderr, "      --x264-vbv-bufsize          x264 VBV size (in kilobits, 0 = one-frame VBV,\n");
	fprintf(stderr, "                                  default: same as --x264-bitrate, that is, one-second VBV)\n");
	fprintf(stderr, "      --x264-vbv-max-bitrate      x264 local max bitrate (in kilobit/sec per --vbv-bufsize,\n");
	fprintf(stderr, "                                  0 = no limit, default: same as --x264-bitrate, i.e., CBR)\n");
	fprintf(stderr, "      --x264-param=NAME[,VALUE]   set any x264 parameter, for fine tuning\n");
	fprintf(stderr, "      --http-mux=NAME             mux to use for HTTP streams (default " DEFAULT_STREAM_MUX_NAME ")\n");
	fprintf(stderr, "      --http-audio-codec=NAME     audio codec to use for HTTP streams\n");
	fprintf(stderr, "                                  (default is to use the same as for the recording)\n");
	fprintf(stderr, "      --http-audio-bitrate=KBITS  audio codec bit rate to use for HTTP streams\n");
	fprintf(stderr, "                                  (default is %d, ignored unless --http-audio-codec is set)\n",
		DEFAULT_AUDIO_OUTPUT_BIT_RATE / 1000);
	if (program == PROGRAM_KAERU) {
		fprintf(stderr, "      --no-transcode-audio        copy encoded audio raw from the source stream\n");
		fprintf(stderr, "                                    (requires --http-audio-codec= to be set)\n");
	}
	fprintf(stderr, "      --http-coarse-timebase      use less timebase for HTTP (recommended for muxers\n");
	fprintf(stderr, "                                  that handle large pts poorly, like e.g. MP4)\n");
	if (program == PROGRAM_NAGERU) {
		fprintf(stderr, "      --flat-audio                start with most audio processing turned off\n");
		fprintf(stderr, "                                    (can be overridden by e.g. --enable-limiter)\n");
		fprintf(stderr, "      --gain-staging=DB           set initial gain staging to the given value\n");
		fprintf(stderr, "                                    (--disable-gain-staging-auto)\n");
		fprintf(stderr, "      --disable-locut             turn off locut filter (also --enable)\n");
		fprintf(stderr, "      --disable-gain-staging-auto  turn off automatic gain staging (also --enable)\n");
		fprintf(stderr, "      --disable-compressor        turn off regular compressor (also --enable)\n");
		fprintf(stderr, "      --disable-limiter           turn off limiter (also --enable)\n");
		fprintf(stderr, "      --disable-makeup-gain-auto  turn off auto-adjustment of final makeup gain (also --enable)\n");
		fprintf(stderr, "      --disable-alsa-output       disable audio monitoring via ALSA\n");
		fprintf(stderr, "      --no-flush-pbos             do not explicitly signal texture data uploads\n");
		fprintf(stderr, "                                    (will give display corruption, but makes it\n");
		fprintf(stderr, "                                    possible to run with apitrace in real time)\n");
		fprintf(stderr, "      --print-video-latency       print out measurements of video latency on stdout\n");
		fprintf(stderr, "      --max-input-queue-frames=FRAMES  never keep more than FRAMES frames for each card\n");
		fprintf(stderr, "                                    (default 6, minimum 1)\n");
		fprintf(stderr, "      --audio-queue-length-ms=MS  length of audio resampling queue (default 100.0)\n");
		fprintf(stderr, "      --output-ycbcr-coefficients={rec601,rec709,auto}\n");
		fprintf(stderr, "                                  Y'CbCr coefficient standard of output (default auto)\n");
		fprintf(stderr, "                                    auto is rec601, unless --output-card is used\n");
		fprintf(stderr, "                                    and a Rec. 709 mode (typically HD modes) is in use\n");
		fprintf(stderr, "      --output-buffer-frames=NUM  number of frames in output buffer for --output-card,\n");
		fprintf(stderr, "                                    can be fractional (default 6.0); note also\n");
		fprintf(stderr, "                                    the audio queue can't be much longer than this\n");
		fprintf(stderr, "      --output-slop-frames=NUM    if more less than this number of frames behind for\n");
		fprintf(stderr, "                                    --output-card, try to submit anyway instead of\n");
		fprintf(stderr, "                                    dropping the frame (default 0.5)\n");
		fprintf(stderr, "      --timecode-stream           show timestamp and timecode in stream\n");
		fprintf(stderr, "      --timecode-stdout           show timestamp and timecode on standard output\n");
		fprintf(stderr, "      --10-bit-input              use 10-bit video input (requires compute shaders)\n");
		fprintf(stderr, "      --10-bit-output             use 10-bit video output (requires compute shaders,\n");
		fprintf(stderr, "                                    implies --record-x264-video)\n");
		fprintf(stderr, "      --input-ycbcr-interpretation=CARD,{rec601,rec709,auto}[,{limited,full}]\n");
		fprintf(stderr, "                                  Y'CbCr coefficient standard of card CARD (default auto)\n");
		fprintf(stderr, "                                    auto is rec601 for SD, rec709 for HD, always limited\n");
		fprintf(stderr, "                                    limited means standard 0-240/0-235 input range (for 8-bit)\n");
	}
}

void parse_flags(Program program, int argc, char * const argv[])
{
	static const option long_options[] = {
		{ "help", no_argument, 0, OPTION_HELP },
		{ "width", required_argument, 0, 'w' },
		{ "height", required_argument, 0, 'h' },
		{ "num-cards", required_argument, 0, 'c' },
		{ "output-card", required_argument, 0, 'o' },
		{ "theme", required_argument, 0, 't' },
		{ "theme-dir", required_argument, 0, 'I' },
		{ "recording-dir", required_argument, 0, 'r' },
		{ "map-signal", required_argument, 0, 'm' },
		{ "input-mapping", required_argument, 0, 'M' },
		{ "va-display", required_argument, 0, 'v' },
		{ "multichannel", no_argument, 0, OPTION_MULTICHANNEL },
		{ "midi-mapping", required_argument, 0, OPTION_MIDI_MAPPING },
		{ "fake-cards-audio", no_argument, 0, OPTION_FAKE_CARDS_AUDIO },
		{ "http-uncompressed-video", no_argument, 0, OPTION_HTTP_UNCOMPRESSED_VIDEO },
		{ "http-x264-video", no_argument, 0, OPTION_HTTP_X264_VIDEO },
		{ "record-x264-video", no_argument, 0, OPTION_RECORD_X264_VIDEO },
		{ "x264-preset", required_argument, 0, OPTION_X264_PRESET },
		{ "x264-tune", required_argument, 0, OPTION_X264_TUNE },
		{ "x264-speedcontrol", no_argument, 0, OPTION_X264_SPEEDCONTROL },
		{ "x264-speedcontrol-verbose", no_argument, 0, OPTION_X264_SPEEDCONTROL_VERBOSE },
		{ "x264-bitrate", required_argument, 0, OPTION_X264_BITRATE },
		{ "x264-crf", required_argument, 0, OPTION_X264_CRF },
		{ "x264-vbv-bufsize", required_argument, 0, OPTION_X264_VBV_BUFSIZE },
		{ "x264-vbv-max-bitrate", required_argument, 0, OPTION_X264_VBV_MAX_BITRATE },
		{ "x264-param", required_argument, 0, OPTION_X264_PARAM },
		{ "http-mux", required_argument, 0, OPTION_HTTP_MUX },
		{ "http-coarse-timebase", no_argument, 0, OPTION_HTTP_COARSE_TIMEBASE },
		{ "http-audio-codec", required_argument, 0, OPTION_HTTP_AUDIO_CODEC },
		{ "http-audio-bitrate", required_argument, 0, OPTION_HTTP_AUDIO_BITRATE },
		{ "no-transcode-audio", no_argument, 0, OPTION_NO_TRANSCODE_AUDIO },
		{ "flat-audio", no_argument, 0, OPTION_FLAT_AUDIO },
		{ "gain-staging", required_argument, 0, OPTION_GAIN_STAGING },
		{ "disable-locut", no_argument, 0, OPTION_DISABLE_LOCUT },
		{ "enable-locut", no_argument, 0, OPTION_ENABLE_LOCUT },
		{ "disable-gain-staging-auto", no_argument, 0, OPTION_DISABLE_GAIN_STAGING_AUTO },
		{ "enable-gain-staging-auto", no_argument, 0, OPTION_ENABLE_GAIN_STAGING_AUTO },
		{ "disable-compressor", no_argument, 0, OPTION_DISABLE_COMPRESSOR },
		{ "enable-compressor", no_argument, 0, OPTION_ENABLE_COMPRESSOR },
		{ "disable-limiter", no_argument, 0, OPTION_DISABLE_LIMITER },
		{ "enable-limiter", no_argument, 0, OPTION_ENABLE_LIMITER },
		{ "disable-makeup-gain-auto", no_argument, 0, OPTION_DISABLE_MAKEUP_GAIN_AUTO },
		{ "enable-makeup-gain-auto", no_argument, 0, OPTION_ENABLE_MAKEUP_GAIN_AUTO },
		{ "disable-alsa-output", no_argument, 0, OPTION_DISABLE_ALSA_OUTPUT },
		{ "no-flush-pbos", no_argument, 0, OPTION_NO_FLUSH_PBOS },
		{ "print-video-latency", no_argument, 0, OPTION_PRINT_VIDEO_LATENCY },
		{ "max-input-queue-frames", required_argument, 0, OPTION_MAX_INPUT_QUEUE_FRAMES },
		{ "audio-queue-length-ms", required_argument, 0, OPTION_AUDIO_QUEUE_LENGTH_MS },
		{ "output-ycbcr-coefficients", required_argument, 0, OPTION_OUTPUT_YCBCR_COEFFICIENTS },
		{ "output-buffer-frames", required_argument, 0, OPTION_OUTPUT_BUFFER_FRAMES },
		{ "output-slop-frames", required_argument, 0, OPTION_OUTPUT_SLOP_FRAMES },
		{ "timecode-stream", no_argument, 0, OPTION_TIMECODE_STREAM },
		{ "timecode-stdout", no_argument, 0, OPTION_TIMECODE_STDOUT },
		{ "10-bit-input", no_argument, 0, OPTION_10_BIT_INPUT },
		{ "10-bit-output", no_argument, 0, OPTION_10_BIT_OUTPUT },
		{ "input-ycbcr-interpretation", required_argument, 0, OPTION_INPUT_YCBCR_INTERPRETATION },
		{ 0, 0, 0, 0 }
	};
	vector<string> theme_dirs;
	string output_ycbcr_coefficients = "auto";
	for ( ;; ) {
		int option_index = 0;
		int c = getopt_long(argc, argv, "c:t:I:r:v:m:M:w:h:", long_options, &option_index);

		if (c == -1) {
			break;
		}
		switch (c) {
		case 'w':
			global_flags.width = atoi(optarg);
			break;
		case 'h':
			global_flags.height = atoi(optarg);
			break;
		case 'c':
			global_flags.num_cards = atoi(optarg);
			break;
		case 'o':
			global_flags.output_card = atoi(optarg);
			break;
		case 't':
			global_flags.theme_filename = optarg;
			break;
		case 'I':
			theme_dirs.push_back(optarg);
			break;
		case 'r':
			global_flags.recording_dir = optarg;
			break;
		case 'm': {
			char *ptr = strchr(optarg, ',');
			if (ptr == nullptr) {
				fprintf(stderr, "ERROR: Invalid argument '%s' to --map-signal (needs a signal and a card number, separated by comma)\n", optarg);
				exit(1);
			}
			*ptr = '\0';
			const int signal_num = atoi(optarg);
			const int card_num = atoi(ptr + 1);
			if (global_flags.default_stream_mapping.count(signal_num)) {
				fprintf(stderr, "ERROR: Signal %d already mapped to card %d\n",
					signal_num, global_flags.default_stream_mapping[signal_num]);
				exit(1);
			}
			global_flags.default_stream_mapping[signal_num] = card_num;
			break;
		}
		case 'M':
			global_flags.input_mapping_filename = optarg;
			break;
		case OPTION_MULTICHANNEL:
			global_flags.multichannel_mapping_mode = true;
			break;
		case 'v':
			global_flags.va_display = optarg;
			break;
		case OPTION_MIDI_MAPPING:
			global_flags.midi_mapping_filename = optarg;
			global_flags.multichannel_mapping_mode = true;
			break;
		case OPTION_FAKE_CARDS_AUDIO:
			global_flags.fake_cards_audio = true;
			break;
		case OPTION_HTTP_UNCOMPRESSED_VIDEO:
			global_flags.uncompressed_video_to_http = true;
			break;
		case OPTION_HTTP_MUX:
			global_flags.stream_mux_name = optarg;
			break;
		case OPTION_HTTP_COARSE_TIMEBASE:
			global_flags.stream_coarse_timebase = true;
			break;
		case OPTION_HTTP_AUDIO_CODEC:
			global_flags.stream_audio_codec_name = optarg;
			break;
		case OPTION_HTTP_AUDIO_BITRATE:
			global_flags.stream_audio_codec_bitrate = atoi(optarg) * 1000;
			break;
		case OPTION_NO_TRANSCODE_AUDIO:
			global_flags.transcode_audio = false;
			break;
		case OPTION_HTTP_X264_VIDEO:
			global_flags.x264_video_to_http = true;
			break;
		case OPTION_RECORD_X264_VIDEO:
			global_flags.x264_video_to_disk = true;
			global_flags.x264_video_to_http = true;
			break;
		case OPTION_X264_PRESET:
			global_flags.x264_preset = optarg;
			break;
		case OPTION_X264_TUNE:
			global_flags.x264_tune = optarg;
			break;
		case OPTION_X264_SPEEDCONTROL:
			global_flags.x264_speedcontrol = true;
			break;
		case OPTION_X264_SPEEDCONTROL_VERBOSE:
			global_flags.x264_speedcontrol_verbose = true;
			break;
		case OPTION_X264_BITRATE:
			global_flags.x264_bitrate = atoi(optarg);
			break;
		case OPTION_X264_CRF:
			global_flags.x264_crf = atof(optarg);
			break;
		case OPTION_X264_VBV_BUFSIZE:
			global_flags.x264_vbv_buffer_size = atoi(optarg);
			break;
		case OPTION_X264_VBV_MAX_BITRATE:
			global_flags.x264_vbv_max_bitrate = atoi(optarg);
			break;
		case OPTION_X264_PARAM:
			global_flags.x264_extra_param.push_back(optarg);
			break;
		case OPTION_FLAT_AUDIO:
			// If --flat-audio is given, turn off everything that messes with the sound,
			// except the final makeup gain.
			global_flags.locut_enabled = false;
			global_flags.gain_staging_auto = false;
			global_flags.compressor_enabled = false;
			global_flags.limiter_enabled = false;
			break;
		case OPTION_GAIN_STAGING:
			global_flags.initial_gain_staging_db = atof(optarg);
			global_flags.gain_staging_auto = false;
			break;
		case OPTION_DISABLE_LOCUT:
			global_flags.locut_enabled = false;
			break;
		case OPTION_ENABLE_LOCUT:
			global_flags.locut_enabled = true;
			break;
		case OPTION_DISABLE_GAIN_STAGING_AUTO:
			global_flags.gain_staging_auto = false;
			break;
		case OPTION_ENABLE_GAIN_STAGING_AUTO:
			global_flags.gain_staging_auto = true;
			break;
		case OPTION_DISABLE_COMPRESSOR:
			global_flags.compressor_enabled = false;
			break;
		case OPTION_ENABLE_COMPRESSOR:
			global_flags.compressor_enabled = true;
			break;
		case OPTION_DISABLE_LIMITER:
			global_flags.limiter_enabled = false;
			break;
		case OPTION_ENABLE_LIMITER:
			global_flags.limiter_enabled = true;
			break;
		case OPTION_DISABLE_MAKEUP_GAIN_AUTO:
			global_flags.final_makeup_gain_auto = false;
			break;
		case OPTION_ENABLE_MAKEUP_GAIN_AUTO:
			global_flags.final_makeup_gain_auto = true;
			break;
		case OPTION_DISABLE_ALSA_OUTPUT:
			global_flags.enable_alsa_output = false;
			break;
		case OPTION_NO_FLUSH_PBOS:
			global_flags.flush_pbos = false;
			break;
		case OPTION_PRINT_VIDEO_LATENCY:
			global_flags.print_video_latency = true;
			break;
		case OPTION_MAX_INPUT_QUEUE_FRAMES:
			global_flags.max_input_queue_frames = atoi(optarg);
			break;
		case OPTION_AUDIO_QUEUE_LENGTH_MS:
			global_flags.audio_queue_length_ms = atof(optarg);
			break;
		case OPTION_OUTPUT_YCBCR_COEFFICIENTS:
			output_ycbcr_coefficients = optarg;
			break;
		case OPTION_OUTPUT_BUFFER_FRAMES:
			global_flags.output_buffer_frames = atof(optarg);
			break;
		case OPTION_OUTPUT_SLOP_FRAMES:
			global_flags.output_slop_frames = atof(optarg);
			break;
		case OPTION_TIMECODE_STREAM:
			global_flags.display_timecode_in_stream = true;
			break;
		case OPTION_TIMECODE_STDOUT:
			global_flags.display_timecode_on_stdout = true;
			break;
		case OPTION_10_BIT_INPUT:
			global_flags.ten_bit_input = true;
			break;
		case OPTION_10_BIT_OUTPUT:
			global_flags.ten_bit_output = true;
			global_flags.x264_video_to_disk = true;
			global_flags.x264_video_to_http = true;
			global_flags.x264_bit_depth = 10;
			break;
		case OPTION_INPUT_YCBCR_INTERPRETATION: {
			char *ptr = strchr(optarg, ',');
			if (ptr == nullptr) {
				fprintf(stderr, "ERROR: Invalid argument '%s' to --input-ycbcr-interpretation (needs a card and an interpretation, separated by comma)\n", optarg);
				exit(1);
			}
			*ptr = '\0';
			const int card_num = atoi(optarg);
			if (card_num < 0 || card_num >= MAX_VIDEO_CARDS) {
				fprintf(stderr, "ERROR: Invalid card number %d\n", card_num);
				exit(1);
			}

			YCbCrInterpretation interpretation;
			char *interpretation_str = ptr + 1;
			ptr = strchr(interpretation_str, ',');
			if (ptr != nullptr) {
				*ptr = '\0';
				const char *range = ptr + 1;
				if (strcmp(range, "full") == 0) {
					interpretation.full_range = true;
				} else if (strcmp(range, "limited") == 0) {
					interpretation.full_range = false;
				} else {
					fprintf(stderr, "ERROR: Invalid Y'CbCr range '%s' (must be “full” or “limited”)\n", range);
					exit(1);
				}
			}

			if (strcmp(interpretation_str, "rec601") == 0) {
				interpretation.ycbcr_coefficients_auto = false;
				interpretation.ycbcr_coefficients = movit::YCBCR_REC_601;
			} else if (strcmp(interpretation_str, "rec709") == 0) {
				interpretation.ycbcr_coefficients_auto = false;
				interpretation.ycbcr_coefficients = movit::YCBCR_REC_709;
			} else if (strcmp(interpretation_str, "auto") == 0) {
				interpretation.ycbcr_coefficients_auto = true;
				if (interpretation.full_range) {
					fprintf(stderr, "ERROR: Cannot use “auto” Y'CbCr coefficients with full range\n");
					exit(1);
				}
			} else {
				fprintf(stderr, "ERROR: Invalid Y'CbCr coefficients '%s' (must be “rec601”, “rec709” or “auto”)\n", interpretation_str);
				exit(1);
			}
			global_flags.ycbcr_interpretation[card_num] = interpretation;
			break;
		}
		case OPTION_HELP:
			usage(program);
			exit(0);
		default:
			fprintf(stderr, "Unknown option '%s'\n", argv[option_index]);
			fprintf(stderr, "\n");
			usage(program);
			exit(1);
		}
	}

	if (global_flags.uncompressed_video_to_http &&
	    global_flags.x264_video_to_http) {
		fprintf(stderr, "ERROR: --http-uncompressed-video and --http-x264-video are mutually incompatible\n");
		exit(1);
	}
	if (global_flags.num_cards <= 0) {
		fprintf(stderr, "ERROR: --num-cards must be at least 1\n");
		exit(1);
	}
	if (global_flags.output_card < -1 ||
	    global_flags.output_card >= global_flags.num_cards) {
		fprintf(stderr, "ERROR: --output-card points to a nonexistant card\n");
		exit(1);
	}
	if (!global_flags.transcode_audio && global_flags.stream_audio_codec_name.empty()) {
		fprintf(stderr, "ERROR: If not transcoding audio, you must specify ahead-of-time what audio codec is in use\n");
		fprintf(stderr, "       (using --http-audio-codec).\n");
		exit(1);
	}
	if (global_flags.x264_speedcontrol) {
		if (!global_flags.x264_preset.empty() && global_flags.x264_preset != "faster") {
			fprintf(stderr, "WARNING: --x264-preset is overridden by --x264-speedcontrol (implicitly uses \"faster\" as base preset)\n");
		}
		global_flags.x264_preset = "faster";
	} else if (global_flags.x264_preset.empty()) {
		global_flags.x264_preset = X264_DEFAULT_PRESET;
	}
	if (!theme_dirs.empty()) {
		global_flags.theme_dirs = theme_dirs;
	}

	// In reality, we could probably do with any even value (we subsample
	// by two in some places), but it's better to be on the safe side
	// wrt. video codecs and such. (I'd set 16 if I could, but 1080 isn't
	// divisible by 16.)
	if (global_flags.width <= 0 || (global_flags.width % 8) != 0 ||
	    global_flags.height <= 0 || (global_flags.height % 8) != 0) {
		fprintf(stderr, "ERROR: --width and --height must be positive integers divisible by 8\n");
		exit(1);
	}

	for (pair<int, int> mapping : global_flags.default_stream_mapping) {
		if (mapping.second >= global_flags.num_cards) {
			fprintf(stderr, "ERROR: Signal %d mapped to card %d, which doesn't exist (try adjusting --num-cards)\n",
				mapping.first, mapping.second);
			exit(1);
		}
	}

	// Rec. 709 would be the sane thing to do, but it seems many players
	// just default to BT.601 coefficients no matter what. We _do_ set
	// the right flags, so that a player that works properly doesn't have
	// to guess, but it's frequently ignored. See discussions
	// in e.g. https://trac.ffmpeg.org/ticket/4978; the situation with
	// browsers is complicated and depends on things like hardware acceleration
	// (https://bugs.chromium.org/p/chromium/issues/detail?id=333619 for
	// extensive discussion). VLC generally fixed this as part of 3.0.0
	// (see e.g. https://github.com/videolan/vlc/commit/bc71288b2e38c07d6921472824b92eef1aa85f7e
	// and https://github.com/videolan/vlc/commit/c3fc2683a9cde1d42674ebf9935dced05733a215),
	// but earlier versions were pretty random.
	//
	// On the other hand, HDMI/SDI output typically requires Rec. 709 for
	// HD resolutions (with no way of signaling anything else), which is
	// a conflicting demand. In this case, we typically let the HDMI/SDI
	// output win if it is active, but the user can override this.
	if (output_ycbcr_coefficients == "auto") {
		// Essentially: BT.709 if HDMI/SDI output is on, otherwise BT.601.
		global_flags.ycbcr_rec709_coefficients = false;
		global_flags.ycbcr_auto_coefficients = true;
	} else if (output_ycbcr_coefficients == "rec709") {
		global_flags.ycbcr_rec709_coefficients = true;
		global_flags.ycbcr_auto_coefficients = false;
	} else if (output_ycbcr_coefficients == "rec601") {
		global_flags.ycbcr_rec709_coefficients = false;
		global_flags.ycbcr_auto_coefficients = false;
	} else {
		fprintf(stderr, "ERROR: --output-ycbcr-coefficients must be “rec601”, “rec709” or “auto”\n");
		exit(1);
	}

	if (global_flags.output_buffer_frames < 0.0f) {
		// Actually, even zero probably won't make sense; there is some internal
		// delay to the card.
		fprintf(stderr, "ERROR: --output-buffer-frames can't be negative.\n");
		exit(1);
	}
	if (global_flags.output_slop_frames < 0.0f) {
		fprintf(stderr, "ERROR: --output-slop-frames can't be negative.\n");
		exit(1);
	}
	if (global_flags.max_input_queue_frames < 1) {
		fprintf(stderr, "ERROR: --max-input-queue-frames must be at least 1.\n");
		exit(1);
	}
	if (global_flags.max_input_queue_frames > 10) {
		fprintf(stderr, "WARNING: --max-input-queue-frames has little effect over 10.\n");
	}

	if (!isinf(global_flags.x264_crf)) {  // CRF mode is selected.
		if (global_flags.x264_bitrate != -1) {
			fprintf(stderr, "ERROR: --x264-bitrate and --x264-crf are mutually incompatible.\n");
			exit(1);
		}
		if (global_flags.x264_vbv_max_bitrate != -1 && global_flags.x264_vbv_buffer_size != -1) {
			fprintf(stderr, "WARNING: VBV settings are ignored with --x264-crf.\n");
		}
	} else if (global_flags.x264_bitrate == -1) {
		global_flags.x264_bitrate = DEFAULT_X264_OUTPUT_BIT_RATE;
	}
}
