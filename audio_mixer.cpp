#include "audio_mixer.h"

#include <assert.h>
#include <bmusb/bmusb.h>
#include <endian.h>
#include <math.h>
#ifdef __SSE2__
#include <immintrin.h>
#endif
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <limits>
#include <utility>

#include "db.h"
#include "flags.h"
#include "metrics.h"
#include "state.pb.h"
#include "timebase.h"

using namespace bmusb;
using namespace std;
using namespace std::chrono;
using namespace std::placeholders;

namespace {

// TODO: If these prove to be a bottleneck, they can be SSSE3-optimized
// (usually including multiple channels at a time).

void convert_fixed16_to_fp32(float *dst, size_t out_channel, size_t out_num_channels,
                             const uint8_t *src, size_t in_channel, size_t in_num_channels,
                             size_t num_samples)
{
	assert(in_channel < in_num_channels);
	assert(out_channel < out_num_channels);
	src += in_channel * 2;
	dst += out_channel;

	for (size_t i = 0; i < num_samples; ++i) {
		int16_t s = le16toh(*(int16_t *)src);
		*dst = s * (1.0f / 32768.0f);

		src += 2 * in_num_channels;
		dst += out_num_channels;
	}
}

void convert_fixed24_to_fp32(float *dst, size_t out_channel, size_t out_num_channels,
                             const uint8_t *src, size_t in_channel, size_t in_num_channels,
                             size_t num_samples)
{
	assert(in_channel < in_num_channels);
	assert(out_channel < out_num_channels);
	src += in_channel * 3;
	dst += out_channel;

	for (size_t i = 0; i < num_samples; ++i) {
		uint32_t s1 = src[0];
		uint32_t s2 = src[1];
		uint32_t s3 = src[2];
		uint32_t s = s1 | (s1 << 8) | (s2 << 16) | (s3 << 24);
		*dst = int(s) * (1.0f / 2147483648.0f);

		src += 3 * in_num_channels;
		dst += out_num_channels;
	}
}

void convert_fixed32_to_fp32(float *dst, size_t out_channel, size_t out_num_channels,
                             const uint8_t *src, size_t in_channel, size_t in_num_channels,
                             size_t num_samples)
{
	assert(in_channel < in_num_channels);
	assert(out_channel < out_num_channels);
	src += in_channel * 4;
	dst += out_channel;

	for (size_t i = 0; i < num_samples; ++i) {
		int32_t s = le32toh(*(int32_t *)src);
		*dst = s * (1.0f / 2147483648.0f);

		src += 4 * in_num_channels;
		dst += out_num_channels;
	}
}

float find_peak_plain(const float *samples, size_t num_samples) __attribute__((unused));

float find_peak_plain(const float *samples, size_t num_samples)
{
	float m = fabs(samples[0]);
	for (size_t i = 1; i < num_samples; ++i) {
		m = max(m, fabs(samples[i]));
	}
	return m;
}

#ifdef __SSE__
static inline float horizontal_max(__m128 m)
{
	__m128 tmp = _mm_shuffle_ps(m, m, _MM_SHUFFLE(1, 0, 3, 2));
	m = _mm_max_ps(m, tmp);
	tmp = _mm_shuffle_ps(m, m, _MM_SHUFFLE(2, 3, 0, 1));
	m = _mm_max_ps(m, tmp);
	return _mm_cvtss_f32(m);
}

float find_peak(const float *samples, size_t num_samples)
{
	const __m128 abs_mask = _mm_castsi128_ps(_mm_set1_epi32(0x7fffffffu));
	__m128 m = _mm_setzero_ps();
	for (size_t i = 0; i < (num_samples & ~3); i += 4) {
		__m128 x = _mm_loadu_ps(samples + i);
		x = _mm_and_ps(x, abs_mask);
		m = _mm_max_ps(m, x);
	}
	float result = horizontal_max(m);

	for (size_t i = (num_samples & ~3); i < num_samples; ++i) {
		result = max(result, fabs(samples[i]));
	}

#if 0
	// Self-test. We should be bit-exact the same.
	float reference_result = find_peak_plain(samples, num_samples);
	if (result != reference_result) {
		fprintf(stderr, "Error: Peak is %f [%f %f %f %f]; should be %f.\n",
			result,
			_mm_cvtss_f32(_mm_shuffle_ps(m, m, _MM_SHUFFLE(0, 0, 0, 0))),
			_mm_cvtss_f32(_mm_shuffle_ps(m, m, _MM_SHUFFLE(1, 1, 1, 1))),
			_mm_cvtss_f32(_mm_shuffle_ps(m, m, _MM_SHUFFLE(2, 2, 2, 2))),
			_mm_cvtss_f32(_mm_shuffle_ps(m, m, _MM_SHUFFLE(3, 3, 3, 3))),
			reference_result);
		abort();
	}
#endif
	return result;
}
#else
float find_peak(const float *samples, size_t num_samples)
{
	return find_peak_plain(samples, num_samples);
}
#endif

void deinterleave_samples(const vector<float> &in, vector<float> *out_l, vector<float> *out_r)
{
	size_t num_samples = in.size() / 2;
	out_l->resize(num_samples);
	out_r->resize(num_samples);

	const float *inptr = in.data();
	float *lptr = &(*out_l)[0];
	float *rptr = &(*out_r)[0];
	for (size_t i = 0; i < num_samples; ++i) {
		*lptr++ = *inptr++;
		*rptr++ = *inptr++;
	}
}

}  // namespace

