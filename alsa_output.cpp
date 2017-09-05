#include "alsa_output.h"

#include <alsa/asoundlib.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <vector>

using namespace std;

namespace {

void die_on_error(const char *func_name, int err)
{
	if (err < 0) {
		fprintf(stderr, "%s: %s\n", func_name, snd_strerror(err));
		exit(1);
	}
}

}  // namespace

ALSAOutput::ALSAOutput(int sample_rate, int num_channels)
	: sample_rate(sample_rate), num_channels(num_channels)
{
	die_on_error("snd_pcm_open()", snd_pcm_open(&pcm_handle, "default", SND_PCM_STREAM_PLAYBACK, 0));

	// Set format.
	snd_pcm_hw_params_t *hw_params;
	snd_pcm_hw_params_alloca(&hw_params);
	die_on_error("snd_pcm_hw_params_any()", snd_pcm_hw_params_any(pcm_handle, hw_params));
	die_on_error("snd_pcm_hw_params_set_access()", snd_pcm_hw_params_set_access(pcm_handle, hw_params, SND_PCM_ACCESS_RW_INTERLEAVED));
	die_on_error("snd_pcm_hw_params_set_format()", snd_pcm_hw_params_set_format(pcm_handle, hw_params, SND_PCM_FORMAT_FLOAT_LE));
	die_on_error("snd_pcm_hw_params_set_rate()", snd_pcm_hw_params_set_rate(pcm_handle, hw_params, sample_rate, 0));
	die_on_error("snd_pcm_hw_params_set_channels", snd_pcm_hw_params_set_channels(pcm_handle, hw_params, num_channels));

	// Fragment size of 512 samples. (A frame at 60 fps/48 kHz is 800 samples.)
	// We ask for 16 such periods (~170 ms buffer).
	unsigned int num_periods = 16;
	int dir = 0;
	die_on_error("snd_pcm_hw_params_set_periods_near()", snd_pcm_hw_params_set_periods_near(pcm_handle, hw_params, &num_periods, &dir));
	period_size = 512;
	dir = 0;
	die_on_error("snd_pcm_hw_params_set_period_size_near()", snd_pcm_hw_params_set_period_size_near(pcm_handle, hw_params, &period_size, &dir));
	die_on_error("snd_pcm_hw_params()", snd_pcm_hw_params(pcm_handle, hw_params));
	//snd_pcm_hw_params_free(hw_params);

	snd_pcm_sw_params_t *sw_params;
	snd_pcm_sw_params_alloca(&sw_params);
	die_on_error("snd_pcm_sw_params_current()", snd_pcm_sw_params_current(pcm_handle, sw_params));
	die_on_error("snd_pcm_sw_params_set_start_threshold", snd_pcm_sw_params_set_start_threshold(pcm_handle, sw_params, num_periods * period_size / 2));
	die_on_error("snd_pcm_sw_params()", snd_pcm_sw_params(pcm_handle, sw_params));

	die_on_error("snd_pcm_nonblock", snd_pcm_nonblock(pcm_handle, 1));
	die_on_error("snd_pcm_prepare()", snd_pcm_prepare(pcm_handle));
}

void ALSAOutput::write(const vector<float> &samples)
{
	buffer.insert(buffer.end(), samples.begin(), samples.end());

try_again:
	int periods_to_write = buffer.size() / (period_size * num_channels);
	if (periods_to_write == 0) {
		return;
	}

	int ret = snd_pcm_writei(pcm_handle, buffer.data(), periods_to_write * period_size);
	if (ret == -EPIPE) {
		fprintf(stderr, "warning: snd_pcm_writei() reported underrun\n");
		snd_pcm_recover(pcm_handle, ret, 1);
		goto try_again;
	} else if (ret == -EAGAIN) {
		ret = 0;
	} else if (ret < 0) {
		fprintf(stderr, "error: snd_pcm_writei() returned '%s'\n", snd_strerror(ret));
		exit(1);
	} else if (ret > 0) {
		buffer.erase(buffer.begin(), buffer.begin() + ret * num_channels);
	}

	if (buffer.size() >= period_size * num_channels) {  // Still more to write.
		if (ret == 0) {
			if (buffer.size() >= period_size * num_channels * 8) {
				// OK, almost 100 ms. Giving up.
				fprintf(stderr, "warning: ALSA overrun, dropping some audio (%d ms)\n",
					int(buffer.size() * 1000 / (num_channels * sample_rate)));
				buffer.clear();
			}
		} else if (ret > 0) {
			// Not a completely failure (effectively a short write),
			// possibly due to a signal.
			goto try_again;
		}
	}
}
