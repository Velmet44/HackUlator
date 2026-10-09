#include "ui_wifiatk.h"
#include "app_config.h"
#include "hal_oled.h"
#include "hal_input.h"
#include "oled_gfx.h"
#include "font_oled.h"
#include "svc_ble.h"
#include "svc_target.h"
#include "svc_deauth.h"
#include "esp_log.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdio.h>

#define ROW_H 9
#define LIST_Y0 20
#define STATUS_Y 47

static int s_timed_out = 0;   /* last run ended on its own 3-min timeout */
static deauth_mode_t s_sel = DEAUTH_MODE_DEAUTH; /* attack list cursor */
static deauth_mode_t s_last = DEAUTH_MODE_DEAUTH; /* mode that last ran */

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

static void draw(void) {
    oled_fb_t *fb = hal_oled_fb();
    oled_clear(fb, 0);
    const char *head = "WiFi attacks";
    int hw = oled_text_w(&oled_font, head);
    oled_text(fb, &oled_font, (OLED_W - hw) / 2, 0, head, 1);
    {
        char t[33];
        const char *ssid = tgt_wifi_ssid();
        snprintf(t, sizeof(t), ">%.27s", ssid[0] ? ssid : "<hidden>");
        t[32] = 0;
        int tw = oled_text_w(&oled_font, t);
        oled_text(fb, &oled_font, (OLED_W - tw) / 2, 10, t, 1);
    }
    /* Attack list (UP/DOWN navigates, OK runs the selected one). */
    for (int i = 0; i < DEAUTH_MODE_COUNT; i++) {
        int y = LIST_Y0 + i * ROW_H;
        deauth_mode_t m = (deauth_mode_t)i;
        int on = (m == s_sel);
        char lb[33];
        snprintf(lb, sizeof(lb), "%d.%s", i + 1, svc_deauth_mode_name(m));
        lb[32] = 0;
        if (on) {
            oled_fill_rect(fb, 0, y, OLED_W, ROW_H, 1);
        }
        oled_text(fb, &oled_font, 4, y, lb, on ? 0 : 1);
    }
    /* Live status: rate + time left. */
    {
        char st[33];
        if (svc_deauth_tx_error()) {
            snprintf(st, sizeof(st), "tx err %d", svc_deauth_tx_error());
        } else if (svc_deauth_running()) {
            snprintf(st, sizeof(st), "%lu/s %lu s",
                     (unsigned long)svc_deauth_fps(),
                     (unsigned long)svc_deauth_remaining_s());
        } else if (s_timed_out) {
            snprintf(st, sizeof(st), "done-timeout");
        } else {
            snprintf(st, sizeof(st), "idle OK=run BK=exit");
        }
        st[32] = 0;
        int sw = oled_text_w(&oled_font, st);
        oled_text(fb, &oled_font, (OLED_W - sw) / 2, STATUS_Y, st, 1);
    }
    hal_oled_flush_all();
}

/* Live tick for a running attack: repaint only the status line so the
 * frame counter visibly climbs (the key handler only redraws on input).
 * Returns 1 when it repainted, so the caller can pace itself. */
int ui_wifiatk_tick(void) {
    /* Auto-stop: the TX callback only raises the expired flag; stop here,
     * outside the timer, and remember that it ended on its own. */
    if (svc_deauth_running() && svc_deauth_expired()) {
        s_timed_out = 1;
        svc_deauth_stop();
        draw();
        return 1;
    }
    if (!svc_deauth_running()) {
        return 0;
    }
    oled_fb_t *fb = hal_oled_fb();
    char st[33];
    if (svc_deauth_tx_error()) {
        snprintf(st, sizeof(st), "tx err %d", svc_deauth_tx_error());
    } else {
        snprintf(st, sizeof(st), "%lu/s %lu s",
                 (unsigned long)svc_deauth_fps(),
                 (unsigned long)svc_deauth_remaining_s());
    }
    st[32] = 0;
    int sw = oled_text_w(&oled_font, st);
    oled_fill_rect(fb, 0, STATUS_Y, OLED_W, 9, 0);
    oled_text(fb, &oled_font, (OLED_W - sw) / 2, STATUS_Y, st, 1);
    hal_oled_flush(0, STATUS_Y, OLED_W, 9);
    return 1;
}

void ui_wifiatk_run(void) {
    s_timed_out = 0; /* entering the screen clears a stale timeout note */
    draw();
    hacku_input_drain();
}

int ui_wifiatk_key(hacku_key_t k) {
    if (k == KEY_BACK) {
        svc_deauth_stop(); /* leave the radio cold */
        return 1;
    }
    if (k == KEY_UP) {
        s_sel = (deauth_mode_t)((s_sel + DEAUTH_MODE_COUNT - 1) %
                                DEAUTH_MODE_COUNT);
        draw();
        return 0;
    }
    if (k == KEY_DOWN) {
        s_sel = (deauth_mode_t)((s_sel + 1) % DEAUTH_MODE_COUNT);
        draw();
        return 0;
    }
    if (k == KEY_OK) {
        if (svc_deauth_running()) {
            svc_deauth_stop();
            s_timed_out = 0;
        } else {
            s_timed_out = 0;
            s_last = s_sel;
            svc_ble_stop();  /* deauth family is WiFi-only */
            int r = svc_deauth_start(s_sel);
            if (r < 0) {
                const char *e =
                    r == -1 ? "no target" :
                    r == -2 ? "wifi init failed" :
                    r == -3 ? "channel failed" :
                    r == -4 ? "wifi restart failed" :
                    r == -6 ? "IDF rejects mgmt" : "start failed";
                draw_msg("deauth failed", e);
                vTaskDelay(pdMS_TO_TICKS(1500));
            }
        }
        draw();
    }
    (void)s_last;
    return 0;
}

/* Entry guard for anything needing the WiFi target: 1 = target verified
 * present, run the attack. 0 = "nothing selected" or "target not found"
 * was shown, caller must redirect to the scan page. */
int ui_wifiatk_require(void) {
    if (!tgt_wifi_has()) {
        draw_msg("nothing selected", "opening scan...");
        vTaskDelay(pdMS_TO_TICKS(1200));
        return 0;
    }
    draw_msg("checking target...", NULL);
    svc_ble_stop();
    int r = tgt_wifi_verify();
    if (r <= 0) {
        draw_msg(r < 0 ? "verify failed" : "target not found",
                 "opening scan...");
        vTaskDelay(pdMS_TO_TICKS(1200));
        return 0;
    }
    return 1;
}
