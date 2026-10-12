#include "ui_nrf.h"
#include "app_config.h"
#include "hal_oled.h"
#include "oled_gfx.h"
#include "font_oled.h"
#include "svc_nrf.h"
#include "svc_wifi.h"
#include "svc_ble.h"
#include "ui_status.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdio.h>

static const char *TAG = "uinrf";

/* Channels measured per tick. 24 x 8 samples x ~190 us is ~36 ms of SPI
 * and busy wait, so the main loop keeps ~160 ms of every 200 ms tick for
 * key handling - and it is what puts a moving cursor on the plot. */
#define STEP_CHANNELS  24

/* ---- Layout ----
 * 0..8    title        9  rule      10..45 plot
 * 46..54  ruler        55..63 status bar
 * The plot's bottom row is the baseline; bars grow upward from it. */
#define TITLE_Y   0
#define RULE_Y    9
#define PLOT_Y0   10
#define PLOT_Y1   45
#define PLOT_H    (PLOT_Y1 - PLOT_Y0 + 1)   /* 36 */
#define RULER_Y   46
#define STATUS_Y  (OLED_H - 9)              /* 55 */

enum { VIEW_SWEEP = 0, VIEW_FALL };
static int s_view;

/* Waterfall history: one column per channel, newest sample at the top, so a
 * sweep advances every column exactly once and the image scrolls as a whole
 * instead of tearing.
 *
 * 64-bit, not 32: a column is PLOT_H = 36 rows deep, and 1 << 35 is not
 * representable in Xtensa's 32-bit unsigned long, so a uint32_t here is
 * both an overflow and a silent truncation. 125 x 8 B = 1 KB of static RAM,
 * which is the honest price of a history deep enough to read. */
static uint64_t s_hist[NRF_CH_COUNT];
#define HIST_TOP_BIT  (PLOT_H - 1)          /* 35 */
#define HIST_TOP_SET  ((uint64_t)1 << HIST_TOP_BIT)

/* Columns per channel. 128 columns for 125 channels means ~3 columns repeat
 * a channel; the alternative is 1px gaps between bars, which reads as a
 * broken line rather than a filled trace. */
static int ch_for_x(int x) {
    return (x * NRF_CH_COUNT) / OLED_W;
}

static int x_for_mhz(int mhz) {
    int ch = mhz - NRF_CH0_MHZ;
    if (ch < 0) {
        ch = 0;
    }
    if (ch > NRF_CH_COUNT - 1) {
        ch = NRF_CH_COUNT - 1;
    }
    return (ch * OLED_W) / NRF_CH_COUNT;
}

/* Hit count -> pixels. The scale is FIXED at 0..samples rather than scaled
 * to the trace's own maximum: an autoscale makes a quiet band look busy,
 * and two screenshots taken a minute apart stop being comparable. The scale
 * is printed in the corner instead. */
static int level_px(uint8_t v) {
    int s = svc_nrf_samples();
    if (v == 0) {
        return 0;
    }
    int h = ((int)v * PLOT_H) / (s > 0 ? s : 1);
    if (h < 1) {
        h = 1;
    }
    if (h > PLOT_H) {
        h = PLOT_H;
    }
    return h;
}

static void put(const char *s, int x, int y) {
    oled_text(hal_oled_fb(), &oled_font, x, y, s, 1);
}

/* Clamp helper for values that reach snprintf: these counters are
 * free-running, so nothing may be trusted to fit under -Werror=all. */
static int clamp_i(int v, int lo, int hi) {
    if (v < lo) {
        return lo;
    }
    if (v > hi) {
        return hi;
    }
    return v;
}

static void put_right(const char *s, int y) {
    int w = oled_text_w(&oled_font, s);
    put(s, OLED_W - w, y);
}

/* ---- plot furniture ---- */

/* Frequency grid, drawn BEFORE the trace so bars sit on top of it. Every
 * third pixel only: a solid grid line competes with the data, a dotted one
 * stays behind it. */
static void draw_grid(oled_fb_t *fb) {
    for (int mhz = NRF_CH0_MHZ; mhz <= NRF_CH0_MHZ + 124; mhz += 25) {
        int x = x_for_mhz(mhz);
        for (int y = PLOT_Y0; y <= PLOT_Y1; y += 3) {
            oled_pset(fb, x, y, 1);
        }
    }
    /* 5 MHz ticks along the baseline: enough to interpolate between the
     * labelled 25 MHz grid lines without a label per tick. */
    for (int mhz = NRF_CH0_MHZ; mhz <= NRF_CH0_MHZ + 124; mhz += 5) {
        int x = x_for_mhz(mhz);
        oled_pset(fb, x, PLOT_Y1 - 2, 1);
        oled_pset(fb, x, PLOT_Y1 - 1, 1);
    }
    oled_hline(fb, 0, PLOT_Y1, OLED_W, 1);
}

/* The bars themselves, plus the one detail that makes a 1-bit plot legible:
 * a 1px knockout at the top of every bar at least 3px tall. Without it the
 * tops merge into a ragged block; with it a flat region reads as a clean
 * dotted outline instead of a solid slab. */
