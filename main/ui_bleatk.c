#include "ui_bleatk.h"
#include "app_config.h"
#include "hal_oled.h"
#include "hal_input.h"
#include "oled_gfx.h"
#include "font_oled.h"
#include "svc_wifi.h"
#include "svc_ble.h"
#include "svc_target.h"
#include "ui_status.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdio.h>

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

/* ---- layout: the same bands ui_wifiatk.c uses, so both attack screens
 * read identically. Modes at LIST_Y0, live status at STATUS_Y, counters at
 * STATS_Y. One mode today; the layout has room for six. ---- */
#define ROW_H    9
#define LIST_Y0  10   /* mode rows: 10..45 */
#define PICK_Y0  10   /* wordlist rows: 10..45 */
#define STATUS_Y 46   /* live status band: 46..54 */
#define STATS_Y  55   /* counters band: 55..63 */

typedef enum {
    BLEATK_MODE_FLOOD = 0,
    BLEATK_MODE_COUNT
} bleatk_mode_t;

static const char *const s_mode_names[BLEATK_MODE_COUNT] = {
    "ADV flood",
};

/* Module-level state, reset by ui_bleatk_run() exactly like ui_wifiatk.c:
 * the screen is entered fresh after every transition. */
static bleatk_mode_t s_sel = BLEATK_MODE_FLOOD;
static int s_picker = 0;      /* 1 = wordlist picker is open */
static int s_picker_sel = 0;  /* cursor inside the picker */
static int s_timed_out = 0;   /* last run ended on its own 3-min timeout */

/* Wordlist picker. Deliberately has NO status line: 5 rows at ROW_H 9 from
 * PICK_Y0 land on y=46, which is where STATUS_Y lives on the main screen.
 * ui_wifiatk.c's picker solves this the same way - rows fill the space and the
 * hint sits at the very bottom. Selected row is inverted, not prefixed. */
static void draw_picker(oled_fb_t *fb) {
    const char *head = "ADV flood names";
    int hw = oled_text_w(&oled_font, head);
    oled_text(fb, &oled_font, (OLED_W - hw) / 2, 0, head, 1);
    oled_hline(fb, 0, 9, OLED_W, 1);
    for (int i = 0; i < BLEADV_NAMES_COUNT; i++) {
        int y = PICK_Y0 + i * ROW_H;
        char lb[33];
        snprintf(lb, sizeof(lb), "%s",
                 svc_ble_flood_name_mode_name((bleadv_name_mode_t)i));
        lb[32] = 0;
        if (i == s_picker_sel) {
            oled_fill_rect(fb, 0, y, OLED_W, ROW_H, 1);
        }
        oled_text(fb, &oled_font, 4, y, lb, i == s_picker_sel ? 0 : 1);
    }
    {
        const char *hint = "OK start BK back";
        int hwid = oled_text_w(&oled_font, hint);
        oled_text(fb, &oled_font, (OLED_W - hwid) / 2, OLED_H - 9, hint, 1);
    }
}

/* Live band while the flood runs. Two rows: STATUS_Y carries state and the
 * remaining time, STATS_Y the counters. There is no separate hint line -
 * STATS_Y occupies y=55..63, so a hint at OLED_H-9 would draw on top of it.
 * The idle key hints go in the status row, as ui_wifiatk.c does. */
static void draw_status(oled_fb_t *fb) {
    char line[33];
    if (s_timed_out) {
        snprintf(line, sizeof(line), "stopped: %d min timeout",
                 BLEADV_TIMEOUT_S / 60);
    } else if (svc_ble_flood_running()) {
        unsigned long r = (unsigned long)svc_ble_flood_rate();
        if (r > 9999UL) {
            r = 9999UL;
        }
        snprintf(line, sizeof(line), "%lu/s %lu s left", r,
                 (unsigned long)svc_ble_flood_remaining_s());
    } else {
        snprintf(line, sizeof(line), "idle OK=run BK=exit");
    }
    line[32] = 0;
    oled_text(fb, &oled_font, 4, STATUS_Y, line, 1);

    /* Counters band. "est" is deliberate: the figure is elapsed/interval,
     * not a packet count. Bluedroid gives no per-packet feedback, so this
     * must never be read as proof of what went out on the air. */
    char st[33];
    if (svc_ble_flood_running()) {
        unsigned long e = (unsigned long)svc_ble_flood_est();
        if (e > 9999999UL) {
            e = 9999999UL;
        }
        snprintf(st, sizeof(st), "est %lu %s", e,
                 svc_ble_flood_name_mode_name(svc_ble_flood_name_mode()));
    } else {
        snprintf(st, sizeof(st), "list %s",
                 svc_ble_flood_name_mode_name(svc_ble_flood_name_mode()));
    }
    st[32] = 0;
    oled_text(fb, &oled_font, 4, STATS_Y, st, 1);
}

static void draw(void) {
    oled_fb_t *fb = hal_oled_fb();
    oled_clear(fb, 0);
    if (s_picker) {
        draw_picker(fb);
        hal_oled_flush_all();
        return;
    }
    const char *head = "BLE attacks";
    int hw = oled_text_w(&oled_font, head);
    oled_text(fb, &oled_font, (OLED_W - hw) / 2, 0, head, 1);
    /* Same shape as ui_wifiatk.c's mode list: numbered, selected row inverted.
     * A mode whose OK asks for a list first (the flood) shows it in the
     * hint band rather than opening silently. */
    for (int i = 0; i < BLEATK_MODE_COUNT; i++) {
        int y = LIST_Y0 + i * ROW_H;
        int on = (i == (int)s_sel);
        char lb[33];
        snprintf(lb, sizeof(lb), "%d.%s", i + 1, s_mode_names[i]);
        lb[32] = 0;
        if (on) {
            oled_fill_rect(fb, 0, y, OLED_W, ROW_H, 1);
        }
        oled_text(fb, &oled_font, 4, y, lb, on ? 0 : 1);
    }
    /* The live name actually in the payload, so a running flood says what it
     * is advertising rather than only which list it drew from. */
    if (svc_ble_flood_running()) {
        char nm[33];
        snprintf(nm, sizeof(nm), "\"%.28s\"", svc_ble_flood_name());
        nm[32] = 0;
        oled_text(fb, &oled_font, 4, LIST_Y0 + 2 * ROW_H, nm, 1);
    }
    draw_status(fb);
    hal_oled_flush_all();
}

