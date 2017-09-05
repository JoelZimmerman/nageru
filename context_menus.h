#ifndef _CONTEXT_MENUS_H
#define _CONTEXT_MENUS_H 1

// Some context menus for controlling various I/O selections,
// based on data from Mixer.

class QMenu;

// Populate a submenu for selecting output card, with an action for each card.
// Will call into the mixer on trigger.
void fill_hdmi_sdi_output_device_menu(QMenu *menu);

// Populate a submenu for choosing the output resolution. Since this is
// card-dependent, the entire menu is disabled if we haven't chosen a card
// (but it's still there so that the user will know it exists).
// Will call into the mixer on trigger.
void fill_hdmi_sdi_output_resolution_menu(QMenu *menu);

#endif  // !defined(_CONTEXT_MENUS_H)
