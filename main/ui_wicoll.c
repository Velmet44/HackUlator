#include "ui_wicoll.h"
#include "app_config.h"
#include "hal_oled.h"
#include "hal_input.h"
#include "oled_gfx.h"
#include "font_oled.h"
#include "svc_wicoll.h"
#include "svc_wifi.h"
#include "ui_status.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdio.h>

/* Two screens, one job each: pick a target, then pick a mode.
 *
 * The previous layout put bandwidth and dwell in the list alongside the
 * modes and needed a scroll window for eight rows. That is two kinds of
 * thing - tuning parameters and tasks - in one list, and it hid half the
 * modes behind a scroll. Tuning lives in the header and in HOT/SWEEP now.
 *
 * ROW_H 9 fits all six modes with no scrolling at all:
 *   header y=0, rule y=9, rows y=10/19/28/37/46/55, last ends at y=63.
 * When running, the list is replaced by a counters view, which is what the
 * operator is actually looking at. */
#define ROW_H       9
#define LIST_Y0     10
#define HEADER_Y    0
#define RULE_Y      9
#define RUN_MODE_Y  10
#define RUN_STATS_Y 28
#define STATUS_Y    46
#define STATS_Y     55

#define WICOL_CH_SEL_MIN 1
#define WICOL_CH_SEL_MAX 14

#define SCR_PICK 0
#define SCR_LIST 1

static wicol_profile_t s_profile;
static wicol_mode_t s_sel;
static int s_row;
static int s_timed_out;
static int s_screen = SCR_PICK;

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

static void header(oled_fb_t *fb) {
    char h[33];
    snprintf(h, sizeof(h), "%s %s", wicoll_profile_name(s_profile),
             wicoll_bw40() ? "BW40" : "BW20");
    h[32] = 0;
    oled_text(fb, &oled_font, 4, HEADER_Y, h, 1);
    char pos[33];
    snprintf(pos, sizeof(pos), "%d/%d", s_row + 1, wicoll_mode_count());
    pos[32] = 0;
    int pw = oled_text_w(&oled_font, pos);
    oled_text(fb, &oled_font, OLED_W - pw - 4, HEADER_Y, pos, 1);
    oled_hline(fb, 0, RULE_Y, OLED_W, 1);
}

/* The dwell actually in force for the selected mode, so a run is never
 * started against a setting the operator cannot see. HOT shows what the
 * survey decided rather than the mode's nominal value. */
static uint32_t dwell_in_force(void) {
    if (s_sel == WICOL_MODE_HOT) {
        return wicoll_hot_dwell_ms();
    }
    return 0;
}

/* Two bands, and the split is not cosmetic. `f<n>` is a REAL count of frames
 * the driver accepted. `~<n>%` is DERIVED from an airtime model, so it
 * carries a leading tilde and must never be printed as the same kind of
 * number - the codebase already learned this the hard way with
 * svc_ble_flood_est() and the "capair" label. */
static void draw_status(oled_fb_t *fb) {
    char line[33];
    int raw = wicoll_tx_power_raw();
    int dbm10 = 0;
    if (raw > 0) {
        dbm10 = raw * 5 / 2; /* 0.25 dBm units -> tenths of a dBm */
    }

    if (s_timed_out) {
        snprintf(line, sizeof(line), "done-timeout");
    } else if (!wicoll_running()) {
        uint32_t dw = dwell_in_force();
        if (dw > 0) {
            snprintf(line, sizeof(line), "HOT sweep %lu ms",
                     (unsigned long)dw);
        } else if (s_sel == WICOL_MODE_LOCK && wicoll_lock_channel() > 0) {
            /* What the last LOCK run actually did, not what it would do.
             * The aim is a survey result, so it is only true after a run. */
            snprintf(line, sizeof(line), "aim ch%u of %u AP ch",
                     (unsigned)wicoll_lock_channel(),
                     (unsigned)wicoll_lock_ap_channels());
        } else if (raw > 0) {
            snprintf(line, sizeof(line), "%d.%d dBm  LT/RT bw",
                     dbm10 / 10, dbm10 % 10);
        } else {
            snprintf(line, sizeof(line), "LT/RT bw");
        }
    } else {
        unsigned long r = (unsigned long)wicoll_remaining_s();
        if (r > 99999UL) {
            r = 99999UL;
        }
        if (raw > 0) {
            snprintf(line, sizeof(line), "%lu s %d.%d dBm",
                     r, dbm10 / 10, dbm10 % 10);
        } else {
            snprintf(line, sizeof(line), "%lu s P?", r);
        }
    }
    line[32] = 0;
    oled_text(fb, &oled_font, 4, STATUS_Y, line, 1);

    char st[33];
    if (wicoll_running()) {
        unsigned long f = (unsigned long)wicoll_frames();
        if (f > 9999999UL) {
            f = 9999999UL;
        }
        unsigned long perm = (unsigned long)wicoll_duty_permille();
        if (perm > 100000UL) {
            perm = 100000UL;
        }
        snprintf(st, sizeof(st), "f%lu ch%u ~%lu.%lu%%",
                 f, (unsigned)wicoll_channel(), perm / 10UL, perm % 10UL);
    } else {
        snprintf(st, sizeof(st), "OK=run  BK=back");
    }
    st[32] = 0;
    oled_text(fb, &oled_font, 4, STATS_Y, st, 1);
}

