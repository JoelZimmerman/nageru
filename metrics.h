#ifndef _METRICS_H
#define _METRICS_H 1

// A simple global class to keep track of metrics export in Prometheus format.
// It would be better to use a more full-featured Prometheus client library for this,
// but it would introduce a dependency that is not commonly packaged in distributions,
// which makes it quite unwieldy. Thus, we'll package our own for the time being.

#include <atomic>
#include <chrono>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

class Histogram;
class Summary;

// Prometheus recommends the use of timestamps instead of “time since event”,
// so you can use this to get the number of seconds since the epoch.
// Note that this will be wrong if your clock changes, so for non-metric use,
// you should use std::chrono::steady_clock instead.
double get_timestamp_for_metrics();

class Metrics {
public:
	enum Type {
		TYPE_COUNTER,
		TYPE_GAUGE,
		TYPE_HISTOGRAM,  // Internal use only.
		TYPE_SUMMARY,  // Internal use only.
	};
	enum Laziness {
		PRINT_ALWAYS,
		PRINT_WHEN_NONEMPTY,
	};

	void add(const std::string &name, std::atomic<int64_t> *location, Type type = TYPE_COUNTER)
	{
		add(name, {}, location, type);
	}

	void add(const std::string &name, std::atomic<double> *location, Type type = TYPE_COUNTER)
	{
		add(name, {}, location, type);
	}

	void add(const std::string &name, Histogram *location)
	{
		add(name, {}, location);
	}

	void add(const std::string &name, Summary *location)
	{
		add(name, {}, location);
	}

	void add(const std::string &name, const std::vector<std::pair<std::string, std::string>> &labels, std::atomic<int64_t> *location, Type type = TYPE_COUNTER);
	void add(const std::string &name, const std::vector<std::pair<std::string, std::string>> &labels, std::atomic<double> *location, Type type = TYPE_COUNTER);
	void add(const std::string &name, const std::vector<std::pair<std::string, std::string>> &labels, Histogram *location, Laziness laziness = PRINT_ALWAYS);
	void add(const std::string &name, const std::vector<std::pair<std::string, std::string>> &labels, Summary *location, Laziness laziness = PRINT_ALWAYS);

	void remove(const std::string &name)
	{
		remove(name, {});
	}

	void remove(const std::string &name, const std::vector<std::pair<std::string, std::string>> &labels);

	std::string serialize() const;

private:
	static std::string serialize_name(const std::string &name, const std::vector<std::pair<std::string, std::string>> &labels);
	static std::string serialize_labels(const std::vector<std::pair<std::string, std::string>> &labels);

	enum DataType {
		DATA_TYPE_INT64,
		DATA_TYPE_DOUBLE,
		DATA_TYPE_HISTOGRAM,
		DATA_TYPE_SUMMARY,
	};
	struct MetricKey {
		MetricKey(const std::string &name, const std::vector<std::pair<std::string, std::string>> labels)
			: name(name), labels(labels), serialized_labels(serialize_labels(labels))
		{
		}

		bool operator< (const MetricKey &other) const
		{
			if (name != other.name)
				return name < other.name;
			return serialized_labels < other.serialized_labels;
		}

		const std::string name;
		const std::vector<std::pair<std::string, std::string>> labels;
		const std::string serialized_labels;
	};
	struct Metric {
		DataType data_type;
		Laziness laziness;  // Only for TYPE_HISTOGRAM.
		union {
			std::atomic<int64_t> *location_int64;
			std::atomic<double> *location_double;
			Histogram *location_histogram;
			Summary *location_summary;
		};
	};

	mutable std::mutex mu;
	std::map<std::string, Type> types;  // Ordered the same as metrics.
	std::map<MetricKey, Metric> metrics;

	friend class Histogram;
	friend class Summary;
};

class Histogram {
public:
	void init(const std::vector<double> &bucket_vals);
	void init_uniform(size_t num_buckets);  // Sets up buckets 0..(N-1).
	void init_geometric(double min, double max, size_t num_buckets);
	void count_event(double val);
	std::string serialize(Metrics::Laziness laziness, const std::string &name, const std::vector<std::pair<std::string, std::string>> &labels) const;

private:
	// Bucket <i> counts number of events where val[i - 1] < x <= val[i].
	// The end histogram ends up being made into a cumulative one,
	// but that's not how we store it here.
	struct Bucket {
		double val;
		std::atomic<int64_t> count{0};
	};
	std::unique_ptr<Bucket[]> buckets;
	size_t num_buckets;
	std::atomic<double> sum{0.0};
	std::atomic<int64_t> count_after_last_bucket{0};
};

// This is a pretty dumb streaming quantile class, but it's exact, and we don't have
// too many values (typically one per frame, and one-minute interval), so we don't
// need anything fancy.
class Summary {
public:
	void init(const std::vector<double> &quantiles, double window_seconds);
	void count_event(double val);
	std::string serialize(Metrics::Laziness laziness, const std::string &name, const std::vector<std::pair<std::string, std::string>> &labels);

private:
	std::vector<double> quantiles;
	std::chrono::duration<double> window;

	mutable std::mutex mu;
	std::deque<std::pair<std::chrono::steady_clock::time_point, double>> values;
	std::atomic<double> sum{0.0};
	std::atomic<int64_t> count{0};
};

extern Metrics global_metrics;

#endif  // !defined(_METRICS_H)
