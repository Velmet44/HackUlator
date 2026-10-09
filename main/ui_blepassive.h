#pragma once

/* BLE passive submenu: the listen-only BLE tools. Mirrors
 * ui_wifipassive.c exactly so both passive trees behave identically -
 * you pick a tool from a list, it does not start scanning on entry.
 * Currently just "Adv monitor"; future passive BLE tools land here. */

#include "hal_input.h"

/* ui_blepassive_key() results */
#define BLEPASS_EXIT_MENU 1  /* BACK: back to the top menu */
#define BLEPASS_OPEN_ADV  2  /* OK on "Adv monitor" */

/* Draw the submenu. Drains stale input. */
void ui_blepassive_run(void);

/* Returns 0 to stay, or a BLEPASS_* code to act on. */
int ui_blepassive_key(hacku_key_t k);