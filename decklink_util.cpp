#include <DeckLinkAPI.h>
#include <DeckLinkAPIModes.h>

#include <assert.h>

#include "decklink_util.h"

using namespace bmusb;
using namespace std;

map<uint32_t, VideoMode> summarize_video_modes(IDeckLinkDisplayModeIterator *mode_it, unsigned card_index)
{
	map<uint32_t, VideoMode> video_modes;

	for (IDeckLinkDisplayMode *mode_ptr; mode_it->Next(&mode_ptr) == S_OK; mode_ptr->Release()) {
		VideoMode mode;

		const char *mode_name;
		if (mode_ptr->GetName(&mode_name) != S_OK) {
			mode.name = "Unknown mode";
		} else {
			mode.name = mode_name;
			free((char *)mode_name);
		}

		mode.autodetect = false;

		mode.width = mode_ptr->GetWidth();
		mode.height = mode_ptr->GetHeight();

		BMDTimeScale frame_rate_num;
		BMDTimeValue frame_rate_den;
		if (mode_ptr->GetFrameRate(&frame_rate_den, &frame_rate_num) != S_OK) {
			fprintf(stderr, "Could not get frame rate for mode '%s' on card %d\n", mode.name.c_str(), card_index);
			exit(1);
		}
		mode.frame_rate_num = frame_rate_num;
		mode.frame_rate_den = frame_rate_den;

		// TODO: Respect the TFF/BFF flag.
		mode.interlaced = (mode_ptr->GetFieldDominance() == bmdLowerFieldFirst || mode_ptr->GetFieldDominance() == bmdUpperFieldFirst);

		uint32_t id = mode_ptr->GetDisplayMode();
		video_modes.insert(make_pair(id, mode));
	}

	return video_modes;
}

BMDVideoConnection pick_default_video_connection(IDeckLink *card, BMDDeckLinkAttributeID attribute_id, unsigned card_index)
{
	assert(attribute_id == BMDDeckLinkVideoInputConnections ||
	       attribute_id == BMDDeckLinkVideoOutputConnections);

	IDeckLinkAttributes *attr;
	if (card->QueryInterface(IID_IDeckLinkAttributes, (void**)&attr) != S_OK) {
		fprintf(stderr, "Card %u has no attributes\n", card_index);
		exit(1);
	}

	int64_t connection_mask;
	if (attr->GetInt(attribute_id, &connection_mask) != S_OK) {
		if (attribute_id == BMDDeckLinkVideoInputConnections) {
			fprintf(stderr, "Failed to enumerate video inputs for card %u\n", card_index);
		} else {
			fprintf(stderr, "Failed to enumerate video outputs for card %u\n", card_index);
		}
		exit(1);
	}
	attr->Release();
	if (connection_mask == 0) {
		if (attribute_id == BMDDeckLinkVideoInputConnections) {
			fprintf(stderr, "Card %u has no input connections\n", card_index);
		} else {
			fprintf(stderr, "Card %u has no outpu connectionss\n", card_index);
		}
		exit(1);
	}

	if (connection_mask & bmdVideoConnectionHDMI) {
		return bmdVideoConnectionHDMI;
	} else if (connection_mask & bmdVideoConnectionSDI) {
		return bmdVideoConnectionSDI;
	} else {
		// Fallback: Return lowest-set bit, whatever that might be.
		return connection_mask & (-connection_mask);
	}
}