static void draw_bars(oled_fb_t *fb) {
    for (int x = 0; x < OLED_W; x++) {
        int ch = ch_for_x(x);
        int h = level_px(svc_nrf_level(ch));
        for (int y = 0; y < h; y++) {
            oled_pset(fb, x, PLOT_Y1 - y, 1);
        }
        if (h >= 3) {
            oled_pset(fb, x, PLOT_Y1 - h, 0);
        }
    }
}

/* Peak hold drawn as a free-floating tick ABOVE the live level, so a signal
 * that has dropped out stays readable as an outline rather than a bar. */
static void draw_hold(oled_fb_t *fb) {
    if (!svc_nrf_peak_hold_on()) {
        return;
    }
    for (int x = 0; x < OLED_W; x++) {
        int ch = ch_for_x(x);
        int hp = level_px(svc_nrf_peak(ch));
        int hl = level_px(svc_nrf_level(ch));
        if (hp > hl) {
            oled_pset(fb, x, PLOT_Y1 - hp, 1);
        }
    }
}

/* Where the sweep currently is. Inverted, so it stays visible both over a
 * tall bar and over bare background. */
static void draw_cursor(oled_fb_t *fb) {
    int x = x_for_mhz(NRF_CH0_MHZ + svc_nrf_cursor());
    oled_invert(fb, x, PLOT_Y0, 1, PLOT_H);
}

/* Dashed line at the strongest channel, tying the ruler's centre number to
 * a visible position. */
static void draw_peak_marker(oled_fb_t *fb, int peak_ch) {
    if (peak_ch < 0) {
        return;
    }
    int x = x_for_mhz(NRF_CH0_MHZ + peak_ch);
    for (int y = PLOT_Y0; y <= PLOT_Y1; y += 2) {
        oled_pset(fb, x, y, 1);
    }
}

static void draw_view_sweep(oled_fb_t *fb, int peak_ch) {
    draw_grid(fb);
    draw_bars(fb);
    draw_hold(fb);
    draw_peak_marker(fb, peak_ch);
    draw_cursor(fb);
}

static void draw_view_fall(oled_fb_t *fb) {
    oled_hline(fb, 0, PLOT_Y1, OLED_W, 1);
    for (int x = 0; x < OLED_W; x++) {
        int ch = ch_for_x(x);
        uint64_t col = s_hist[ch];
        int h = level_px(svc_nrf_level(ch));
        for (int y = 0; y < h; y++) {
            int bit = HIST_TOP_BIT - y;
            if (bit >= 0 && (col >> bit) & 1u) {
                oled_pset(fb, x, PLOT_Y1 - y, 1);
            }
        }
    }
    draw_cursor(fb);
}

/* Scale + receiver bandwidth, in the plot's empty top corners. Both are
 * always on screen: the level is a hit RATE, and which bandwidth measured
 * it changes what it means, so neither can be left to the header. */
static void draw_corner_text(void) {
    char buf[20];
    snprintf(buf, sizeof(buf), "hits %u/%d",
             (unsigned)clamp_i((int)svc_nrf_peak_level(), 0, 999),
             clamp_i(svc_nrf_samples(), 0, 99));
    buf[19] = 0;
    put(buf, 1, PLOT_Y0);
    put_right(svc_nrf_rate_name(), PLOT_Y0);
}

/* 2400 / <peak MHz> / 2525. Centring the peak under the plot and marking it
 * with a dashed line makes the middle number mean a position, not a caption. */
static void draw_ruler(int peak_ch) {
    put("2400", 0, RULER_Y);
    put_right("2525", RULER_Y);
    char buf[8];
    if (peak_ch >= 0) {
        snprintf(buf, sizeof(buf), "%d", clamp_i(NRF_CH0_MHZ + peak_ch, 2400, 2524));
    } else {
        snprintf(buf, sizeof(buf), "--");
    }
    buf[7] = 0;
    int w = oled_text_w(&oled_font, buf);
    put(buf, (OLED_W - w) / 2, RULER_Y);
}

static void draw(void) {
    oled_fb_t *fb = hal_oled_fb();
    oled_clear(fb, 0);
    const char *head = "2.4G SPECTRUM";
    int hw = oled_text_w(&oled_font, head);
    put(head, (OLED_W - hw) / 2, TITLE_Y);
    oled_hline(fb, 0, RULE_Y, OLED_W, 1);

    int peak_ch = svc_nrf_peak_ch();
    if (s_view == VIEW_FALL) {
        draw_view_fall(fb);
    } else {
        draw_view_sweep(fb, peak_ch);
    }
    draw_corner_text();
    draw_ruler(peak_ch);

    {
        char st[20];
        ui_status_line(st, sizeof(st));
        int sw = oled_text_w(&oled_font, st);
        put(st, (OLED_W - sw) / 2, STATUS_Y);
    }
    hal_oled_flush_all();
}

