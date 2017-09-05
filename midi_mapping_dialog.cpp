#include "midi_mapping_dialog.h"

#include <assert.h>
#include <google/protobuf/descriptor.h>
#include <google/protobuf/message.h>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QMessageBox>
#include <QPushButton>
#include <QSpinBox>
#include <QStringList>
#include <QTreeWidget>
#include <stdio.h>
#include <algorithm>
#include <cstddef>
#include <functional>
#include <limits>
#include <string>

#include "midi_mapper.h"
#include "midi_mapping.pb.h"
#include "post_to_main_thread.h"
#include "ui_midi_mapping.h"

class QObject;

using namespace google::protobuf;
using namespace std;

vector<MIDIMappingDialog::Control> per_bus_controllers = {
	{ "Treble",                   MIDIMappingBusProto::kTrebleFieldNumber, MIDIMappingProto::kTrebleBankFieldNumber },
	{ "Mid",                      MIDIMappingBusProto::kMidFieldNumber,    MIDIMappingProto::kMidBankFieldNumber },
	{ "Bass",                     MIDIMappingBusProto::kBassFieldNumber,   MIDIMappingProto::kBassBankFieldNumber },
	{ "Gain",                     MIDIMappingBusProto::kGainFieldNumber,   MIDIMappingProto::kGainBankFieldNumber },
	{ "Compressor threshold",     MIDIMappingBusProto::kCompressorThresholdFieldNumber,
	                              MIDIMappingProto::kCompressorThresholdBankFieldNumber},
	{ "Fader",                    MIDIMappingBusProto::kFaderFieldNumber,  MIDIMappingProto::kFaderBankFieldNumber }
};
vector<MIDIMappingDialog::Control> per_bus_buttons = {
	{ "Toggle mute",              MIDIMappingBusProto::kToggleMuteFieldNumber,
	                              MIDIMappingProto::kToggleMuteBankFieldNumber },
	{ "Toggle locut",             MIDIMappingBusProto::kToggleLocutFieldNumber,
	                              MIDIMappingProto::kToggleLocutBankFieldNumber },
	{ "Togle auto gain staging",  MIDIMappingBusProto::kToggleAutoGainStagingFieldNumber,
	                              MIDIMappingProto::kToggleAutoGainStagingBankFieldNumber },
	{ "Togle compressor",         MIDIMappingBusProto::kToggleCompressorFieldNumber,
	                              MIDIMappingProto::kToggleCompressorBankFieldNumber },
	{ "Clear peak",               MIDIMappingBusProto::kClearPeakFieldNumber,
	                              MIDIMappingProto::kClearPeakBankFieldNumber }
};
vector<MIDIMappingDialog::Control> per_bus_lights = {
	{ "Is muted",                 MIDIMappingBusProto::kIsMutedFieldNumber, 0 },
	{ "Locut is on",              MIDIMappingBusProto::kLocutIsOnFieldNumber, 0 },
	{ "Auto gain staging is on",  MIDIMappingBusProto::kAutoGainStagingIsOnFieldNumber, 0 },
	{ "Compressor is on",         MIDIMappingBusProto::kCompressorIsOnFieldNumber, 0 },
	{ "Bus has peaked",           MIDIMappingBusProto::kHasPeakedFieldNumber, 0 }
};
vector<MIDIMappingDialog::Control> global_controllers = {
	{ "Locut cutoff",             MIDIMappingBusProto::kLocutFieldNumber,  MIDIMappingProto::kLocutBankFieldNumber },
	{ "Limiter threshold",        MIDIMappingBusProto::kLimiterThresholdFieldNumber,
	                              MIDIMappingProto::kLimiterThresholdBankFieldNumber },
	{ "Makeup gain",              MIDIMappingBusProto::kMakeupGainFieldNumber,
	                              MIDIMappingProto::kMakeupGainBankFieldNumber }
};
vector<MIDIMappingDialog::Control> global_buttons = {
	{ "Previous bank",            MIDIMappingBusProto::kPrevBankFieldNumber, 0 },
	{ "Next bank",                MIDIMappingBusProto::kNextBankFieldNumber, 0 },
	{ "Select bank 1",            MIDIMappingBusProto::kSelectBank1FieldNumber, 0 },
	{ "Select bank 2",            MIDIMappingBusProto::kSelectBank2FieldNumber, 0 },
	{ "Select bank 3",            MIDIMappingBusProto::kSelectBank3FieldNumber, 0 },
	{ "Select bank 4",            MIDIMappingBusProto::kSelectBank4FieldNumber, 0 },
	{ "Select bank 5",            MIDIMappingBusProto::kSelectBank5FieldNumber, 0 },
	{ "Toggle limiter",           MIDIMappingBusProto::kToggleLimiterFieldNumber, MIDIMappingProto::kToggleLimiterBankFieldNumber },
	{ "Toggle auto makeup gain",  MIDIMappingBusProto::kToggleAutoMakeupGainFieldNumber, MIDIMappingProto::kToggleAutoMakeupGainBankFieldNumber }
};
vector<MIDIMappingDialog::Control> global_lights = {
	{ "Bank 1 is selected",       MIDIMappingBusProto::kBank1IsSelectedFieldNumber, 0 },
	{ "Bank 2 is selected",       MIDIMappingBusProto::kBank2IsSelectedFieldNumber, 0 },
	{ "Bank 3 is selected",       MIDIMappingBusProto::kBank3IsSelectedFieldNumber, 0 },
	{ "Bank 4 is selected",       MIDIMappingBusProto::kBank4IsSelectedFieldNumber, 0 },
	{ "Bank 5 is selected",       MIDIMappingBusProto::kBank5IsSelectedFieldNumber, 0 },
	{ "Limiter is on",            MIDIMappingBusProto::kLimiterIsOnFieldNumber, 0 },
	{ "Auto makeup gain is on",   MIDIMappingBusProto::kAutoMakeupGainIsOnFieldNumber, 0 },
};

