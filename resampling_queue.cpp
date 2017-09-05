// Parts of the code is adapted from Adriaensen's project Zita-ajbridge
// (as of November 2015), although it has been heavily reworked for this use
// case. Original copyright follows:
//
//  Copyright (C) 2012-2015 Fons Adriaensen <fons@linuxaudio.org>
//    
//  This program is free software; you can redistribute it and/or modify
//  it under the terms of the GNU General Public License as published by
//  the Free Software Foundation; either version 3 of the License, or
//  (at your option) any later version.
//
//  This program is distributed in the hope that it will be useful,
//  but WITHOUT ANY WARRANTY; without even the implied warranty of
//  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
//  GNU General Public License for more details.
//
//  You should have received a copy of the GNU General Public License
//  along with this program.  If not, see <http://www.gnu.org/licenses/>.

#include "resampling_queue.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zita-resampler/vresampler.h>
#include <algorithm>
#include <cmath>

using namespace std;
using namespace std::chrono;

ResamplingQueue::ResamplingQueue(unsigned card_num, unsigned freq_in, unsigned freq_out, unsigned num_channels, double expected_delay_seconds)
	: card_num(card_num), freq_in(freq_in), freq_out(freq_out), num_channels(num_channels),
	  current_estimated_freq_in(freq_in),
	  ratio(double(freq_out) / double(freq_in)), expected_delay(expected_delay_seconds * OUTPUT_FREQUENCY)
{
	vresampler.setup(ratio, num_channels, /*hlen=*/32);

	// Prime the resampler so there's no more delay.
	vresampler.inp_count = vresampler.inpsize() / 2 - 1;
        vresampler.out_count = 1048576;
        vresampler.process ();
}

void ResamplingQueue::add_input_samples(steady_clock::time_point ts, const float *samples, ssize_t num_samples, ResamplingQueue::RateAdjustmentPolicy rate_adjustment_policy)
{
	if (num_samples == 0) {
		return;
	}

	bool good_sample = (rate_adjustment_policy == ADJUST_RATE);
	if (good_sample && a1.good_sample) {
		a0 = a1;
	}
	a1.ts = ts;
	a1.input_samples_received += num_samples;
	a1.good_sample = good_sample;
	if (a0.good_sample && a1.good_sample) {
		current_estimated_freq_in = (a1.input_samples_received - a0.input_samples_received) / duration<double>(a1.ts - a0.ts).count();
		assert(current_estimated_freq_in >= 0.0);

		// Bound the frequency, so that a single wild result won't throw the filter off guard.
		current_estimated_freq_in = min(current_estimated_freq_in, 1.2 * freq_in);
		current_estimated_freq_in = max(current_estimated_freq_in, 0.8 * freq_in);
	}

	buffer.insert(buffer.end(), samples, samples + num_samples * num_channels);
}

