#include "ui_wifiatk.h"
#include "app_config.h"
#include "hal_display.h"
#include "hal_input.h"
#include "gfx.h"
#include "font.h"
#include "svc_ble.h"
#include "svc_target.h"
#include "ui_status.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdio.h>

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
        gfx_text(fb, &hacku_font, (HACKU_DISP_W - tw) / 2, 52,
                 t, C_GREEN, -1, 1);
    }
    const char *l1 = "empty";
    int w1 = gfx_text_w(&hacku_font, l1, 1);
    gfx_text(fb, &hacku_font, (HACKU_DISP_W - w1) / 2, 140,
             l1, CALC_DIM, -1, 1);
    {
        char st[24];
        ui_status_line(st, sizeof(st));
        int sw = gfx_text_w(&hacku_font, st, 1);
        gfx_text(fb, &hacku_font, (HACKU_DISP_W - sw) / 2,
                 HACKU_DISP_H - 52, st, CALC_DIM, -1, 1);
    }
    const char *l2 = "BACK to return";
    int w2 = gfx_text_w(&hacku_font, l2, 1);
    gfx_text(fb, &hacku_font, (HACKU_DISP_W - w2) / 2, HACKU_DISP_H - 24,
             l2, CALC_DIM, -1, 1);
    hacku_display_flush_all();
}

void ui_wifiatk_run(void) {
    draw();
    hacku_input_drain();
}

int ui_wifiatk_key(hacku_key_t k) {
    return k == KEY_BACK ? 1 : 0;
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
