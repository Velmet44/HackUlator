#include "ui_blepassive.h"
#include "app_config.h"
#include "hal_oled.h"
#include "hal_input.h"
#include "oled_gfx.h"
#include "font_oled.h"
#include "svc_ble.h"
#include "svc_wifi.h"
#include "svc_sniff.h"
#include "ui_status.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdio.h>

/* Same vertical rhythm as ui_sniff.c so the two screens feel identical. */
#define RATE_Y    10
#define TOT_Y     19
#define RSSI_Y    28
#define NOTE_Y    37
#define STAT_Y    46
#define HINT_Y    (OLED_H - 9)

#define BLOCK_Y   RATE_Y
#define BLOCK_H   (NOTE_Y + 9 - RATE_Y)

/* Same bring-up floor as the scan screens (ui_wifiscan.c). */
#define BRINGUP_MIN_KB 16

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

static void draw_counters(oled_fb_t *fb) {
    char line[33];
    unsigned long r = (unsigned long)svc_ble_adv_tick();
    if (r > 999999UL) {
        r = 999999UL;
    }
    snprintf(line, sizeof(line), "%lu adv/s", r);
    line[32] = 0;
    oled_text(fb, &oled_font, 4, RATE_Y, line, 1);

    unsigned long t = (unsigned long)svc_ble_adv_total();
    if (t > 99999999UL) {
        t = 99999999UL;
    }
    snprintf(line, sizeof(line), "total %lu", t);
    line[32] = 0;
    oled_text(fb, &oled_font, 4, TOT_Y, line, 1);

    int rssi = svc_ble_adv_best_rssi();
    if (rssi == 0) {
        snprintf(line, sizeof(line), "no device in range");
    } else {
        snprintf(line, sizeof(line), "best %d dBm", rssi);
    }
    line[32] = 0;
    oled_text(fb, &oled_font, 4, RSSI_Y, line, 1);

    /* Say what the screen is NOT doing: this is the point of the mode. */
    snprintf(line, sizeof(line), "passive - never replies");
    line[32] = 0;
    oled_text(fb, &oled_font, 4, NOTE_Y, line, 1);
}

static void draw(void) {
    oled_fb_t *fb = hal_oled_fb();
    oled_clear(fb, 0);
    const char *head = "BLE passive";
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
    oled_text(fb, &oled_font, (OLED_W - w) / 2, HINT_Y, hint, 1);
    hal_oled_flush_all();
}

void ui_blepassive_run(void) {
    svc_sniff_stop();       /* promiscuous RX must die before BT comes up */
    svc_wifi_teardown();    /* one radio at a time */
    unsigned kb = (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT)
                             / 1024);
    if (kb < BRINGUP_MIN_KB) {
        char l2[24];
        snprintf(l2, sizeof(l2), "need 16K, have %uK", kb > 9999 ? 9999 : kb);
        draw_msg("mem too low", l2);
        vTaskDelay(pdMS_TO_TICKS(1200));
        return;
    }
    int r = svc_ble_passive_start();
    if (r < 0) {
        draw_msg("BLE passive failed", esp_err_to_name((esp_err_t)r));
        vTaskDelay(pdMS_TO_TICKS(1200));
        return;
    }
    draw();
    hacku_input_drain();
}

int ui_blepassive_tick(void) {
    if (!svc_ble_passive_running()) {
        return 0;
    }
    oled_fb_t *fb = hal_oled_fb();
    oled_fill_rect(fb, 0, BLOCK_Y, OLED_W, BLOCK_H, 0);
    draw_counters(fb);
    hal_oled_flush(0, BLOCK_Y, OLED_W, BLOCK_H);
    return 1;
}

int ui_blepassive_key(hacku_key_t k) {
    if (k == KEY_BACK || k == KEY_LEFT) {
        svc_ble_passive_stop();
        return BLEPASS_EXIT_MENU;
    }
    if (k == KEY_OK) {
        draw();
    }
    return 0;
}