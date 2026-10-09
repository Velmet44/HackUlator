#pragma once

/* BLE attacks screen (placeholder: attack list goes here). */

#include "hal_input.h"

/* ui_bleatk_key() results */
#define BLEATK_EXIT_MENU  1
#define BLEATK_EXIT_SCAN  2

/* Draw the screen. Drains stale input so queued keys don't leak in. */
void ui_bleatk_run(void);

/* Returns 0 to stay, or a BLEATK_EXIT_* code to leave. */
int ui_bleatk_key(hacku_key_t k);

/* Entry guard for a BLE attack that needs a session target: 1 = verified,
 * 0 = caller must redirect to the scan page. Called when the attack is
 * launched, not on menu entry. */
int ui_bleatk_require(void);

/* Repaint the live status line of a running attack (main-loop tick).
 * 1 = repainted. */
int ui_bleatk_tick(void);