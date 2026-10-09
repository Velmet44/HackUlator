#include "ui_menu.h"
#include "app_config.h"
#include "hal_oled.h"
#include "oled_gfx.h"
#include "font_oled.h"
#include "svc_resume.h"
#include "ui_status.h"
#include <stdio.h>

static const char *s_title;
static const char * const *s_items;
static int s_n, s_sel;

#define ROW_H 9
/* Five items at 10,19,28,37,46: the last row ends at y=54 and the footer
 * starts at y=55. Do not raise LIST_Y0 without re-checking that fit. */
#define LIST_Y0 10

static void draw(void) {
    oled_fb_t *fb = hal_oled_fb();
    oled_clear(fb, 0);
    int tw = oled_text_w(&oled_font, s_title);
    oled_text(fb, &oled_font, (OLED_W - tw) / 2, 0, s_title, 1);
    oled_hline(fb, 0, 9, OLED_W, 1);
    for (int i = 0; i < s_n; i++) {
        int y = LIST_Y0 + i * ROW_H;
        char buf[32];
        snprintf(buf, sizeof(buf), "%d. %s", i + 1, s_items[i]);
        buf[31] = 0;
        int bw = oled_text_w(&oled_font, buf);
        int bx = 4;
        if (bw > OLED_W - 8) {
            /* truncate to the visible width */
            buf[OLED_W / oled_font.w - 2] = 0;
            bw = oled_text_w(&oled_font, buf);
        }
        if (i == s_sel) {
            oled_fill_rect(fb, 0, y, OLED_W, ROW_H, 1);
        }
        oled_text(fb, &oled_font, bx, y, buf, i == s_sel ? 0 : 1);
    }
    {
        /* Single footer line: mem + targets + boot cause (32 cells max). */
        char st[20];
        char f[36];
        ui_status_line(st, sizeof(st));
        snprintf(f, sizeof(f), "%s %s", st, resume_boot_cause());
        f[31] = 0;
        int sw = oled_text_w(&oled_font, f);
        oled_text(fb, &oled_font, (OLED_W - sw) / 2, OLED_H - 9, f, 1);
    }
    hal_oled_flush_all();
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
        case KEY_UP:
            s_sel = (s_sel + s_n - 1) % s_n;
            break;
        case KEY_DOWN:
            s_sel = (s_sel + 1) % s_n;
            break;
        case KEY_OK:
            rc = s_sel;
            return rc;
        case KEY_BACK:
            return MENU_BACK;
        default:
            return MENU_NONE;
    }
    draw();
    return rc;
}
