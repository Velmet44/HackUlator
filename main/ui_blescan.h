#pragma once

/* BLE device scan screen: GAP scan, deduped list sorted by discovery. */

#include "hal_input.h"

/* Draw "scanning...", run the scan, draw results. Returns dev count (<0 err).
 * Stops WiFi first (radio coexistence), drains stale input afterwards. */
int ui_blescan_run(void);

/* Returns 1 when the user asks to leave (BACK), 0 otherwise. */
int ui_blescan_key(hacku_key_t k);

/* Free the device list (lets the other radio's bring-up reuse the block). */
void ui_blescan_drop(void);
