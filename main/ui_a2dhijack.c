#include "ui_a2dhijack.h"
#include "app_config.h"
#include "hal_oled.h"
#include "hal_input.h"
#include "oled_gfx.h"
#include "font_oled.h"
#include "svc_a2dp.h"
#include "tune_data.h"
#include "ui_status.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdio.h>

#define ROW_H    9
#define LIST_Y0  10
#define HEADER_Y 0
#define RULE_Y   9
#define STATUS_Y 55

/* Rows occupy y=10..54, which is five at ROW_H 9. */
#define LIST_VISIBLE 5

#define SCR_PAIR   0   /* "put it in pairing mode" */
#define SCR_SINKS  1
#define SCR_TUNES  2
#define SCR_PLAY   3

static int s_screen;
static a2dp_sink_t s_sinks[A2DP_MAX_SINKS];
static int s_nsinks;
static int s_sink_sel;
static int s_tune_sel;
static int s_tune_top;
static int s_sink_top;
static char s_msg[40];
static int s_error;   /* a failure is on screen, waiting for a keypress */

static void set_msg(const char *m) {
    snprintf(s_msg, sizeof(s_msg), "%s", m);
    s_msg[32] = 0;
}

static void scroll(int *top, int sel, int n) {
    /* Both lists WRAP. With the one-button mode there is no other way back
     * to an earlier row, so a clamped list is a trap here. */
    if (sel < *top) {
        *top = sel;
    }
    if (sel >= *top + LIST_VISIBLE) {
        *top = sel - LIST_VISIBLE + 1;
    }
    if (*top > n - LIST_VISIBLE) {
        *top = n - LIST_VISIBLE;
    }
    if (*top < 0) {
        *top = 0;
    }
}

static void chrome(oled_fb_t *fb, const char *title) {
    int w = oled_text_w(&oled_font, title);
    oled_text(fb, &oled_font, (OLED_W - w) / 2, HEADER_Y, title, 1);
    oled_hline(fb, 0, RULE_Y, OLED_W, 1);
}

static void footer(oled_fb_t *fb, const char *l) {
    char f[33];
    char st[20];
    ui_status_line(st, sizeof(st));
    snprintf(f, sizeof(f), "%s %s", st, l);
    f[31] = 0;
    oled_text(fb, &oled_font, 4, STATUS_Y, f, 1);
}

static void rows(oled_fb_t *fb, int n, int sel, int top, int first_label) {
    int i;
    for (i = 0; i < LIST_VISIBLE; i++) {
        int idx = top + i;
        int y = LIST_Y0 + i * ROW_H;
        char lb[33];
        if (idx >= n) {
            break;
        }
        if (first_label == 0) {
            /* Sinks: name, or the address when the device gave no name,
             * which is common and not an error. */
            const char *nm = s_sinks[idx].name;
            if (nm && nm[0]) {
                snprintf(lb, sizeof(lb), "%d.%.16s", idx + 1, nm);
            } else {
                snprintf(lb, sizeof(lb), "%d.%.17s", idx + 1, "unknown");
            }
        } else {
            snprintf(lb, sizeof(lb), "%d.%.17s", idx + 1, k_tunes[idx].name);
        }
        lb[32] = 0;
        int on = (idx == sel);
        if (on) {
            oled_fill_rect(fb, 0, y, OLED_W, ROW_H, 1);
        }
        oled_text(fb, &oled_font, 6, y + 1, lb, on ? 0 : 1);
    }
}

static void draw_msg(const char *l1, const char *l2) {
    oled_fb_t *fb = hal_oled_fb();
    oled_clear(fb, 0);
    int w1 = oled_text_w(&oled_font, l1);
    oled_text(fb, &oled_font, (OLED_W - w1) / 2, 26, l1, 1);
    if (l2) {
        int w2 = oled_text_w(&oled_font, l2);
        oled_text(fb, &oled_font, (OLED_W - w2) / 2, 38, l2, 1);
    }
    hal_oled_flush_all();
}

