#pragma once

/* Channel survey: sweep all 13 channels and show what was on the air, so
 * "which channel is empty" stops being a guess. PowerKill's CSA element
 * steers a victim somewhere useless, and until now it always picked 13 -
 * which is not reliably empty. */

#include "hal_input.h"

#define SURVEY_EXIT_MENU 1

void ui_survey_run(void);
int  ui_survey_key(hacku_key_t k);