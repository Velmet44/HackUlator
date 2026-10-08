#include "ui_menu.h"
#include "app_config.h"
#include "hal_display.h"
#include "gfx.h"
#include "font.h"
#include "svc_resume.h"
#include "ui_status.h"
#include <stdio.h>

static const char *s_title;
static const char * const *s_items;
static int s_n, s_sel;

#define ROW_H 30
#define LIST_Y0 52

static void draw(void) {
    hacku_fb_t *fb = hacku_display_fb();
    gfx_fill_rect(fb, 0, 0, HACKU_DISP_W, HACKU_DISP_H, C_BLACK);
    int tw = gfx_text_w(&hacku_font, s_title, 1);
    gfx_text(fb, &hacku_font, (HACKU_DISP_W - tw) / 2, 12,
             s_title, C_ORANGE, -1, 1);
    gfx_hline(fb, 0, 42, HACKU_DISP_W, C_GRAY25);
    for (int i = 0; i < s_n; i++) {
        int y = LIST_Y0 + i * ROW_H;
        if (i == s_sel)
            gfx_fill_rect(fb, 4, y, HACKU_DISP_W - 8, ROW_H - 4,
                          C_GRAY25);
        char buf[24];
        snprintf(buf, sizeof(buf), "%d. %s", i + 1, s_items[i]);
        gfx_text(fb, &hacku_font, 14, y + 4, buf,
                 i == s_sel ? C_WHITE : CALC_DIM, -1, 1);
    }
    {
        /* Status bar above the boot-cause footer: mem + targets. */
        char st[24];
        ui_status_line(st, sizeof(st));
        int sw = gfx_text_w(&hacku_font, st, 1);
        gfx_text(fb, &hacku_font, (HACKU_DISP_W - sw) / 2, HACKU_DISP_H - 52,
                 st, CALC_DIM, -1, 1);
        /* Boot-cause tag: tells brownouts / EN glitches / panics apart. */
        char f[16];
        snprintf(f, sizeof(f), "boot %s", resume_boot_cause());
        int fw = gfx_text_w(&hacku_font, f, 1);
        gfx_text(fb, &hacku_font, HACKU_DISP_W - 6 - fw, HACKU_DISP_H - 24,
                 f, CALC_DIM, -1, 1);
    }
    hacku_display_flush_all();
}

void ui_menu_enter(const char *title, const char * const *items, int n) {
    s_title = title;
    s_items = items;
    s_n = n;
    s_sel = 0;
    draw();
}

int ui_menu_key(hacku_key_t k) {
    int rc = MENU_NONE;
    switch (k) {
        case KEY_UP:   s_sel = (s_sel + s_n - 1) % s_n; break;
        case KEY_DOWN: s_sel = (s_sel + 1) % s_n; break;
        case KEY_OK:   rc = s_sel; return rc;
        case KEY_BACK: return MENU_BACK;
        default: return MENU_NONE;
    }
    draw();
    return rc;
}
