#include "metrics.h"

#include <assert.h>
#include <math.h>

#include <algorithm>
#include <chrono>
#include <locale>
#include <sstream>

using namespace std;
using namespace std::chrono;

Metrics global_metrics;

double get_timestamp_for_metrics()
{
	return duration<double>(system_clock::now().time_since_epoch()).count();
}

string Metrics::serialize_name(const string &name, const vector<pair<string, string>> &labels)
{
	return "nageru_" + name + serialize_labels(labels);
}

string Metrics::serialize_labels(const vector<pair<string, string>> &labels)
{
	if (labels.empty()) {
		return "";
	}

	string label_str;
	for (const pair<string, string> &label : labels) {
		if (!label_str.empty()) {
			label_str += ",";
		}
		label_str += label.first + "=\"" + label.second + "\"";
	}
	return "{" + label_str + "}";
}

void Metrics::add(const string &name, const vector<pair<string, string>> &labels, atomic<int64_t> *location, Metrics::Type type)
{
	Metric metric;
	metric.data_type = DATA_TYPE_INT64;
	metric.location_int64 = location;

	lock_guard<mutex> lock(mu);
	metrics.emplace(MetricKey(name, labels), metric);
	assert(types.count(name) == 0 || types[name] == type);
	types[name] = type;
}

void Metrics::add(const string &name, const vector<pair<string, string>> &labels, atomic<double> *location, Metrics::Type type)
{
	Metric metric;
	metric.data_type = DATA_TYPE_DOUBLE;
	metric.location_double = location;

	lock_guard<mutex> lock(mu);
	metrics.emplace(MetricKey(name, labels), metric);
	assert(types.count(name) == 0 || types[name] == type);
	types[name] = type;
}

void Metrics::add(const string &name, const vector<pair<string, string>> &labels, Histogram *location, Laziness laziness)
{
	Metric metric;
	metric.data_type = DATA_TYPE_HISTOGRAM;
	metric.laziness = laziness;
	metric.location_histogram = location;

	lock_guard<mutex> lock(mu);
	metrics.emplace(MetricKey(name, labels), metric);
	assert(types.count(name) == 0 || types[name] == TYPE_HISTOGRAM);
	types[name] = TYPE_HISTOGRAM;
}

void Metrics::add(const string &name, const vector<pair<string, string>> &labels, Summary *location, Laziness laziness)
{
	Metric metric;
	metric.data_type = DATA_TYPE_SUMMARY;
	metric.laziness = laziness;
	metric.location_summary = location;

	lock_guard<mutex> lock(mu);
	metrics.emplace(MetricKey(name, labels), metric);
	assert(types.count(name) == 0 || types[name] == TYPE_SUMMARY);
	types[name] = TYPE_SUMMARY;
}

void Metrics::remove(const string &name, const vector<pair<string, string>> &labels)
{
	lock_guard<mutex> lock(mu);

	auto it = metrics.find(MetricKey(name, labels));
	assert(it != metrics.end());

	// If this is the last metric with this name, remove the type as well.
	if (!((it != metrics.begin() && prev(it)->first.name == name) ||
	      (it != metrics.end() && next(it)->first.name == name))) {
		types.erase(name);
	}

	metrics.erase(it);
}

string Metrics::serialize() const
{
	stringstream ss;
	ss.imbue(locale("C"));
	ss.precision(20);

	lock_guard<mutex> lock(mu);
	auto type_it = types.cbegin();
	for (const auto &key_and_metric : metrics) {
		string name = "nageru_" + key_and_metric.first.name + key_and_metric.first.serialized_labels;
		const Metric &metric = key_and_metric.second;

		if (type_it != types.cend() &&
		    key_and_metric.first.name == type_it->first) {
			// It's the first time we print out any metric with this name,
			// so add the type header.
			if (type_it->second == TYPE_GAUGE) {
				ss << "# TYPE nageru_" << type_it->first << " gauge\n";
			} else if (type_it->second == TYPE_HISTOGRAM) {
				ss << "# TYPE nageru_" << type_it->first << " histogram\n";
			} else if (type_it->second == TYPE_SUMMARY) {
				ss << "# TYPE nageru_" << type_it->first << " summary\n";
			}
			++type_it;
		}

		if (metric.data_type == DATA_TYPE_INT64) {
			ss << name << " " << metric.location_int64->load() << "\n";
		} else if (metric.data_type == DATA_TYPE_DOUBLE) {
			double val = metric.location_double->load();
			if (isnan(val)) {
				// Prometheus can't handle “-nan”.
				ss << name << " NaN\n";
			} else {
				ss << name << " " << val << "\n";
			}
		} else if (metric.data_type == DATA_TYPE_HISTOGRAM) {
			ss << metric.location_histogram->serialize(metric.laziness, key_and_metric.first.name, key_and_metric.first.labels);
		} else {
			ss << metric.location_summary->serialize(metric.laziness, key_and_metric.first.name, key_and_metric.first.labels);
		}
	}

	return ss.str();
}

void Histogram::init(const vector<double> &bucket_vals)
{
	this->num_buckets = bucket_vals.size();
	buckets.reset(new Bucket[num_buckets]);
	for (size_t i = 0; i < num_buckets; ++i) {
		buckets[i].val = bucket_vals[i];
	}
}

void Histogram::init_uniform(size_t num_buckets)
{
	this->num_buckets = num_buckets;
	buckets.reset(new Bucket[num_buckets]);
	for (size_t i = 0; i < num_buckets; ++i) {
		buckets[i].val = i;
	}
}

