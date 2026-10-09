#include "ui_wifipassive.h"
#include "app_config.h"
#include "hal_oled.h"
#include "hal_input.h"
#include "oled_gfx.h"
#include "font_oled.h"
#include "svc_sniff.h"
#include "ui_status.h"
#include <stdio.h>

/* Deliberately hand-rolled rather than reusing ui_menu.c: that widget owns
 * a single module-level cursor, and nesting it would clobber the top
 * menu's selection when we come back. */
static const char *const ITEMS[] = {
    "RX monitor",
};
#define N_ITEMS 1

#define ROW_H    9
#define LIST_Y0  12
static int s_sel;

static void draw(void) {
    oled_fb_t *fb = hal_oled_fb();
    oled_clear(fb, 0);
    const char *head = "WiFi passive";
    int hw = oled_text_w(&oled_font, head);
    oled_text(fb, &oled_font, (OLED_W - hw) / 2, 0, head, 1);
    oled_hline(fb, 0, 10, OLED_W, 1);
    for (int i = 0; i < N_ITEMS; i++) {
        int y = LIST_Y0 + i * ROW_H;
        char buf[32];
        snprintf(buf, sizeof(buf), "%d. %s", i + 1, ITEMS[i]);
        buf[31] = 0;
        int bw = oled_text_w(&oled_font, buf);
        if (bw > OLED_W - 8) {
            buf[OLED_W / oled_font.w - 2] = 0;
            bw = oled_text_w(&oled_font, buf);
        }
        if (i == s_sel) {
            oled_fill_rect(fb, 0, y, OLED_W, ROW_H, 1);
        }
        oled_text(fb, &oled_font, 4, y, buf, i == s_sel ? 0 : 1);
    }
    {
        /* If a monitor is somehow still running, say so - otherwise the
         * submenu would hide live radio activity. */
        const char *note = svc_sniff_running() ? "RX still running" :
                           "listen only - sends nothing";
        int nw = oled_text_w(&oled_font, note);
        oled_text(fb, &oled_font, (OLED_W - nw) / 2, OLED_H - 27, note, 1);
    }
    {
        char st[20];
        ui_status_line(st, sizeof(st));
        int sw = oled_text_w(&oled_font, st);
        oled_text(fb, &oled_font, (OLED_W - sw) / 2, OLED_H - 18, st, 1);
    }
    {
        const char *hint = "OK open  BK back";
        int hwid = oled_text_w(&oled_font, hint);
        oled_text(fb, &oled_font, (OLED_W - hwid) / 2, OLED_H - 9, hint, 1);
    }
    hal_oled_flush_all();
}

void ui_wifipassive_run(void) {
    s_sel = 0;
    draw();
    hacku_input_drain();
}

int ui_wifipassive_key(hacku_key_t k) {
    switch (k) {
        case KEY_UP:
            s_sel = (s_sel + N_ITEMS - 1) % N_ITEMS;
            draw();
            return 0;
        case KEY_DOWN:
            s_sel = (s_sel + 1) % N_ITEMS;
            draw();
            return 0;
        case KEY_OK:
            return WIFIPASS_OPEN_RX;
        case KEY_BACK:
        case KEY_LEFT:
            return WIFIPASS_EXIT_MENU;
        default:
            return 0;
    }
}