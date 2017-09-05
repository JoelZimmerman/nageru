#ifndef _CORRELATION_MEASURER_H
#define _CORRELATION_MEASURER_H 1

// Measurement of left/right stereo correlation. +1 is pure mono
// (okay but not ideal), 0 is no correlation (usually bad, unless
// it is due to silence), strongly negative values means inverted
// phase (bad). Typical values for e.g. music would be somewhere
// around +0.7, although you can expect it to vary a bit.
//
// This is, of course, based on the regular Pearson correlation,
// where µ_L and µ_R is taken to be 0 (ie., no DC offset). It is
// filtered through a simple IIR filter so that older values are
// weighed less than newer, depending on <falloff_seconds>.
//
//
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

#include <vector>

class CorrelationMeasurer {
public:
	CorrelationMeasurer(unsigned sample_rate, float lowpass_cutoff_hz = 1000.0f,
	                    float falloff_seconds = 0.150f);
	void process_samples(const std::vector<float> &samples);  // Taken to be stereo, interleaved.
	void reset();
	float get_correlation() const;

private:
	float w1, w2;

	// Filtered values of left and right channel, respectively.
	float zl = 0.0f, zr = 0.0f;

	// Filtered values of l², r² and lr (where l and r are the filtered
	// versions, given by zl and zr). Together, they make up what we need
	// to calculate the correlation.
	float zll = 0.0f, zlr = 0.0f, zrr = 0.0f;
};

#endif  // !defined(_CORRELATION_MEASURER_H)
