#pragma once

/* Minimal immediate-mode 2D drawing on the RGB565 framebuffer.
 * The framebuffer is banked (2x 240x160) so it fits fragmented DRAM;
 * all access goes through these helpers — callers never index it. */

#include <stdint.h>
#include "app_config.h"

#define FB_HALF_H 160

typedef struct {
    uint16_t *top; /* rows 0..159 */
    uint16_t *bot; /* rows 160..319 */
} hacku_fb_t;

typedef struct {
    uint8_t w, h;          /* cell size, pixels */
    uint8_t first, count;  /* covered codepoints */
    const uint8_t *data;   /* 1bpp, row-major, stride = (w+7)/8 */
} hacku_font_t;

#define C_RGB(r, g, b) \
    ((uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | ((b) >> 3)))

#define C_BLACK   0x0000
#define C_WHITE   0xFFFF
#define C_RED     C_RGB(255, 0, 0)
#define C_GREEN   C_RGB(0, 200, 0)
#define C_BLUE    C_RGB(60, 120, 255)
#define C_YELLOW  C_RGB(255, 200, 0)
#define C_ORANGE  C_RGB(255, 140, 0)
#define C_GRAY25  C_RGB(64, 64, 64)
#define C_GRAY50  C_RGB(128, 128, 128)
#define C_LTGRAY  C_RGB(200, 200, 200)

/* Calculator theme */
#define CALC_BG     C_RGB(24, 26, 32)
#define CALC_DISP   C_RGB(16, 18, 22)
#define CALC_KEY    C_RGB(48, 52, 62)
#define CALC_KEY_FN C_RGB(255, 140, 0)
#define CALC_CURSOR C_RGB(255, 200, 0)
#define CALC_TEXT   C_WHITE
#define CALC_DIM    C_RGB(150, 155, 165)

void gfx_fill_rect(hacku_fb_t *fb, int x, int y, int w, int h, uint16_t c);
void gfx_rect(hacku_fb_t *fb, int x, int y, int w, int h, uint16_t c);
void gfx_hline(hacku_fb_t *fb, int x, int y, int w, uint16_t c);

/* bg = -1 for transparent. scale 1..4. */
void gfx_text(hacku_fb_t *fb, const hacku_font_t *f,
              int x, int y, const char *s, uint16_t fg, int bg, int scale);
int  gfx_text_w(const hacku_font_t *f, const char *s, int scale);