AudioMixer::AudioMixer(unsigned num_cards)
	: num_cards(num_cards),
	  limiter(OUTPUT_FREQUENCY),
	  correlation(OUTPUT_FREQUENCY)
{
	for (unsigned bus_index = 0; bus_index < MAX_BUSES; ++bus_index) {
		locut[bus_index].init(FILTER_HPF, 2);
		eq[bus_index][EQ_BAND_BASS].init(FILTER_LOW_SHELF, 1);
		// Note: EQ_BAND_MID isn't used (see comments in apply_eq()).
		eq[bus_index][EQ_BAND_TREBLE].init(FILTER_HIGH_SHELF, 1);
		compressor[bus_index].reset(new StereoCompressor(OUTPUT_FREQUENCY));
		level_compressor[bus_index].reset(new StereoCompressor(OUTPUT_FREQUENCY));

		set_bus_settings(bus_index, get_default_bus_settings());
	}
	set_limiter_enabled(global_flags.limiter_enabled);
	set_final_makeup_gain_auto(global_flags.final_makeup_gain_auto);

	r128.init(2, OUTPUT_FREQUENCY);
	r128.integr_start();

	// hlen=16 is pretty low quality, but we use quite a bit of CPU otherwise,
	// and there's a limit to how important the peak meter is.
	peak_resampler.setup(OUTPUT_FREQUENCY, OUTPUT_FREQUENCY * 4, /*num_channels=*/2, /*hlen=*/16, /*frel=*/1.0);

	global_audio_mixer = this;
	alsa_pool.init();

	if (!global_flags.input_mapping_filename.empty()) {
		// Must happen after ALSAPool is initialized, as it needs to know the card list.
		current_mapping_mode = MappingMode::MULTICHANNEL;
		InputMapping new_input_mapping;
		if (!load_input_mapping_from_file(get_devices(),
		                                  global_flags.input_mapping_filename,
		                                  &new_input_mapping)) {
			fprintf(stderr, "Failed to load input mapping from '%s', exiting.\n",
				global_flags.input_mapping_filename.c_str());
			exit(1);
		}
		set_input_mapping(new_input_mapping);
	} else {
		set_simple_input(/*card_index=*/0);
		if (global_flags.multichannel_mapping_mode) {
			current_mapping_mode = MappingMode::MULTICHANNEL;
		}
	}

	global_metrics.add("audio_loudness_short_lufs", &metric_audio_loudness_short_lufs, Metrics::TYPE_GAUGE);
	global_metrics.add("audio_loudness_integrated_lufs", &metric_audio_loudness_integrated_lufs, Metrics::TYPE_GAUGE);
	global_metrics.add("audio_loudness_range_low_lufs", &metric_audio_loudness_range_low_lufs, Metrics::TYPE_GAUGE);
	global_metrics.add("audio_loudness_range_high_lufs", &metric_audio_loudness_range_high_lufs, Metrics::TYPE_GAUGE);
	global_metrics.add("audio_peak_dbfs", &metric_audio_peak_dbfs, Metrics::TYPE_GAUGE);
	global_metrics.add("audio_final_makeup_gain_db", &metric_audio_final_makeup_gain_db, Metrics::TYPE_GAUGE);
	global_metrics.add("audio_correlation", &metric_audio_correlation, Metrics::TYPE_GAUGE);
}

void AudioMixer::reset_resampler(DeviceSpec device_spec)
{
	lock_guard<timed_mutex> lock(audio_mutex);
	reset_resampler_mutex_held(device_spec);
}

void AudioMixer::reset_resampler_mutex_held(DeviceSpec device_spec)
{
	AudioDevice *device = find_audio_device(device_spec);

	if (device->interesting_channels.empty()) {
		device->resampling_queue.reset();
	} else {
		// TODO: ResamplingQueue should probably take the full device spec.
		// (It's only used for console output, though.)
		device->resampling_queue.reset(new ResamplingQueue(
			device_spec.index, device->capture_frequency, OUTPUT_FREQUENCY, device->interesting_channels.size(),
			global_flags.audio_queue_length_ms * 0.001));
	}
}

bool AudioMixer::add_audio(DeviceSpec device_spec, const uint8_t *data, unsigned num_samples, AudioFormat audio_format, int64_t frame_length, steady_clock::time_point frame_time)
{
	AudioDevice *device = find_audio_device(device_spec);

	unique_lock<timed_mutex> lock(audio_mutex, defer_lock);
	if (!lock.try_lock_for(chrono::milliseconds(10))) {
		return false;
	}
	if (device->resampling_queue == nullptr) {
		// No buses use this device; throw it away.
		return true;
	}

	unsigned num_channels = device->interesting_channels.size();
	assert(num_channels > 0);

	// Convert the audio to fp32.
	unique_ptr<float[]> audio(new float[num_samples * num_channels]);
	unsigned channel_index = 0;
	for (auto channel_it = device->interesting_channels.cbegin(); channel_it != device->interesting_channels.end(); ++channel_it, ++channel_index) {
		switch (audio_format.bits_per_sample) {
		case 0:
			assert(num_samples == 0);
			break;
		case 16:
			convert_fixed16_to_fp32(audio.get(), channel_index, num_channels, data, *channel_it, audio_format.num_channels, num_samples);
			break;
		case 24:
			convert_fixed24_to_fp32(audio.get(), channel_index, num_channels, data, *channel_it, audio_format.num_channels, num_samples);
			break;
		case 32:
			convert_fixed32_to_fp32(audio.get(), channel_index, num_channels, data, *channel_it, audio_format.num_channels, num_samples);
			break;
		default:
			fprintf(stderr, "Cannot handle audio with %u bits per sample\n", audio_format.bits_per_sample);
			assert(false);
		}
	}

	// If we changed frequency since last frame, we'll need to reset the resampler.
	if (audio_format.sample_rate != device->capture_frequency) {
		device->capture_frequency = audio_format.sample_rate;
		reset_resampler_mutex_held(device_spec);
	}

	// Now add it.
	device->resampling_queue->add_input_samples(frame_time, audio.get(), num_samples, ResamplingQueue::ADJUST_RATE);
	return true;
}

