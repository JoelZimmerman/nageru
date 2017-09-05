#ifndef _MIDI_MAPPING_DIALOG_H
#define _MIDI_MAPPING_DIALOG_H

#include <stdbool.h>
#include <QDialog>
#include <QString>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "midi_mapper.h"

class QEvent;
class QObject;

namespace Ui {
class MIDIMappingDialog;
}  // namespace Ui

class MIDIMappingProto;
class QComboBox;
class QSpinBox;
class QTreeWidgetItem;

class MIDIMappingDialog : public QDialog, public ControllerReceiver
{
	Q_OBJECT

public:
	MIDIMappingDialog(MIDIMapper *mapper);
	~MIDIMappingDialog();

	bool eventFilter(QObject *obj, QEvent *event) override;

	// For use in midi_mapping_dialog.cpp only.
	struct Control {
		std::string label;
		int field_number;  // In MIDIMappingBusProto.
		int bank_field_number;  // In MIDIMappingProto.
	};

	// ControllerReceiver interface. We only implement the raw events.
	// All values are [0.0, 1.0].
	void set_locut(float value) override {}
	void set_limiter_threshold(float value) override {}
	void set_makeup_gain(float value) override {}

	void set_treble(unsigned bus_idx, float value) override {}
	void set_mid(unsigned bus_idx, float value) override {}
	void set_bass(unsigned bus_idx, float value) override {}
	void set_gain(unsigned bus_idx, float value) override {}
	void set_compressor_threshold(unsigned bus_idx, float value) override {}
	void set_fader(unsigned bus_idx, float value) override {}

	void toggle_mute(unsigned bus_idx) override {}
	void toggle_locut(unsigned bus_idx) override {}
	void toggle_auto_gain_staging(unsigned bus_idx) override {}
	void toggle_compressor(unsigned bus_idx) override {}
	void clear_peak(unsigned bus_idx) override {}
	void toggle_limiter() override {}
	void toggle_auto_makeup_gain() override {}

	void clear_all_highlights() override {}

	void highlight_locut(bool highlight) override {}
	void highlight_limiter_threshold(bool highlight) override {}
	void highlight_makeup_gain(bool highlight) override {}

	void highlight_treble(unsigned bus_idx, bool highlight) override {}
	void highlight_mid(unsigned bus_idx, bool highlight) override {}
	void highlight_bass(unsigned bus_idx, bool highlight) override {}
	void highlight_gain(unsigned bus_idx, bool highlight) override {}
	void highlight_compressor_threshold(unsigned bus_idx, bool highlight) override {}
	void highlight_fader(unsigned bus_idx, bool highlight) override {}

	void highlight_mute(unsigned bus_idx, bool highlight) override {}
	void highlight_toggle_locut(unsigned bus_idx, bool highlight) override {}
	void highlight_toggle_auto_gain_staging(unsigned bus_idx, bool highlight) override {}
	void highlight_toggle_compressor(unsigned bus_idx, bool highlight) override {}
	void highlight_clear_peak(unsigned bus_idx, bool highlight) override {}
	void highlight_toggle_limiter(bool highlight) override {}
	void highlight_toggle_auto_makeup_gain(bool highlight) override {}

	// Raw events; used for the editor dialog only.
	void controller_changed(unsigned controller) override;
	void note_on(unsigned note) override;

public slots:
	void guess_clicked(bool limit_to_group);
	void ok_clicked();
	void cancel_clicked();
	void save_clicked();
	void load_clicked();

private:
	static constexpr unsigned num_buses = 8;

	// Each spinner belongs to exactly one group, corresponding to the
	// subheadings in the UI. This is so that we can extrapolate data
	// across only single groups if need be.
	enum class SpinnerGroup {
		ALL_GROUPS = -1,
		PER_BUS_CONTROLLERS,
		PER_BUS_BUTTONS,
		PER_BUS_LIGHTS,
		GLOBAL_CONTROLLERS,
		GLOBAL_BUTTONS,
		GLOBAL_LIGHTS
	};

	void add_bank_selector(QTreeWidgetItem *item, const MIDIMappingProto &mapping_proto, int bank_field_number);
	
	enum class ControlType { CONTROLLER, BUTTON, LIGHT };
	void add_controls(const std::string &heading, ControlType control_type,
	                  SpinnerGroup spinner_group,
	                  const MIDIMappingProto &mapping_proto, const std::vector<Control> &controls);
	void fill_controls_from_mapping(const MIDIMappingProto &mapping_proto);

	// Tries to find a source bus and an offset to it that would give
	// a consistent offset for the rest of the mappings in this bus.
	// Returns -1 for the bus if no consistent offset can be found.
	std::pair<int, int> guess_offset(unsigned bus_idx, SpinnerGroup spinner_group);
	bool bus_is_empty(unsigned bus_idx, SpinnerGroup spinner_group);

	void update_guess_button_state();
	struct FocusInfo {
		int bus_idx;  // -1 for none.
		SpinnerGroup spinner_group;
		int field_number;
	};
	FocusInfo find_focus() const;

	std::unique_ptr<MIDIMappingProto> construct_mapping_proto_from_ui();

	Ui::MIDIMappingDialog *ui;
	MIDIMapper *mapper;
	ControllerReceiver *old_receiver;
	FocusInfo last_focus{-1, SpinnerGroup::ALL_GROUPS, -1};

	// All controllers actually laid out on the grid (we need to store them
	// so that we can move values back and forth between the controls and
	// the protobuf on save/load).
	struct InstantiatedSpinner {
		QSpinBox *spinner;
		unsigned bus_idx;
		SpinnerGroup spinner_group;
		int field_number;  // In MIDIMappingBusProto.
	};
	struct InstantiatedComboBox {
		QComboBox *combo_box;
		int field_number;  // In MIDIMappingProto.
	};
	std::vector<InstantiatedSpinner> controller_spinners;
	std::vector<InstantiatedSpinner> button_spinners;
	std::vector<InstantiatedSpinner> light_spinners;
	std::vector<InstantiatedComboBox> bank_combo_boxes;

	// Keyed on bus index, then field number.
	struct SpinnerAndGroup {
		QSpinBox *spinner;
		SpinnerGroup group;
	};
	std::map<unsigned, std::map<unsigned, SpinnerAndGroup>> spinners;
};

#endif  // !defined(_MIDI_MAPPING_DIALOG_H)
