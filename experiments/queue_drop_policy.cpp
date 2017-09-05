/*
 * A program to simulate various queue-drop strategies, using real frame
 * arrival data as input. Contains various anchors, as well as parametrized
 * values of the real algorithms that have been used in Nageru over time.
 *
 * Expects a log of frame arrivals (in and out). This isn't included in the
 * git repository because it's quite large, but there's one available 
 * in compressed form at
 *
 *   https://storage.sesse.net/nageru-latency-log.txt.xz
 *
 * The data set in question contains a rather difficult case, with two 50 Hz
 * clocks slowly drifting from each other (at the rate of about a frame an hour).
 * This means they are very nearly in sync for a long time, where rare bursts
 * of jitter can make it hard for the algorithm to find the right level of
 * conservatism.
 *
 * This is not meant to be production-quality code.
 */

#include <assert.h>
#include <getopt.h>
#include <math.h>
#include <stdio.h>
#include <locale.h>
#include <string.h>
#include <stdlib.h>
#include <algorithm>
#include <vector>
#include <deque>
#include <memory>
#include <string>
#include <limits>

using namespace std;

size_t max_drops = numeric_limits<size_t>::max();
size_t max_underruns = numeric_limits<size_t>::max();
double max_latency_ms = numeric_limits<double>::max();

struct Event {
	enum { IN, OUT } direction;
	double t;
};

class Queue {
public:
	void add_frame(double t);
	void get_frame(double now);
	void drop_frame();
	void eval(const string &name);
	size_t queue_len() const { return frames_in_queue.size(); }
	bool should_abort() const { return num_underruns > max_underruns || num_drops > max_drops; }

private:
	deque<double> frames_in_queue;
	size_t num_underruns = 0;
	size_t num_drops = 0;
	size_t frames_since_underrun = 0;
	size_t num_drops_on_first = 0;

	double latency_sum = 0.0;
	size_t latency_count = 0;
};

void Queue::add_frame(double t)
{
	frames_in_queue.push_back(t);
}

void Queue::get_frame(double now)
{
	if (frames_in_queue.empty()) {
		++num_underruns;
		frames_since_underrun = 0;
		return;
	}
	double t = frames_in_queue.front();
	frames_in_queue.pop_front();
	assert(now >= t);
	latency_sum += (now - t);
	++latency_count;
	++frames_since_underrun;
}

void Queue::drop_frame()
{
	assert(!frames_in_queue.empty());
	frames_in_queue.pop_front();
	++num_drops;
	if (frames_since_underrun <= 1) {
		++num_drops_on_first;
	}
}

void Queue::eval(const string &name)
{
	double latency_ms = 1e3 * latency_sum / latency_count;
	if (num_underruns > max_underruns) return;
	if (num_drops > max_drops) return;
	if (latency_ms > max_latency_ms) return;
	printf("%-50s: %2lu frames left in queue at end, %5lu underruns, %5lu drops (%5lu immediate), %6.2f ms avg latency\n",
		name.c_str(), frames_in_queue.size(), num_underruns, num_drops, num_drops_on_first, latency_ms);
}

// A strategy that never drops; low anchor for drops and underruns, high anchor for latency.
void test_nodrop(const vector<Event> &events)
{
	Queue q;
	for (const Event &event : events) {
		if (event.direction == Event::IN) {
			q.add_frame(event.t);
		} else {
			q.get_frame(event.t);
		}
	}
	q.eval("no-drop");
}

// A strategy that accepts only one element in the queue; low anchor for latency.
void test_limit_to_1(const vector<Event> &events)
{
	Queue q;
	for (const Event &event : events) {
		if (event.direction == Event::IN) {
			q.add_frame(event.t);
			while (q.queue_len() > 1) q.drop_frame();
		} else {
			q.get_frame(event.t);
		}
	}
	q.eval("limit-to-1");
}

// A strategy that accepts one or two elements in the queue.
void test_limit_to_2(const vector<Event> &events)
{
	Queue q;
	for (const Event &event : events) {
		if (event.direction == Event::IN) {
			q.add_frame(event.t);
			while (q.queue_len() > 2) q.drop_frame();
		} else {
			q.get_frame(event.t);
		}
	}
	q.eval("limit-to-2");
}