bool AudioMixer::add_silence(DeviceSpec device_spec, unsigned samples_per_frame, unsigned num_frames, int64_t frame_length)
{
	AudioDevice *device = find_audio_device(device_spec);

	unique_lock<timed_mutex> lock(audio_mutex, defer_lock);
	if (!lock.try_lock_for(chrono::milliseconds(10))) {
		return false;
	}
	if (device->resampling_queue == nullptr) {
		// No buses use this device; throw it away.
		return true;
	}

	unsigned num_channels = device->interesting_channels.size();
	assert(num_channels > 0);

	vector<float> silence(samples_per_frame * num_channels, 0.0f);
	for (unsigned i = 0; i < num_frames; ++i) {
		device->resampling_queue->add_input_samples(steady_clock::now(), silence.data(), samples_per_frame, ResamplingQueue::DO_NOT_ADJUST_RATE);
	}
	return true;
}

bool AudioMixer::silence_card(DeviceSpec device_spec, bool silence)
{
	AudioDevice *device = find_audio_device(device_spec);

	unique_lock<timed_mutex> lock(audio_mutex, defer_lock);
	if (!lock.try_lock_for(chrono::milliseconds(10))) {
		return false;
	}

	if (device->silenced && !silence) {
		reset_resampler_mutex_held(device_spec);
	}
	device->silenced = silence;
	return true;
}

AudioMixer::BusSettings AudioMixer::get_default_bus_settings()
{
	BusSettings settings;
	settings.fader_volume_db = 0.0f;
	settings.muted = false;
	settings.locut_enabled = global_flags.locut_enabled;
	for (unsigned band_index = 0; band_index < NUM_EQ_BANDS; ++band_index) {
		settings.eq_level_db[band_index] = 0.0f;
	}
	settings.gain_staging_db = global_flags.initial_gain_staging_db;
	settings.level_compressor_enabled = global_flags.gain_staging_auto;
	settings.compressor_threshold_dbfs = ref_level_dbfs - 12.0f;  // -12 dB.
	settings.compressor_enabled = global_flags.compressor_enabled;
	return settings;
}

AudioMixer::BusSettings AudioMixer::get_bus_settings(unsigned bus_index) const
{
	lock_guard<timed_mutex> lock(audio_mutex);
	BusSettings settings;
	settings.fader_volume_db = fader_volume_db[bus_index];
	settings.muted = mute[bus_index];
	settings.locut_enabled = locut_enabled[bus_index];
	for (unsigned band_index = 0; band_index < NUM_EQ_BANDS; ++band_index) {
		settings.eq_level_db[band_index] = eq_level_db[bus_index][band_index];
	}
	settings.gain_staging_db = gain_staging_db[bus_index];
	settings.level_compressor_enabled = level_compressor_enabled[bus_index];
	settings.compressor_threshold_dbfs = compressor_threshold_dbfs[bus_index];
	settings.compressor_enabled = compressor_enabled[bus_index];
	return settings;
}

void AudioMixer::set_bus_settings(unsigned bus_index, const AudioMixer::BusSettings &settings)
{
	lock_guard<timed_mutex> lock(audio_mutex);
	fader_volume_db[bus_index] = settings.fader_volume_db;
	mute[bus_index] = settings.muted;
	locut_enabled[bus_index] = settings.locut_enabled;
	for (unsigned band_index = 0; band_index < NUM_EQ_BANDS; ++band_index) {
		eq_level_db[bus_index][band_index] = settings.eq_level_db[band_index];
	}
	gain_staging_db[bus_index] = settings.gain_staging_db;
	last_gain_staging_db[bus_index] = gain_staging_db[bus_index];
	level_compressor_enabled[bus_index] = settings.level_compressor_enabled;
	compressor_threshold_dbfs[bus_index] = settings.compressor_threshold_dbfs;
	compressor_enabled[bus_index] = settings.compressor_enabled;
}

AudioMixer::AudioDevice *AudioMixer::find_audio_device(DeviceSpec device)
{
	switch (device.type) {
	case InputSourceType::CAPTURE_CARD:
		return &video_cards[device.index];
	case InputSourceType::ALSA_INPUT:
		return &alsa_inputs[device.index];
	case InputSourceType::SILENCE:
	default:
		assert(false);
	}
	return nullptr;
}

// Get a pointer to the given channel from the given device.
// The channel must be picked out earlier and resampled.
void AudioMixer::find_sample_src_from_device(const map<DeviceSpec, vector<float>> &samples_card, DeviceSpec device_spec, int source_channel, const float **srcptr, unsigned *stride)
{
	static float zero = 0.0f;
	if (source_channel == -1 || device_spec.type == InputSourceType::SILENCE) {
		*srcptr = &zero;
		*stride = 0;
		return;
	}
	AudioDevice *device = find_audio_device(device_spec);
	assert(device->interesting_channels.count(source_channel) != 0);
	unsigned channel_index = 0;
	for (int channel : device->interesting_channels) {
		if (channel == source_channel) break;
		++channel_index;
	}
	assert(channel_index < device->interesting_channels.size());
	const auto it = samples_card.find(device_spec);
	assert(it != samples_card.end());
	*srcptr = &(it->second)[channel_index];
	*stride = device->interesting_channels.size();
}

// TODO: Can be SSSE3-optimized if need be.
void AudioMixer::fill_audio_bus(const map<DeviceSpec, vector<float>> &samples_card, const InputMapping::Bus &bus, unsigned num_samples, float *output)
{
	if (bus.device.type == InputSourceType::SILENCE) {
		memset(output, 0, num_samples * 2 * sizeof(*output));
	} else {
		assert(bus.device.type == InputSourceType::CAPTURE_CARD ||
		       bus.device.type == InputSourceType::ALSA_INPUT);
		const float *lsrc, *rsrc;
		unsigned lstride, rstride;
		float *dptr = output;
		find_sample_src_from_device(samples_card, bus.device, bus.source_channel[0], &lsrc, &lstride);
		find_sample_src_from_device(samples_card, bus.device, bus.source_channel[1], &rsrc, &rstride);
		for (unsigned i = 0; i < num_samples; ++i) {
			*dptr++ = *lsrc;
			*dptr++ = *rsrc;
			lsrc += lstride;
			rsrc += rstride;
		}
	}
}

