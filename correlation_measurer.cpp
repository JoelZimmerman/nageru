// Adapted from Adriaensen's project Zita-mu1 (as of January 2016).
// Original copyright follows:
//
//  Copyright (C) 2008-2015 Fons Adriaensen <fons@linuxaudio.org>
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

#include "correlation_measurer.h"

#include <assert.h>
#include <cmath>
#include <cstddef>

using namespace std;

CorrelationMeasurer::CorrelationMeasurer(unsigned sample_rate,
                                         float lowpass_cutoff_hz,
					 float falloff_seconds)
    : w1(2.0 * M_PI * lowpass_cutoff_hz / sample_rate),
      w2(1.0 / (falloff_seconds * sample_rate))
{
}

void CorrelationMeasurer::reset()
{
	zl = zr = zll = zlr = zrr = 0.0f;
}

void CorrelationMeasurer::process_samples(const std::vector<float> &samples)
{
	assert(samples.size() % 2 == 0);

	// The compiler isn't always happy about modifying members,
	// since it doesn't always know they can't alias on <samples>.
	// Help it out a bit.
	float l = zl, r = zr, ll = zll, lr = zlr, rr = zrr;
	const float w1c = w1, w2c = w2;

	for (size_t i = 0; i < samples.size(); i += 2) {
		// The 1e-15f epsilon is to avoid denormals.
		// TODO: Just set the SSE flush-to-zero flags instead.
		l += w1c * (samples[i + 0] - l) + 1e-15f;
		r += w1c * (samples[i + 1] - r) + 1e-15f;
		lr += w2c * (l * r - lr);
		ll += w2c * (l * l - ll);
		rr += w2c * (r * r - rr);
	}

	zl = l;
	zr = r;
	zll = ll;
	zlr = lr;
	zrr = rr;
}

float CorrelationMeasurer::get_correlation() const
{
	// The 1e-12f epsilon is to avoid division by zero.
	// zll and zrr are both always non-negative, so we do not risk negative values.
	return zlr / sqrt(zll * zrr + 1e-12f);
}
