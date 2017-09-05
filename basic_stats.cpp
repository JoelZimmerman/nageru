#include "basic_stats.h"
#include "metrics.h"

#include <assert.h>
#include <sys/resource.h>

using namespace std;
using namespace std::chrono;

bool uses_mlock = false;

BasicStats::BasicStats(bool verbose)
	: verbose(verbose)
{
	start = steady_clock::now();

	metric_start_time_seconds = get_timestamp_for_metrics();
	global_metrics.add("frames_output_total", &metric_frames_output_total);
	global_metrics.add("frames_output_dropped", &metric_frames_output_dropped);
	global_metrics.add("start_time_seconds", &metric_start_time_seconds, Metrics::TYPE_GAUGE);
	global_metrics.add("memory_used_bytes", &metrics_memory_used_bytes);
	global_metrics.add("memory_locked_limit_bytes", &metrics_memory_locked_limit_bytes);
}

void BasicStats::update(int frame_num, int stats_dropped_frames)
{
	steady_clock::time_point now = steady_clock::now();
	double elapsed = duration<double>(now - start).count();

	metric_frames_output_total = frame_num;
	metric_frames_output_dropped = stats_dropped_frames;

	if (frame_num % 100 != 0) {
		return;
	}

	if (verbose) {
		printf("%d frames (%d dropped) in %.3f seconds = %.1f fps (%.1f ms/frame)",
			frame_num, stats_dropped_frames, elapsed, frame_num / elapsed,
			1e3 * elapsed / frame_num);
	}

	// Check our memory usage, to see if we are close to our mlockall()
	// limit (if at all set).
	rusage used;
	if (getrusage(RUSAGE_SELF, &used) == -1) {
		perror("getrusage(RUSAGE_SELF)");
		assert(false);
	}
	metrics_memory_used_bytes = used.ru_maxrss * 1024;

	if (uses_mlock) {
		rlimit limit;
		if (getrlimit(RLIMIT_MEMLOCK, &limit) == -1) {
			perror("getrlimit(RLIMIT_MEMLOCK)");
			assert(false);
		}
		metrics_memory_locked_limit_bytes = limit.rlim_cur;

		if (verbose) {
			if (limit.rlim_cur == 0) {
				printf(", using %ld MB memory (locked)",
						long(used.ru_maxrss / 1024));
			} else {
				printf(", using %ld / %ld MB lockable memory (%.1f%%)",
						long(used.ru_maxrss / 1024),
						long(limit.rlim_cur / 1048576),
						float(100.0 * (used.ru_maxrss * 1024.0) / limit.rlim_cur));
			}
		}
	} else {
		metrics_memory_locked_limit_bytes = 0.0 / 0.0;
		if (verbose) {
			printf(", using %ld MB memory (not locked)",
					long(used.ru_maxrss / 1024));
		}
	}

	if (verbose) {
		printf("\n");
	}
}