namespace {

int get_bank(const MIDIMappingProto &mapping_proto, int bank_field_number, int default_value)
{
	const FieldDescriptor *bank_descriptor = mapping_proto.GetDescriptor()->FindFieldByNumber(bank_field_number);
	const Reflection *reflection = mapping_proto.GetReflection();
	if (!reflection->HasField(mapping_proto, bank_descriptor)) {
		return default_value;
	}
	return reflection->GetInt32(mapping_proto, bank_descriptor);
}

int get_controller_mapping(const MIDIMappingProto &mapping_proto, size_t bus_idx, int field_number, int default_value)
{
	if (bus_idx >= size_t(mapping_proto.bus_mapping_size())) {
		return default_value;
	}

	const MIDIMappingBusProto &bus_mapping = mapping_proto.bus_mapping(bus_idx);
	const FieldDescriptor *descriptor = bus_mapping.GetDescriptor()->FindFieldByNumber(field_number);
	const Reflection *bus_reflection = bus_mapping.GetReflection();
	if (!bus_reflection->HasField(bus_mapping, descriptor)) {
		return default_value;
	}
	const MIDIControllerProto &controller_proto =
		static_cast<const MIDIControllerProto &>(bus_reflection->GetMessage(bus_mapping, descriptor));
	return controller_proto.controller_number();
}

int get_button_mapping(const MIDIMappingProto &mapping_proto, size_t bus_idx, int field_number, int default_value)
{
	if (bus_idx >= size_t(mapping_proto.bus_mapping_size())) {
		return default_value;
	}

	const MIDIMappingBusProto &bus_mapping = mapping_proto.bus_mapping(bus_idx);
	const FieldDescriptor *descriptor = bus_mapping.GetDescriptor()->FindFieldByNumber(field_number);
	const Reflection *bus_reflection = bus_mapping.GetReflection();
	if (!bus_reflection->HasField(bus_mapping, descriptor)) {
		return default_value;
	}
	const MIDIButtonProto &bus_proto =
		static_cast<const MIDIButtonProto &>(bus_reflection->GetMessage(bus_mapping, descriptor));
	return bus_proto.note_number();
}

int get_light_mapping(const MIDIMappingProto &mapping_proto, size_t bus_idx, int field_number, int default_value)
{
	if (bus_idx >= size_t(mapping_proto.bus_mapping_size())) {
		return default_value;
	}

	const MIDIMappingBusProto &bus_mapping = mapping_proto.bus_mapping(bus_idx);
	const FieldDescriptor *descriptor = bus_mapping.GetDescriptor()->FindFieldByNumber(field_number);
	const Reflection *bus_reflection = bus_mapping.GetReflection();
	if (!bus_reflection->HasField(bus_mapping, descriptor)) {
		return default_value;
	}
	const MIDILightProto &bus_proto =
		static_cast<const MIDILightProto &>(bus_reflection->GetMessage(bus_mapping, descriptor));
	return bus_proto.note_number();
}

}  // namespace

