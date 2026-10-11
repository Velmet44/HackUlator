#pragma once

/* Collateral screen: mode list, channel picker for the pinned modes, and
 * the live counters band. */

#include "hal_input.h"
#include "svc_wicoll.h"

/* ui_wicoll_key() results. NOTE the return-code trap that has bitten this
 * codebase twice: the EJECT code is literally 1, so every sub-handler in
 * this screen must return 0 or main.c will treat the keypress as "leave to
 * menu" and drop the user at the top menu on the next press. */
#define WICOL_EXIT_MENU 1

void ui_wicoll_run(void);

/* Returns 0 to stay, or WICOL_EXIT_MENU to leave. */
int ui_wicoll_key(hacku_key_t k);

/* Live counters repaint, called from the main loop every 500 ms.
 * Honours the auto-stop flag here, never inside the TX task. */
int ui_wicoll_tick(void);