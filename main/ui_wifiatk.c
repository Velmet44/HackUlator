#include "ui_wifiatk.h"
#include "app_config.h"
#include "hal_display.h"
#include "hal_input.h"
#include "gfx.h"
#include "font.h"
#include "svc_ble.h"
#include "svc_target.h"
#include "svc_deauth.h"
#include "ui_status.h"
#include "esp_log.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdio.h>

#define ROW_H 28
#define LIST_Y0 70

static int s_timed_out = 0;   /* last run ended on its own 3-min timeout */
static deauth_mode_t s_sel = DEAUTH_MODE_DEAUTH; /* attack list cursor */
static deauth_mode_t s_last = DEAUTH_MODE_DEAUTH; /* mode that last ran */

static void draw_msg(const char *l1, const char *l2) {
    hacku_fb_t *fb = hacku_display_fb();
    gfx_fill_rect(fb, 0, 0, HACKU_DISP_W, HACKU_DISP_H, C_BLACK);
    int w1 = gfx_text_w(&hacku_font, l1, 1);
    gfx_text(fb, &hacku_font, (HACKU_DISP_W - w1) / 2, 130,
             l1, C_WHITE, -1, 1);
    if (l2) {
        int w2 = gfx_text_w(&hacku_font, l2, 1);
        gfx_text(fb, &hacku_font, (HACKU_DISP_W - w2) / 2, 160,
                 l2, CALC_DIM, -1, 1);
    }
    hacku_display_flush_all();
}

static void draw(void) {
    hacku_fb_t *fb = hacku_display_fb();
    gfx_fill_rect(fb, 0, 0, HACKU_DISP_W, HACKU_DISP_H, C_BLACK);
    const char *head = "WiFi attacks";
    int hw = gfx_text_w(&hacku_font, head, 1);
    gfx_text(fb, &hacku_font, (HACKU_DISP_W - hw) / 2, 8,
             head, C_ORANGE, -1, 1);
    gfx_hline(fb, 0, 36, HACKU_DISP_W, C_GRAY25);
    {
        char t[24];
        const char *ssid = tgt_wifi_ssid();
        snprintf(t, sizeof(t), "target %.15s",
                 ssid[0] ? ssid : "<hidden>");
        int tw = gfx_text_w(&hacku_font, t, 1);
        gfx_text(fb, &hacku_font, (HACKU_DISP_W - tw) / 2, 44,
                 t, C_GREEN, -1, 1);
    }
    /* Attack list (UP/DOWN navigates, OK runs the selected one). */
    for (int i = 0; i < DEAUTH_MODE_COUNT; i++) {
        int y = LIST_Y0 + i * ROW_H;
        deauth_mode_t m = (deauth_mode_t)i;
        int on = (m == s_sel);
        if (on)
            gfx_fill_rect(fb, 4, y, HACKU_DISP_W - 8, ROW_H - 4, C_GRAY25);
        char lb[24];
        snprintf(lb, sizeof(lb), "%d. %s", i + 1, svc_deauth_mode_name(m));
        gfx_text(fb, &hacku_font, 14, y + 3, lb,
                 on ? C_WHITE : CALC_DIM, -1, 1);
    }
    /* Live status: rate, cumulative frames, time left. */
    {
        char st[32];
        int y = LIST_Y0 + DEAUTH_MODE_COUNT * ROW_H + 14;
        if (svc_deauth_tx_error())
            snprintf(st, sizeof(st), "tx err %d", svc_deauth_tx_error());
        else if (svc_deauth_running())
            snprintf(st, sizeof(st), "%s %lu/s %lu s",
                     svc_deauth_mode_name(svc_deauth_mode()),
                     (unsigned long)svc_deauth_fps(),
                     (unsigned long)svc_deauth_remaining_s());
        else if (s_timed_out)
            snprintf(st, sizeof(st), "done - timed out");
        else if (s_timed_out)
            snprintf(st, sizeof(st), "done - timed out");
        else
            snprintf(st, sizeof(st), "idle");
        int sw = gfx_text_w(&hacku_font, st, 1);
        gfx_text(fb, &hacku_font, (HACKU_DISP_W - sw) / 2, y,
                 st, svc_deauth_running() ? C_RED : CALC_DIM, -1, 1);
    }
    {
        char st[24];
        ui_status_line(st, sizeof(st));
        int sw = gfx_text_w(&hacku_font, st, 1);
        gfx_text(fb, &hacku_font, (HACKU_DISP_W - sw) / 2,
                 HACKU_DISP_H - 52, st, CALC_DIM, -1, 1);
    }
    {
        const char *l2 = svc_deauth_running() ? "OK stop  BACK exit"
                                             : "OK run   BACK exit";
        int w2 = gfx_text_w(&hacku_font, l2, 1);
        gfx_text(fb, &hacku_font, (HACKU_DISP_W - w2) / 2, HACKU_DISP_H - 24,
                 l2, CALC_DIM, -1, 1);
    }
    hacku_display_flush_all();
}

/* Live tick for a running attack: repaint only the status line so the
 * frame counter visibly climbs (the key handler only redraws on input).
 * Returns 1 when it repainted, so the caller can pace itself. */
int ui_wifiatk_tick(void) {
    /* NOTE: caster mode silences the log bus, so nothing may be inferred
     * from missing logs here. */
    /* Auto-stop: the TX callback only raises the expired flag; stop here,
     * outside the timer, and remember that it ended on its own. */
    if (svc_deauth_running() && svc_deauth_expired()) {
        s_timed_out = 1;
        svc_deauth_stop();
    }
    if (!svc_deauth_running())
        return 0;
    hacku_fb_t *fb = hacku_display_fb();
    char st[32];
    if (svc_deauth_tx_error())
        snprintf(st, sizeof(st), "tx err %d", svc_deauth_tx_error());
    else
        snprintf(st, sizeof(st), "%s %lu/s %lu s",
                 svc_deauth_mode_name(svc_deauth_mode()),
                 (unsigned long)svc_deauth_fps(),
                 (unsigned long)svc_deauth_remaining_s());
    int sw = gfx_text_w(&hacku_font, st, 1);
    /* Erase the band, then redraw centred. */
    int y = LIST_Y0 + DEAUTH_MODE_COUNT * ROW_H + 14;
    gfx_fill_rect(fb, 0, y - 6, HACKU_DISP_W, 30, C_BLACK);
    gfx_text(fb, &hacku_font, (HACKU_DISP_W - sw) / 2, y,
             st, C_RED, -1, 1);
    hacku_display_flush(0, y - 6, HACKU_DISP_W, 30);
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
                    r == -6 ? "IDF rejects mgmt frame" : "start failed";
                draw_msg("deauth failed", e);
                vTaskDelay(pdMS_TO_TICKS(1500));
            }
        }
        draw();
    }
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