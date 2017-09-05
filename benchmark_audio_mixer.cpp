// Rather simplistic benchmark of AudioMixer. Sets up a simple mapping
// with the default settings, feeds some white noise to the inputs and
// runs a while. Useful for e.g. profiling.

#include <assert.h>
#include <bmusb/bmusb.h>
#include <stdint.h>
#include <stdio.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <ratio>
#include <vector>

#include "audio_mixer.h"
#include "db.h"
#include "defs.h"
#include "input_mapping.h"
#include "resampling_queue.h"
#include "timebase.h"

#define NUM_BENCHMARK_CARDS 4
#define NUM_WARMUP_FRAMES 100
#define NUM_BENCHMARK_FRAMES 1000
#define NUM_TEST_FRAMES 10
#define NUM_CHANNELS 8
#define NUM_SAMPLES 1024

using namespace std;
using namespace std::chrono;

// 16-bit samples, white noise at full volume.
uint8_t samples16[(NUM_SAMPLES * NUM_CHANNELS + 1024) * sizeof(uint16_t)];

// 24-bit samples, white noise at low volume (-48 dB).
uint8_t samples24[(NUM_SAMPLES * NUM_CHANNELS + 1024) * 3];

static uint32_t seed = 1234;

// We use our own instead of rand() to get deterministic behavior.
// Quality doesn't really matter much.
uint32_t lcgrand()
{
	seed = seed * 1103515245u + 12345u;
	return seed;
}

void reset_lcgrand()
{
	seed = 1234;
}

void callback(float level_lufs, float peak_db,
              std::vector<AudioMixer::BusLevel> bus_levels,
	      float global_level_lufs, float range_low_lufs, float range_high_lufs,
	      float final_makeup_gain_db,
	      float correlation)
{
	// Empty.
}

vector<float> process_frame(unsigned frame_num, AudioMixer *mixer)
{
	duration<int64_t, ratio<NUM_SAMPLES, OUTPUT_FREQUENCY>> frame_duration(frame_num);
	steady_clock::time_point ts = steady_clock::time_point::min() +
		duration_cast<steady_clock::duration>(frame_duration);

	// Feed the inputs.
	for (unsigned card_index = 0; card_index < NUM_BENCHMARK_CARDS; ++card_index) {
		bmusb::AudioFormat audio_format;
		audio_format.bits_per_sample = card_index == 3 ? 24 : 16;
		audio_format.num_channels = NUM_CHANNELS;
		
		unsigned num_samples = NUM_SAMPLES + (lcgrand() % 9) - 5;
		bool ok = mixer->add_audio(DeviceSpec{InputSourceType::CAPTURE_CARD, card_index},
			card_index == 3 ? samples24 : samples16, num_samples, audio_format,
			NUM_SAMPLES * TIMEBASE / OUTPUT_FREQUENCY, ts);
		assert(ok);
	}

	return mixer->get_output(ts, NUM_SAMPLES, ResamplingQueue::ADJUST_RATE);
}

void init_mapping(AudioMixer *mixer)
{
	InputMapping mapping;

	InputMapping::Bus bus1;
	bus1.device = DeviceSpec{InputSourceType::CAPTURE_CARD, 0};
	bus1.source_channel[0] = 0;
	bus1.source_channel[1] = 1;
	mapping.buses.push_back(bus1);

	InputMapping::Bus bus2;
	bus2.device = DeviceSpec{InputSourceType::CAPTURE_CARD, 3};
	bus2.source_channel[0] = 6;
	bus2.source_channel[1] = 4;
	mapping.buses.push_back(bus2);

	mixer->set_input_mapping(mapping);
}

void do_test(const char *filename)
{
	AudioMixer mixer(NUM_BENCHMARK_CARDS);
	mixer.set_audio_level_callback(callback);
	init_mapping(&mixer);

	reset_lcgrand();

	vector<float> output;
	for (unsigned i = 0; i < NUM_TEST_FRAMES; ++i) {
		vector<float> frame_output = process_frame(i, &mixer);
		output.insert(output.end(), frame_output.begin(), frame_output.end());
	}

	FILE *fp = fopen(filename, "rb");
	if (fp == nullptr) {
		fprintf(stderr, "%s not found, writing new reference.\n", filename);
		fp = fopen(filename, "wb");
		fwrite(&output[0], output.size() * sizeof(float), 1, fp);
		fclose(fp);
		return;
	}

	vector<float> ref;
	ref.resize(output.size());
	fread(&ref[0], output.size() * sizeof(float), 1, fp);
	fclose(fp);

	float max_err = 0.0f, sum_sq_err = 0.0f;
	for (unsigned i = 0; i < output.size(); ++i) {
		float err = output[i] - ref[i];
		max_err = max(max_err, fabs(err));
		sum_sq_err += err * err;
	}

	printf("Largest error: %.6f (%+.1f dB)\n", max_err, to_db(max_err));
	printf("RMS error:     %+.1f dB\n", to_db(sqrt(sum_sq_err) / output.size()));
}

void do_benchmark()
{
	AudioMixer mixer(NUM_BENCHMARK_CARDS);
	mixer.set_audio_level_callback(callback);
	init_mapping(&mixer);

	size_t out_samples = 0;

	reset_lcgrand();

	steady_clock::time_point start, end;
	for (unsigned i = 0; i < NUM_WARMUP_FRAMES + NUM_BENCHMARK_FRAMES; ++i) {
		if (i == NUM_WARMUP_FRAMES) {
			start = steady_clock::now();
		}
		vector<float> output = process_frame(i, &mixer);
		if (i >= NUM_WARMUP_FRAMES) {
			out_samples += output.size();
		}
	}
	end = steady_clock::now();

	double elapsed = duration<double>(end - start).count();
	double simulated = double(out_samples) / (OUTPUT_FREQUENCY * 2);
	printf("%ld samples produced in %.1f ms (%.1f%% CPU, %.1fx realtime).\n",
		out_samples, elapsed * 1e3, 100.0 * elapsed / simulated, simulated / elapsed);
}

int main(int argc, char **argv)
{
	for (unsigned i = 0; i < NUM_SAMPLES * NUM_CHANNELS + 1024; ++i) {
		samples16[i * 2] = lcgrand() & 0xff;
		samples16[i * 2 + 1] = lcgrand() & 0xff;

		samples24[i * 3] = lcgrand() & 0xff;
		samples24[i * 3 + 1] = lcgrand() & 0xff;
		samples24[i * 3 + 2] = 0;
	}

	if (argc == 2) {
		do_test(argv[1]);
	}
	do_benchmark();
}