MIDIMappingDialog::MIDIMappingDialog(MIDIMapper *mapper)
	: ui(new Ui::MIDIMappingDialog),
          mapper(mapper)
{
	ui->setupUi(this);

	const MIDIMappingProto mapping_proto = mapper->get_current_mapping();  // Take a copy.
	old_receiver = mapper->set_receiver(this);

	QStringList labels;
	labels << "";
	labels << "Controller bank";
	for (unsigned bus_idx = 0; bus_idx < num_buses; ++bus_idx) {
		char buf[256];
		snprintf(buf, sizeof(buf), "Bus %d", bus_idx + 1);
		labels << buf;
	}
	labels << "";
	ui->treeWidget->setColumnCount(num_buses + 3);
	ui->treeWidget->setHeaderLabels(labels);

	add_controls("Per-bus controllers", ControlType::CONTROLLER, SpinnerGroup::PER_BUS_CONTROLLERS, mapping_proto, per_bus_controllers);
	add_controls("Per-bus buttons", ControlType::BUTTON, SpinnerGroup::PER_BUS_BUTTONS, mapping_proto, per_bus_buttons);
	add_controls("Per-bus lights", ControlType::LIGHT, SpinnerGroup::PER_BUS_LIGHTS, mapping_proto, per_bus_lights);
	add_controls("Global controllers", ControlType::CONTROLLER, SpinnerGroup::GLOBAL_CONTROLLERS, mapping_proto, global_controllers);
	add_controls("Global buttons", ControlType::BUTTON, SpinnerGroup::GLOBAL_BUTTONS, mapping_proto, global_buttons);
	add_controls("Global lights", ControlType::LIGHT, SpinnerGroup::GLOBAL_LIGHTS, mapping_proto, global_lights);
	fill_controls_from_mapping(mapping_proto);

	// Auto-resize every column but the last.
	for (unsigned column_idx = 0; column_idx < num_buses + 3; ++column_idx) {
		ui->treeWidget->resizeColumnToContents(column_idx);
	}

	connect(ui->guess_bus_button, &QPushButton::clicked,
	        bind(&MIDIMappingDialog::guess_clicked, this, false));
	connect(ui->guess_group_button, &QPushButton::clicked,
	        bind(&MIDIMappingDialog::guess_clicked, this, true));
	connect(ui->ok_cancel_buttons, &QDialogButtonBox::accepted, this, &MIDIMappingDialog::ok_clicked);
	connect(ui->ok_cancel_buttons, &QDialogButtonBox::rejected, this, &MIDIMappingDialog::cancel_clicked);
	connect(ui->save_button, &QPushButton::clicked, this, &MIDIMappingDialog::save_clicked);
	connect(ui->load_button, &QPushButton::clicked, this, &MIDIMappingDialog::load_clicked);

	update_guess_button_state();
}

MIDIMappingDialog::~MIDIMappingDialog()
{
	mapper->set_receiver(old_receiver);
	mapper->refresh_highlights();
}

