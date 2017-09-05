#ifndef _BASIC_STATS_H
#define _BASIC_STATS_H

// Holds some metrics for basic statistics about uptime, memory usage and such.

#include <stdint.h>

#include <atomic>
#include <chrono>

extern bool uses_mlock;

class BasicStats {
public:
	BasicStats(bool verbose);
	void update(int frame_num, int stats_dropped_frames);

private:
	std::chrono::steady_clock::time_point start;
	bool verbose;

	// Metrics.
	std::atomic<int64_t> metric_frames_output_total{0};
	std::atomic<int64_t> metric_frames_output_dropped{0};
	std::atomic<double> metric_start_time_seconds{0.0 / 0.0};
	std::atomic<int64_t> metrics_memory_used_bytes{0};
	std::atomic<double> metrics_memory_locked_limit_bytes{0.0 / 0.0};
};

#endif  // !defined(_BASIC_STATS_H)
