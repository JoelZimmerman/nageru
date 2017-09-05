#ifndef _STEREOCOMPRESSOR_H
#define _STEREOCOMPRESSOR_H 1

#include <stddef.h>
// A simple compressor based on absolute values, with independent
// attack/release times. There is no sidechain or lookahead, but the
// peak value is shared between both channels.
//
// The compressor was originally written by, and is copyrighted by, Rune Holm.
// It has been adapted and relicensed under GPLv3 (or, at your option,
// any later version) for Nageru, so that its license matches the rest of the code.

class StereoCompressor {
public:
	StereoCompressor(float sample_rate)
		: sample_rate(sample_rate) {
		reset();
	}

	void reset() {
		peak_level = compr_level = 0.1f;
		scalefactor = 0.0f;
	}

	// Process <num_samples> interleaved stereo data in-place.
	// Attack and release times are in seconds.
	void process(float *buf, size_t num_samples, float threshold, float ratio,
	             float attack_time, float release_time, float makeup_gain);

	// Last level estimated (after attack/decay applied).
	float get_level() { return compr_level; }

	// Last attenuation factor applied, e.g. if 5x compression is currently applied,
	// this number will be 0.2.
	float get_attenuation() { return scalefactor; }

private:
	float sample_rate;
	float peak_level;
	float compr_level;
	float scalefactor;
};

#endif /* !defined(_STEREOCOMPRESSOR_H) */
