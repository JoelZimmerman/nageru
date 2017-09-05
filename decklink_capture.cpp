#include "decklink_capture.h"

#include <DeckLinkAPI.h>
#include <DeckLinkAPIConfiguration.h>
#include <DeckLinkAPIDiscovery.h>
#include <DeckLinkAPIModes.h>
#include <assert.h>
#ifdef __SSE2__
#include <immintrin.h>
#endif
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <chrono>
#include <cstdint>
#include <utility>
#include <vector>

#include "bmusb/bmusb.h"
#include "decklink_util.h"
#include "flags.h"
#include "v210_converter.h"

#define FRAME_SIZE (8 << 20)  // 8 MB.

using namespace std;
using namespace std::chrono;
using namespace std::placeholders;
using namespace bmusb;

namespace {

// TODO: Support stride.
void memcpy_interleaved(uint8_t *dest1, uint8_t *dest2, const uint8_t *src, size_t n)
{
	assert(n % 2 == 0);
	uint8_t *dptr1 = dest1;
	uint8_t *dptr2 = dest2;

	for (size_t i = 0; i < n; i += 2) {
		*dptr1++ = *src++;
		*dptr2++ = *src++;
	}
}

#ifdef __SSE2__

// Returns the number of bytes consumed.
size_t memcpy_interleaved_fastpath(uint8_t *dest1, uint8_t *dest2, const uint8_t *src, size_t n)
{
	const uint8_t *limit = src + n;
	size_t consumed = 0;

	// Align end to 32 bytes.
	limit = (const uint8_t *)(intptr_t(limit) & ~31);

	if (src >= limit) {
		return 0;
	}

	// Process [0,31] bytes, such that start gets aligned to 32 bytes.
	const uint8_t *aligned_src = (const uint8_t *)(intptr_t(src + 31) & ~31);
	if (aligned_src != src) {
		size_t n2 = aligned_src - src;
		memcpy_interleaved(dest1, dest2, src, n2);
		dest1 += n2 / 2;
		dest2 += n2 / 2;
		if (n2 % 2) {
			swap(dest1, dest2);
		}
		src = aligned_src;
		consumed += n2;
	}

	// Make the length a multiple of 64.
	if (((limit - src) % 64) != 0) {
		limit -= 32;
	}
	assert(((limit - src) % 64) == 0);

#if __AVX2__
	const __m256i * __restrict in = (const __m256i *)src;
	__m256i * __restrict out1 = (__m256i *)dest1;
	__m256i * __restrict out2 = (__m256i *)dest2;

	__m256i shuffle_cw = _mm256_set_epi8(
		15, 13, 11, 9, 7, 5, 3, 1, 14, 12, 10, 8, 6, 4, 2, 0,
		15, 13, 11, 9, 7, 5, 3, 1, 14, 12, 10, 8, 6, 4, 2, 0);
	while (in < (const __m256i *)limit) {
		// Note: For brevity, comments show lanes as if they were 2x64-bit (they're actually 2x128).
		__m256i data1 = _mm256_stream_load_si256(in);         // AaBbCcDd EeFfGgHh
		__m256i data2 = _mm256_stream_load_si256(in + 1);     // IiJjKkLl MmNnOoPp

		data1 = _mm256_shuffle_epi8(data1, shuffle_cw);       // ABCDabcd EFGHefgh
		data2 = _mm256_shuffle_epi8(data2, shuffle_cw);       // IJKLijkl MNOPmnop
	
		data1 = _mm256_permute4x64_epi64(data1, 0b11011000);  // ABCDEFGH abcdefgh
		data2 = _mm256_permute4x64_epi64(data2, 0b11011000);  // IJKLMNOP ijklmnop

		__m256i lo = _mm256_permute2x128_si256(data1, data2, 0b00100000);
		__m256i hi = _mm256_permute2x128_si256(data1, data2, 0b00110001);

		_mm256_storeu_si256(out1, lo);
		_mm256_storeu_si256(out2, hi);

		in += 2;
		++out1;
		++out2;
		consumed += 64;
	}
#else
	const __m128i * __restrict in = (const __m128i *)src;
	__m128i * __restrict out1 = (__m128i *)dest1;
	__m128i * __restrict out2 = (__m128i *)dest2;

	__m128i mask_lower_byte = _mm_set1_epi16(0x00ff);
	while (in < (const __m128i *)limit) {
		__m128i data1 = _mm_load_si128(in);
		__m128i data2 = _mm_load_si128(in + 1);
		__m128i data1_lo = _mm_and_si128(data1, mask_lower_byte);
		__m128i data2_lo = _mm_and_si128(data2, mask_lower_byte);
		__m128i data1_hi = _mm_srli_epi16(data1, 8);
		__m128i data2_hi = _mm_srli_epi16(data2, 8);
		__m128i lo = _mm_packus_epi16(data1_lo, data2_lo);
		_mm_storeu_si128(out1, lo);
		__m128i hi = _mm_packus_epi16(data1_hi, data2_hi);
		_mm_storeu_si128(out2, hi);

		in += 2;
		++out1;
		++out2;
		consumed += 32;
	}
#endif

	return consumed;
}

#endif  // __SSE2__

BMDPixelFormat pixel_format_to_bmd(PixelFormat pixel_format)
{
	switch (pixel_format) {
	case PixelFormat_8BitYCbCr:
		return bmdFormat8BitYUV;
	case PixelFormat_10BitYCbCr:
		return bmdFormat10BitYUV;
	default:
		assert(false);
	}
}

}  // namespace

