#pragma once

/* Passive RX monitor screen: live frame counters for promiscuous mode. */

#include "hal_input.h"

/* ui_sniff_key() result */
#define SNIFF_EXIT_MENU 1 /* BACK: leave to the top menu */

/* Enter the screen. Brings up promiscuous RX and drains stale input. */
void ui_sniff_run(void);

/* Returns 0 to stay, or SNIFF_EXIT_MENU to leave. */
int ui_sniff_key(hacku_key_t k);

/* Repaint the live counter block (main-loop tick). 1 = repainted. */
int ui_sniff_tick(void);