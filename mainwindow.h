#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <stdbool.h>
#include <sys/types.h>
#include <QMainWindow>
#include <QString>
#include <chrono>
#include <string>
#include <vector>

#include "analyzer.h"
#include "audio_mixer.h"
#include "midi_mapper.h"
#include "mixer.h"

class QEvent;
class QObject;
class QResizeEvent;
class Ui_AudioExpandedView;

namespace Ui {
class AudioExpandedView;
class AudioMiniView;
class Display;
class MainWindow;
}  // namespace Ui

class QLabel;
class QPushButton;

class MainWindow : public QMainWindow, public ControllerReceiver
{
	Q_OBJECT

public:
	MainWindow();
	void resizeEvent(QResizeEvent *event) override;
	void mixer_created(Mixer *mixer);

	// Used to release FBOs on the global ResourcePool. Call after the
	// mixer has been shut down but not destroyed yet.
	void mixer_shutting_down();

public slots:
	void cut_triggered();
	void x264_bitrate_triggered();
	void exit_triggered();
	void manual_triggered();
	void about_triggered();
	void open_analyzer_triggered();
	void simple_audio_mode_triggered();
	void multichannel_audio_mode_triggered();
	void input_mapping_triggered();
	void midi_mapping_triggered();
	void timecode_stream_triggered();
	void timecode_stdout_triggered();
	void transition_clicked(int transition_number);
	void channel_clicked(int channel_number);
	void wb_button_clicked(int channel_number);
	void set_transition_names(std::vector<std::string> transition_names);
	void update_channel_name(Mixer::Output output, const std::string &name);
	void update_channel_color(Mixer::Output output, const std::string &color);
	void gain_staging_knob_changed(unsigned bus_index, int value);
	void final_makeup_gain_knob_changed(int value);
	void cutoff_knob_changed(int value);
	void eq_knob_changed(unsigned bus_index, EQBand band, int value);
	void limiter_threshold_knob_changed(int value);
	void compressor_threshold_knob_changed(unsigned bus_index, int value);
	void mini_fader_changed(int bus, double db_volume);
	void mute_button_toggled(int bus, bool checked);
	void reset_meters_button_clicked();
	void relayout();

	// ControllerReceiver interface.
	void set_locut(float value) override;
	void set_limiter_threshold(float value) override;
	void set_makeup_gain(float value) override;

	void set_treble(unsigned bus_idx, float value) override;
	void set_mid(unsigned bus_idx, float value) override;
	void set_bass(unsigned bus_idx, float value) override;
	void set_gain(unsigned bus_idx, float value) override;
	void set_compressor_threshold(unsigned bus_idx, float value) override;
	void set_fader(unsigned bus_idx, float value) override;

	void toggle_mute(unsigned bus_idx) override;
	void toggle_locut(unsigned bus_idx) override;
	void toggle_auto_gain_staging(unsigned bus_idx) override;
	void toggle_compressor(unsigned bus_idx) override;
	void clear_peak(unsigned bus_idx) override;
	void toggle_limiter() override;
	void toggle_auto_makeup_gain() override;

	void clear_all_highlights() override;

	void highlight_locut(bool highlight) override;
	void highlight_limiter_threshold(bool highlight) override;
	void highlight_makeup_gain(bool highlight) override;

	void highlight_treble(unsigned bus_idx, bool highlight) override;
	void highlight_mid(unsigned bus_idx, bool highlight) override;
	void highlight_bass(unsigned bus_idx, bool highlight) override;
	void highlight_gain(unsigned bus_idx, bool highlight) override;
	void highlight_compressor_threshold(unsigned bus_idx, bool highlight) override;
	void highlight_fader(unsigned bus_idx, bool highlight) override;

	void highlight_mute(unsigned bus_idx, bool highlight) override;
	void highlight_toggle_locut(unsigned bus_idx, bool highlight) override;
	void highlight_toggle_auto_gain_staging(unsigned bus_idx, bool highlight) override;
	void highlight_toggle_compressor(unsigned bus_idx, bool highlight) override;
	void highlight_clear_peak(unsigned bus_idx, bool highlight) override {}  // We don't mark this currently.
	void highlight_toggle_limiter(bool highlight) override;
	void highlight_toggle_auto_makeup_gain(bool highlight) override;

	// Raw receivers are not used.
	void controller_changed(unsigned controller) override {}
	void note_on(unsigned note) override {}

private:
	void reset_audio_mapping_ui();
	void setup_audio_miniview();
	void setup_audio_expanded_view();
	bool eventFilter(QObject *watched, QEvent *event) override;
	void closeEvent(QCloseEvent *event) override;
	void set_white_balance(int channel_number, int x, int y);
	void update_cutoff_labels(float cutoff_hz);
	void update_eq_label(unsigned bus_index, EQBand band, float gain_db);

	// Called from DiskSpaceEstimator.
	void report_disk_space(off_t free_bytes, double estimated_seconds_left);

	// Called from the mixer.
	void audio_level_callback(float level_lufs, float peak_db, std::vector<AudioMixer::BusLevel> bus_levels, float global_level_lufs, float range_low_lufs, float range_high_lufs, float final_makeup_gain_db, float correlation);
	std::chrono::steady_clock::time_point last_audio_level_callback;

	void audio_state_changed();

	template<class T>
	void set_relative_value(T *control, float value);

	template<class T>
	void set_relative_value_if_exists(unsigned bus_idx, T *Ui_AudioExpandedView::*control, float value);

	template<class T>
	void click_button_if_exists(unsigned bus_idx, T *Ui_AudioExpandedView::*control);

	template<class T>
	void highlight_control(T *control, bool highlight);

	template<class T>
	void highlight_mute_control(T *control, bool highlight);

	template<class T>
	void highlight_control_if_exists(unsigned bus_idx, T *(Ui_AudioExpandedView::*control), bool highlight, bool is_mute_button = false);

	Ui::MainWindow *ui;
	QLabel *disk_free_label;
	QPushButton *transition_btn1, *transition_btn2, *transition_btn3;
	std::vector<Ui::Display *> previews;
	std::vector<Ui::AudioMiniView *> audio_miniviews;
	std::vector<Ui::AudioExpandedView *> audio_expanded_views;
	int current_wb_pick_display = -1;
	MIDIMapper midi_mapper;
	std::unique_ptr<Analyzer> analyzer;
};

extern MainWindow *global_mainwindow;

#endif
