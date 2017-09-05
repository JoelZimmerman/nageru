#ifndef _DECKLINK_CAPTURE_H
#define _DECKLINK_CAPTURE_H 1

#include <DeckLinkAPI.h>
#include <stdint.h>
#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>

#include "DeckLinkAPIModes.h"
#include "DeckLinkAPITypes.h"
#include "LinuxCOM.h"
#include "bmusb/bmusb.h"

class IDeckLink;
class IDeckLinkConfiguration;

// TODO: Adjust CaptureInterface to be a little less bmusb-centric.
// There are too many member functions here that don't really do anything.
class DeckLinkCapture : public bmusb::CaptureInterface, IDeckLinkInputCallback
{
public:
	DeckLinkCapture(IDeckLink *card, int card_index);  // Takes ownership of <card>.
	~DeckLinkCapture();

	// IDeckLinkInputCallback.
	HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, LPVOID *) override;
	ULONG STDMETHODCALLTYPE AddRef() override;
	ULONG STDMETHODCALLTYPE Release() override;
	HRESULT STDMETHODCALLTYPE VideoInputFormatChanged(
		BMDVideoInputFormatChangedEvents,
		IDeckLinkDisplayMode*,
		BMDDetectedVideoInputFormatFlags) override;
	HRESULT STDMETHODCALLTYPE VideoInputFrameArrived(
		IDeckLinkVideoInputFrame *video_frame,
		IDeckLinkAudioInputPacket *audio_frame) override;

	// CaptureInterface.
	void set_video_frame_allocator(bmusb::FrameAllocator *allocator) override
	{
		video_frame_allocator = allocator;
		if (owned_video_frame_allocator.get() != allocator) {
			owned_video_frame_allocator.reset();
		}
	}

	bmusb::FrameAllocator *get_video_frame_allocator() override
	{
		return video_frame_allocator;
	}

	// Does not take ownership.
	void set_audio_frame_allocator(bmusb::FrameAllocator *allocator) override
	{
		audio_frame_allocator = allocator;
		if (owned_audio_frame_allocator.get() != allocator) {
			owned_audio_frame_allocator.reset();
		}
	}

	bmusb::FrameAllocator *get_audio_frame_allocator() override
	{
		return audio_frame_allocator;
	}

	void set_frame_callback(bmusb::frame_callback_t callback) override
	{
		frame_callback = callback;
	}

	void set_dequeue_thread_callbacks(std::function<void()> init, std::function<void()> cleanup) override
	{
		dequeue_init_callback = init;
		dequeue_cleanup_callback = cleanup;
		has_dequeue_callbacks = true;
	}

	std::string get_description() const override
	{
		return description;
	}

	void configure_card() override;
	void start_bm_capture() override;
	void stop_dequeue_thread() override;

	// TODO: Can the API communicate this to us somehow, for e.g. Thunderbolt cards?
	bool get_disconnected() const override { return false; }

	std::map<uint32_t, bmusb::VideoMode> get_available_video_modes() const override { return video_modes; }
	void set_video_mode(uint32_t video_mode_id) override;
	uint32_t get_current_video_mode() const override { return current_video_mode; }

	std::set<bmusb::PixelFormat> get_available_pixel_formats() const override {
		return std::set<bmusb::PixelFormat>{ bmusb::PixelFormat_8BitYCbCr, bmusb::PixelFormat_10BitYCbCr };
	}
	void set_pixel_format(bmusb::PixelFormat pixel_format) override;
	bmusb::PixelFormat get_current_pixel_format() const override {
		return current_pixel_format;
	}

	std::map<uint32_t, std::string> get_available_video_inputs() const override { return video_inputs; }
	void set_video_input(uint32_t video_input_id) override;
	uint32_t get_current_video_input() const override { return current_video_input; }

	std::map<uint32_t, std::string> get_available_audio_inputs() const override { return audio_inputs; }
	void set_audio_input(uint32_t audio_input_id) override;
	uint32_t get_current_audio_input() const override { return current_audio_input; }

private:
	void set_video_mode_no_restart(uint32_t video_mode_id);

	std::atomic<int> refcount{1};
	bool done_init = false;
	std::string description;
	uint16_t timecode = 0;
	int card_index;

	bool has_dequeue_callbacks = false;
	std::function<void()> dequeue_init_callback = nullptr;
	std::function<void()> dequeue_cleanup_callback = nullptr;

	bmusb::FrameAllocator *video_frame_allocator = nullptr;
	bmusb::FrameAllocator *audio_frame_allocator = nullptr;
	std::unique_ptr<bmusb::FrameAllocator> owned_video_frame_allocator;
	std::unique_ptr<bmusb::FrameAllocator> owned_audio_frame_allocator;
	bmusb::frame_callback_t frame_callback = nullptr;

	IDeckLinkConfiguration *config = nullptr;

	IDeckLink *card = nullptr;
	IDeckLinkInput *input = nullptr;
	BMDTimeValue frame_duration;
	BMDTimeScale time_scale;
	BMDFieldDominance field_dominance;
	bool running = false;
	bool supports_autodetect = false;

	std::map<uint32_t, bmusb::VideoMode> video_modes;
	BMDDisplayMode current_video_mode;
	bmusb::PixelFormat current_pixel_format = bmusb::PixelFormat_8BitYCbCr;

	std::map<uint32_t, std::string> video_inputs;
	BMDVideoConnection current_video_input;

	std::map<uint32_t, std::string> audio_inputs;
	BMDAudioConnection current_audio_input;
};

#endif  // !defined(_DECKLINK_CAPTURE_H)
