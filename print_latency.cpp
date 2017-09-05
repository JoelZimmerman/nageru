#include "print_latency.h"

#include "flags.h"
#include "metrics.h"
#include "mixer.h"

#include <stdio.h>
#include <algorithm>
#include <chrono>
#include <string>

using namespace std;
using namespace std::chrono;

ReceivedTimestamps find_received_timestamp(const vector<RefCountedFrame> &input_frames)
{
	unsigned num_cards = global_mixer->get_num_cards();
	assert(input_frames.size() == num_cards * FRAME_HISTORY_LENGTH);

	ReceivedTimestamps ts;
	for (unsigned card_index = 0; card_index < num_cards; ++card_index) {
		for (unsigned frame_index = 0; frame_index < FRAME_HISTORY_LENGTH; ++frame_index) {
			const RefCountedFrame &input_frame = input_frames[card_index * FRAME_HISTORY_LENGTH + frame_index];
			if (input_frame == nullptr ||
			    (frame_index > 0 && input_frame.get() == input_frames[card_index * FRAME_HISTORY_LENGTH + frame_index - 1].get())) {
				ts.ts.push_back(steady_clock::time_point::min());
			} else {
				ts.ts.push_back(input_frame->received_timestamp);
			}
		}
	}
	return ts;
}

void LatencyHistogram::init(const string &measuring_point)
{
	unsigned num_cards = global_flags.num_cards;  // The mixer might not be ready yet.
	summaries.resize(num_cards * FRAME_HISTORY_LENGTH * 2);
	for (unsigned card_index = 0; card_index < num_cards; ++card_index) {
		char card_index_str[64];
		snprintf(card_index_str, sizeof(card_index_str), "%u", card_index);
		summaries[card_index].resize(FRAME_HISTORY_LENGTH);
		for (unsigned frame_index = 0; frame_index < FRAME_HISTORY_LENGTH; ++frame_index) {
			char frame_index_str[64];
			snprintf(frame_index_str, sizeof(frame_index_str), "%u", frame_index);

			vector<double> quantiles{0.01, 0.1, 0.25, 0.5, 0.75, 0.9, 0.99};
			summaries[card_index][frame_index].reset(new Summary[3]);
			summaries[card_index][frame_index][0].init(quantiles, 60.0);
			summaries[card_index][frame_index][1].init(quantiles, 60.0);
			summaries[card_index][frame_index][2].init(quantiles, 60.0);
			global_metrics.add("latency_seconds",
				{{ "measuring_point", measuring_point },
				 { "card", card_index_str },
				 { "frame_age", frame_index_str },
				 { "frame_type", "i/p" }},
				 &summaries[card_index][frame_index][0],
				(frame_index == 0) ? Metrics::PRINT_ALWAYS : Metrics::PRINT_WHEN_NONEMPTY);
			global_metrics.add("latency_seconds",
				{{ "measuring_point", measuring_point },
				 { "card", card_index_str },
				 { "frame_age", frame_index_str },
				 { "frame_type", "b" }},
				 &summaries[card_index][frame_index][1],
				Metrics::PRINT_WHEN_NONEMPTY);
			global_metrics.add("latency_seconds",
				{{ "measuring_point", measuring_point },
				 { "card", card_index_str },
				 { "frame_age", frame_index_str },
				 { "frame_type", "total" }},
				 &summaries[card_index][frame_index][2],
				(frame_index == 0) ? Metrics::PRINT_ALWAYS : Metrics::PRINT_WHEN_NONEMPTY);
		}
	}
}

void print_latency(const string &header, const ReceivedTimestamps &received_ts, bool is_b_frame, int *frameno, LatencyHistogram *histogram)
{
	if (received_ts.ts.empty())
		return;

	const steady_clock::time_point now = steady_clock::now();

	if (global_mixer == nullptr) {
		// Kaeru.
		assert(received_ts.ts.size() == 1);
		steady_clock::time_point ts = received_ts.ts[0];
		if (ts != steady_clock::time_point::min()) {
			duration<double> latency = now - ts;
			histogram->summaries[0][0][is_b_frame].count_event(latency.count());
			histogram->summaries[0][0][2].count_event(latency.count());
		}
	} else {
		unsigned num_cards = global_mixer->get_num_cards();
		assert(received_ts.ts.size() == num_cards * FRAME_HISTORY_LENGTH);
		for (unsigned card_index = 0; card_index < num_cards; ++card_index) {
			for (unsigned frame_index = 0; frame_index < FRAME_HISTORY_LENGTH; ++frame_index) {
				steady_clock::time_point ts = received_ts.ts[card_index * FRAME_HISTORY_LENGTH + frame_index];
				if (ts == steady_clock::time_point::min()) {
					continue;
				}
				duration<double> latency = now - ts;
				histogram->summaries[card_index][frame_index][is_b_frame].count_event(latency.count());
				histogram->summaries[card_index][frame_index][2].count_event(latency.count());
			}
		}
	}

	// 101 is chosen so that it's prime, which is unlikely to get the same frame type every time.
	if (global_flags.print_video_latency && (++*frameno % 101) == 0) {
		// Find min and max timestamp of all input frames that have a timestamp.
		steady_clock::time_point min_ts = steady_clock::time_point::max(), max_ts = steady_clock::time_point::min();
		for (const auto &ts : received_ts.ts) {
			if (ts > steady_clock::time_point::min()) {
				min_ts = min(min_ts, ts);
				max_ts = max(max_ts, ts);
			}
		}
		duration<double> lowest_latency = now - max_ts;
		duration<double> highest_latency = now - min_ts;

		printf("%-60s %4.0f ms (lowest-latency input), %4.0f ms (highest-latency input)",
			header.c_str(), 1e3 * lowest_latency.count(), 1e3 * highest_latency.count());

		if (is_b_frame) {
			printf("  [on B-frame; potential extra latency]\n");
		} else {
			printf("\n");
		}
	}
}