static void row_label(int r, char *lb, int n) {
    wicol_mode_t m;
    int cnt = wicoll_mode_count();
    if (r >= cnt) {
        return;
    }
    /* The profile decides which modes exist, so the row index has to go
     * through the same list the service uses rather than assuming mode N. */
    m = wicoll_mode_at(r);
    if (m == WICOL_MODE_LOCK) {
        /* Show where the last run actually parked, and how much of the band
         * held an AP. "ch1" alone used to read as an answer when it was
         * really a default the user had never set. */
        uint8_t ch = wicoll_lock_channel();
        uint8_t aps = wicoll_lock_ap_channels();
        if (ch == 0) {
            ch = wicoll_pinned();
            if (wicoll_hot_lock_channel() > 0) {
                ch = wicoll_hot_lock_channel();
            }
            snprintf(lb, (size_t)n, "%d.Lock  ch%u?", r + 1, (unsigned)ch);
        } else {
            snprintf(lb, (size_t)n, "%d.Lock  ch%u %ua", r + 1, (unsigned)ch,
                     (unsigned)aps);
        }
        return;
    }
    snprintf(lb, (size_t)n, "%d.%s", r + 1, wicoll_mode_name(m));
}

static void draw(void) {
    oled_fb_t *fb = hal_oled_fb();
    oled_clear(fb, 0);
    header(fb);

    if (wicoll_running()) {
        /* Counters view: the list is gone, the operator is watching an
         * attack, not choosing one. */
        char m[33];
        snprintf(m, sizeof(m), "%s  ch%u", wicoll_mode_name(s_sel),
                 (unsigned)wicoll_channel());
        m[32] = 0;
        oled_text(fb, &oled_font, 4, RUN_MODE_Y, m, 1);
        char big[33];
        snprintf(big, sizeof(big), "f%lu",
                 (unsigned long)wicoll_frames());
        big[32] = 0;
        int bw = oled_text_w(&oled_font, big);
        oled_text(fb, &oled_font, (OLED_W - bw) / 2, RUN_STATS_Y, big, 1);
    } else {
        int cnt = wicoll_mode_count();
        for (int r = 0; r < cnt; r++) {
            int y = LIST_Y0 + r * ROW_H;
            char lb[33];
            row_label(r, lb, (int)sizeof(lb));
            lb[32] = 0;
            int on = (r == s_row);
            if (on) {
                oled_fill_rect(fb, 0, y, OLED_W, ROW_H, 1);
            }
            oled_text(fb, &oled_font, 6, y + 1, lb, on ? 0 : 1);
        }
    }
    draw_status(fb);
    hal_oled_flush_all();
}

static void draw_pick(void) {
    oled_fb_t *fb = hal_oled_fb();
    oled_clear(fb, 0);
    const char *head = "Target";
    int hw = oled_text_w(&oled_font, head);
    oled_text(fb, &oled_font, (OLED_W - hw) / 2, HEADER_Y, head, 1);
    oled_hline(fb, 0, RULE_Y, OLED_W, 1);

    for (int i = 0; i < WICOL_PROFILE_COUNT; i++) {
        int y = LIST_Y0 + i * ROW_H;
        char lb[33];
        int on = (i == (int)s_profile);
        const char *nm = wicoll_profile_name((wicol_profile_t)i);
        if (i == WICOL_PROFILE_WIFI) {
            snprintf(lb, sizeof(lb), "%d.%s  jam APs", i + 1, nm);
        } else {
            snprintf(lb, sizeof(lb), "%d.%s  BT", i + 1, nm);
        }
        lb[32] = 0;
        if (on) {
            oled_fill_rect(fb, 0, y, OLED_W, ROW_H, 1);
        }
        oled_text(fb, &oled_font, 6, y + 1, lb, on ? 0 : 1);
    }
    oled_text(fb, &oled_font, 4, STATUS_Y, "UP/DN OK=go", 1);
    oled_text(fb, &oled_font, 4, STATS_Y, "BK=exit", 1);
    hal_oled_flush_all();
}

static void show_start_error(int r) {
    const char *e =
        r == -1 ? "wifi init failed" :
        r == -2 ? "no radio output" :
        r == -3 ? "task failed" :
        r == -5 ? "bad profile" :
        r == -6 ? "frame too long" :
        r == -8 ? "wrong mode" : "start failed";
    draw_msg("attack failed", e);
    vTaskDelay(pdMS_TO_TICKS(1500));
}

void ui_wicoll_run(void) {
    s_timed_out = 0;
    s_screen = SCR_PICK;
    draw_pick();
    hacku_input_drain();
}

