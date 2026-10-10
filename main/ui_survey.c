#include "ui_survey.h"
#include "hal_oled.h"
#include "hal_input.h"
#include "oled_gfx.h"
#include "font_oled.h"
#include "svc_sniff.h"
#include "svc_wifi.h"
#include "svc_ble.h"
#include "svc_deauth.h"
#include "ui_status.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdio.h>

#define HEAD_Y    0
#define BAR_Y     10
#define BAR_H     18
#define ROW_Y     30
#define ROW2_Y    39
#define STAT_Y    48
#define HINT_Y    (OLED_H - 9)

/* Listen window per channel. 400 ms is ~4 beacons from a 100 TU AP, enough
 * to rank channels; 300 ms would be faster overall but too close to the
 * noise floor on a sparse band, where "quiet" and "unheard" are the same
 * measurement. */
#define SURVEY_MS 400

static sniff_survey_t s_sv;
static int s_have;

/* Rank channels by the same score svc_sniff_survey() uses, so the bars and
 * the "quietest" line can never disagree. Data is weighted heavily: a stray
 * beacon on a dead channel is survivable, real traffic is not. */
static uint32_t score(int i) {
    return s_sv.ch[i].total * 4 + s_sv.ch[i].data * 4;
}

static void draw(void) {
    oled_fb_t *fb = hal_oled_fb();
    oled_clear(fb, 0);
    const char *head = "Channel survey";
    int hw = oled_text_w(&oled_font, head);
    oled_text(fb, &oled_font, (OLED_W - hw) / 2, HEAD_Y, head, 1);

    char line[33];

    if (!s_have) {
        snprintf(line, sizeof(line), "not run yet");
        line[32] = 0;
        oled_text(fb, &oled_font, 4, BAR_Y, line, 1);
        snprintf(line, sizeof(line), "OK to scan the band");
        line[32] = 0;
        oled_text(fb, &oled_font, 4, ROW_Y, line, 1);
    } else {
        /* Bar chart: 13 channels across 128 px. Height is scaled to the
         * busiest channel seen, so the chart always uses the full box even
         * on a quiet band - an absolute scale would read as empty here. */
        uint32_t peak = 1;
        for (int i = 0; i < s_sv.n; i++) {
            uint32_t sc = score(i);
            if (sc > peak) {
                peak = sc;
            }
        }
        int colw = OLED_W / SURVEY_CH_MAX;
        for (int i = 0; i < SURVEY_CH_MAX; i++) {
            int x = i * colw;
            int h = 1;
            if (i < s_sv.n) {
                h = (int)((uint32_t)BAR_H * score(i) / peak);
                if (h < 1) {
                    h = 1;
                }
            }
            int y = BAR_Y + BAR_H - h;
            /* Mark the chosen target so it is obvious which one to aim at. */
            int on = (s_sv.ch[i].channel == s_sv.quietest);
            oled_fill_rect(fb, x + 1, y, colw - 2, h, on ? 2 : 1);
        }

        snprintf(line, sizeof(line), "quietest ch%d  loudest ch%d",
                 s_sv.quietest, s_sv.loudest);
        line[32] = 0;
        oled_text(fb, &oled_font, 4, ROW_Y, line, 1);

        int q = 0;
        uint32_t qs = 0xFFFFFFFFu;
        for (int i = 0; i < s_sv.n; i++) {
            if (score(i) < qs) {
                qs = score(i);
                q = i;
            }
        }
        snprintf(line, sizeof(line), "ch%d %lu f  %lu bcn  %d ap",
                 s_sv.ch[q].channel,
                 (unsigned long)s_sv.ch[q].total,
                 (unsigned long)s_sv.ch[q].beacons, s_sv.ch[q].aps);
        line[32] = 0;
        oled_text(fb, &oled_font, 4, ROW2_Y, line, 1);
    }

    {
        char st[20];
        ui_status_line(st, sizeof(st));
        int sw = oled_text_w(&oled_font, st);
        oled_text(fb, &oled_font, (OLED_W - sw) / 2, STAT_Y, st, 1);
    }
    int w = oled_text_w(&oled_font, "OK scan  BK exit");
    oled_text(fb, &oled_font, (OLED_W - w) / 2, HINT_Y, "OK scan  BK exit", 1);
    hal_oled_flush_all();
}

static void scan(void) {
    /* A survey is 13 blocking bursts: the device is deaf throughout and
     * cannot answer a keypress. Say so, because a frozen screen with no
     * explanation reads as a hang. */
    oled_fb_t *fb = hal_oled_fb();
    oled_clear(fb, 0);
    const char *m = "scanning 13 ch...";
    int w = oled_text_w(&oled_font, m);
    oled_text(fb, &oled_font, (OLED_W - w) / 2, 28, m, 1);
    hal_oled_flush_all();

    /* Never run this with an attack live: the bursts take the promiscuous
     * callback over and move the radio, which would silently de-pin it. */
    svc_deauth_stop();
    s_have = svc_sniff_survey(SURVEY_MS, &s_sv) > 0;
    draw();
}

void ui_survey_run(void) {
    svc_ble_stop();  /* one radio at a time */
    svc_sniff_stop();
    s_have = 0;
    draw();
    hacku_input_drain();
}

int ui_survey_key(hacku_key_t k) {
    if (k == KEY_BACK || k == KEY_LEFT) {
        svc_sniff_stop();
        return SURVEY_EXIT_MENU;
    }
    if (k == KEY_OK) {
        scan();
        return 0;
    }
    return 0;
}