vector<DeviceSpec> AudioMixer::get_active_devices() const
{
	vector<DeviceSpec> ret;
	for (unsigned card_index = 0; card_index < MAX_VIDEO_CARDS; ++card_index) {
		const DeviceSpec device_spec{InputSourceType::CAPTURE_CARD, card_index};
		if (!find_audio_device(device_spec)->interesting_channels.empty()) {
			ret.push_back(device_spec);
		}
	}
	for (unsigned card_index = 0; card_index < MAX_ALSA_CARDS; ++card_index) {
		const DeviceSpec device_spec{InputSourceType::ALSA_INPUT, card_index};
		if (!find_audio_device(device_spec)->interesting_channels.empty()) {
			ret.push_back(device_spec);
		}
	}
	return ret;
}

namespace {

void apply_gain(float db, float last_db, vector<float> *samples)
{
	if (fabs(db - last_db) < 1e-3) {
		// Constant over this frame.
		const float gain = from_db(db);
		for (size_t i = 0; i < samples->size(); ++i) {
			(*samples)[i] *= gain;
		}
	} else {
		// We need to do a fade.
		unsigned num_samples = samples->size() / 2;
		float gain = from_db(last_db);
		const float gain_inc = pow(from_db(db - last_db), 1.0 / num_samples);
		for (size_t i = 0; i < num_samples; ++i) {
			(*samples)[i * 2 + 0] *= gain;
			(*samples)[i * 2 + 1] *= gain;
			gain *= gain_inc;
		}
	}
}

}  // namespace

vector<float> AudioMixer::get_output(steady_clock::time_point ts, unsigned num_samples, ResamplingQueue::RateAdjustmentPolicy rate_adjustment_policy)
{
	map<DeviceSpec, vector<float>> samples_card;
	vector<float> samples_bus;

	lock_guard<timed_mutex> lock(audio_mutex);

	// Pick out all the interesting channels from all the cards.
	for (const DeviceSpec &device_spec : get_active_devices()) {
		AudioDevice *device = find_audio_device(device_spec);
		samples_card[device_spec].resize(num_samples * device->interesting_channels.size());
		if (device->silenced) {
			memset(&samples_card[device_spec][0], 0, samples_card[device_spec].size() * sizeof(float));
		} else {
			device->resampling_queue->get_output_samples(
				ts,
				&samples_card[device_spec][0],
				num_samples,
				rate_adjustment_policy);
		}
	}

	vector<float> samples_out, left, right;
	samples_out.resize(num_samples * 2);
	samples_bus.resize(num_samples * 2);
	for (unsigned bus_index = 0; bus_index < input_mapping.buses.size(); ++bus_index) {
		fill_audio_bus(samples_card, input_mapping.buses[bus_index], num_samples, &samples_bus[0]);
		apply_eq(bus_index, &samples_bus);

		{
			lock_guard<mutex> lock(compressor_mutex);

			// Apply a level compressor to get the general level right.
			// Basically, if it's over about -40 dBFS, we squeeze it down to that level
			// (or more precisely, near it, since we don't use infinite ratio),
			// then apply a makeup gain to get it to -14 dBFS. -14 dBFS is, of course,
			// entirely arbitrary, but from practical tests with speech, it seems to
			// put ut around -23 LUFS, so it's a reasonable starting point for later use.
			if (level_compressor_enabled[bus_index]) {
				float threshold = 0.01f;   // -40 dBFS.
				float ratio = 20.0f;
				float attack_time = 0.5f;
				float release_time = 20.0f;
				float makeup_gain = from_db(ref_level_dbfs - (-40.0f));  // +26 dB.
				level_compressor[bus_index]->process(samples_bus.data(), samples_bus.size() / 2, threshold, ratio, attack_time, release_time, makeup_gain);
				gain_staging_db[bus_index] = to_db(level_compressor[bus_index]->get_attenuation() * makeup_gain);
			} else {
				// Just apply the gain we already had.
				float db = gain_staging_db[bus_index];
				float last_db = last_gain_staging_db[bus_index];
				apply_gain(db, last_db, &samples_bus);
			}
			last_gain_staging_db[bus_index] = gain_staging_db[bus_index];

#if 0
			printf("level=%f (%+5.2f dBFS) attenuation=%f (%+5.2f dB) end_result=%+5.2f dB\n",
				level_compressor.get_level(), to_db(level_compressor.get_level()),
				level_compressor.get_attenuation(), to_db(level_compressor.get_attenuation()),
				to_db(level_compressor.get_level() * level_compressor.get_attenuation() * makeup_gain));
#endif

			// The real compressor.
			if (compressor_enabled[bus_index]) {
				float threshold = from_db(compressor_threshold_dbfs[bus_index]);
				float ratio = 20.0f;
				float attack_time = 0.005f;
				float release_time = 0.040f;
				float makeup_gain = 2.0f;  // +6 dB.
				compressor[bus_index]->process(samples_bus.data(), samples_bus.size() / 2, threshold, ratio, attack_time, release_time, makeup_gain);
		//		compressor_att = compressor.get_attenuation();
			}
		}

		add_bus_to_master(bus_index, samples_bus, &samples_out);
		deinterleave_samples(samples_bus, &left, &right);
		measure_bus_levels(bus_index, left, right);
	}

	{
		lock_guard<mutex> lock(compressor_mutex);

		// Finally a limiter at -4 dB (so, -10 dBFS) to take out the worst peaks only.
		// Note that since ratio is not infinite, we could go slightly higher than this.
		if (limiter_enabled) {
			float threshold = from_db(limiter_threshold_dbfs);
			float ratio = 30.0f;
			float attack_time = 0.0f;  // Instant.
			float release_time = 0.020f;
			float makeup_gain = 1.0f;  // 0 dB.
			limiter.process(samples_out.data(), samples_out.size() / 2, threshold, ratio, attack_time, release_time, makeup_gain);
	//		limiter_att = limiter.get_attenuation();
		}

	//	printf("limiter=%+5.1f  compressor=%+5.1f\n", to_db(limiter_att), to_db(compressor_att));
	}

	// At this point, we are most likely close to +0 LU (at least if the
	// faders sum to 0 dB and the compressors are on), but all of our
	// measurements have been on raw sample values, not R128 values.
	// So we have a final makeup gain to get us to +0 LU; the gain
	// adjustments required should be relatively small, and also, the
	// offset shouldn't change much (only if the type of audio changes
	// significantly). Thus, we shoot for updating this value basically
	// “whenever we process buffers”, since the R128 calculation isn't exactly
	// something we get out per-sample.
	//
	// Note that there's a feedback loop here, so we choose a very slow filter
	// (half-time of 30 seconds).
	double target_loudness_factor, alpha;
	double loudness_lu = r128.loudness_M() - ref_level_lufs;
	target_loudness_factor = final_makeup_gain * from_db(-loudness_lu);

	// If we're outside +/- 5 LU (after correction), we don't count it as
	// a normal signal (probably silence) and don't change the
	// correction factor; just apply what we already have.
	if (fabs(loudness_lu) >= 5.0 || !final_makeup_gain_auto) {
		alpha = 0.0;
	} else {
		// Formula adapted from
		// https://en.wikipedia.org/wiki/Low-pass_filter#Simple_infinite_impulse_response_filter.
		const double half_time_s = 30.0;
		const double fc_mul_2pi_delta_t = 1.0 / (half_time_s * OUTPUT_FREQUENCY);
		alpha = fc_mul_2pi_delta_t / (fc_mul_2pi_delta_t + 1.0);
	}

	{
		lock_guard<mutex> lock(compressor_mutex);
		double m = final_makeup_gain;
		for (size_t i = 0; i < samples_out.size(); i += 2) {
			samples_out[i + 0] *= m;
			samples_out[i + 1] *= m;
			m += (target_loudness_factor - m) * alpha;
		}
		final_makeup_gain = m;
	}

	update_meters(samples_out);

	return samples_out;
}

