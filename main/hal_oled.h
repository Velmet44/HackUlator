#pragma once

/* OLED HAL: 1 KB mono page framebuffer + SSD1306 (I2C) and/or caster sink.
 * The display is always probed; when nothing answers on I2C the device
 * still runs (caster mirror only). */

#include <stdint.h>
#include "app_config.h"
#include "oled_gfx.h"

/* 1 = OLED present, 0 = caster-only. <0 = fatal (never for OLED). */
int hal_oled_init(void);
int hal_oled_has_display(void);

/* Static 1 KB page framebuffer (no heap). */
oled_fb_t *hal_oled_fb(void);
int hal_oled_get_px(int x, int y);

/* Push a framebuffer rect to OLED and/or caster. Clipped internally. */
void hal_oled_flush(int x, int y, int w, int h);
void hal_oled_flush_all(void);

/* Caster catch-up + periodic self-heal. Call every main-loop iteration. */
void hal_oled_caster_poll(void);

/* 0-100 mapped to SSD1306 contrast. No-op without a display. */
void hal_oled_set_contrast(int pct);