static void draw_msg(const char *l1, const char *l2) {
    oled_fb_t *fb = hal_oled_fb();
    oled_clear(fb, 0);
    int w1 = oled_text_w(&oled_font, l1);
    put(l1, (OLED_W - w1) / 2, 22);
    if (l2) {
        int w2 = oled_text_w(&oled_font, l2);
        put(l2, (OLED_W - w2) / 2, 34);
    }
    hal_oled_flush_all();
}

static void clear_hist(void) {
    for (int i = 0; i < NRF_CH_COUNT; i++) {
        s_hist[i] = 0;
    }
}

/* Push one sweep's worth of history. Called when the cursor wraps, so every
 * column advances exactly once per sweep and the image scrolls as a whole. */
static void push_hist(void) {
    for (int i = 0; i < NRF_CH_COUNT; i++) {
        uint32_t bit = (svc_nrf_level(i) > 0) ? HIST_TOP_SET : 0u;
        s_hist[i] = (s_hist[i] >> 1) | bit;
    }
}

void ui_nrf_run(void) {
    /* One radio at a time, and here it is a strict requirement rather than
     * a convention: the ESP32's own Wi-Fi and BT transmit in the same band
     * and would light every column of the trace. main.c's enter_menu_item()
     * already stops every other radio owner; this is the same guard the
     * other screens carry so the screen is safe to enter from anywhere. */
    svc_ble_stop();
    svc_wifi_teardown();

    /* GPIO14 and GPIO26 belong to the radio while this screen is open. */
    hacku_input_set_button_mask((1UL << PIN_NRF_CSN) | (1UL << PIN_NRF_CE));

    s_view = VIEW_SWEEP;
    clear_hist();

    int r = svc_nrf_init();
    if (r != 0) {
        ESP_LOGE(TAG, "init failed: %d", r);
        const char *e = r == NRF_ERR_HEAP   ? "need more free RAM"
                      : r == NRF_ERR_ABSENT ? "no nRF24L01 on SPI3"
                      : r == NRF_ERR_SPI    ? "SPI3 init failed"
                                             : "unknown error";
        draw_msg("2.4G spectrum failed", e);
        vTaskDelay(pdMS_TO_TICKS(2500));
        hacku_input_set_button_mask(0);
        return;
    }

    draw_msg("OK view  UP/DN res", "BK out");
    vTaskDelay(pdMS_TO_TICKS(2200));
    draw();
    hacku_input_drain();
}

int ui_nrf_tick(void) {
    if (!svc_nrf_present()) {
        return 0;
    }
    int before = svc_nrf_cursor();
    svc_nrf_step(STEP_CHANNELS);
    /* Wrapped: a complete sweep just finished, so age the waterfall. */
    if (svc_nrf_cursor() < before) {
        push_hist();
    }
    draw();
    return 1;
}

/* Key map, and the one that is NOT on it:
 *
 *   OK        cycle SWEEP / FALL
 *   UP / DOWN samples per channel (resolution against sweep speed)
 *   RIGHT     receiver bandwidth 250k / 1M / 2M
 *   LEFT/BACK exit
 *
 * RIGHT is unreachable on the six-key keypad while this screen is open,
 * because its pin (GPIO26) is the radio's CE - see app_config.h. It is
 * deliberately NOT advertised in the on-screen legend: a hint that names a
 * key which cannot physically fire on this screen is a lie about what the
 * device can do. The caster still delivers it, and the current bandwidth is
 * shown in the plot's top-right corner at all times, so the setting is never
 * in doubt even where the key is missing. One-button mode reaches OK, UP,
 * DOWN and BACK, which is everything except the bandwidth.
 */
int ui_nrf_key(hacku_key_t k) {
    if (!svc_nrf_present()) {
        /* Init failed and ui_nrf_run() already handed the pins back.
         * Returning from here must not clear the mask again - it would
         * un-mask pins nothing owns. Harmless today, wrong the day
         * something does. */
        return NRF_EXIT_MENU;
    }
    if (k == KEY_BACK || k == KEY_LEFT) {
        svc_nrf_stop();
        hacku_input_set_button_mask(0);
        return NRF_EXIT_MENU;
    }
    if (k == KEY_OK) {
        s_view = (s_view + 1) % 2;
        draw();
        return 0;
    }
    /* Resolution against sweep speed. Every extra sample costs ~190 us of
     * settle, so this is the knob that trades a fast trace against a tall
     * scale. */
    if (k == KEY_UP || k == KEY_DOWN) {
        int cur = svc_nrf_samples();
        int next = cur + ((k == KEY_UP) ? 4 : -4);
        svc_nrf_set_samples(next);
        draw();
        return 0;
    }
    /* Receiver measurement bandwidth. Not a display preference: it sets both
     * the RPD threshold (-83 dBm at 250k, -64 dBm above) and how much of a
     * channel is actually observed, so it changes what the trace MEANS. */
    if (k == KEY_RIGHT) {
        int n = NRF_RATE_COUNT;
        svc_nrf_set_rate((nrf_rate_t)((svc_nrf_rate() + 1) % n));
        draw();
        return 0;
    }
    return 0;
}