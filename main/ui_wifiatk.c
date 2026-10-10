#include "ui_wifiatk.h"
#include "app_config.h"
#include "hal_oled.h"
#include "hal_input.h"
#include "oled_gfx.h"
#include "font_oled.h"
#include "svc_ble.h"
#include "svc_target.h"
#include "svc_deauth.h"
#include "svc_sniff.h"
#include "ui_status.h"
#include "esp_log.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdio.h>

#define ROW_H 9
#define LIST_Y0 10        /* 4 mode rows: 10..45 */
#define STATUS_Y 46       /* status band: 46..54 */
#define STATS_Y 55        /* counters band: 55..63 */
#define PICK_Y0  10       /* 5 name-list rows: 10..54 */

static int s_timed_out = 0;   /* last run ended on its own 3-min timeout */
static deauth_mode_t s_sel = DEAUTH_MODE_DEAUTH; /* attack list cursor */
static int s_picker = 0;      /* 1 = SSID-list picker is open */
static int s_picker_sel = 0;  /* cursor inside the picker */
/* RIGHT toggles the measurement panel: promiscuous RX filtered to the
 * target's BSSID, counting DATA frames only. Works whether or not an attack
 * is running, so the useful flow is measure a baseline first, then attack
 * and watch the figure collapse. */
static int s_meas = 0;

/* Victim-traffic measurement on/off. Data frames only, so the deauth /
 * beacon frames this device injects cannot be counted as victim traffic. */
static void meas_start(void) {
    const uint8_t *bs = tgt_wifi_has() ? tgt_wifi_bssid() : NULL;
    int r = svc_sniff_start(SNIFF_TRAFFIC, bs);
    if (r < 0) {
        s_meas = 0;
        return;
    }
    /* Park on the target's channel. The attacks screen tears WiFi down on
     * entry, so a fresh promiscuous start would sit on the ESP32 default of
     * ch1 and measure an empty band instead of the victim. */
    if (bs) {
        int ch = tgt_wifi_channel();
        if (ch >= 1 && ch <= 13) {
            svc_sniff_set_channel(ch);
        }
    }
    s_meas = 1;
}

static void meas_toggle(void) {
    if (s_meas) {
        svc_sniff_stop();
        s_meas = 0;
        return;
    }
    meas_start();
}

/* Measurement panel: replaces the mode list while active. Rows are 10,19,
 * 28,37 so the status line at 46 and the stats/hint row at 55 are clear. */
static void draw_meas(oled_fb_t *fb) {
    char line[33];
    const char *head = "RX measure";
    int hw = oled_text_w(&oled_font, head);
    oled_text(fb, &oled_font, (OLED_W - hw) / 2, 0, head, 1);

    unsigned long r = (unsigned long)svc_sniff_rate();
    if (r > 999999UL) {
        r = 999999UL;
    }
    snprintf(line, sizeof(line), "v %lu f/s", r);
    line[32] = 0;
    oled_text(fb, &oled_font, 4, LIST_Y0, line, 1);

    unsigned long t = (unsigned long)svc_sniff_hits();
    if (t > 99999999UL) {
        t = 99999999UL;
    }
    snprintf(line, sizeof(line), "v total %lu", t);
    line[32] = 0;
    oled_text(fb, &oled_font, 4, LIST_Y0 + ROW_H, line, 1);

    if (svc_sniff_have_rssi()) {
        snprintf(line, sizeof(line), "ch%d  %d dBm",
                 svc_sniff_channel(), svc_sniff_best_rssi());
    } else {
        snprintf(line, sizeof(line), "ch%d  no target traffic",
                 svc_sniff_channel());
    }
    line[32] = 0;
    oled_text(fb, &oled_font, 4, LIST_Y0 + 2 * ROW_H, line, 1);

    /* Can the measurement itself be trusted? Same beacon-derived ratio the
     * RX monitor self-check uses, so a low number warns the user off
     * reading too much into the victim figure. */
    uint32_t exp = svc_sniff_beacon_exp_hz_milli();
    if (exp == 0) {
        snprintf(line, sizeof(line), "cap - no beacons");
    } else {
        unsigned e = (unsigned)((exp + 500) / 1000);
        unsigned p = e ? (unsigned)svc_sniff_beacon_rate() * 100u / e : 0u;
        if (p > 999u) {
            p = 999u;
        }
        snprintf(line, sizeof(line), "cap %u%%", p);
    }
    line[32] = 0;
    oled_text(fb, &oled_font, 4, LIST_Y0 + 3 * ROW_H, line, 1);
}

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