int ui_wicoll_tick(void) {
    /* Auto-stop honoured HERE, never in the TX task: the task must not wait
     * on its own completion. Same two-phase rule as svc_deauth and the BLE
     * flood. */
    if (wicoll_running() && wicoll_expired()) {
        s_timed_out = 1;
        wicoll_stop();
        draw();
        return 1;
    }
    if (!wicoll_running()) {
        return 0;
    }
    /* Repaint the mode line and both bands, not the whole screen: a full
     * redraw 5x a second drowns the OLED and the caster link. */
    oled_fb_t *fb = hal_oled_fb();
    oled_fill_rect(fb, 0, RUN_MODE_Y, OLED_W, 54, 0);
    char m[33];
    snprintf(m, sizeof(m), "%s  ch%u", wicoll_mode_name(s_sel),
             (unsigned)wicoll_channel());
    m[32] = 0;
    oled_text(fb, &oled_font, 4, RUN_MODE_Y, m, 1);
    char big[33];
    snprintf(big, sizeof(big), "f%lu", (unsigned long)wicoll_frames());
    big[32] = 0;
    int bw = oled_text_w(&oled_font, big);
    oled_text(fb, &oled_font, (OLED_W - bw) / 2, RUN_STATS_Y, big, 1);
    draw_status(fb);
    hal_oled_flush(0, RUN_MODE_Y, OLED_W, 54);
    return 1;
}

int ui_wicoll_key(hacku_key_t k) {
    if (s_screen == SCR_PICK) {
        if (k == KEY_BACK) {
            return WICOL_EXIT_MENU;
        }
        if (k == KEY_UP) {
            s_profile = (wicol_profile_t)((s_profile + WICOL_PROFILE_COUNT - 1) %
                                          WICOL_PROFILE_COUNT);
            wicoll_set_profile(s_profile);
            draw_pick();
            return 0;
        }
        if (k == KEY_DOWN) {
            s_profile = (wicol_profile_t)((s_profile + 1) % WICOL_PROFILE_COUNT);
            wicoll_set_profile(s_profile);
            draw_pick();
            return 0;
        }
        if (k == KEY_OK) {
            /* The mode row a user presses resolves through the SERVICE's
             * profile, so it has to be told about the change before the
             * list is drawn - otherwise row N means something different
             * from what is shown, and wicoll_start() can be handed a mode
             * the chosen profile does not offer. */
            wicoll_set_profile(s_profile);
            s_screen = SCR_LIST;
            s_row = 0;
            s_sel = wicoll_mode_at(0);
            draw();
            return 0;
        }
        return 0;
    }

    int cnt = wicoll_mode_count();

    if (k == KEY_BACK) {
        wicoll_stop();
        s_screen = SCR_PICK;
        draw_pick();
        return 0;
    }
    if (k == KEY_UP) {
        if (wicoll_running()) {
            return 0;
        }
        s_row--;
        if (s_row < 0) {
            s_row = cnt - 1;
        }
        s_sel = wicoll_mode_at(s_row);
        draw();
        return 0;
    }
    if (k == KEY_DOWN) {
        if (wicoll_running()) {
            return 0;
        }
        s_row++;
        if (s_row >= cnt) {
            s_row = 0;
        }
        s_sel = wicoll_mode_at(s_row);
        draw();
        return 0;
    }
    /* LEFT/RIGHT always edits and never moves the cursor. That one rule is
     * what un-traps Lock: when this also moved the cursor, Lock could not
     * own the arrows for channel selection and UP/DOWN had to take them,
     * which left the cursor unable to leave the row. */
    if (k == KEY_LEFT || k == KEY_RIGHT) {
        if (wicoll_running()) {
            return 0;
        }
        int up = (k == KEY_RIGHT);
        if (s_sel == WICOL_MODE_LOCK) {
            uint8_t ch = wicoll_pinned();
            if (up) {
                ch = (ch >= WICOL_CH_SEL_MAX) ? WICOL_CH_SEL_MIN
                                             : (uint8_t)(ch + 1);
            } else {
                ch = (ch <= WICOL_CH_SEL_MIN) ? WICOL_CH_SEL_MAX
                                             : (uint8_t)(ch - 1);
            }
            wicoll_set_pinned(ch);
        } else {
            wicoll_set_bw40(up ? 1 : 0);
        }
        draw();
        return 0;
    }
    if (k == KEY_OK) {
        if (wicoll_running()) {
            wicoll_stop();
            s_timed_out = 0;
            draw();
            return 0;
        }
        /* HOT runs a ~7 s survey before the radio goes to work, and start
         * blocks through svc_wifi_init() too. Say so rather than leaving the
         * screen frozen on the idle band for the whole of it. */
        if (s_sel == WICOL_MODE_HOT) {
            draw_msg("surveying", "watching the band");
            vTaskDelay(pdMS_TO_TICKS(50));
        } else {
            draw_msg("starting", NULL);
        }
        int r = wicoll_start(s_profile, s_sel);
        if (r < 0) {
            show_start_error(r);
        }
        draw();
        return 0;
    }
    return 0;
}