static void draw_pair(void) {
    oled_fb_t *fb = hal_oled_fb();
    oled_clear(fb, 0);
    chrome(fb, "AUDIO HIJACK");
    const char *l1 = "Speaker must be ON";
    const char *l2 = "and not connected.";
    int w1 = oled_text_w(&oled_font, l1);
    int w2 = oled_text_w(&oled_font, l2);
    oled_text(fb, &oled_font, (OLED_W - w1) / 2, 20, l1, 1);
    oled_text(fb, &oled_font, (OLED_W - w2) / 2, 32, l2, 1);
    /* Say what is and is not supported while there is room to say it. LE
     * audio is unreachable on this radio; a target that only offers it will
     * never appear in the next list, and silence here reads as a fault. */
    const char *l3 = "classic BT audio only";
    int w3 = oled_text_w(&oled_font, l3);
    oled_text(fb, &oled_font, (OLED_W - w3) / 2, 44, l3, 1);
    /* RECONNECT IS THE NORMAL CASE. Once a speaker has been bonded its
     * address is stored and the device can connect straight to it, so the
     * pairing prompt must NOT be the front door - otherwise the feature looks
     * like it needs pairing mode forever, and on a speaker with no pairing
     * button it would look broken. RIGHT re-runs discovery. */
    if (svc_a2dp_have_stored()) {
        char r[33];
        snprintf(r, sizeof(r), "OK=%.24s", svc_a2dp_stored_name());
        r[32] = 0;
        int wr = oled_text_w(&oled_font, r);
        oled_text(fb, &oled_font, (OLED_W - wr) / 2, 54, r, 1);
        footer(fb, "RGT=search BK=back");
    } else {
        footer(fb, "OK=search BK=back");
    }
    hal_oled_flush_all();
}

static void draw_list(const char *title, int n, int sel, int top,
                      int tune, const char *foot) {
    oled_fb_t *fb = hal_oled_fb();
    oled_clear(fb, 0);
    chrome(fb, title);
    if (n == 0) {
        const char *none = tune ? "no tunes compiled" : "no speakers found";
        int w = oled_text_w(&oled_font, none);
        oled_text(fb, &oled_font, (OLED_W - w) / 2, 28, none, 1);
    } else {
        rows(fb, n, sel, top, tune);
    }
    footer(fb, foot);
    hal_oled_flush_all();
}

static void draw_play(void) {
    oled_fb_t *fb = hal_oled_fb();
    oled_clear(fb, 0);
    chrome(fb, "PLAYING");
    const char *nm = svc_a2dp_peer_name();
    char l[33];
    snprintf(l, sizeof(l), "%.31s", nm && nm[0] ? nm : "sink");
    l[32] = 0;
    int w = oled_text_w(&oled_font, l);
    oled_text(fb, &oled_font, (OLED_W - w) / 2, 18, l, 1);
    snprintf(l, sizeof(l), "notes %lu", (unsigned long)svc_a2dp_notes());
    l[32] = 0;
    w = oled_text_w(&oled_font, l);
    oled_text(fb, &oled_font, (OLED_W - w) / 2, 32, l, 1);
    footer(fb, "BK=stop");
    hal_oled_flush_all();
}

static void redraw(void) {
    if (s_screen == SCR_PAIR) {
        draw_pair();
    } else if (s_screen == SCR_SINKS) {
        draw_list("SPEAKERS", s_nsinks, s_sink_sel, s_sink_top, 0,
                  "OK=link RGT=more");
    } else if (s_screen == SCR_TUNES) {
        /* The link state belongs on this screen. "TUNES" over a dead link
         * looks identical to a working one, and the media stream starts
         * asynchronously, so without this there is no way to tell a speaker
         * that connected from one that is still negotiating. */
        char foot[33];
        snprintf(foot, sizeof(foot), "%s OK=play", svc_a2dp_state_name());
        foot[32] = 0;
        draw_list("TUNES", TUNE_COUNT, s_tune_sel, s_tune_top, 1, foot);
    } else {
        draw_play();
    }
}

