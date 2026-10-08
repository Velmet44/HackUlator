#pragma once

/* Calculator disguise screen. Returns ACT_UNLOCK when "4+6=" is entered. */

#include "hal_input.h"

#define ACT_NONE   0
#define ACT_UNLOCK 1

void ui_calc_enter(void);         /* full redraw */
int  ui_calc_key(hacku_key_t k);  /* ACT_NONE or ACT_UNLOCK */
