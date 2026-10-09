#include "oled_gfx.h"
#include <stddef.h>

void oled_clear(oled_fb_t *fb, int on) {
    uint8_t v = on ? 0xFF : 0x00;
    for (int i = 0; i < OLED_BUF_SZ; i++)
        fb->pages[i] = v;
}

void oled_pset(oled_fb_t *fb, int x, int y, int on) {
    if ((unsigned)x >= (unsigned)OLED_W) {
        return;
    }
    if ((unsigned)y >= (unsigned)OLED_H) {
        return;
    }
    uint8_t *b = &fb->pages[(y >> 3) * OLED_W + x];
    if (on) {
        *b |= (uint8_t)(1u << (y & 7));
    } else {
        *b &= (uint8_t)~(1u << (y & 7));
    }
}

int oled_pget(oled_fb_t *fb, int x, int y) {
    if ((unsigned)x >= (unsigned)OLED_W) {
        return 0;
    }
    if ((unsigned)y >= (unsigned)OLED_H) {
        return 0;
    }
    return (fb->pages[(y >> 3) * OLED_W + x] >> (y & 7)) & 1;
}

void oled_fill_rect(oled_fb_t *fb, int x, int y, int w, int h, int on) {
    if (x < 0) {
        w += x;
        x = 0;
    }
    if (y < 0) {
        h += y;
        y = 0;
    }
    if (x + w > OLED_W) {
        w = OLED_W - x;
    }
    if (y + h > OLED_H) {
        h = OLED_H - y;
    }
    if (w <= 0 || h <= 0) {
        return;
    }
    for (int r = 0; r < h; r++) {
        for (int i = 0; i < w; i++)
            oled_pset(fb, x + i, y + r, on);
    }
}

void oled_rect(oled_fb_t *fb, int x, int y, int w, int h, int on) {
    oled_fill_rect(fb, x, y, w, 1, on);
    oled_fill_rect(fb, x, y + h - 1, w, 1, on);
    oled_fill_rect(fb, x, y, 1, h, on);
    oled_fill_rect(fb, x + w - 1, y, 1, h, on);
}

void oled_hline(oled_fb_t *fb, int x, int y, int w, int on) {
    oled_fill_rect(fb, x, y, w, 1, on);
}

void oled_invert(oled_fb_t *fb, int x, int y, int w, int h) {
    if (x < 0) {
        w += x;
        x = 0;
    }
    if (y < 0) {
        h += y;
        y = 0;
    }
    if (x + w > OLED_W) {
        w = OLED_W - x;
    }
    if (y + h > OLED_H) {
        h = OLED_H - y;
    }
    if (w <= 0 || h <= 0) {
        return;
    }
    for (int r = 0; r < h; r++) {
        for (int i = 0; i < w; i++) {
            int px = oled_pget(fb, x + i, y + r);
            oled_pset(fb, x + i, y + r, !px);
        }
    }
}

void oled_text(oled_fb_t *fb, const oled_font_t *f,
               int x, int y, const char *s, int on) {
    if (!f || !s) {
        return;
    }
    int stride = (f->w + 7) / 8;
    int cx = x;
    for (const char *p = s; *p; p++) {
        unsigned ch = (unsigned char)*p;
        if (ch < f->first || ch >= (unsigned)(f->first + f->count)) {
            cx += f->w;
            continue;
        }
        const uint8_t *g = f->data + (size_t)(ch - f->first) * f->h * stride;
        for (int gy = 0; gy < f->h; gy++) {
            for (int gx = 0; gx < f->w; gx++) {
                int bit = (g[gy * stride + gx / 8] >> (7 - (gx & 7))) & 1;
                if (bit) {
                    oled_pset(fb, cx + gx, y + gy, on);
                }
            }
        }
        cx += f->w;
    }
}

int oled_text_w(const oled_font_t *f, const char *s) {
    int n = 0;
    for (const char *p = s; *p; p++)
        n++;
    return n * f->w;
}
