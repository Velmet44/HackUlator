#pragma once

/* BLE passive advert monitor screen. Opened from the "BLE passive" submenu
 * (ui_blepassive.c) - both passive trees are structured the same way:
 * a submenu holding one or more listen-only screens. */

#include "hal_input.h"

/* ui_bleadv_key() result */
#define BLEADV_EXIT_MENU 1 /* BACK: back to the BLE passive submenu */

/* Enter the screen. Brings up Bluedroid in passive scan mode. */
void ui_bleadv_run(void);

/* Returns 0 to stay, or BLEADV_EXIT_MENU to leave. */
int ui_bleadv_key(hacku_key_t k);

/* Repaint the live counter block (main-loop tick). 1 = repainted. */
int ui_bleadv_tick(void);