namespace {

void apply_filter_fade(StereoFilter *filter, float *data, unsigned num_samples, float cutoff_hz, float db, float last_db)
{
	// A granularity of 32 samples is an okay tradeoff between speed and
	// smoothness; recalculating the filters is pretty expensive, so it's
	// good that we don't do this all the time.
	static constexpr unsigned filter_granularity_samples = 32;

	const float cutoff_linear = cutoff_hz * 2.0 * M_PI / OUTPUT_FREQUENCY;
	if (fabs(db - last_db) < 1e-3) {
		// Constant over this frame.
		if (fabs(db) > 0.01f) {
			filter->render(data, num_samples, cutoff_linear, 0.5f, db / 40.0f);
		}
	} else {
		// We need to do a fade. (Rounding up avoids division by zero.)
		unsigned num_blocks = (num_samples + filter_granularity_samples - 1) / filter_granularity_samples;
		const float inc_db_norm = (db - last_db) / 40.0f / num_blocks;
		float db_norm = db / 40.0f;
		for (size_t i = 0; i < num_samples; i += filter_granularity_samples) {
			size_t samples_this_block = std::min<size_t>(num_samples - i, filter_granularity_samples);
			filter->render(data + i * 2, samples_this_block, cutoff_linear, 0.5f, db_norm);
			db_norm += inc_db_norm;
		}
	}
}

}  // namespace

void AudioMixer::apply_eq(unsigned bus_index, vector<float> *samples_bus)
{
	constexpr float bass_freq_hz = 200.0f;
	constexpr float treble_freq_hz = 4700.0f;

	// Cut away everything under 120 Hz (or whatever the cutoff is);
	// we don't need it for voice, and it will reduce headroom
	// and confuse the compressor. (In particular, any hums at 50 or 60 Hz
	// should be dampened.)
	if (locut_enabled[bus_index]) {
		locut[bus_index].render(samples_bus->data(), samples_bus->size() / 2, locut_cutoff_hz * 2.0 * M_PI / OUTPUT_FREQUENCY, 0.5f);
	}

	// Apply the rest of the EQ. Since we only have a simple three-band EQ,
	// we can implement it with two shelf filters. We use a simple gain to
	// set the mid-level filter, and then offset the low and high bands
	// from that if we need to. (We could perhaps have folded the gain into
	// the next part, but it's so cheap that the trouble isn't worth it.)
	//
	// If any part of the EQ has changed appreciably since last frame,
	// we fade smoothly during the course of this frame.
	const float bass_db = eq_level_db[bus_index][EQ_BAND_BASS];
	const float mid_db = eq_level_db[bus_index][EQ_BAND_MID];
	const float treble_db = eq_level_db[bus_index][EQ_BAND_TREBLE];

	const float last_bass_db = last_eq_level_db[bus_index][EQ_BAND_BASS];
	const float last_mid_db = last_eq_level_db[bus_index][EQ_BAND_MID];
	const float last_treble_db = last_eq_level_db[bus_index][EQ_BAND_TREBLE];

	assert(samples_bus->size() % 2 == 0);
	const unsigned num_samples = samples_bus->size() / 2;

	apply_gain(mid_db, last_mid_db, samples_bus);

	apply_filter_fade(&eq[bus_index][EQ_BAND_BASS], samples_bus->data(), num_samples, bass_freq_hz, bass_db - mid_db, last_bass_db - last_mid_db);
	apply_filter_fade(&eq[bus_index][EQ_BAND_TREBLE], samples_bus->data(), num_samples, treble_freq_hz, treble_db - mid_db, last_treble_db - last_mid_db);

	last_eq_level_db[bus_index][EQ_BAND_BASS] = bass_db;
	last_eq_level_db[bus_index][EQ_BAND_MID] = mid_db;
	last_eq_level_db[bus_index][EQ_BAND_TREBLE] = treble_db;
}

