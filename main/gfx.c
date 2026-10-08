#include "gfx.h"
#include <stddef.h>

/* Row pointer into the banked framebuffer. */
static inline uint16_t *rowptr(hacku_fb_t *fb, int y) {
    return y < FB_HALF_H ? fb->top + (size_t)y * HACKU_DISP_W
                         : fb->bot + (size_t)(y - FB_HALF_H) * HACKU_DISP_W;
}

static void px(hacku_fb_t *fb, int x, int y, uint16_t c) {
    if ((unsigned)x < (unsigned)HACKU_DISP_W &&
        (unsigned)y < (unsigned)HACKU_DISP_H)
        rowptr(fb, y)[x] = c;
}

void gfx_fill_rect(hacku_fb_t *fb, int x, int y, int w, int h, uint16_t c) {
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > HACKU_DISP_W) w = HACKU_DISP_W - x;
    if (y + h > HACKU_DISP_H) h = HACKU_DISP_H - y;
    if (w <= 0 || h <= 0)
        return;
    for (int r = 0; r < h; r++) {
        uint16_t *row = rowptr(fb, y + r) + x;
        for (int i = 0; i < w; i++)
            row[i] = c;
    }
}

void gfx_rect(hacku_fb_t *fb, int x, int y, int w, int h, uint16_t c) {
    gfx_fill_rect(fb, x, y, w, 1, c);
    gfx_fill_rect(fb, x, y + h - 1, w, 1, c);
    gfx_fill_rect(fb, x, y, 1, h, c);
    gfx_fill_rect(fb, x + w - 1, y, 1, h, c);
}

void gfx_hline(hacku_fb_t *fb, int x, int y, int w, uint16_t c) {
    gfx_fill_rect(fb, x, y, w, 1, c);
}

void gfx_text(hacku_fb_t *fb, const hacku_font_t *f,
              int x, int y, const char *s, uint16_t fg, int bg, int scale) {
    if (!f || !s || scale < 1)
        return;
    int stride = (f->w + 7) / 8;
    int cx = x;
    for (const char *p = s; *p; p++) {
        unsigned ch = (unsigned char)*p;
        if (ch < f->first || ch >= f->first + f->count) {
            cx += f->w * scale;
            continue;
        }
        const uint8_t *g = f->data + (size_t)(ch - f->first) * f->h * stride;
        for (int gy = 0; gy < f->h; gy++) {
            for (int gx = 0; gx < f->w; gx++) {
                int bit = (g[gy * stride + gx / 8] >> (7 - (gx & 7))) & 1;
                if (bit) {
                    if (scale == 1) {
                        px(fb, cx + gx, y + gy, fg);
                    } else {
                        gfx_fill_rect(fb, cx + gx * scale, y + gy * scale,
                                      scale, scale, fg);
                    }
                } else if (bg >= 0) {
                    if (scale == 1) {
                        px(fb, cx + gx, y + gy, (uint16_t)bg);
                    } else {
                        gfx_fill_rect(fb, cx + gx * scale, y + gy * scale,
                                      scale, scale, (uint16_t)bg);
                    }
                }
            }
        }
        cx += f->w * scale;
    }
}

int gfx_text_w(const hacku_font_t *f, const char *s, int scale) {
    int n = 0;
    for (const char *p = s; *p; p++)
        n++;
    return n * f->w * scale;
}