bool MIDIMappingDialog::eventFilter(QObject *obj, QEvent *event)
{
	if (event->type() == QEvent::FocusIn ||
	    event->type() == QEvent::FocusOut) {
		// We ignore the guess buttons themselves; it should be allowed
		// to navigate from a spinner to focus on a button (to click it).
		if (obj != ui->guess_bus_button && obj != ui->guess_group_button) {
			update_guess_button_state();
		}
	}
	return false;
}

void MIDIMappingDialog::guess_clicked(bool limit_to_group)
{
	FocusInfo focus = find_focus();
	if (focus.bus_idx == -1) {
		// The guess button probably took the focus away from us.
		focus = last_focus;
	}
	assert(focus.bus_idx != -1);  // The button should have been disabled.
	pair<int, int> bus_and_offset = guess_offset(focus.bus_idx, limit_to_group ? focus.spinner_group : SpinnerGroup::ALL_GROUPS);
	const int source_bus_idx = bus_and_offset.first;
	const int offset = bus_and_offset.second;
	assert(source_bus_idx != -1);  // The button should have been disabled.

	for (const auto &field_number_and_spinner : spinners[focus.bus_idx]) {
		int field_number = field_number_and_spinner.first;
		QSpinBox *spinner = field_number_and_spinner.second.spinner;
		SpinnerGroup this_spinner_group = field_number_and_spinner.second.group;

		if (limit_to_group && this_spinner_group != focus.spinner_group) {
			continue;
		}

		assert(spinners[source_bus_idx].count(field_number));
		QSpinBox *source_spinner = spinners[source_bus_idx][field_number].spinner;
		assert(spinners[source_bus_idx][field_number].group == this_spinner_group);

		if (source_spinner->value() != -1) {
			spinner->setValue(source_spinner->value() + offset);
		}
	}

	// See if we can find a “next” bus to move the focus to.
	const int next_bus_idx = focus.bus_idx + (focus.bus_idx - source_bus_idx);  // Note: Could become e.g. -1.
	for (const InstantiatedSpinner &is : controller_spinners) {
		if (int(is.bus_idx) == next_bus_idx && is.field_number == focus.field_number) {
			is.spinner->setFocus();
		}
	}
	for (const InstantiatedSpinner &is : button_spinners) {
		if (int(is.bus_idx) == next_bus_idx && is.field_number == focus.field_number) {
			is.spinner->setFocus();
		}
	}
	for (const InstantiatedSpinner &is : light_spinners) {
		if (int(is.bus_idx) == next_bus_idx && is.field_number == focus.field_number) {
			is.spinner->setFocus();
		}
	}
}

void MIDIMappingDialog::ok_clicked()
{
	unique_ptr<MIDIMappingProto> new_mapping = construct_mapping_proto_from_ui();
	mapper->set_midi_mapping(*new_mapping);
	mapper->set_receiver(old_receiver);
	accept();
}

void MIDIMappingDialog::cancel_clicked()
{
	mapper->set_receiver(old_receiver);
	reject();
}

void MIDIMappingDialog::save_clicked()
{
	unique_ptr<MIDIMappingProto> new_mapping = construct_mapping_proto_from_ui();
	QString filename = QFileDialog::getSaveFileName(this,
		"Save MIDI mapping", QString(), tr("Mapping files (*.midimapping)"));
	if (!filename.endsWith(".midimapping")) {
		filename += ".midimapping";
	}
	if (!save_midi_mapping_to_file(*new_mapping, filename.toStdString())) {
		QMessageBox box;
		box.setText("Could not save mapping to '" + filename + "'. Check that you have the right permissions and try again.");
		box.exec();
	}
}

void MIDIMappingDialog::load_clicked()
{
	QString filename = QFileDialog::getOpenFileName(this,
		"Load MIDI mapping", QString(), tr("Mapping files (*.midimapping)"));
	MIDIMappingProto new_mapping;
	if (!load_midi_mapping_from_file(filename.toStdString(), &new_mapping)) {
		QMessageBox box;
		box.setText("Could not load mapping from '" + filename + "'. Check that the file exists, has the right permissions and is valid.");
		box.exec();
		return;
	}

	fill_controls_from_mapping(new_mapping);
}