void ui_a2dhijack_run(void) {
    s_screen = SCR_PAIR;
    s_error = 0;
    set_msg("");
    /* Read the stored address before drawing. The pair screen asks "is there
     * a stored speaker?" to decide whether OK reconnects or runs discovery,
     * and that answer comes from NVS - which is not loaded until the BT stack
     * comes up, which is the thing this branch decides whether to do. Asking
     * first therefore always said "no". */
    svc_a2dp_load_stored();
    /* The A2DP stack is NOT brought up here. Discovery needs the controller
     * twice - once BLE-only for the advert scan, once BTDM for classic
     * inquiry - and bringing up the 41 KB profile first would block both.
     * It comes up in do_connect(), which is also the honest place for it:
     * that is the point where the RAM is actually wanted. */
    redraw();
    hacku_input_drain();
}

int ui_a2dhijack_tick(void) {
    /* End of tune is honoured HERE, never in the producer task: the task must
     * not wait on its own completion, same two-phase rule as svc_deauth,
     * svc_ble's flood and wicoll. */
    if (svc_a2dp_tune_done() && !svc_a2dp_busy()) {
        s_screen = SCR_TUNES;
        redraw();
        return 1;
    }
    if (s_screen != SCR_PLAY) {
        return 0;
    }
    oled_fb_t *fb = hal_oled_fb();
    oled_fill_rect(fb, 0, 28, OLED_W, 12, 0);
    char l[33];
    snprintf(l, sizeof(l), "notes %lu", (unsigned long)svc_a2dp_notes());
    l[32] = 0;
    int w = oled_text_w(&oled_font, l);
    oled_text(fb, &oled_font, (OLED_W - w) / 2, 32, l, 1);
    hal_oled_flush(0, 28, OLED_W, 12);
    return 1;
}

static void do_discover(void) {
    draw_pair();
    /* Both halves run inside svc_a2dp_discover(): a BLE advert scan first
     * (finds a speaker that is merely switched on - no pairing mode needed)
     * then classic inquiry (finds anything in pairing mode, or a device with
     * no BLE at all). Twelve seconds is a compromise; the screen says what is
     * happening rather than sitting frozen. */
    s_nsinks = svc_a2dp_discover(s_sinks, A2DP_MAX_SINKS, 12);
    s_sink_sel = 0;
    s_sink_top = 0;
    if (s_nsinks > 0) {
        s_screen = SCR_SINKS;
    }
    redraw();
}

/* Failures STAY on screen until a key is pressed. They used to clear
 * themselves after 1.5 s, which is worse than useless for a bring-up that
 * failed: the screen went straight back to the prompt and the only evidence
 * was gone before anyone could read it. Same reason as svc_wicoll's
 * show_start_error() keeping its message up. */
static void fail(const char *l1, const char *l2) {
    s_error = 1;
    draw_msg(l1, l2);
}

static int stack_up(void) {
    /* Brought up on the connect path, not on entering the screen: discovery
     * drives the controller itself, and holding 41 KB of A2DP profile open
     * while nobody is connected is exactly what the lazy design avoids. */
    int r = svc_a2dp_up();
    if (r < 0) {
        fail("BT init failed",
             r == -10 ? "controller" :
             r == -11 ? "BT mode" :
             r == -12 ? "bluedroid" :
             r == -13 ? "bluedroid on" : "A2DP source");
        return r;
    }
    return 0;
}

