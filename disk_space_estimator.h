#ifndef _DISK_SPACE_ESTIMATOR_H
#define _DISK_SPACE_ESTIMATOR_H

// A class responsible for measuring how much disk there is left when we
// store our video to disk, and how much recording time that equates to.
// It gets callbacks from the Mux writing the stream to disk (which also
// knows which filesystem the file is going to), makes its calculations,
// and calls back to the MainWindow, which shows it to the user.
//
// The bitrate is measured over a simple 30-second sliding window.

#include <stdint.h>
#include <sys/types.h>
#include <atomic>
#include <deque>
#include <functional>
#include <string>

#include "timebase.h"

class DiskSpaceEstimator
{
public:
	typedef std::function<void(off_t free_bytes, double estimated_seconds_left)> callback_t;
	DiskSpaceEstimator(callback_t callback);

	// Report that a video frame with the given pts has just been written
	// to the given file, so the estimator should stat the file and see
	// by how much it grew since last time. Called by the Mux object
	// responsible for writing to the stream on disk.
	//
	// If the filename changed since last time, the estimation is reset.
	// <pts> is taken to be in TIMEBASE units (see timebase.h).
	void report_write(const std::string &filename, uint64_t pts);

private:
	static constexpr int64_t window_length = 30 * TIMEBASE;

	callback_t callback;
	std::string last_filename;

	struct MeasurePoint {
		uint64_t pts;
		off_t size;
	};
	std::deque<MeasurePoint> measure_points;
	uint64_t last_pts_reported = 0;

	// Metrics.
	std::atomic<int64_t> metric_disk_free_bytes{-1};
};

extern DiskSpaceEstimator *global_disk_space_estimator;

#endif  // !defined(_DISK_SPACE_ESTIMATOR_H)
