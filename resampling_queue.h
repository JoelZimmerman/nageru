#ifndef _RESAMPLING_QUEUE_H
#define _RESAMPLING_QUEUE_H 1

// Takes in samples from an input source, possibly with jitter, and outputs a fixed number
// of samples every iteration. Used to a) change sample rates if needed, and b) deal with
// input sources that don't have audio locked to video. For every input video
// frame, you call add_input_samples() with the received time point of the video frame,
// taken to be the _end_ point of the frame's audio. When you want to _output_ a finished
// frame with audio, you get_output_samples() with the number of samples you want, and will
// get exactly that number of samples back. If the input and output clocks are not in sync,
// the audio will be stretched for you. (If they are _very_ out of sync, this will come through
// as a pitch shift.) Of course, the process introduces some delay; you specify a target delay
// (typically measured in milliseconds, although more is fine) and the algorithm works to
// provide exactly that.
//
// A/V sync is a much harder problem than one would intuitively assume. This implementation
// is based on a 2012 paper by Fons Adriaensen, “Controlling adaptive resampling”
// (http://kokkinizita.linuxaudio.org/papers/adapt-resamp.pdf). The paper gives an algorithm
// that converges to jitter of <100 ns; the basic idea is to measure the _rate_ the input
// queue fills and is drained (as opposed to the length of the queue itself), and smoothly
// adjust the resampling rate so that it reaches steady state at the desired delay.
//
// Parts of the code is adapted from Adriaensen's project Zita-ajbridge (based on the same
// algorithm), although it has been heavily reworked for this use case. Original copyright follows:
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

#include <sys/types.h>
#include <zita-resampler/vresampler.h>
#include <chrono>
#include <deque>
#include <memory>

#include "defs.h"

class ResamplingQueue {
public:
	// card_num is for debugging outputs only.
	ResamplingQueue(unsigned card_num, unsigned freq_in, unsigned freq_out, unsigned num_channels, double expected_delay_seconds);

	// If policy is DO_NOT_ADJUST_RATE, the resampling rate will not be changed.
	// This is primarily useful if you have an extraordinary situation, such as
	// dropped frames.
	enum RateAdjustmentPolicy {
		DO_NOT_ADJUST_RATE,
		ADJUST_RATE
	};

	void add_input_samples(std::chrono::steady_clock::time_point ts, const float *samples, ssize_t num_samples, RateAdjustmentPolicy rate_adjustment_policy);
	// Returns false if underrun.
	bool get_output_samples(std::chrono::steady_clock::time_point ts, float *samples, ssize_t num_samples, RateAdjustmentPolicy rate_adjustment_policy);

private:
	void init_loop_filter(double bandwidth_hz);

	VResampler vresampler;

	unsigned card_num;
	unsigned freq_in, freq_out, num_channels;

	bool first_output = true;

	struct InputPoint {
		// Equivalent to t_a0 or t_a1 in the paper.
		std::chrono::steady_clock::time_point ts;

		// Number of samples that have been written to the queue (in total)
		// at this time point. Equivalent to k_a0 or k_a1 in the paper.
		size_t input_samples_received = 0;

		// Set to false if we should not use the timestamp from this sample
		// (e.g. if it is from a dropped frame and thus bad). In particular,
		// we will not use it for updateing current_estimated_freq_in.
		bool good_sample = false;
	};
	InputPoint a0, a1;

	// The current rate at which we seem to get input samples, in Hz.
	// For an ideal input, identical to freq_in.
	double current_estimated_freq_in;

	ssize_t total_consumed_samples = 0;

	// Filter state for the loop filter.
	double z1 = 0.0, z2 = 0.0, z3 = 0.0;

	// Ratio between the two frequencies.
	const double ratio;

	// Current correction ratio. ratio * rcorr gives the true ratio,
	// so values above 1.0 means to pitch down (consume input samples slower).
	double rcorr = 1.0;

	// How much delay we are expected to have, in input samples.
	// If actual delay drifts too much away from this, we will start
	// changing the resampling ratio to compensate.
	const double expected_delay;

	// Input samples not yet fed into the resampler.
	// TODO: Use a circular buffer instead, for efficiency.
	std::deque<float> buffer;
};

#endif  // !defined(_RESAMPLING_QUEUE_H)
