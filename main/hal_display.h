#pragma once

/* Display HAL: banked RGB565 framebuffer + TFT (auto-detected) and/or
 * caster sink. When no TFT answers on SPI the device runs headless. */

#include <stdint.h>
#include "app_config.h"
#include "gfx.h"

/* 1 = TFT present, 0 = headless (caster). Honors HACKU_FORCE_CASTER. */
int hacku_display_init(void);
int hacku_display_has_tft(void);

/* Banked full-screen framebuffer (2x 240x160 RGB565). */
hacku_fb_t *hacku_display_fb(void);

/* Row pointer for a screen y (0..319) — for flush internals. */
uint16_t *hacku_display_row(int y);

/* Push a framebuffer rect to TFT and/or caster. Clipped internally. */
void hacku_display_flush(int x, int y, int w, int h);
void hacku_display_flush_all(void);

/* Caster catch-up + periodic self-heal (full refresh). Call every main-loop
 * iteration; TFT untouched. Cheap timestamp check when nothing is owed. */
void hacku_display_caster_poll(void);

void hacku_display_set_brightness(int pct); /* 0-100 */