namespace {

template<class T>
T *get_mutable_bus_message(MIDIMappingProto *mapping_proto, unsigned bus_idx, int field_number)
{
	while (size_t(mapping_proto->bus_mapping_size()) <= bus_idx) {
		mapping_proto->add_bus_mapping();
	}

	MIDIMappingBusProto *bus_mapping = mapping_proto->mutable_bus_mapping(bus_idx);
	const FieldDescriptor *descriptor = bus_mapping->GetDescriptor()->FindFieldByNumber(field_number);
	const Reflection *bus_reflection = bus_mapping->GetReflection();
	return static_cast<T *>(bus_reflection->MutableMessage(bus_mapping, descriptor));
}

}  // namespace

unique_ptr<MIDIMappingProto> MIDIMappingDialog::construct_mapping_proto_from_ui()
{
	unique_ptr<MIDIMappingProto> mapping_proto(new MIDIMappingProto);
	for (const InstantiatedSpinner &is : controller_spinners) {
		const int val = is.spinner->value();
		if (val == -1) {
			continue;
		}

		MIDIControllerProto *controller_proto =
			get_mutable_bus_message<MIDIControllerProto>(mapping_proto.get(), is.bus_idx, is.field_number);
		controller_proto->set_controller_number(val);
	}
	for (const InstantiatedSpinner &is : button_spinners) {
		const int val = is.spinner->value();
		if (val == -1) {
			continue;
		}

		MIDIButtonProto *button_proto =
			get_mutable_bus_message<MIDIButtonProto>(mapping_proto.get(), is.bus_idx, is.field_number);
		button_proto->set_note_number(val);
	}
	for (const InstantiatedSpinner &is : light_spinners) {
		const int val = is.spinner->value();
		if (val == -1) {
			continue;
		}

		MIDILightProto *light_proto =
			get_mutable_bus_message<MIDILightProto>(mapping_proto.get(), is.bus_idx, is.field_number);
		light_proto->set_note_number(val);
	}
	int highest_bank_used = 0;  // 1-indexed.
	for (const InstantiatedComboBox &ic : bank_combo_boxes) {
		const int val = ic.combo_box->currentIndex();
		highest_bank_used = std::max(highest_bank_used, val);
		if (val == 0) {
			continue;
		}

		const FieldDescriptor *descriptor = mapping_proto->GetDescriptor()->FindFieldByNumber(ic.field_number);
		const Reflection *bus_reflection = mapping_proto->GetReflection();
		bus_reflection->SetInt32(mapping_proto.get(), descriptor, val - 1);
	}
	mapping_proto->set_num_controller_banks(highest_bank_used);
	return mapping_proto;
}

void MIDIMappingDialog::add_bank_selector(QTreeWidgetItem *item, const MIDIMappingProto &mapping_proto, int bank_field_number)
{
	if (bank_field_number == 0) {
		return;
	}
	QComboBox *bank_selector = new QComboBox(this);
	bank_selector->addItems(QStringList() << "" << "Bank 1" << "Bank 2" << "Bank 3" << "Bank 4" << "Bank 5");
	bank_selector->setAutoFillBackground(true);

	bank_combo_boxes.push_back(InstantiatedComboBox{ bank_selector, bank_field_number });

	ui->treeWidget->setItemWidget(item, 1, bank_selector);
}