DeckLinkCapture::DeckLinkCapture(IDeckLink *card, int card_index)
	: card_index(card_index), card(card)
{
	{
		const char *model_name;
		char buf[256];
		if (card->GetModelName(&model_name) == S_OK) {
			snprintf(buf, sizeof(buf), "PCI card %d: %s", card_index, model_name);
		} else {
			snprintf(buf, sizeof(buf), "PCI card %d: Unknown DeckLink card", card_index);
		}
		description = buf;
	}

	if (card->QueryInterface(IID_IDeckLinkInput, (void**)&input) != S_OK) {
		fprintf(stderr, "Card %d has no inputs\n", card_index);
		exit(1);
	}

	IDeckLinkAttributes *attr;
	if (card->QueryInterface(IID_IDeckLinkAttributes, (void**)&attr) != S_OK) {
		fprintf(stderr, "Card %d has no attributes\n", card_index);
		exit(1);
	}

	// Get the list of available video inputs.
	int64_t video_input_mask;
	if (attr->GetInt(BMDDeckLinkVideoInputConnections, &video_input_mask) != S_OK) {
		fprintf(stderr, "Failed to enumerate video inputs for card %d\n", card_index);
		exit(1);
	}
	const vector<pair<BMDVideoConnection, string>> video_input_types = {
		{ bmdVideoConnectionSDI, "SDI" },
		{ bmdVideoConnectionHDMI, "HDMI" },
		{ bmdVideoConnectionOpticalSDI, "Optical SDI" },
		{ bmdVideoConnectionComponent, "Component" },
		{ bmdVideoConnectionComposite, "Composite" },
		{ bmdVideoConnectionSVideo, "S-Video" }
	};
	for (const auto &video_input : video_input_types) {
		if (video_input_mask & video_input.first) {
			video_inputs.emplace(video_input.first, video_input.second);
		}
	}

	// And then the available audio inputs.
	int64_t audio_input_mask;
	if (attr->GetInt(BMDDeckLinkAudioInputConnections, &audio_input_mask) != S_OK) {
		fprintf(stderr, "Failed to enumerate audio inputs for card %d\n", card_index);
		exit(1);
	}
	const vector<pair<BMDAudioConnection, string>> audio_input_types = {
		{ bmdAudioConnectionEmbedded, "Embedded" },
		{ bmdAudioConnectionAESEBU, "AES/EBU" },
		{ bmdAudioConnectionAnalog, "Analog" },
		{ bmdAudioConnectionAnalogXLR, "Analog XLR" },
		{ bmdAudioConnectionAnalogRCA, "Analog RCA" },
		{ bmdAudioConnectionMicrophone, "Microphone" },
		{ bmdAudioConnectionHeadphones, "Headphones" }
	};
	for (const auto &audio_input : audio_input_types) {
		if (audio_input_mask & audio_input.first) {
			audio_inputs.emplace(audio_input.first, audio_input.second);
		}
	}

	// Check if we the card supports input autodetection.
	if (attr->GetFlag(BMDDeckLinkSupportsInputFormatDetection, &supports_autodetect) != S_OK) {
		fprintf(stderr, "Warning: Failed to ask card %d whether it supports input format autodetection\n", card_index);
		supports_autodetect = false;
	}

	// If there's more than one subdevice on this card, label them.
	int64_t num_subdevices, subdevice_idx;
	if (attr->GetInt(BMDDeckLinkNumberOfSubDevices, &num_subdevices) == S_OK && num_subdevices > 1) {
		if (attr->GetInt(BMDDeckLinkSubDeviceIndex, &subdevice_idx) == S_OK) {
			char buf[256];
			snprintf(buf, sizeof(buf), " (subdevice %d)", int(subdevice_idx));
			description += buf;
		}
	}

	attr->Release();

	/* Set up the video and audio sources. */
	if (card->QueryInterface(IID_IDeckLinkConfiguration, (void**)&config) != S_OK) {
		fprintf(stderr, "Failed to get configuration interface for card %d\n", card_index);
		exit(1);
	}

	BMDVideoConnection connection = pick_default_video_connection(card, BMDDeckLinkVideoInputConnections, card_index);

	set_video_input(connection);
	set_audio_input(bmdAudioConnectionEmbedded);

	IDeckLinkDisplayModeIterator *mode_it;
	if (input->GetDisplayModeIterator(&mode_it) != S_OK) {
		fprintf(stderr, "Failed to enumerate display modes for card %d\n", card_index);
		exit(1);
	}

	video_modes = summarize_video_modes(mode_it, card_index);
	mode_it->Release();

	set_video_mode_no_restart(bmdModeHD720p5994);

	input->SetCallback(this);
}

