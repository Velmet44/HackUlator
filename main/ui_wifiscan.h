#pragma once

/* WiFi AP scan screen: runs a blocking scan, shows sortable-by-RSSI list. */

#include "hal_input.h"

/* Draw "scanning...", run the scan, draw results. Returns AP count (<0 err).
 * Drains stale input so queued keys don't leak into the results screen. */
int ui_wifiscan_run(void);

/* Returns 1 when the user asks to leave (BACK), 0 otherwise. */
int ui_wifiscan_key(hacku_key_t k);

/* Free the AP list (lets the other radio's bring-up reuse the block). */
void ui_wifiscan_drop(void);