void AudioMixer::add_bus_to_master(unsigned bus_index, const vector<float> &samples_bus, vector<float> *samples_out)
{
	assert(samples_bus.size() == samples_out->size());
	assert(samples_bus.size() % 2 == 0);
	unsigned num_samples = samples_bus.size() / 2;
	const float new_volume_db = mute[bus_index] ? -90.0f : fader_volume_db[bus_index].load();
	if (fabs(new_volume_db - last_fader_volume_db[bus_index]) > 1e-3) {
		// The volume has changed; do a fade over the course of this frame.
		// (We might have some numerical issues here, but it seems to sound OK.)
		// For the purpose of fading here, the silence floor is set to -90 dB
		// (the fader only goes to -84).
		float old_volume = from_db(max<float>(last_fader_volume_db[bus_index], -90.0f));
		float volume = from_db(max<float>(new_volume_db, -90.0f));

		float volume_inc = pow(volume / old_volume, 1.0 / num_samples);
		volume = old_volume;
		if (bus_index == 0) {
			for (unsigned i = 0; i < num_samples; ++i) {
				(*samples_out)[i * 2 + 0] = samples_bus[i * 2 + 0] * volume;
				(*samples_out)[i * 2 + 1] = samples_bus[i * 2 + 1] * volume;
				volume *= volume_inc;
			}
		} else {
			for (unsigned i = 0; i < num_samples; ++i) {
				(*samples_out)[i * 2 + 0] += samples_bus[i * 2 + 0] * volume;
				(*samples_out)[i * 2 + 1] += samples_bus[i * 2 + 1] * volume;
				volume *= volume_inc;
			}
		}
	} else if (new_volume_db > -90.0f) {
		float volume = from_db(new_volume_db);
		if (bus_index == 0) {
			for (unsigned i = 0; i < num_samples; ++i) {
				(*samples_out)[i * 2 + 0] = samples_bus[i * 2 + 0] * volume;
				(*samples_out)[i * 2 + 1] = samples_bus[i * 2 + 1] * volume;
			}
		} else {
			for (unsigned i = 0; i < num_samples; ++i) {
				(*samples_out)[i * 2 + 0] += samples_bus[i * 2 + 0] * volume;
				(*samples_out)[i * 2 + 1] += samples_bus[i * 2 + 1] * volume;
			}
		}
	}

	last_fader_volume_db[bus_index] = new_volume_db;
}

void AudioMixer::measure_bus_levels(unsigned bus_index, const vector<float> &left, const vector<float> &right)
{
	assert(left.size() == right.size());
	const float volume = mute[bus_index] ? 0.0f : from_db(fader_volume_db[bus_index]);
	const float peak_levels[2] = {
		find_peak(left.data(), left.size()) * volume,
		find_peak(right.data(), right.size()) * volume
	};
	for (unsigned channel = 0; channel < 2; ++channel) {
		// Compute the current value, including hold and falloff.
		// The constants are borrowed from zita-mu1 by Fons Adriaensen.
		static constexpr float hold_sec = 0.5f;
		static constexpr float falloff_db_sec = 15.0f;  // dB/sec falloff after hold.
		float current_peak;
		PeakHistory &history = peak_history[bus_index][channel];
		history.historic_peak = max(history.historic_peak, peak_levels[channel]);
		if (history.age_seconds < hold_sec) {
			current_peak = history.last_peak;
		} else {
			current_peak = history.last_peak * from_db(-falloff_db_sec * (history.age_seconds - hold_sec));
		}

		// See if we have a new peak to replace the old (possibly falling) one.
		if (peak_levels[channel] > current_peak) {
			history.last_peak = peak_levels[channel];
			history.age_seconds = 0.0f;  // Not 100% correct, but more than good enough given our frame sizes.
			current_peak = peak_levels[channel];
		} else {
			history.age_seconds += float(left.size()) / OUTPUT_FREQUENCY;
		}
		history.current_level = peak_levels[channel];
		history.current_peak = current_peak;
	}
}

void AudioMixer::update_meters(const vector<float> &samples)
{
	// Upsample 4x to find interpolated peak.
	peak_resampler.inp_data = const_cast<float *>(samples.data());
	peak_resampler.inp_count = samples.size() / 2;

	vector<float> interpolated_samples;
	interpolated_samples.resize(samples.size());
	{
		lock_guard<mutex> lock(audio_measure_mutex);

		while (peak_resampler.inp_count > 0) {  // About four iterations.
			peak_resampler.out_data = &interpolated_samples[0];
			peak_resampler.out_count = interpolated_samples.size() / 2;
			peak_resampler.process();
			size_t out_stereo_samples = interpolated_samples.size() / 2 - peak_resampler.out_count;
			peak = max<float>(peak, find_peak(interpolated_samples.data(), out_stereo_samples * 2));
			peak_resampler.out_data = nullptr;
		}
	}

	// Find R128 levels and L/R correlation.
	vector<float> left, right;
	deinterleave_samples(samples, &left, &right);
	float *ptrs[] = { left.data(), right.data() };
	{
		lock_guard<mutex> lock(audio_measure_mutex);
		r128.process(left.size(), ptrs);
		correlation.process_samples(samples);
	}

	send_audio_level_callback();
}

void AudioMixer::reset_meters()
{
	lock_guard<mutex> lock(audio_measure_mutex);
	peak_resampler.reset();
	peak = 0.0f;
	r128.reset();
	r128.integr_start();
	correlation.reset();
}

