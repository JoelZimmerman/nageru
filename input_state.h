#ifndef _INPUT_STATE_H
#define _INPUT_STATE_H 1

#include <movit/image_format.h>

#include "defs.h"
#include "ref_counted_frame.h"

struct BufferedFrame {
	RefCountedFrame frame;
	unsigned field_number;
};

// Encapsulates the state of all inputs at any given instant.
// In particular, this is captured by Theme::get_chain(),
// so that it can hold on to all the frames it needs for rendering.
struct InputState {
	// For each card, the last five frames (or fields), with 0 being the
	// most recent one. Note that we only need the actual history if we have
	// interlaced output (for deinterlacing), so if we detect progressive input,
	// we immediately clear out all history and all entries will point to the same
	// frame.
	BufferedFrame buffered_frames[MAX_VIDEO_CARDS][FRAME_HISTORY_LENGTH];

	// For each card, the current Y'CbCr input settings. Ignored for BGRA inputs.
	// If ycbcr_coefficients_auto = true for a given card, the others are ignored
	// for that card (SD is taken to be Rec. 601, HD is taken to be Rec. 709,
	// both limited range).
	bool ycbcr_coefficients_auto[MAX_VIDEO_CARDS];
	movit::YCbCrLumaCoefficients ycbcr_coefficients[MAX_VIDEO_CARDS];
	bool full_range[MAX_VIDEO_CARDS];
};

#endif  // !defined(_INPUT_STATE_H)
