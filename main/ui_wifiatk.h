#pragma once

/* WiFi attacks screen: attack list, live status/counters, and the
 * beacon-spam SSID-list picker. */

#include "hal_input.h"

/* ui_wifiatk_key() results */
#define WIFATK_EXIT_MENU  1  /* BACK: leave to the top menu */
#define WIFATK_EXIT_SCAN  2  /* no/vanished target: caller must open scan */

/* Draw the screen. Drains stale input so queued keys don't leak in. */
void ui_wifiatk_run(void);

/* Returns 0 to stay, or a WIFATK_EXIT_* code to leave. */
int ui_wifiatk_key(hacku_key_t k);

/* Repaint the live status + counters of a running attack (main-loop tick).
 * 1 = repainted. */
int ui_wifiatk_tick(void);