bool ResamplingQueue::get_output_samples(steady_clock::time_point ts, float *samples, ssize_t num_samples, ResamplingQueue::RateAdjustmentPolicy rate_adjustment_policy)
{
	assert(num_samples > 0);
	if (a1.input_samples_received == 0) {
		// No data yet, just return zeros.
		memset(samples, 0, num_samples * num_channels * sizeof(float));
		return true;
	}

	// This can happen when we get dropped frames on the master card.
	if (duration<double>(ts.time_since_epoch()).count() <= 0.0) {
		rate_adjustment_policy = DO_NOT_ADJUST_RATE;
	}

	if (rate_adjustment_policy == ADJUST_RATE && (a0.good_sample || a1.good_sample)) {
		// Estimate the current number of input samples produced at
		// this instant in time, by extrapolating from the last known
		// good point. Note that we could be extrapolating backward or
		// forward, depending on the timing of the calls.
		const InputPoint &base_point = a1.good_sample ? a1 : a0;
		const double input_samples_received = base_point.input_samples_received +
			current_estimated_freq_in * duration<double>(ts - base_point.ts).count();

		// Estimate the number of input samples _consumed_ after we've run the resampler.
		const double input_samples_consumed = total_consumed_samples +
			num_samples / (ratio * rcorr);

		double actual_delay = input_samples_received - input_samples_consumed;
		actual_delay += vresampler.inpdist();    // Delay in the resampler itself.
		double err = actual_delay - expected_delay;
		if (first_output) {
			// Before the very first block, insert artificial delay based on our initial estimate,
			// so that we don't need a long period to stabilize at the beginning.
			if (err < 0.0) {
				int delay_samples_to_add = lrintf(-err);
				for (ssize_t i = 0; i < delay_samples_to_add * num_channels; ++i) {
					buffer.push_front(0.0f);
				}
				total_consumed_samples -= delay_samples_to_add;  // Equivalent to increasing input_samples_received on a0 and a1.
				err += delay_samples_to_add;
			} else if (err > 0.0) {
				int delay_samples_to_remove = min<int>(lrintf(err), buffer.size() / num_channels);
				buffer.erase(buffer.begin(), buffer.begin() + delay_samples_to_remove * num_channels);
				total_consumed_samples += delay_samples_to_remove;
				err -= delay_samples_to_remove;
			}
		}
		first_output = false;

		// Compute loop filter coefficients for the two filters. We need to compute them
		// every time, since they depend on the number of samples the user asked for.
		//
		// The loop bandwidth is at 0.02 Hz; our jitter is pretty large
		// since none of the threads involved run at real-time priority.
		// However, the first four seconds, we use a larger loop bandwidth (2 Hz),
		// because there's a lot going on during startup, and thus the
		// initial estimate might be tainted by jitter during that phase,
		// and we want to converge faster.
		//
		// NOTE: The above logic might only hold during Nageru startup
		// (we start ResamplingQueues also when we e.g. switch sound sources),
		// but in general, a little bit of increased timing jitter is acceptable
		// right after a setup change like this.
		double loop_bandwidth_hz = (total_consumed_samples < 4 * freq_in) ? 0.2 : 0.02;

		// Set filters. The first filter much wider than the first one (20x as wide).
		double w = (2.0 * M_PI) * loop_bandwidth_hz * num_samples / freq_out;
		double w0 = 1.0 - exp(-20.0 * w);
		double w1 = w * 1.5 / num_samples / ratio;
		double w2 = w / 1.5;

		// Filter <err> through the loop filter to find the correction ratio.
		z1 += w0 * (w1 * err - z1);
		z2 += w0 * (z1 - z2);
		z3 += w2 * z2;
		rcorr = 1.0 - z2 - z3;
		if (rcorr > 1.05) rcorr = 1.05;
		if (rcorr < 0.95) rcorr = 0.95;
		assert(!isnan(rcorr));
		vresampler.set_rratio(rcorr);
	}

	// Finally actually resample, producing exactly <num_samples> output samples.
	vresampler.out_data = samples;
	vresampler.out_count = num_samples;
	while (vresampler.out_count > 0) {
		if (buffer.empty()) {
			// This should never happen unless delay is set way too low,
			// or we're dropping a lot of data.
			fprintf(stderr, "Card %u: PANIC: Out of input samples to resample, still need %d output samples! (correction factor is %f)\n",
				card_num, int(vresampler.out_count), rcorr);
			memset(vresampler.out_data, 0, vresampler.out_count * num_channels * sizeof(float));

			// Reset the loop filter.
			z1 = z2 = z3 = 0.0;

			return false;
		}

		float inbuf[1024];
		size_t num_input_samples = sizeof(inbuf) / (sizeof(float) * num_channels);
		if (num_input_samples * num_channels > buffer.size()) {
			num_input_samples = buffer.size() / num_channels;
		}
		copy(buffer.begin(), buffer.begin() + num_input_samples * num_channels, inbuf);

		vresampler.inp_count = num_input_samples;
		vresampler.inp_data = inbuf;

		int err = vresampler.process();
		assert(err == 0);

		size_t consumed_samples = num_input_samples - vresampler.inp_count;
		total_consumed_samples += consumed_samples;
		buffer.erase(buffer.begin(), buffer.begin() + consumed_samples * num_channels);
	}
	return true;
}