DeckLinkCapture::~DeckLinkCapture()
{
	if (has_dequeue_callbacks) {
		dequeue_cleanup_callback();
	}
	input->Release();
	config->Release();
	card->Release();
}

HRESULT STDMETHODCALLTYPE DeckLinkCapture::QueryInterface(REFIID, LPVOID *)
{
	return E_NOINTERFACE;
}

ULONG STDMETHODCALLTYPE DeckLinkCapture::AddRef(void)
{
	return refcount.fetch_add(1) + 1;
}

ULONG STDMETHODCALLTYPE DeckLinkCapture::Release(void)
{
	int new_ref = refcount.fetch_sub(1) - 1;
	if (new_ref == 0)
		delete this;
	return new_ref;
}

HRESULT STDMETHODCALLTYPE DeckLinkCapture::VideoInputFormatChanged(
	BMDVideoInputFormatChangedEvents,
	IDeckLinkDisplayMode* display_mode,
	BMDDetectedVideoInputFormatFlags format_flags)
{
	if (format_flags & bmdDetectedVideoInputRGB444) {
		fprintf(stderr, "WARNING: Input detected as 4:4:4 RGB, but Nageru can't consume that yet.\n");
		fprintf(stderr, "Doing hardware conversion to 4:2:2 Y'CbCr.\n");
	}
	if (supports_autodetect && display_mode->GetDisplayMode() != current_video_mode) {
		set_video_mode(display_mode->GetDisplayMode());
	}
	if (display_mode->GetFrameRate(&frame_duration, &time_scale) != S_OK) {
		fprintf(stderr, "Could not get new frame rate\n");
		exit(1);
	}
	field_dominance = display_mode->GetFieldDominance();
	return S_OK;
}