void MIDIMappingDialog::add_controls(const string &heading,
                                     MIDIMappingDialog::ControlType control_type,
                                     MIDIMappingDialog::SpinnerGroup spinner_group,
                                     const MIDIMappingProto &mapping_proto,
                                     const vector<MIDIMappingDialog::Control> &controls)
{
	QTreeWidgetItem *heading_item = new QTreeWidgetItem(ui->treeWidget);
	heading_item->setText(0, QString::fromStdString(heading));
	heading_item->setFirstColumnSpanned(true);
	heading_item->setExpanded(true);
	for (const Control &control : controls) {
		QTreeWidgetItem *item = new QTreeWidgetItem(heading_item);
		heading_item->addChild(item);
		add_bank_selector(item, mapping_proto, control.bank_field_number);
		item->setText(0, QString::fromStdString(control.label + "   "));

		for (unsigned bus_idx = 0; bus_idx < num_buses; ++bus_idx) {
			QSpinBox *spinner = new QSpinBox(this);
			spinner->setRange(-1, 127);
			spinner->setAutoFillBackground(true);
			spinner->setSpecialValueText("\u200d");  // Zero-width joiner (ie., empty).
			spinner->installEventFilter(this);  // So we know when the focus changes.
			ui->treeWidget->setItemWidget(item, bus_idx + 2, spinner);

			if (control_type == ControlType::CONTROLLER) {
				controller_spinners.push_back(InstantiatedSpinner{ spinner, bus_idx, spinner_group, control.field_number });
			} else if (control_type == ControlType::BUTTON) {
				button_spinners.push_back(InstantiatedSpinner{ spinner, bus_idx, spinner_group, control.field_number });
			} else {
				assert(control_type == ControlType::LIGHT);
				light_spinners.push_back(InstantiatedSpinner{ spinner, bus_idx, spinner_group, control.field_number });
			}
			spinners[bus_idx][control.field_number] = SpinnerAndGroup{ spinner, spinner_group };
			connect(spinner, static_cast<void(QSpinBox::*)(int)>(&QSpinBox::valueChanged),
				bind(&MIDIMappingDialog::update_guess_button_state, this));
		}
	}
	ui->treeWidget->addTopLevelItem(heading_item);
}

void MIDIMappingDialog::fill_controls_from_mapping(const MIDIMappingProto &mapping_proto)
{
	for (const InstantiatedSpinner &is : controller_spinners) {
		is.spinner->setValue(get_controller_mapping(mapping_proto, is.bus_idx, is.field_number, -1));
	}
	for (const InstantiatedSpinner &is : button_spinners) {
		is.spinner->setValue(get_button_mapping(mapping_proto, is.bus_idx, is.field_number, -1));
	}
	for (const InstantiatedSpinner &is : light_spinners) {
		is.spinner->setValue(get_light_mapping(mapping_proto, is.bus_idx, is.field_number, -1));
	}
	for (const InstantiatedComboBox &ic : bank_combo_boxes) {
		ic.combo_box->setCurrentIndex(get_bank(mapping_proto, ic.field_number, -1) + 1);
	}
}

void MIDIMappingDialog::controller_changed(unsigned controller)
{
	post_to_main_thread([=]{
		for (const InstantiatedSpinner &is : controller_spinners) {
			if (is.spinner->hasFocus()) {
				is.spinner->setValue(controller);
				is.spinner->selectAll();
			}
		}
	});
}

void MIDIMappingDialog::note_on(unsigned note)
{
	post_to_main_thread([=]{
		for (const InstantiatedSpinner &is : button_spinners) {
			if (is.spinner->hasFocus()) {
				is.spinner->setValue(note);
				is.spinner->selectAll();
			}
		}
		for (const InstantiatedSpinner &is : light_spinners) {
			if (is.spinner->hasFocus()) {
				is.spinner->setValue(note);
				is.spinner->selectAll();
			}
		}
	});
}