void AudioMixer::send_audio_level_callback()
{
	if (audio_level_callback == nullptr) {
		return;
	}

	lock_guard<mutex> lock(audio_measure_mutex);
	double loudness_s = r128.loudness_S();
	double loudness_i = r128.integrated();
	double loudness_range_low = r128.range_min();
	double loudness_range_high = r128.range_max();

	metric_audio_loudness_short_lufs = loudness_s;
	metric_audio_loudness_integrated_lufs = loudness_i;
	metric_audio_loudness_range_low_lufs = loudness_range_low;
	metric_audio_loudness_range_high_lufs = loudness_range_high;
	metric_audio_peak_dbfs = to_db(peak);
	metric_audio_final_makeup_gain_db = to_db(final_makeup_gain);
	metric_audio_correlation = correlation.get_correlation();

	vector<BusLevel> bus_levels;
	bus_levels.resize(input_mapping.buses.size());
	{
		lock_guard<mutex> lock(compressor_mutex);
		for (unsigned bus_index = 0; bus_index < bus_levels.size(); ++bus_index) {
			BusLevel &levels = bus_levels[bus_index];
			BusMetrics &metrics = bus_metrics[bus_index];

			levels.current_level_dbfs[0] = metrics.current_level_dbfs[0] = to_db(peak_history[bus_index][0].current_level);
			levels.current_level_dbfs[1] = metrics.current_level_dbfs[1] = to_db(peak_history[bus_index][1].current_level);
			levels.peak_level_dbfs[0] = metrics.peak_level_dbfs[0] = to_db(peak_history[bus_index][0].current_peak);
			levels.peak_level_dbfs[1] = metrics.peak_level_dbfs[1] = to_db(peak_history[bus_index][1].current_peak);
			levels.historic_peak_dbfs = metrics.historic_peak_dbfs = to_db(
				max(peak_history[bus_index][0].historic_peak,
				    peak_history[bus_index][1].historic_peak));
			levels.gain_staging_db = metrics.gain_staging_db = gain_staging_db[bus_index];
			if (compressor_enabled[bus_index]) {
				levels.compressor_attenuation_db = metrics.compressor_attenuation_db = -to_db(compressor[bus_index]->get_attenuation());
			} else {
				levels.compressor_attenuation_db = 0.0;
				metrics.compressor_attenuation_db = 0.0 / 0.0;
			}
		}
	}

	audio_level_callback(loudness_s, to_db(peak), bus_levels,
		loudness_i, loudness_range_low, loudness_range_high,
		to_db(final_makeup_gain),
		correlation.get_correlation());
}

map<DeviceSpec, DeviceInfo> AudioMixer::get_devices()
{
	lock_guard<timed_mutex> lock(audio_mutex);

	map<DeviceSpec, DeviceInfo> devices;
	for (unsigned card_index = 0; card_index < num_cards; ++card_index) {
		const DeviceSpec spec{ InputSourceType::CAPTURE_CARD, card_index };
		const AudioDevice *device = &video_cards[card_index];
		DeviceInfo info;
		info.display_name = device->display_name;
		info.num_channels = 8;
		devices.insert(make_pair(spec, info));
	}
	vector<ALSAPool::Device> available_alsa_devices = alsa_pool.get_devices();
	for (unsigned card_index = 0; card_index < available_alsa_devices.size(); ++card_index) {
		const DeviceSpec spec{ InputSourceType::ALSA_INPUT, card_index };
		const ALSAPool::Device &device = available_alsa_devices[card_index];
		DeviceInfo info;
		info.display_name = device.display_name();
		info.num_channels = device.num_channels;
		info.alsa_name = device.name;
		info.alsa_info = device.info;
		info.alsa_address = device.address;
		devices.insert(make_pair(spec, info));
	}
	return devices;
}

void AudioMixer::set_display_name(DeviceSpec device_spec, const string &name)
{
	AudioDevice *device = find_audio_device(device_spec);

	lock_guard<timed_mutex> lock(audio_mutex);
	device->display_name = name;
}

void AudioMixer::serialize_device(DeviceSpec device_spec, DeviceSpecProto *device_spec_proto)
{
	lock_guard<timed_mutex> lock(audio_mutex);
	switch (device_spec.type) {
		case InputSourceType::SILENCE:
			device_spec_proto->set_type(DeviceSpecProto::SILENCE);
			break;
		case InputSourceType::CAPTURE_CARD:
			device_spec_proto->set_type(DeviceSpecProto::CAPTURE_CARD);
			device_spec_proto->set_index(device_spec.index);
			device_spec_proto->set_display_name(video_cards[device_spec.index].display_name);
			break;
		case InputSourceType::ALSA_INPUT:
			alsa_pool.serialize_device(device_spec.index, device_spec_proto);
			break;
	}
}

void AudioMixer::set_simple_input(unsigned card_index)
{
	InputMapping new_input_mapping;
	InputMapping::Bus input;
	input.name = "Main";
	input.device.type = InputSourceType::CAPTURE_CARD;
	input.device.index = card_index;
	input.source_channel[0] = 0;
	input.source_channel[1] = 1;

	new_input_mapping.buses.push_back(input);

	lock_guard<timed_mutex> lock(audio_mutex);
	current_mapping_mode = MappingMode::SIMPLE;
	set_input_mapping_lock_held(new_input_mapping);
	fader_volume_db[0] = 0.0f;
}

unsigned AudioMixer::get_simple_input() const
{
	lock_guard<timed_mutex> lock(audio_mutex);
	if (input_mapping.buses.size() == 1 &&
	    input_mapping.buses[0].device.type == InputSourceType::CAPTURE_CARD &&
	    input_mapping.buses[0].source_channel[0] == 0 &&
	    input_mapping.buses[0].source_channel[1] == 1) {
		return input_mapping.buses[0].device.index;
	} else {
		return numeric_limits<unsigned>::max();
	}
}

void AudioMixer::set_input_mapping(const InputMapping &new_input_mapping)
{
	lock_guard<timed_mutex> lock(audio_mutex);
	set_input_mapping_lock_held(new_input_mapping);
	current_mapping_mode = MappingMode::MULTICHANNEL;
}

AudioMixer::MappingMode AudioMixer::get_mapping_mode() const
{
	lock_guard<timed_mutex> lock(audio_mutex);
	return current_mapping_mode;
}

