#ifndef _PRINT_LATENCY_H
#define _PRINT_LATENCY_H 1

// A small utility function to print the latency between two end points
// (typically when the frame was received from the video card, and some
// point when the frame is ready to be output in some form).

#include <chrono>
#include <string>
#include <vector>

#include "ref_counted_frame.h"
#include "metrics.h"

// Since every output frame is based on multiple input frames, we need
// more than one start timestamp; one for each input.
// For all of these, steady_clock::time_point::min() is used for “not set”.
struct ReceivedTimestamps {
	std::vector<std::chrono::steady_clock::time_point> ts;
};
struct LatencyHistogram {
	void init(const std::string &measuring_point);  // Initializes histograms and registers them in global_metrics.

	// Indices: card number, frame history number, b-frame or not (1/0, where 2 counts both).
	std::vector<std::vector<std::unique_ptr<Summary[]>>> summaries;
};

ReceivedTimestamps find_received_timestamp(const std::vector<RefCountedFrame> &input_frames);

void print_latency(const std::string &header, const ReceivedTimestamps &received_ts, bool is_b_frame, int *frameno, LatencyHistogram *histogram);

#endif  // !defined(_PRINT_LATENCY_H)
