#pragma once

/* WiFi passive submenu: the listen-only tools. Currently just the RX
 * monitor; kept as a container so more passive modes (probe harvesting,
 * per-target traffic metering) have a home without another top-level entry. */

#include "hal_input.h"

/* ui_wifipassive_key() results */
#define WIFIPASS_EXIT_MENU  1 /* BACK: back to the top menu */
#define WIFIPASS_OPEN_RX    2 /* OK on "RX monitor" */

/* Draw the submenu. Drains stale input. */
void ui_wifipassive_run(void);

/* Returns 0 to stay, or a WIFIPASS_* code to act on. */
int ui_wifipassive_key(hacku_key_t k);