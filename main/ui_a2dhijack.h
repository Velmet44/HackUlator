#pragma once

/* Audio Device Hijacker screen: pair prompt, sink list, tune list, now
 * playing.
 *
 * RETURN-CODE CONTRACT, and it is the same trap ui_bleatk.c fell into: 0
 * means STAY. A sub-handler that returns 1 makes main.c eject the user to the
 * top menu, so the screen appears to do nothing and the next keypress lands
 * somewhere unrelated. Every path here returns 0 unless it means to leave. */

#include "hal_input.h"

#define A2DH_EXIT_MENU 1

void ui_a2dhijack_run(void);
int  ui_a2dhijack_key(hacku_key_t k);

/* Repaint the live counters while playing (main-loop tick). 1 = repainted. */
int ui_a2dhijack_tick(void);