void Histogram::init_geometric(double min, double max, size_t num_buckets)
{
	this->num_buckets = num_buckets;
	buckets.reset(new Bucket[num_buckets]);
	for (size_t i = 0; i < num_buckets; ++i) {
		buckets[i].val = min * pow(max / min, double(i) / (num_buckets - 1));
	}
}

void Histogram::count_event(double val)
{
	Bucket ref_bucket;
	ref_bucket.val = val;
	auto it = lower_bound(buckets.get(), buckets.get() + num_buckets, ref_bucket,
		[](const Bucket &a, const Bucket &b) { return a.val < b.val; });
	if (it == buckets.get() + num_buckets) {
		++count_after_last_bucket;
	} else {
		++it->count;
	}
	// Non-atomic add, but that's fine, since there are no concurrent writers.
	sum = sum + val;
}

string Histogram::serialize(Metrics::Laziness laziness, const string &name, const vector<pair<string, string>> &labels) const
{
	// Check if the histogram is empty and should not be serialized.
	if (laziness == Metrics::PRINT_WHEN_NONEMPTY && count_after_last_bucket.load() == 0) {
		bool empty = true;
		for (size_t bucket_idx = 0; bucket_idx < num_buckets; ++bucket_idx) {
			if (buckets[bucket_idx].count.load() != 0) {
				empty = false;
				break;
			}
		}
		if (empty) {
			return "";
		}
	}

	stringstream ss;
	ss.imbue(locale("C"));
	ss.precision(20);

	int64_t count = 0;
	for (size_t bucket_idx = 0; bucket_idx < num_buckets; ++bucket_idx) {
		stringstream le_ss;
		le_ss.imbue(locale("C"));
		le_ss.precision(20);
		le_ss << buckets[bucket_idx].val;
		vector<pair<string, string>> bucket_labels = labels;
		bucket_labels.emplace_back("le", le_ss.str());

		count += buckets[bucket_idx].count.load();
		ss << Metrics::serialize_name(name + "_bucket", bucket_labels) << " " << count << "\n";
	}

	count += count_after_last_bucket.load();

	ss << Metrics::serialize_name(name + "_sum", labels) << " " << sum.load() << "\n";
	ss << Metrics::serialize_name(name + "_count", labels) << " " << count << "\n";

	return ss.str();
}

void Summary::init(const vector<double> &quantiles, double window_seconds)
{
	this->quantiles = quantiles;
	window = duration<double>(window_seconds);
}

void Summary::count_event(double val)
{
	steady_clock::time_point now = steady_clock::now();
	steady_clock::time_point cutoff = now - duration_cast<steady_clock::duration>(window);

	lock_guard<mutex> lock(mu);
	values.emplace_back(now, val);
	while (!values.empty() && values.front().first < cutoff) {
		values.pop_front();
	}

	// Non-atomic add, but that's fine, since there are no concurrent writers.
	sum = sum + val;
	++count;
}

string Summary::serialize(Metrics::Laziness laziness, const string &name, const vector<pair<string, string>> &labels)
{
	steady_clock::time_point now = steady_clock::now();
	steady_clock::time_point cutoff = now - duration_cast<steady_clock::duration>(window);

	vector<double> values_copy;
	{
		lock_guard<mutex> lock(mu);
		while (!values.empty() && values.front().first < cutoff) {
			values.pop_front();
		}
		values_copy.reserve(values.size());
		for (const auto &time_and_value : values) {
			values_copy.push_back(time_and_value.second);
		}
	}

	vector<pair<double, double>> answers;
	if (values_copy.size() == 0) {
		if (laziness == Metrics::PRINT_WHEN_NONEMPTY) {
			return "";
		}
		for (double quantile : quantiles) {
			answers.emplace_back(quantile, 0.0 / 0.0);
		}
	} else if (values_copy.size() == 1) {
		for (double quantile : quantiles) {
			answers.emplace_back(quantile, values_copy[0]);
		}
	} else {
		// We could probably do repeated nth_element, but the constant factor
		// gets a bit high, so just sorting probably is about as fast.
		sort(values_copy.begin(), values_copy.end());
		for (double quantile : quantiles) {
			double idx = quantile * (values_copy.size() - 1);
			size_t idx_floor = size_t(floor(idx));
			const double v0 = values_copy[idx_floor];

			if (idx_floor == values_copy.size() - 1) {
				answers.emplace_back(quantile, values_copy[idx_floor]);
			} else {
				// Linear interpolation.
				double t = idx - idx_floor;
				const double v1 = values_copy[idx_floor + 1];
				answers.emplace_back(quantile, v0 + t * (v1 - v0));
			}
		}
	}

	stringstream ss;
	ss.imbue(locale("C"));
	ss.precision(20);

	for (const auto &quantile_and_value : answers) {
		stringstream quantile_ss;
		quantile_ss.imbue(locale("C"));
		quantile_ss.precision(3);
		quantile_ss << quantile_and_value.first;
		vector<pair<string, string>> quantile_labels = labels;
		quantile_labels.emplace_back("quantile", quantile_ss.str());

		double val = quantile_and_value.second;;
		if (isnan(val)) {
			// Prometheus can't handle “-nan”.
			ss << Metrics::serialize_name(name, quantile_labels) << " NaN\n";
		} else {
			ss << Metrics::serialize_name(name, quantile_labels) << " " << val << "\n";
		}
	}

	ss << Metrics::serialize_name(name + "_sum", labels) << " " << sum.load() << "\n";
	ss << Metrics::serialize_name(name + "_count", labels) << " " << count.load() << "\n";
	return ss.str();
}
