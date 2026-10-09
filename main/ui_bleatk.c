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

static void draw(void) {
    oled_fb_t *fb = hal_oled_fb();
    oled_clear(fb, 0);
    const char *head = "BLE attacks";
    int hw = oled_text_w(&oled_font, head);
    oled_text(fb, &oled_font, (OLED_W - hw) / 2, 0, head, 1);
    {
        char t[33];
        snprintf(t, sizeof(t), ">%.27s", tgt_ble_mac());
        t[32] = 0;
        int tw = oled_text_w(&oled_font, t);
        oled_text(fb, &oled_font, (OLED_W - tw) / 2, 10, t, 1);
    }
    const char *l1 = "empty";
    int w1 = oled_text_w(&oled_font, l1);
    oled_text(fb, &oled_font, (OLED_W - w1) / 2, 28, l1, 1);
    {
        char st[20];
        ui_status_line(st, sizeof(st));
        int sw = oled_text_w(&oled_font, st);
        oled_text(fb, &oled_font, (OLED_W - sw) / 2, OLED_H - 18, st, 1);
    }
    const char *l2 = "BACK to return";
    int w2 = oled_text_w(&oled_font, l2);
    oled_text(fb, &oled_font, (OLED_W - w2) / 2, OLED_H - 9, l2, 1);
    hal_oled_flush_all();
}

void ui_bleatk_run(void) {
    draw();
    hacku_input_drain();
}

int ui_bleatk_key(hacku_key_t k) {
    return k == KEY_BACK ? 1 : 0;
}

/* Entry guard for anything needing the BLE target: 1 = target verified
 * present, run the attack. 0 = "nothing selected" or "target not found"
 * was shown, caller must redirect to the scan page. */
/* No running BLE attack yet: nothing to repaint. */
int ui_bleatk_tick(void) {
    return 0;
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
