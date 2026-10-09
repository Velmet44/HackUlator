#pragma once

/* BLE passive monitor: counts BLE advertisements without answering them.
 * The listen-only counterpart of ui_sniff.c - same idea, different radio. */

#include "hal_input.h"

/* ui_blepassive_key() result */
#define BLEPASS_EXIT_MENU 1 /* BACK: leave to the top menu */

/* Enter the screen. Brings up Bluedroid in passive scan mode. */
void ui_blepassive_run(void);

/* Returns 0 to stay, or BLEPASS_EXIT_MENU to leave. */
int ui_blepassive_key(hacku_key_t k);

/* Repaint the live counter block (main-loop tick). 1 = repainted. */
int ui_blepassive_tick(void);