/* Counters band. Mirrors what the run has actually achieved: frames TXed
 * plus live fake APs for beacon spam, otherwise the frame count and the
 * target name. Clamped ints keep snprintf safe under -Werror=all. */
static void draw_stats(oled_fb_t *fb) {
    char st[33];
    unsigned long n = (unsigned long)svc_deauth_frames();
    if (s_sel == DEAUTH_MODE_BEACON) {
        unsigned long ap = (unsigned long)svc_deauth_fake_aps();
        if (n > 999999UL) {
            n = 999999UL;
        }
        if (ap > 999UL) {
            ap = 999UL;
        }
        snprintf(st, sizeof(st), "f%lu a%lu %s", n, ap,
                 svc_deauth_name_mode_name(svc_deauth_name_mode()));
    } else {
        const char *ssid = tgt_wifi_ssid();
        if (n > 9999999UL) {
            n = 9999999UL;
        }
        snprintf(st, sizeof(st), "f%lu %.9s", n,
                 ssid[0] ? ssid : "<hidden>");
    }
    st[32] = 0;
    int sw = oled_text_w(&oled_font, st);
    oled_text(fb, &oled_font, (OLED_W - sw) / 2, STATS_Y, st, 1);
}

static void draw(void) {
    oled_fb_t *fb = hal_oled_fb();
    oled_clear(fb, 0);
    if (s_meas) {
        draw_meas(fb);
        {
            char st[20];
            ui_status_line(st, sizeof(st));
            int sw = oled_text_w(&oled_font, st);
            oled_text(fb, &oled_font, (OLED_W - sw) / 2, STATUS_Y, st, 1);
        }
        const char *hint = "RIGHT off  BK exit";
        int hw2 = oled_text_w(&oled_font, hint);
        oled_text(fb, &oled_font, (OLED_W - hw2) / 2, STATS_Y, hint, 1);
        hal_oled_flush_all();
        return;
    }
    const char *head = "WiFi attacks";
    int hw = oled_text_w(&oled_font, head);
    oled_text(fb, &oled_font, (OLED_W - hw) / 2, 0, head, 1);
    /* Attack list (UP/DOWN navigates, OK launches). Beacon spam needs no
     * target and asks for an SSID list first. */
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
    draw_stats(fb);
    hal_oled_flush_all();
}

/* SSID-list picker: which name source beacon spam should use. OK confirms
 * and launches; BACK/LEFT returns to the attack list without running. */
static void draw_picker(void) {
    oled_fb_t *fb = hal_oled_fb();
    oled_clear(fb, 0);
    const char *head = "Beacon lists";
    int hw = oled_text_w(&oled_font, head);
    oled_text(fb, &oled_font, (OLED_W - hw) / 2, 0, head, 1);
    oled_hline(fb, 0, 9, OLED_W, 1);
    for (int i = 0; i < BEACON_NAMES_COUNT; i++) {
        int y = PICK_Y0 + i * ROW_H;
        char lb[33];
        snprintf(lb, sizeof(lb), "%s", svc_deauth_name_mode_name(
                     (beacon_name_mode_t)i));
        lb[32] = 0;
        if (i == s_picker_sel) {
            oled_fill_rect(fb, 0, y, OLED_W, ROW_H, 1);
        }
        oled_text(fb, &oled_font, 4, y, lb, i == s_picker_sel ? 0 : 1);
    }
    {
        const char *hint = "OK run BK back";
        int hwid = oled_text_w(&oled_font, hint);
        oled_text(fb, &oled_font, (OLED_W - hwid) / 2, OLED_H - 9, hint, 1);
    }
    hal_oled_flush_all();
}

/* Report a failed launch and flash the message. */
static void show_start_error(int r) {
    const char *e =
        r == -1 ? "no target" :
        r == -2 ? "wifi init failed" :
        r == -3 ? "channel failed" :
        r == -4 ? "timer failed" :
        r == -5 ? "timer failed" :
        r == -6 ? "IDF rejects mgmt" : "start failed";
    draw_msg("attack failed", e);
    vTaskDelay(pdMS_TO_TICKS(1500));
}

/* Live tick for a running attack: repaint only the status + counters so the
 * frame counter visibly climbs (the key handler only redraws on input).
 * Returns 1 when it repainted, so the caller can pace itself. */