void ui_bleatk_run(void) {
    s_timed_out = 0; /* entering clears a stale timeout note */
    s_picker = 0;
    draw();
    hacku_input_drain();
}

static int picker_key(hacku_key_t k) {
    if (k == KEY_UP) {
        s_picker_sel = (s_picker_sel + BLEADV_NAMES_COUNT - 1) %
                       BLEADV_NAMES_COUNT;
        draw();
        return 0;
    }
    if (k == KEY_DOWN) {
        s_picker_sel = (s_picker_sel + 1) % BLEADV_NAMES_COUNT;
        draw();
        return 0;
    }
    if (k == KEY_BACK) {
        s_picker = 0;
        draw();
        return 0;
    }
    if (k == KEY_OK) {
        svc_ble_flood_set_name_mode((bleadv_name_mode_t)s_picker_sel);
        s_picker = 0;
        /* Self-targeting like beacon spam on the WiFi side: it invents its
         * own identity and needs no session BLE target, so ui_bleatk_require()
         * is deliberately NOT called here. Doing so would bounce the user to
         * the scan page for a mode that never needed a target. */
        svc_wifi_teardown(); /* BT needs the heap WiFi holds */
        int r = svc_ble_flood_start();
        if (r < 0) {
            /* Must block here: draw() below immediately repaints the list,
             * so without the delay the failure flashes for zero frames and
             * the user never learns why nothing started. Same reason
             * ui_wifiatk.c's show_start_error() sleeps.
             * The stage code is printed verbatim (-10..-15 come from
             * ble_up(), -17/-18 from the flood start), which is the only way
             * to tell "BT ran out of heap" from "the driver refused the
             * payload" on a bench with no logs. */
            char why[33];
            const char *w =
                r == -17 ? "adv payload" :
                r == -18 ? "adv start" :
                r == -12 ? "bluedroid" :
                r == -13 ? "bt enable" :
                r == -11 ? "ctrl enable" :
                r == -10 ? "ctrl init" : "ble init";
            snprintf(why, sizeof(why), "%s %d", w, r);
            why[32] = 0;
            draw_msg("attack failed", why);
            vTaskDelay(pdMS_TO_TICKS(1500));
        }
        draw();
        return 0;
    }
    return 0;
}

int ui_bleatk_key(hacku_key_t k) {
    if (s_picker) {
        return picker_key(k);
    }
    if (k == KEY_BACK) {
        svc_ble_flood_stop(); /* leave the radio cold */
        return BLEATK_EXIT_MENU;
    }
    if (k == KEY_UP) {
        s_sel = (bleatk_mode_t)((s_sel + BLEATK_MODE_COUNT - 1) %
                                BLEATK_MODE_COUNT);
        draw();
        return 0;
    }
    if (k == KEY_DOWN) {
        s_sel = (bleatk_mode_t)((s_sel + 1) % BLEATK_MODE_COUNT);
        draw();
        return 0;
    }
    if (k == KEY_OK) {
        /* Open the picker on whichever list is currently selected. */
        s_picker_sel = (int)svc_ble_flood_name_mode();
        s_picker = 1;
        draw();
        return 0;
    }
    return 0;
}

/* Entry guard for anything needing the BLE target: 1 = target verified
 * present, run the attack. 0 = "nothing selected" or "target not found"
 * was shown, caller must redirect to the scan page.
 *
 * NOTE: the ADV flood does NOT use this. It is self-targeting, like WiFi
 * beacon spam. Calling this for a mode that needs no target would bounce the
 * user to the scan page for nothing. */
int ui_bleatk_tick(void) {
    /* Auto-stop is honoured HERE, never in the GAP callback: the callback
     * only sets svc_ble_flood_expired(), because deinitialising Bluedroid
     * from inside a BT callback faults the stack. Same two-phase rule as
     * svc_deauth's TX timer. */
    if (svc_ble_flood_running() && svc_ble_flood_expired()) {
        s_timed_out = 1;
        svc_ble_flood_stop();
        draw();
        return 1;
    }
    if (!svc_ble_flood_running()) {
        return 0;
    }
    /* Repaint only the live bands: clearing and redrawing the whole screen
     * 5x a second would drown the OLED and the caster link in traffic. */
    oled_fb_t *fb = hal_oled_fb();
    oled_fill_rect(fb, 0, STATUS_Y, OLED_W, 18, 0);
    draw_status(fb);
    hal_oled_flush(0, STATUS_Y, OLED_W, 18);
    return 1;
}

int ui_bleatk_require(void) {
    if (!tgt_ble_has()) {
        draw_msg("nothing selected", "opening scan...");
        vTaskDelay(pdMS_TO_TICKS(1200));
        return 0;
    }
    draw_msg("checking target...", NULL);
    svc_wifi_teardown(); /* BT needs the heap WiFi holds */
    int r = tgt_ble_verify();
    if (r <= 0) {
        draw_msg(r < 0 ? "verify failed" : "target not found",
                 "opening scan...");
        vTaskDelay(pdMS_TO_TICKS(1200));
        return 0;
    }
    return 1;
}