// The algorithm used from Nageru 1.2.0 to 1.6.0; raise the ceiling by 1 every time
// we underrun, drop it if the ceiling hasn't been needed for 1000 frames.
void test_nageru_1_2_0(const vector<Event> &events)
{
	Queue q;
	unsigned safe_queue_length = 1;
	unsigned frames_with_at_least_one = 0;
	bool been_at_safe_point_since_last_starvation = false;
	for (const Event &event : events) {
		if (event.direction == Event::IN) {
			q.add_frame(event.t);
		} else {
			unsigned queue_length = q.queue_len();
			if (queue_length == 0) {  // Starvation.
				if (been_at_safe_point_since_last_starvation /*&& safe_queue_length < unsigned(global_flags.max_input_queue_frames)*/) {
					++safe_queue_length;
				}
				frames_with_at_least_one = 0;
				been_at_safe_point_since_last_starvation = false;
				q.get_frame(event.t);  // mark it
				continue;
			}
			if (queue_length >= safe_queue_length) {
				been_at_safe_point_since_last_starvation = true;
			}
			if (++frames_with_at_least_one >= 1000 && safe_queue_length > 1) {
				--safe_queue_length;
				frames_with_at_least_one = 0;
			}
			while (q.queue_len() > safe_queue_length) {
				q.drop_frame();
			}
			q.get_frame(event.t);
		}
	}
	q.eval("nageru-1.2.0");
}

class Jitter {
	const double multiplier, alpha;
	double expected_timestamp = -1.0;
	double max_jitter_seconds = 0.0;

public:
	Jitter(double multiplier, double alpha)
		: multiplier(multiplier), alpha(alpha) {}

	void update(double timestamp, double frame_duration, size_t dropped_frames)
	{
		if (expected_timestamp >= 0.0) {
			expected_timestamp += dropped_frames * frame_duration;
			double jitter_seconds = fabs(expected_timestamp - timestamp);
			max_jitter_seconds = max(multiplier * jitter_seconds, alpha * max_jitter_seconds);  // About two seconds half-time.

			// Cap at 100 ms.
			max_jitter_seconds = min(max_jitter_seconds, 0.1);
		}
		expected_timestamp = timestamp + frame_duration;
	}

	double get_expected() const
	{
		return expected_timestamp;
	}

	double get_jitter() const
	{
		return max_jitter_seconds;
	}
};

// Keep a running estimate of k times max jitter seen, decreasing by a factor alpha every frame.
void test_jitter_filter(const vector<Event> &events, double multiplier, double alpha, double margin)
{
	Queue q;
	Jitter input_jitter(multiplier, alpha);
	Jitter output_jitter(multiplier, alpha);
	
	for (const Event &event : events) {
		if (event.direction == Event::IN) {
			input_jitter.update(event.t, 0.020, 0);
			q.add_frame(event.t);
		} else {
			double now = event.t;
			output_jitter.update(event.t, 0.020, 0);
			q.get_frame(event.t);

			double seconds_until_next_frame = max(input_jitter.get_expected() - now + input_jitter.get_jitter(), 0.0);
			double master_frame_length_seconds = 0.020;

			seconds_until_next_frame += margin;  // Hack.

			size_t safe_queue_length = max<int>(floor((seconds_until_next_frame + output_jitter.get_jitter()) / master_frame_length_seconds), 0);
			while (q.queue_len() > safe_queue_length) {
				q.drop_frame();
			}
		}
		if (q.should_abort()) return;
	}

	char name[256];
	snprintf(name, sizeof(name), "jitter-filter[mul=%.1f,alpha=%.4f,margin=%.1f]", multiplier, alpha, 1e3 * margin);
	q.eval(name);
}

// Implements an unbalanced binary search tree that can also satisfy order queries
// (e.g. “give me the 86th largest entry”).
class HistoryJitter {
	const size_t history_length;
	const double multiplier, percentile;
	double expected_timestamp = 0.0;
	double max_jitter_seconds = 0.0;
	size_t num_updates = 0;

	deque<double> history;
	struct TreeNode {
		double val;
		size_t children = 0;
		unique_ptr<TreeNode> left, right;
	};
	unique_ptr<TreeNode> root;

	unique_ptr<TreeNode> alloc_cache;  // Holds the last freed value, for fast reallocation.

	TreeNode *alloc_node()
	{
		if (alloc_cache == nullptr) {
			return new TreeNode;
		}
		alloc_cache->children = 0;
		return alloc_cache.release();
	}

	void insert(double val)
	{
		if (root == nullptr) {
			root.reset(alloc_node());
			root->val = val;
			return;
		} else {
			insert(root.get(), val);
		}
	}

	void insert(TreeNode *node, double val)
	{
		++node->children;
		if (val <= node->val) {
			// Goes into left.
			if (node->left == nullptr) {
				node->left.reset(alloc_node());
				node->left->val = val;
			} else {
				insert(node->left.get(), val);
			}
		} else {
			// Goes into right.
			if (node->right == nullptr) {
				node->right.reset(alloc_node());
				node->right->val = val;
			} else {
				insert(node->right.get(), val);
			}
		}
	}