int ui_wifiatk_tick(void) {
    /* Auto-stop: the TX callback only raises the expired flag; stop here,
     * outside the timer, and remember that it ended on its own. */
    if (svc_deauth_running() && svc_deauth_expired()) {
        s_timed_out = 1;
        svc_deauth_stop();
        /* Stop measuring too: a meter with no attack is just a second
         * RX monitor, and leaving it live would outlive its context. */
        if (s_meas) {
            svc_sniff_stop();
            s_meas = 0;
        }
        draw();
        return 1;
    }
    if (s_meas) {
        /* Measurement is live on its own: repaint even with no attack. */
        oled_fb_t *mfb = hal_oled_fb();
        oled_fill_rect(mfb, 0, LIST_Y0, OLED_W, 36, 0);
        draw_meas(mfb);
        hal_oled_flush(0, LIST_Y0, OLED_W, 36);
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
    /* Repaint both bands (status + counters) as one 18 px strip. */
    oled_fill_rect(fb, 0, STATUS_Y, OLED_W, 18, 0);
    oled_text(fb, &oled_font, (OLED_W - sw) / 2, STATUS_Y, st, 1);
    draw_stats(fb);
    hal_oled_flush(0, STATUS_Y, OLED_W, 18);
    return 1;
}

void ui_wifiatk_run(void) {
    s_timed_out = 0; /* entering the screen clears a stale timeout note */
    s_picker = 0;
    s_meas = 0;
    draw();
    hacku_input_drain();
}

/* Picker input: OK confirms and launches beacon spam, BACK/LEFT cancels. */
static int picker_key(hacku_key_t k) {
    switch (k) {
        case KEY_UP:
            s_picker_sel = (s_picker_sel + BEACON_NAMES_COUNT - 1) %
                           BEACON_NAMES_COUNT;
            draw_picker();
            return 0;
        case KEY_DOWN:
            s_picker_sel = (s_picker_sel + 1) % BEACON_NAMES_COUNT;
            draw_picker();
            return 0;
        case KEY_LEFT:
        case KEY_BACK:
            s_picker = 0;
            draw();
            return 0;
        case KEY_OK:
            svc_deauth_set_name_mode((beacon_name_mode_t)s_picker_sel);
            s_picker = 0;
            svc_ble_stop();  /* deauth family is WiFi-only */
            /* Beacon spam is self-targeting, so no verify scan - but the
             * meter still has to stand down while the TX timer runs? No:
             * it can stay up, TX and promiscuous RX coexist fine. */
            int r = svc_deauth_start(DEAUTH_MODE_BEACON);
            if (r < 0) {
                show_start_error(r);
            }
            draw();
            return 0;
        default:
            return 0;
    }
}

int ui_wifiatk_key(hacku_key_t k) {
    if (s_picker) {
        return picker_key(k);
    }
    if (k == KEY_BACK) {
        svc_deauth_stop(); /* leave the radio cold */
        svc_sniff_stop();  /* and never leave a promiscuous callback live */
        s_meas = 0;
        return WIFATK_EXIT_MENU;
    }
    if (k == KEY_RIGHT) {
        meas_toggle();
        draw();
        return 0;
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
            draw();
            return 0;
        }
        /* Beacon spam invents its own targets: ask which SSID list first
         * and run with no verification. */
        if (s_sel == DEAUTH_MODE_BEACON) {
            s_picker = 1;
            draw_picker();
            return 0;
        }
        /* The deauth family needs the stored target: verify it now, at
         * launch, instead of on menu entry. */
        if (!tgt_wifi_has()) {
            if (s_meas) {
                svc_sniff_stop();
                s_meas = 0;
            }
            draw_msg("nothing selected", "opening scan...");
            vTaskDelay(pdMS_TO_TICKS(1200));
            return WIFATK_EXIT_SCAN;
        }
        draw_msg("checking target...", NULL);
        svc_ble_stop();
        /* The verify does a full scan, and promiscuous RX shares the same
         * receiver path - running both makes the scan fail, which reads as
         * "target not found" and bounces to the scan page. Drop the meter
         * for the duration and restore it once the attack is up. */
        int was_meas = s_meas;
        if (was_meas) {
            svc_sniff_stop();
            s_meas = 0;
        }
        int v = tgt_wifi_verify();
        if (v <= 0) {
            if (was_meas) {
                meas_start();
            }
            draw_msg(v < 0 ? "verify failed" : "target not found",
                     "opening scan...");
            vTaskDelay(pdMS_TO_TICKS(1200));
            return WIFATK_EXIT_SCAN;
        }
        s_timed_out = 0;
        int r = svc_deauth_start(s_sel);
        if (r < 0) {
            show_start_error(r);
        }
        if (was_meas) {
            meas_start();
        }
        draw();
    }
    return 0;
}