static void do_connect(void) {
    if (s_sink_sel < 0 || s_sink_sel >= s_nsinks) {
        return;
    }
    const char *nm = s_sinks[s_sink_sel].name;
    if (stack_up() < 0) {
        return;
    }
    if (svc_a2dp_connect(s_sinks[s_sink_sel].bda) < 0) {
        fail("connect refused", "check speaker is on");
        return;
    }
    s_error = 0;
    svc_a2dp_set_name(nm);
    s_tune_sel = 0;
    s_tune_top = 0;
    s_screen = SCR_TUNES;
    redraw();
}

/* Connect to the stored speaker: no inquiry, so the speaker does not need to
 * be in pairing mode. This is the path that will be used every day. */
static void do_connect_stored(void) {
    if (stack_up() < 0) {
        return;
    }
    if (svc_a2dp_connect_stored() < 0) {
        fail("no stored peer", "press RGT to search");
        return;
    }
    s_error = 0;
    svc_a2dp_set_name(svc_a2dp_stored_name());
    s_tune_sel = 0;
    s_tune_top = 0;
    s_screen = SCR_TUNES;
    redraw();
}

int ui_a2dhijack_key(hacku_key_t k) {
    if (k == KEY_ONEBTN_HOLD || k == KEY_ONEBTN) {
        return 0;   /* the hold is the global escape hatch; main.c owns it */
    }
    /* Any key clears a standing error and puts the screen back. */
    if (s_error) {
        s_error = 0;
        redraw();
        return 0;
    }

    if (s_screen == SCR_PAIR) {
        if (k == KEY_BACK) {
            return A2DH_EXIT_MENU;
        }
        if (k == KEY_OK) {
            if (svc_a2dp_have_stored()) {
                do_connect_stored();
            } else {
                do_discover();
            }
        }
        if (k == KEY_RIGHT || k == KEY_LEFT) {
            /* Force a fresh inquiry even when a stored speaker exists. */
            do_discover();
        }
        return 0;
    }

    if (s_screen == SCR_SINKS) {
        if (k == KEY_BACK) {
            s_screen = SCR_PAIR;
            redraw();
            return 0;
        }
        if (k == KEY_UP) {
            s_sink_sel = (s_sink_sel + s_nsinks - 1) % s_nsinks;
            scroll(&s_sink_top, s_sink_sel, s_nsinks);
            redraw();
            return 0;
        }
        if (k == KEY_DOWN) {
            s_sink_sel = (s_sink_sel + 1) % s_nsinks;
            scroll(&s_sink_top, s_sink_sel, s_nsinks);
            redraw();
            return 0;
        }
        if (k == KEY_RIGHT || k == KEY_LEFT) {
            do_discover();
            return 0;
        }
        if (k == KEY_OK) {
            do_connect();
        }
        return 0;
    }

    if (s_screen == SCR_TUNES) {
        if (k == KEY_BACK) {
            svc_a2dp_disconnect();
            s_screen = SCR_SINKS;
            redraw();
            return 0;
        }
        if (k == KEY_UP) {
            s_tune_sel = (s_tune_sel + TUNE_COUNT - 1) % TUNE_COUNT;
            scroll(&s_tune_top, s_tune_sel, TUNE_COUNT);
            redraw();
            return 0;
        }
        if (k == KEY_DOWN) {
            s_tune_sel = (s_tune_sel + 1) % TUNE_COUNT;
            scroll(&s_tune_top, s_tune_sel, TUNE_COUNT);
            redraw();
            return 0;
        }
        if (k == KEY_OK) {
            /* LEFT/RIGHT would be a stop/play toggle, but a list that only
             * responds to OK and BACK is one fewer thing to get wrong. */
            if (svc_a2dp_play_tune(k_tunes[s_tune_sel].rtttl) == 0) {
                s_screen = SCR_PLAY;
            }
            redraw();
        }
        return 0;
    }

    /* SCR_PLAY */
    if (k == KEY_BACK || k == KEY_OK) {
        svc_a2dp_stop_tune();
        s_screen = SCR_TUNES;
        redraw();
        return 0;
    }
    return 0;
}