	void remove(double val)
	{
		assert(root != nullptr);
		if (root->children == 0) {
			assert(root->val == val);
			alloc_cache = move(root);
		} else {
			remove(root.get(), val);
		}
	}

	void remove(TreeNode *node, double val)
	{
		//printf("Down into %p looking for %f [left=%p right=%p]\n", node, val, node->left.get(), node->right.get());
		if (node->val == val) {
			remove(node);
		} else if (val < node->val) {
			assert(node->left != nullptr);
			--node->children;
			if (node->left->children == 0) {
				assert(node->left->val == val);
				alloc_cache = move(node->left);
			} else {
				remove(node->left.get(), val);
			}
		} else {
			assert(node->right != nullptr);
			--node->children;
			if (node->right->children == 0) {
				assert(node->right->val == val);
				alloc_cache = move(node->right);
			} else {
				remove(node->right.get(), val);
			}
		}
	}

	// Declares a node to be empty, so it should pull up the value of one of its children.
	// The node must be an interior node (ie., have at least one child).
	void remove(TreeNode *node)
	{
		//printf("Decided that %p must be removed\n", node);
		assert(node->children > 0);
		--node->children;

		bool remove_left;
		if (node->right == nullptr) {
			remove_left = true;
		} else if (node->left == nullptr) {
			remove_left = false;
		} else {
			remove_left = (node->left->children >= node->right->children);
		}
		if (remove_left) {
			if (node->left->children == 0) {
				node->val = node->left->val;
				alloc_cache = move(node->left);
			} else {
				// Move maximum value up to this node.
				node->val = elem_at(node->left.get(), node->left->children);
				remove(node->left.get(), node->val);
			}
		} else {
			if (node->right->children == 0) {
				node->val = node->right->val;
				alloc_cache = move(node->right);
			} else {
				// Move minimum value up to this node.
				node->val = elem_at(node->right.get(), 0);
				remove(node->right.get(), node->val);
			}
		}
	}

	double elem_at(size_t elem_idx)
	{
		return elem_at(root.get(), elem_idx);
	}

	double elem_at(TreeNode *node, size_t elem_idx)
	{
		//printf("Looking for %lu in node %p [%lu children]\n", elem_idx, node, node->children);
		assert(node != nullptr);
		assert(elem_idx <= node->children);
		if (node->left != nullptr) {
			if (elem_idx <= node->left->children) {
				return elem_at(node->left.get(), elem_idx);
			} else {
				elem_idx -= node->left->children + 1;
			}
		}
		if (elem_idx == 0) {
			return node->val;
		}
		return elem_at(node->right.get(), elem_idx - 1);
	}

	void print_tree(TreeNode *node, size_t indent, double min, double max)
	{
		if (node == nullptr) return;
		if (!(node->val >= min && node->val <= max)) {
			//printf("node %p is outside range [%f,%f]\n", node, min, max);
			assert(false);
		}
		for (size_t i = 0; i < indent * 2; ++i) putchar(' ');
		printf("%f [%p, %lu children]\n", node->val, node, node->children);
		print_tree(node->left.get(), indent + 1, min, node->val);
		print_tree(node->right.get(), indent + 1, node->val, max);
	}

public:
	HistoryJitter(size_t history_length, double multiplier, double percentile)
		: history_length(history_length), multiplier(multiplier), percentile(percentile) {}

	void update(double timestamp, double frame_duration, size_t dropped_frames)
	{
		//if (++num_updates % 1000 == 0) {
		//	printf("%d... [%lu in tree %p]\n", num_updates, root->children + 1, root.get());
		//}

		if (expected_timestamp >= 0.0) {
			expected_timestamp += dropped_frames * frame_duration;
			double jitter_seconds = fabs(expected_timestamp - timestamp);

			history.push_back(jitter_seconds);
			insert(jitter_seconds);
			//printf("\nTree %p after insert of %f:\n", root.get(), jitter_seconds);
			//print_tree(root.get(), 0, -HUGE_VAL, HUGE_VAL);
			while (history.size() > history_length) {
			//	printf("removing %f, because %p has %lu elements and history has %lu elements\n", history.front(), root.get(), root->children + 1, history.size());
				remove(history.front());
				history.pop_front();
			}
			
			size_t elem_idx = lrint(percentile * (history.size() - 1));
//			printf("Searching for element %lu in %p, which has %lu elements (history has %lu elements)\n", elem_idx, root.get(), root->children + 1, history.size());
//			fflush(stdout);
//
			// Cap at 100 ms.
			max_jitter_seconds = min(elem_at(elem_idx), 0.1);
		}
		expected_timestamp = timestamp + frame_duration;
	}

