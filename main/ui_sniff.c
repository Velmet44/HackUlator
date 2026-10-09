#include "ui_sniff.h"
#include "app_config.h"
#include "hal_oled.h"
#include "hal_input.h"
#include "oled_gfx.h"
#include "font_oled.h"
#include "svc_sniff.h"
#include "svc_wifi.h"
#include "svc_ble.h"
#include "ui_status.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdio.h>

/* Four counter rows at 9 px pitch from y=10: the last ends at y=45, leaving
 * y=46 for the status line and y=55 for the hint with no overlap. */
#define RATE_Y    10
#define TOT_Y     19
#define SPLIT_Y   28
#define CHAN_Y    37
#define STAT_Y    46
#define HINT_Y    (OLED_H - 9)

/* The block the tick repaints: all four counter rows. */
#define BLOCK_Y   RATE_Y
#define BLOCK_H   (CHAN_Y + 9 - RATE_Y)

static void draw_msg(const char *l1, const char *l2) {
    oled_fb_t *fb = hal_oled_fb();
    oled_clear(fb, 0);
    int w1 = oled_text_w(&oled_font, l1);
    oled_text(fb, &oled_font, (OLED_W - w1) / 2, 24, l1, 1);
    if (l2) {
        int w2 = oled_text_w(&oled_font, l2);
        oled_text(fb, &oled_font, (OLED_W - w2) / 2, 36, l2, 1);
    }
    hal_oled_flush_all();
}

/* Counter block. Clamped ints keep snprintf safe under -Werror=all: these
 * are free-running counters, so they must never be trusted to fit. */
static void draw_counters(oled_fb_t *fb) {
    char line[33];
    unsigned long r = (unsigned long)svc_sniff_rate();
    if (r > 999999UL) {
        r = 999999UL;
    }
    snprintf(line, sizeof(line), "%lu frames/s", r);
    line[32] = 0;
    oled_text(fb, &oled_font, 4, RATE_Y, line, 1);

    unsigned long tot = (unsigned long)svc_sniff_total();
    if (tot > 99999999UL) {
        tot = 99999999UL;
    }
    snprintf(line, sizeof(line), "total %lu", tot);
    line[32] = 0;
    oled_text(fb, &oled_font, 4, TOT_Y, line, 1);

    unsigned long m = (unsigned long)svc_sniff_mgmt();
    unsigned long d = (unsigned long)svc_sniff_data();
    if (m > 999999UL) {
        m = 999999UL;
    }
    if (d > 999999UL) {
        d = 999999UL;
    }
    snprintf(line, sizeof(line), "mgmt %lu  data %lu", m, d);
    line[32] = 0;
    oled_text(fb, &oled_font, 4, SPLIT_Y, line, 1);

    if (svc_sniff_have_rssi()) {
        snprintf(line, sizeof(line), "ch%d  best %d dBm",
                 svc_sniff_channel(), svc_sniff_best_rssi());
    } else {
        snprintf(line, sizeof(line), "ch%d  no signal yet",
                 svc_sniff_channel());
    }
    line[32] = 0;
    oled_text(fb, &oled_font, 4, CHAN_Y, line, 1);
    /* No trailing pad rect here on purpose: the tick already clears the
     * whole block before redrawing, and any clear extending past CHAN_Y+9
     * would erase the top of the status line that sits at STAT_Y. */
}

static void draw(void) {
    oled_fb_t *fb = hal_oled_fb();
    oled_clear(fb, 0);
    const char *head = "RX monitor";
    int hw = oled_text_w(&oled_font, head);
    oled_text(fb, &oled_font, (OLED_W - hw) / 2, 0, head, 1);
    draw_counters(fb);
    {
        char st[20];
        ui_status_line(st, sizeof(st));
        int sw = oled_text_w(&oled_font, st);
        oled_text(fb, &oled_font, (OLED_W - sw) / 2, STAT_Y, st, 1);
    }
    const char *hint = "listening only - BK exit";
    int w = oled_text_w(&oled_font, hint);
    oled_text(fb, &oled_font, (OLED_W - w) / 2, OLED_H - 9, hint, 1);
    hal_oled_flush_all();
}

void ui_sniff_run(void) {
    svc_sniff_stop();          /* idempotent: clean slate if re-entered */
    svc_ble_stop();            /* one radio at a time */
    int r = svc_sniff_start(SNIFF_ALL, NULL);
    if (r < 0) {
        const char *e =
            r == -1 ? "wifi init failed" :
            r == -2 ? "promisc failed" :
            r == -3 ? "filter failed" : "callback failed";
        draw_msg("RX monitor failed", e);
        vTaskDelay(pdMS_TO_TICKS(1200));
        return;
    }
    draw();
    hacku_input_drain();
}

int ui_sniff_tick(void) {
    if (!svc_sniff_running()) {
        return 0;
    }
    oled_fb_t *fb = hal_oled_fb();
    /* Repaint the whole counter block as one strip: cheap on the OLED and
     * a single caster rect on the wire. */
    oled_fill_rect(fb, 0, BLOCK_Y, OLED_W, BLOCK_H, 0);
    draw_counters(fb);
    hal_oled_flush(0, BLOCK_Y, OLED_W, BLOCK_H);
    return 1;
}

int ui_sniff_key(hacku_key_t k) {
    if (k == KEY_BACK || k == KEY_LEFT) {
        svc_sniff_stop();
        return SNIFF_EXIT_MENU;
    }
    if (k == KEY_OK) {
        /* Toggle: pressing OK without leaving re-reads the counters. */
        draw();
    }
    return 0;
}