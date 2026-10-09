#pragma once

/* BLE attacks screen (placeholder: attack list goes here). */

#include "hal_input.h"

/* Draw the screen. Drains stale input so queued keys don't leak in. */
void ui_bleatk_run(void);

/* Returns 1 when the user asks to leave (BACK), 0 otherwise. */
int ui_bleatk_key(hacku_key_t k);

/* Entry guard: 1 = target verified present, 0 = caller must show scan. */
int ui_bleatk_require(void);

/* Repaint the live status line of a running attack (main-loop tick).
 * 1 = repainted. */
int ui_bleatk_tick(void);
