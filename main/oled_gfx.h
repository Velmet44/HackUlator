#pragma once

/* Mono 1-bit graphics on the 128x64 OLED page buffer.
 * Buffer layout matches the SSD1306 GDDRAM: pages of 8 vertical pixels,
 * buf[page * 128 + col], bit (y % 8). Callers never index it directly. */

#include <stdint.h>

#define OLED_W      128
#define OLED_H      64
#define OLED_PAGES  8
#define OLED_BUF_SZ (OLED_W * OLED_PAGES) /* 1024 bytes */

typedef struct {
    uint8_t pages[OLED_BUF_SZ];
} oled_fb_t;

typedef struct {
    uint8_t w, h;          /* cell size, pixels */
    uint8_t first, count;  /* covered codepoints */
    const uint8_t *data;   /* 1bpp, row-major, stride = (w+7)/8 */
} oled_font_t;

void oled_clear(oled_fb_t *fb, int on);
void oled_pset(oled_fb_t *fb, int x, int y, int on);
int  oled_pget(oled_fb_t *fb, int x, int y);
void oled_fill_rect(oled_fb_t *fb, int x, int y, int w, int h, int on);
void oled_rect(oled_fb_t *fb, int x, int y, int w, int h, int on);
void oled_hline(oled_fb_t *fb, int x, int y, int w, int on);
void oled_invert(oled_fb_t *fb, int x, int y, int w, int h);
void oled_text(oled_fb_t *fb, const oled_font_t *f,
               int x, int y, const char *s, int on);
int  oled_text_w(const oled_font_t *f, const char *s);