HRESULT STDMETHODCALLTYPE DeckLinkCapture::VideoInputFrameArrived(
	IDeckLinkVideoInputFrame *video_frame,
	IDeckLinkAudioInputPacket *audio_frame)
{
	if (!done_init) {
		char thread_name[16];
		snprintf(thread_name, sizeof(thread_name), "DeckLink_C_%d", card_index);
		pthread_setname_np(pthread_self(), thread_name);

		sched_param param;
		memset(&param, 0, sizeof(param));
		param.sched_priority = 1;
		if (sched_setscheduler(0, SCHED_RR, &param) == -1) {
			printf("couldn't set realtime priority for DeckLink thread: %s\n", strerror(errno));
		}

		if (has_dequeue_callbacks) {
			dequeue_init_callback();
		}
		done_init = true;
	}

	steady_clock::time_point now = steady_clock::now();

	FrameAllocator::Frame current_video_frame, current_audio_frame;
	VideoFormat video_format;
	AudioFormat audio_format;

	video_format.frame_rate_nom = time_scale;
	video_format.frame_rate_den = frame_duration;
	// TODO: Respect the TFF/BFF flag.
	video_format.interlaced = (field_dominance == bmdLowerFieldFirst || field_dominance == bmdUpperFieldFirst);
	video_format.second_field_start = 1;

	if (video_frame) {
		video_format.has_signal = !(video_frame->GetFlags() & bmdFrameHasNoInputSource);

		const int width = video_frame->GetWidth();
		const int height = video_frame->GetHeight();
		const int stride = video_frame->GetRowBytes();
		const BMDPixelFormat format = video_frame->GetPixelFormat();
		assert(format == pixel_format_to_bmd(current_pixel_format));
		if (global_flags.ten_bit_input) {
			assert(stride == int(v210Converter::get_v210_stride(width)));
		} else {
			assert(stride == width * 2);
		}

		current_video_frame = video_frame_allocator->alloc_frame();
		if (current_video_frame.data != nullptr) {
			const uint8_t *frame_bytes;
			video_frame->GetBytes((void **)&frame_bytes);
			size_t num_bytes = stride * height;

			if (current_video_frame.interleaved) {
				uint8_t *data = current_video_frame.data;
				uint8_t *data2 = current_video_frame.data2;
#ifdef __SSE2__
				size_t consumed = memcpy_interleaved_fastpath(data, data2, frame_bytes, num_bytes);
				frame_bytes += consumed;
				data += consumed / 2;
				data2 += consumed / 2;
				if (num_bytes % 2) {
					swap(data, data2);
				}
				current_video_frame.len += consumed;
				num_bytes -= consumed;
#endif

				if (num_bytes > 0) {
					memcpy_interleaved(data, data2, frame_bytes, num_bytes);
				}
			} else {
				memcpy(current_video_frame.data, frame_bytes, num_bytes);
			}
			current_video_frame.len += num_bytes;

			video_format.width = width;
			video_format.height = height;
			video_format.stride = stride;
		}
	}

	if (audio_frame) {
		int num_samples = audio_frame->GetSampleFrameCount();

		current_audio_frame = audio_frame_allocator->alloc_frame();
		if (current_audio_frame.data != nullptr) {
			const uint8_t *frame_bytes;
			audio_frame->GetBytes((void **)&frame_bytes);
			current_audio_frame.len = sizeof(int32_t) * 2 * num_samples;

			memcpy(current_audio_frame.data, frame_bytes, current_audio_frame.len);

			audio_format.bits_per_sample = 32;
			audio_format.num_channels = 2;
		}
	}

	current_video_frame.received_timestamp = now;
	current_audio_frame.received_timestamp = now;

	if (current_video_frame.data != nullptr || current_audio_frame.data != nullptr) {
		// TODO: Put into a queue and put into a dequeue thread, if the
		// BlackMagic drivers don't already do that for us?
		frame_callback(timecode,
			current_video_frame, /*video_offset=*/0, video_format,
			current_audio_frame, /*audio_offset=*/0, audio_format);
	}

	timecode++;
	return S_OK;
}

void DeckLinkCapture::configure_card()
{
	if (video_frame_allocator == nullptr) {
		owned_video_frame_allocator.reset(new MallocFrameAllocator(FRAME_SIZE, NUM_QUEUED_VIDEO_FRAMES));
		set_video_frame_allocator(owned_video_frame_allocator.get());
	}
	if (audio_frame_allocator == nullptr) {
		owned_audio_frame_allocator.reset(new MallocFrameAllocator(65536, NUM_QUEUED_AUDIO_FRAMES));
		set_audio_frame_allocator(owned_audio_frame_allocator.get());
	}
}

