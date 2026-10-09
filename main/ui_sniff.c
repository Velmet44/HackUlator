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
#define ROW2_Y    19
#define ROW3_Y    28
#define ROW4_Y    37
#define STAT_Y    46
#define HINT_Y    (OLED_H - 9)

/* The block the tick repaints: all four counter rows. */
#define BLOCK_Y   RATE_Y
#define BLOCK_H   (ROW4_Y + 9 - RATE_Y)

/* OK toggles the self-check panel. It is diagnostic, not everyday reading,
 * so it stays off the default view. */
static int s_diag;

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

/* Clamp helper: these counters are free-running, so nothing may be trusted
 * to fit in the 32-char line buffers under -Werror=all. */
static unsigned long clamp_ul(unsigned long v, unsigned long max) {
    return v > max ? max : v;
}

/* Draw one 32-char row at (x, y), forcing termination because every snprintf
 * above clamps against a 33-byte buffer. */
static void put(char *buf, int x, int y) {
    buf[32] = 0;
    oled_text(hal_oled_fb(), &oled_font, x, y, buf, 1);
}

static void draw_channel_row(char *buf) {
    int ch = svc_sniff_channel();
    int aps = svc_sniff_beacon_aps();
    if (svc_sniff_have_rssi()) {
        snprintf(buf, 33, "ch%d  %d dBm  %d ap", ch,
                 svc_sniff_best_rssi(), aps);
    } else {
        snprintf(buf, 33, "ch%d  empty band", ch);
    }
}

/* Default view: what the listener is hearing, nothing else. */
static void draw_counters(char *line) {
    snprintf(line, 33, "%lu frames/s",
             clamp_ul((unsigned long)svc_sniff_rate(), 999999UL));
    put(line, 4, RATE_Y);
    snprintf(line, 33, "total %lu",
             clamp_ul((unsigned long)svc_sniff_total(), 99999999UL));
    put(line, 4, ROW2_Y);
    snprintf(line, 33, "mgmt %lu  data %lu",
             clamp_ul((unsigned long)svc_sniff_mgmt(), 999999UL),
             clamp_ul((unsigned long)svc_sniff_data(), 999999UL));
    put(line, 4, ROW3_Y);
    draw_channel_row(line);
    put(line, 4, ROW4_Y);
}

/* Self-check: observed beacons/s against the rate the beacons themselves
 * advertise. 100% means nothing is being dropped; well below means the
 * six-buffer pool is shedding frames. This is the only accuracy check
 * available without a second receiver.
 *
 * Compares BEACONS to BEACONS - the all-management rate also counts probes,
 * auth and deauth, so mixing that in would make the ratio meaningless. */
static void draw_diag(char *line) {
    uint32_t exp_milli = svc_sniff_beacon_exp_hz_milli();
    if (exp_milli == 0) {
        snprintf(line, 33, "no beacons on this ch");
        put(line, 4, RATE_Y);
        snprintf(line, 33, "hop to a busy channel");
        put(line, 4, ROW2_Y);
    } else {
        unsigned exp = (unsigned)((exp_milli + 500) / 1000);
        unsigned obs = (unsigned)svc_sniff_beacon_rate();
        unsigned pct = exp ? clamp_ul(obs * 100u / exp, 9999u) : 0u;
        snprintf(line, 33, "beacons %u/s of %u/s", obs, exp);
        put(line, 4, RATE_Y);
        snprintf(line, 33, "capturing %u%%", pct);
        put(line, 4, ROW2_Y);
    }
    snprintf(line, 33, "interval %u TU  x%d",
             (unsigned)svc_sniff_beacon_interval(),
             svc_sniff_beacon_aps());
    put(line, 4, ROW3_Y);
    draw_channel_row(line);
    put(line, 4, ROW4_Y);
}

static void draw(void) {
    oled_fb_t *fb = hal_oled_fb();
    oled_clear(fb, 0);
    const char *head = "RX monitor";
    int hw = oled_text_w(&oled_font, head);
    oled_text(fb, &oled_font, (OLED_W - hw) / 2, 0, head, 1);

    char line[33];
    if (s_diag) {
        draw_diag(line);
    } else {
        draw_counters(line);
    }

    {
        char st[20];
        ui_status_line(st, sizeof(st));
        int sw = oled_text_w(&oled_font, st);
        oled_text(fb, &oled_font, (OLED_W - sw) / 2, STAT_Y, st, 1);
    }
    const char *hint = s_diag ? "OK back   BK exit" :
                                  "UP/DN ch  OK chk  BK exit";
    int w = oled_text_w(&oled_font, hint);
    oled_text(fb, &oled_font, (OLED_W - w) / 2, HINT_Y, hint, 1);
    hal_oled_flush_all();
}

void ui_sniff_run(void) {
    svc_sniff_stop();          /* idempotent: clean slate if re-entered */
    svc_ble_stop();            /* one radio at a time */
    s_diag = 0;
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
    char line[33];
    if (s_diag) {
        draw_diag(line);
    } else {
        draw_counters(line);
    }
    hal_oled_flush(0, BLOCK_Y, OLED_W, BLOCK_H);
    return 1;
}

int ui_sniff_key(hacku_key_t k) {
    if (k == KEY_BACK || k == KEY_LEFT) {
        svc_sniff_stop();
        return SNIFF_EXIT_MENU;
    }
    if (k == KEY_OK) {
        s_diag = !s_diag;
        draw();
        return 0;
    }
    /* UP/DOWN hop the listener across the band. Without this the monitor is
     * stranded on channel 1 - often an empty one - and reads near zero. */
    if (k == KEY_UP || k == KEY_DOWN) {
        int ch = svc_sniff_channel();
        int next = (k == KEY_UP) ? ch - 1 : ch + 1;
        /* wrap 1..13 so a repeated press keeps sweeping the whole band */
        if (next < 1) {
            next = 13;
        }
        if (next > 13) {
            next = 1;
        }
        svc_sniff_set_channel(next);
        draw();
        return 0;
    }
    return 0;
}