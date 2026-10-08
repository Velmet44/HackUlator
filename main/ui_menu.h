#pragma once

/* Minimal scrollable menu. Returns selection events to the caller. */

#include "hal_input.h"

#define MENU_BACK -2
#define MENU_NONE -1

void ui_menu_enter(const char *title, const char * const *items, int n);
int  ui_menu_key(hacku_key_t k); /* >=0 selected, MENU_BACK, MENU_NONE */
