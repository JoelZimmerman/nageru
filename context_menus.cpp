#include <QActionGroup>
#include <QMenu>
#include <QObject>

#include <map>

#include "mixer.h"

using namespace std;

void fill_hdmi_sdi_output_device_menu(QMenu *menu)
{
	menu->clear();
	QActionGroup *card_group = new QActionGroup(menu);
	int current_card = global_mixer->get_output_card_index();

	QAction *none_action = new QAction("None", card_group);
	none_action->setCheckable(true);
	if (current_card == -1) {
		none_action->setChecked(true);
	}
	QObject::connect(none_action, &QAction::triggered, []{ global_mixer->set_output_card(-1); });
	menu->addAction(none_action);

	unsigned num_cards = global_mixer->get_num_cards();
	for (unsigned card_index = 0; card_index < num_cards; ++card_index) {
		if (!global_mixer->card_can_be_used_as_output(card_index)) {
			continue;
		}

		QString description(QString::fromStdString(global_mixer->get_output_card_description(card_index)));
		QAction *action = new QAction(description, card_group);
		action->setCheckable(true);
		if (current_card == int(card_index)) {
			action->setChecked(true);
		}
		QObject::connect(action, &QAction::triggered, [card_index]{ global_mixer->set_output_card(card_index); });
		menu->addAction(action);
	}
}

void fill_hdmi_sdi_output_resolution_menu(QMenu *menu)
{
	menu->clear();
	int current_card = global_mixer->get_output_card_index();
	if (current_card == -1) {
		menu->setEnabled(false);
		return;
	}

	menu->setEnabled(true);
	QActionGroup *resolution_group = new QActionGroup(menu);
	uint32_t current_mode = global_mixer->get_output_video_mode();
	map<uint32_t, bmusb::VideoMode> video_modes = global_mixer->get_available_output_video_modes();
	for (const auto &mode : video_modes) {
		QString description(QString::fromStdString(mode.second.name));
		QAction *action = new QAction(description, resolution_group);
		action->setCheckable(true);
		if (current_mode == mode.first) {
			action->setChecked(true);
		}
		const uint32_t mode_id = mode.first;
		QObject::connect(action, &QAction::triggered, [mode_id]{ global_mixer->set_output_video_mode(mode_id); });
		menu->addAction(action);
	}
}