pair<int, int> MIDIMappingDialog::guess_offset(unsigned bus_idx, MIDIMappingDialog::SpinnerGroup spinner_group)
{
	constexpr pair<int, int> not_found(-1, 0);

	if (bus_is_empty(bus_idx, spinner_group)) {
		return not_found;
	}

	// See if we can find a non-empty bus to source from (prefer from the left).
	unsigned source_bus_idx;
	if (bus_idx > 0 && !bus_is_empty(bus_idx - 1, spinner_group)) {
		source_bus_idx = bus_idx - 1;
	} else if (bus_idx < num_buses - 1 && !bus_is_empty(bus_idx + 1, spinner_group)) {
		source_bus_idx = bus_idx + 1;
	} else {
		return not_found;
	}

	// See if we can find a consistent offset.
	bool found_offset = false;
	int offset = 0;
	int minimum_allowed_offset = numeric_limits<int>::min();
	int maximum_allowed_offset = numeric_limits<int>::max();
	for (const auto &field_number_and_spinner : spinners[bus_idx]) {
		int field_number = field_number_and_spinner.first;
		QSpinBox *spinner = field_number_and_spinner.second.spinner;
		SpinnerGroup this_spinner_group = field_number_and_spinner.second.group;
		assert(spinners[source_bus_idx].count(field_number));
		QSpinBox *source_spinner = spinners[source_bus_idx][field_number].spinner;
		assert(spinners[source_bus_idx][field_number].group == this_spinner_group);

		if (spinner_group != SpinnerGroup::ALL_GROUPS &&
		    spinner_group != this_spinner_group) {
			continue;
		}
		if (spinner->value() == -1) {
			if (source_spinner->value() != -1) {
				// If the source value is e.g. 3, offset can't be less than -2 or larger than 124.
				// Otherwise, we'd extrapolate values outside [1..127].
				minimum_allowed_offset = max(minimum_allowed_offset, 1 - source_spinner->value());
				maximum_allowed_offset = min(maximum_allowed_offset, 127 - source_spinner->value());
			}
			continue;
		}
		if (source_spinner->value() == -1) {
			// The bus has a controller set that the source bus doesn't set.
			return not_found;
		}

		int candidate_offset = spinner->value() - source_spinner->value();
		if (!found_offset) {
			offset = candidate_offset;
			found_offset = true;
		} else if (candidate_offset != offset) {
			return not_found;
		}
	}

	if (!found_offset) {
		// Given that the bus wasn't empty, this shouldn't happen.
		assert(false);
		return not_found;
	}

	if (offset < minimum_allowed_offset || offset > maximum_allowed_offset) {
		return not_found;
	}
	return make_pair(source_bus_idx, offset);
}

bool MIDIMappingDialog::bus_is_empty(unsigned bus_idx, SpinnerGroup spinner_group)
{
	for (const auto &field_number_and_spinner : spinners[bus_idx]) {
		QSpinBox *spinner = field_number_and_spinner.second.spinner;
		SpinnerGroup this_spinner_group = field_number_and_spinner.second.group;
		if (spinner_group != SpinnerGroup::ALL_GROUPS &&
		    spinner_group != this_spinner_group) {
			continue;
		}
		if (spinner->value() != -1) {
			return false;
		}
	}
	return true;
}

void MIDIMappingDialog::update_guess_button_state()
{
	FocusInfo focus = find_focus();
	if (focus.bus_idx < 0) {
		return;
	}
	{
		pair<int, int> bus_and_offset = guess_offset(focus.bus_idx, SpinnerGroup::ALL_GROUPS);
		ui->guess_bus_button->setEnabled(bus_and_offset.first != -1);
	}
	{
		pair<int, int> bus_and_offset = guess_offset(focus.bus_idx, focus.spinner_group);
		ui->guess_group_button->setEnabled(bus_and_offset.first != -1);
	}
	last_focus = focus;
}

MIDIMappingDialog::FocusInfo MIDIMappingDialog::find_focus() const
{
	for (const InstantiatedSpinner &is : controller_spinners) {
		if (is.spinner->hasFocus()) {
			return FocusInfo{ int(is.bus_idx), is.spinner_group, is.field_number };
		}
	}
	for (const InstantiatedSpinner &is : button_spinners) {
		if (is.spinner->hasFocus()) {
			return FocusInfo{ int(is.bus_idx), is.spinner_group, is.field_number };
		}
	}
	for (const InstantiatedSpinner &is : light_spinners) {
		if (is.spinner->hasFocus()) {
			return FocusInfo{ int(is.bus_idx), is.spinner_group, is.field_number };
		}
	}
	return FocusInfo{ -1, SpinnerGroup::ALL_GROUPS, -1 };
}
