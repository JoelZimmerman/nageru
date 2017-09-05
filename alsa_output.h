#ifndef _ALSA_OUTPUT_H
#define _ALSA_OUTPUT_H 1

// Extremely minimalistic ALSA output. Will not resample to fit
// sound card clock, will not care much about over- or underflows
// (so it will not block), will not care about A/V sync.
//
// This means that if you run it for long enough, clocks will
// probably drift out of sync enough to make a little pop.

#include <alsa/asoundlib.h>
#include <vector>

class ALSAOutput {
public:
	ALSAOutput(int sample_rate, int num_channels);
	void write(const std::vector<float> &samples);

private:
	snd_pcm_t *pcm_handle;
	std::vector<float> buffer;
	snd_pcm_uframes_t period_size;
	int sample_rate, num_channels;
};

#endif  // !defined(_ALSA_OUTPUT_H)