void AudioMixer::set_input_mapping_lock_held(const InputMapping &new_input_mapping)
{
	map<DeviceSpec, set<unsigned>> interesting_channels;
	for (const InputMapping::Bus &bus : new_input_mapping.buses) {
		if (bus.device.type == InputSourceType::CAPTURE_CARD ||
		    bus.device.type == InputSourceType::ALSA_INPUT) {
			for (unsigned channel = 0; channel < 2; ++channel) {
				if (bus.source_channel[channel] != -1) {
					interesting_channels[bus.device].insert(bus.source_channel[channel]);
				}
			}
		}
	}

	// Kill all the old metrics, and set up new ones.
	for (unsigned bus_index = 0; bus_index < input_mapping.buses.size(); ++bus_index) {
		BusMetrics &metrics = bus_metrics[bus_index];

		vector<pair<string, string>> labels_left = metrics.labels;
		labels_left.emplace_back("channel", "left");
		vector<pair<string, string>> labels_right = metrics.labels;
		labels_right.emplace_back("channel", "right");

		global_metrics.remove("bus_current_level_dbfs", labels_left);
		global_metrics.remove("bus_current_level_dbfs", labels_right);
		global_metrics.remove("bus_peak_level_dbfs", labels_left);
		global_metrics.remove("bus_peak_level_dbfs", labels_right);
		global_metrics.remove("bus_historic_peak_dbfs", metrics.labels);
		global_metrics.remove("bus_gain_staging_db", metrics.labels);
		global_metrics.remove("bus_compressor_attenuation_db", metrics.labels);
	}
	bus_metrics.reset(new BusMetrics[new_input_mapping.buses.size()]);
	for (unsigned bus_index = 0; bus_index < new_input_mapping.buses.size(); ++bus_index) {
		const InputMapping::Bus &bus = new_input_mapping.buses[bus_index];
		BusMetrics &metrics = bus_metrics[bus_index];

		char bus_index_str[16], source_index_str[16], source_channels_str[64];
		snprintf(bus_index_str, sizeof(bus_index_str), "%u", bus_index);
		snprintf(source_index_str, sizeof(source_index_str), "%u", bus.device.index);
		snprintf(source_channels_str, sizeof(source_channels_str), "%d:%d", bus.source_channel[0], bus.source_channel[1]);

		vector<pair<string, string>> labels;
		metrics.labels.emplace_back("index", bus_index_str);
		metrics.labels.emplace_back("name", bus.name);
		if (bus.device.type == InputSourceType::SILENCE) {
			metrics.labels.emplace_back("source_type", "silence");
		} else if (bus.device.type == InputSourceType::CAPTURE_CARD) {
			metrics.labels.emplace_back("source_type", "capture_card");
		} else if (bus.device.type == InputSourceType::ALSA_INPUT) {
			metrics.labels.emplace_back("source_type", "alsa_input");
		} else {
			assert(false);
		}
		metrics.labels.emplace_back("source_index", source_index_str);
		metrics.labels.emplace_back("source_channels", source_channels_str);

		vector<pair<string, string>> labels_left = metrics.labels;
		labels_left.emplace_back("channel", "left");
		vector<pair<string, string>> labels_right = metrics.labels;
		labels_right.emplace_back("channel", "right");

		global_metrics.add("bus_current_level_dbfs", labels_left, &metrics.current_level_dbfs[0], Metrics::TYPE_GAUGE);
		global_metrics.add("bus_current_level_dbfs", labels_right, &metrics.current_level_dbfs[1], Metrics::TYPE_GAUGE);
		global_metrics.add("bus_peak_level_dbfs", labels_left, &metrics.peak_level_dbfs[0], Metrics::TYPE_GAUGE);
		global_metrics.add("bus_peak_level_dbfs", labels_right, &metrics.peak_level_dbfs[1], Metrics::TYPE_GAUGE);
		global_metrics.add("bus_historic_peak_dbfs", metrics.labels, &metrics.historic_peak_dbfs, Metrics::TYPE_GAUGE);
		global_metrics.add("bus_gain_staging_db", metrics.labels, &metrics.gain_staging_db, Metrics::TYPE_GAUGE);
		global_metrics.add("bus_compressor_attenuation_db", metrics.labels, &metrics.compressor_attenuation_db, Metrics::TYPE_GAUGE);
	}

	// Reset resamplers for all cards that don't have the exact same state as before.
	for (unsigned card_index = 0; card_index < MAX_VIDEO_CARDS; ++card_index) {
		const DeviceSpec device_spec{InputSourceType::CAPTURE_CARD, card_index};
		AudioDevice *device = find_audio_device(device_spec);
		if (device->interesting_channels != interesting_channels[device_spec]) {
			device->interesting_channels = interesting_channels[device_spec];
			reset_resampler_mutex_held(device_spec);
		}
	}
	for (unsigned card_index = 0; card_index < MAX_ALSA_CARDS; ++card_index) {
		const DeviceSpec device_spec{InputSourceType::ALSA_INPUT, card_index};
		AudioDevice *device = find_audio_device(device_spec);
		if (interesting_channels[device_spec].empty()) {
			alsa_pool.release_device(card_index);
		} else {
			alsa_pool.hold_device(card_index);
		}
		if (device->interesting_channels != interesting_channels[device_spec]) {
			device->interesting_channels = interesting_channels[device_spec];
			alsa_pool.reset_device(device_spec.index);
			reset_resampler_mutex_held(device_spec);
		}
	}

	input_mapping = new_input_mapping;
}

InputMapping AudioMixer::get_input_mapping() const
{
	lock_guard<timed_mutex> lock(audio_mutex);
	return input_mapping;
}

unsigned AudioMixer::num_buses() const
{
	lock_guard<timed_mutex> lock(audio_mutex);
	return input_mapping.buses.size();
}

void AudioMixer::reset_peak(unsigned bus_index)
{
	lock_guard<timed_mutex> lock(audio_mutex);
	for (unsigned channel = 0; channel < 2; ++channel) {
		PeakHistory &history = peak_history[bus_index][channel];
		history.current_level = 0.0f;
		history.historic_peak = 0.0f;
		history.current_peak = 0.0f;
		history.last_peak = 0.0f;
		history.age_seconds = 0.0f;
	}
}

AudioMixer *global_audio_mixer = nullptr;