	double get_expected() const
	{
		return expected_timestamp;
	}

	double get_jitter() const
	{
		return max_jitter_seconds * multiplier;
	}
};

void test_jitter_history(const vector<Event> &events, size_t history_length, double multiplier, double percentile, double margin)
{
	Queue q;
	HistoryJitter input_jitter(history_length, multiplier, percentile);
	HistoryJitter output_jitter(history_length, multiplier, percentile);
	
	for (const Event &event : events) {
		if (event.direction == Event::IN) {
			input_jitter.update(event.t, 0.020, 0);
			q.add_frame(event.t);
		} else {
			double now = event.t;
			output_jitter.update(event.t, 0.020, 0);
			q.get_frame(event.t);

			double seconds_until_next_frame = max(input_jitter.get_expected() - now + input_jitter.get_jitter(), 0.0);
			double master_frame_length_seconds = 0.020;

			seconds_until_next_frame += margin;  // Hack.

			size_t safe_queue_length = max<int>(floor((seconds_until_next_frame + output_jitter.get_jitter()) / master_frame_length_seconds), 0);
			while (q.queue_len() > safe_queue_length) {
				q.drop_frame();
			}
		}
		if (q.should_abort()) return;
	}
	char name[256];
	snprintf(name, sizeof(name), "history[len=%lu,mul=%.1f,pct=%.4f,margin=%.1f]", history_length, multiplier, percentile, 1e3 * margin);
	q.eval(name);
}

int main(int argc, char **argv)
{
	static const option long_options[] = {
		{ "max-drops", required_argument, 0, 'd' },
		{ "max-underruns", required_argument, 0, 'u' },
		{ "max-latency-ms", required_argument, 0, 'l' },
		{ 0, 0, 0, 0 }
	};      
	for ( ;; ) {
		int option_index = 0;
		int c = getopt_long(argc, argv, "d:u:l:", long_options, &option_index);

		if (c == -1) {
			break;
		}
                switch (c) {
                case 'd':
			max_drops = atof(optarg);
			break;
                case 'u':
			max_underruns = atof(optarg);
			break;
                case 'l':
			max_latency_ms = atof(optarg);
			break;
		default:
			fprintf(stderr, "Usage: simul [--max-drops NUM] [--max-underruns NUM] [--max-latency-ms TIME]\n");
			exit(1);
		}
	}

	vector<Event> events;

	const char *filename = (optind < argc) ? argv[optind] : "nageru-latency-log.txt";
	FILE *fp = fopen(filename, "r");
	if (fp == nullptr) {
		perror(filename);
		exit(1);
	}
	while (!feof(fp)) {
		char dir[256];
		double t;

		if (fscanf(fp, "%s %lf", dir, &t) != 2) {
			break;
		}
		if (dir[0] == 'I') {
			events.push_back(Event{Event::IN, t});
		} else if (dir[0] == 'O') {
			events.push_back(Event{Event::OUT, t});
		} else {
			fprintf(stderr, "ERROR: Unreadable line\n");
			exit(1);
		}
	}
	fclose(fp);

	sort(events.begin(), events.end(), [](const Event &a, const Event &b) { return a.t < b.t; });

	test_nodrop(events);
	test_limit_to_1(events);
	test_limit_to_2(events);
	test_nageru_1_2_0(events);
	for (double multiplier : { 0.0, 0.5, 1.0, 2.0, 3.0, 5.0 }) {
		for (double alpha : { 0.5, 0.9, 0.99, 0.995, 0.999, 0.9999 }) {
			for (double margin_ms : { -1.0, 0.0, 1.0, 2.0, 5.0, 10.0, 20.0 }) {
				test_jitter_filter(events, multiplier, alpha, 1e-3 * margin_ms);
			}
		}
	}
	for (size_t history_samples : { 10, 100, 500, 1000, 5000, 10000, 25000 }) {
		for (double multiplier : { 0.5, 1.0, 2.0, 3.0, 5.0, 10.0 }) {
			for (double percentile : { 0.5, 0.75, 0.9, 0.99, 0.995, 0.999, 1.0 }) {
				if (lrint(percentile * (history_samples - 1)) == int(history_samples - 1) && percentile != 1.0) {
					// Redundant.
					continue;
				}

				//for (double margin_ms : { -1.0, 0.0, 1.0, 2.0, 5.0, 10.0, 20.0 }) {
				for (double margin_ms : { 0.0 }) {
					test_jitter_history(events, history_samples, multiplier, percentile, 1e-3 * margin_ms);
				}
			}
		}
	}
}