void DeckLinkCapture::start_bm_capture()
{
	if (running) {
		return;
	}
	if (input->EnableVideoInput(current_video_mode, pixel_format_to_bmd(current_pixel_format), supports_autodetect ? bmdVideoInputEnableFormatDetection : 0) != S_OK) {
		fprintf(stderr, "Failed to set video mode 0x%04x for card %d\n", current_video_mode, card_index);
		exit(1);
	}
	if (input->EnableAudioInput(48000, bmdAudioSampleType32bitInteger, 2) != S_OK) {
		fprintf(stderr, "Failed to enable audio input for card %d\n", card_index);
		exit(1);
	}

	if (input->StartStreams() != S_OK) {
		fprintf(stderr, "StartStreams failed\n");
		exit(1);
	}
	running = true;
}

void DeckLinkCapture::stop_dequeue_thread()
{
	if (!running) {
		return;
	}
	HRESULT result = input->StopStreams();
	if (result != S_OK) {
		fprintf(stderr, "StopStreams failed with error 0x%x\n", result);
		exit(1);
	}
	if (input->DisableVideoInput() != S_OK) {
		fprintf(stderr, "Failed to disable video input for card %d\n", card_index);
		exit(1);
	}
	if (input->DisableAudioInput() != S_OK) {
		fprintf(stderr, "Failed to disable audio input for card %d\n", card_index);
		exit(1);
	}
	running = false;
}

void DeckLinkCapture::set_video_mode(uint32_t video_mode_id)
{
	if (running) {
		if (input->PauseStreams() != S_OK) {
			fprintf(stderr, "PauseStreams failed\n");
			exit(1);
		}
		if (input->FlushStreams() != S_OK) {
			fprintf(stderr, "FlushStreams failed\n");
			exit(1);
		}
	}

	set_video_mode_no_restart(video_mode_id);

	if (running) {
		if (input->StartStreams() != S_OK) {
			fprintf(stderr, "StartStreams failed\n");
			exit(1);
		}
	}
}

void DeckLinkCapture::set_pixel_format(PixelFormat pixel_format)
{
	current_pixel_format = pixel_format;
	set_video_mode(current_video_mode);
}

void DeckLinkCapture::set_video_mode_no_restart(uint32_t video_mode_id)
{
	BMDDisplayModeSupport support;
	IDeckLinkDisplayMode *display_mode;
	if (input->DoesSupportVideoMode(video_mode_id, pixel_format_to_bmd(current_pixel_format), /*flags=*/0, &support, &display_mode)) {
		fprintf(stderr, "Failed to query display mode for card %d\n", card_index);
		exit(1);
	}

	if (support == bmdDisplayModeNotSupported) {
		fprintf(stderr, "Card %d does not support display mode\n", card_index);
		exit(1);
	}

	if (display_mode->GetFrameRate(&frame_duration, &time_scale) != S_OK) {
		fprintf(stderr, "Could not get frame rate for card %d\n", card_index);
		exit(1);
	}

	field_dominance = display_mode->GetFieldDominance();

	if (running) {
		if (input->EnableVideoInput(video_mode_id, pixel_format_to_bmd(current_pixel_format), supports_autodetect ? bmdVideoInputEnableFormatDetection : 0) != S_OK) {
			fprintf(stderr, "Failed to set video mode 0x%04x for card %d\n", video_mode_id, card_index);
			exit(1);
		}
	}

	current_video_mode = video_mode_id;
}

void DeckLinkCapture::set_video_input(uint32_t video_input_id)
{
	if (config->SetInt(bmdDeckLinkConfigVideoInputConnection, video_input_id) != S_OK) {
		fprintf(stderr, "Failed to set video input connection for card %d\n", card_index);
		exit(1);
	}

	current_video_input = video_input_id;
}

void DeckLinkCapture::set_audio_input(uint32_t audio_input_id)
{
	if (config->SetInt(bmdDeckLinkConfigAudioInputConnection, audio_input_id) != S_OK) {
		fprintf(stderr, "Failed to set audio input connection for card %d\n", card_index);
		exit(1);
	}

	current_audio_input = audio_input_id;
}
