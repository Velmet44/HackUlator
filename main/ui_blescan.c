#include "ui_blescan.h"
#include "app_config.h"
#include "hal_display.h"
#include "hal_input.h"
#include "gfx.h"
#include "font.h"
#include "svc_ble.h"
#include "svc_wifi.h"
#include "esp_heap_caps.h"
#include "esp_bt_defs.h"
#include "esp_gap_ble_api.h"
#include "svc_resume.h"
#include "ui_wifiscan.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static ble_dev_t *s_devs = NULL; /* heap, not static: protects the FB block */
static int s_count = -2;
static int s_top = 0;
static int s_sel = 0;
static int s_detail = 0;

#define ROWS 5
#define ROW_PX 48
#define LIST_Y0 52
#define SCAN_SECS 5

static const char *addr_label(int a) {
    switch (a) {
        case BLE_ADDR_TYPE_PUBLIC:     return "PUBLIC";
        case BLE_ADDR_TYPE_RANDOM:     return "RANDOM";
        case BLE_ADDR_TYPE_RPA_PUBLIC: return "RPA-PUB";
        case BLE_ADDR_TYPE_RPA_RANDOM: return "RPA-RND";
        default:                       return "?";
    }
}

static const char *evt_label(int e) {
    switch (e) {
        case ESP_BLE_EVT_CONN_ADV:     return "ADV_IND";
        case ESP_BLE_EVT_CONN_DIR_ADV: return "ADV_DIR";
        case ESP_BLE_EVT_DISC_ADV:     return "SCAN_IND";
        case ESP_BLE_EVT_NON_CONN_ADV: return "ADV_NON";
        case ESP_BLE_EVT_SCAN_RSP:     return "SCAN_RSP";
        default:                       return "?";
    }
}

static void draw_msg(const char *l1, const char *l2) {
    hacku_fb_t *fb = hacku_display_fb();
    gfx_fill_rect(fb, 0, 0, HACKU_DISP_W, HACKU_DISP_H, C_BLACK);
    int w1 = gfx_text_w(&hacku_font, l1, 1);
    gfx_text(fb, &hacku_font, (HACKU_DISP_W - w1) / 2, 130, l1, C_WHITE, -1, 1);
    if (l2) {
        int w2 = gfx_text_w(&hacku_font, l2, 1);
        gfx_text(fb, &hacku_font, (HACKU_DISP_W - w2) / 2, 160, l2,
                 CALC_DIM, -1, 1);
    }
    hacku_display_flush_all();
}

static void draw_list(void) {
    hacku_fb_t *fb = hacku_display_fb();
    gfx_fill_rect(fb, 0, 0, HACKU_DISP_W, HACKU_DISP_H, C_BLACK);
    char head[24];
    snprintf(head, sizeof(head), s_count < 0 ? "scan failed" : "%d devices",
             s_count < 0 ? 0 : s_count);
    int hw = gfx_text_w(&hacku_font, head, 1);
    gfx_text(fb, &hacku_font, (HACKU_DISP_W - hw) / 2, 8, head, C_ORANGE, -1, 1);
    gfx_hline(fb, 0, 36, HACKU_DISP_W, C_GRAY25);
    if (s_count > 0) {
        for (int i = 0; i < ROWS && s_top + i < s_count; i++) {
            int idx = s_top + i;
            ble_dev_t *dv = &s_devs[idx];
            int y = LIST_Y0 + i * ROW_PX;
            if (idx == s_sel)
                gfx_fill_rect(fb, 2, y - 2, HACKU_DISP_W - 4, ROW_PX - 2,
                              C_GRAY25);
            int rs = dv->rssi;
            if (rs < -999) rs = -999;
            if (rs > 999) rs = 999;
            char rsb[12], det[24];
            snprintf(rsb, sizeof(rsb), "%4d", rs);
            snprintf(det, sizeof(det), "%.4sdBm", rsb);
            char name[20];
            snprintf(name, sizeof(name), "%.18s", dv->name);
            gfx_text(fb, &hacku_font, 6, y, name, C_WHITE, -1, 1);
            gfx_text(fb, &hacku_font, 6, y + 22, det, CALC_DIM, -1, 1);
        }
        if (s_count > ROWS) {
            char more[24];
            snprintf(more, sizeof(more), "%d/%d", s_top + 1, s_count);
            gfx_text(fb, &hacku_font, 6, HACKU_DISP_H - 24,
                     more, CALC_DIM, -1, 1);
        }
        gfx_text(fb, &hacku_font,
                 HACKU_DISP_W - 6 - gfx_text_w(&hacku_font, "OK", 1),
                 HACKU_DISP_H - 24, "OK", CALC_DIM, -1, 1);
    }
    hacku_display_flush_all();
}

/* Detail line: "label value" two-tone on one line when it fits in 21
 * cells, else dim label line + wrapped white value lines. Advances *y. */
static void det_field(hacku_fb_t *fb, int *y,
                      const char *label, const char *val) {
    int lw = gfx_text_w(&hacku_font, label, 1);
    int sp = gfx_text_w(&hacku_font, " ", 1);
    int vw = gfx_text_w(&hacku_font, val, 1);
    if (lw + sp + vw <= 21 * hacku_font.w) {
        gfx_text(fb, &hacku_font, 6, *y, label, CALC_DIM, -1, 1);
        gfx_text(fb, &hacku_font, 6 + lw + sp, *y, val, C_WHITE, -1, 1);
        *y += 23;
        return;
    }
    gfx_text(fb, &hacku_font, 6, *y, label, CALC_DIM, -1, 1);
    *y += 23;
    size_t n = strlen(val), off = 0;
    do {
        char seg[24];
        size_t k = n - off > 21 ? 21 : n - off;
        memcpy(seg, val + off, k);
        seg[k] = 0;
        gfx_text(fb, &hacku_font, 6, *y, seg, C_WHITE, -1, 1);
        *y += 23;
        off += k;
    } while (off < n);
}

static void draw_detail(void) {
    hacku_fb_t *fb = hacku_display_fb();
    gfx_fill_rect(fb, 0, 0, HACKU_DISP_W, HACKU_DISP_H, C_BLACK);
    const char *head = "BLE detail";
    int hw = gfx_text_w(&hacku_font, head, 1);
    gfx_text(fb, &hacku_font, (HACKU_DISP_W - hw) / 2, 8,
             head, C_ORANGE, -1, 1);
    gfx_hline(fb, 0, 36, HACKU_DISP_W, C_GRAY25);
    ble_dev_t *dv = &s_devs[s_sel];
    int y = 44;
    char rsb[16], tx[16], fl[12], svc[8], mfr[8];
    int rs = dv->rssi < -999 ? -999 : dv->rssi > 999 ? 999 : dv->rssi;
    snprintf(rsb, sizeof(rsb), "%d dBm", rs);
    if (dv->has_tx)
        snprintf(tx, sizeof(tx), "%d dBm", dv->tx_pwr);
    else
        snprintf(tx, sizeof(tx), "-");
    if (dv->has_flags)
        snprintf(fl, sizeof(fl), "0x%02X%s%s", dv->flags,
                 dv->flags & 0x01 ? " L" : "",
                 dv->flags & 0x02 ? " G" : "");
    else
        snprintf(fl, sizeof(fl), "-");
    if (dv->has_svc)
        snprintf(svc, sizeof(svc), "0x%04X", dv->svc16);
    else
        snprintf(svc, sizeof(svc), "-");
    if (dv->has_mfr)
        snprintf(mfr, sizeof(mfr), "0x%04X", dv->mfr);
    else
        snprintf(mfr, sizeof(mfr), "-");
    det_field(fb, &y, "NAME", dv->name);
    det_field(fb, &y, "MAC", dv->mac);
    det_field(fb, &y, "RSSI", rsb);
    det_field(fb, &y, "ADDR", addr_label(dv->addr_type));
    det_field(fb, &y, "TYPE", evt_label(dv->evt_type));
    det_field(fb, &y, "TX", tx);
    det_field(fb, &y, "FLAGS", fl);
    det_field(fb, &y, "SVC", svc);
    det_field(fb, &y, "MFR", mfr);
    hacku_display_flush_all();
}

/* Heap floor for a radio bring-up (see ui_wifiscan.c): below this, IDF's
 * own BT bring-up unwinds hit NULL allocs and assert - persist the scan
 * and reboot to pristine heap instead of crashing. Two strikes parks. */
#define BRINGUP_MIN_KB 16

/* 1 when heap can take a bring-up, else resume-or-park (returns 0). */
static int heap_ok_or_reboot(void) {
    unsigned kb = (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT)
                             / 1024);
    if (kb >= BRINGUP_MIN_KB)
        return 1;
    if (resume_note_lowmem() >= 2) {
        draw_msg("mem too low", "reset device");
        for (;;)
            vTaskDelay(pdMS_TO_TICKS(1000));
    }
    char l2[24];
    snprintf(l2, sizeof(l2), "low mem %uK reboot", kb > 9999 ? 9999 : kb);
    draw_msg("switching...", l2);
    vTaskDelay(pdMS_TO_TICKS(800));
    resume_request(RESUME_BLE);
    return 0;
}

int ui_blescan_run(void) {
    svc_wifi_teardown(); /* one radio at a time: free WiFi heap for BT */
    ui_wifiscan_drop(); /* the other radio's list is dead weight now */
    if (!heap_ok_or_reboot())
        return -1;
    char sl2[24];
    snprintf(sl2, sizeof(sl2), "BLE %uK",
             (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT)
                        / 1024));
    draw_msg("scanning BLE...", sl2);
    if (!s_devs) {
        s_devs = heap_caps_malloc(sizeof(ble_dev_t) * BLE_MAX_DEV,
                                  MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL);
        if (!s_devs) {
            draw_msg("out of memory", "BACK to return");
            return -1;
        }
    }
    s_count = svc_ble_scan(s_devs, BLE_MAX_DEV, SCAN_SECS);
    s_top = 0;
    s_sel = 0;
    s_detail = 0;
    hacku_input_drain();
    if (s_count < 0) {
        char det[24];
        unsigned kb = (unsigned)(heap_caps_get_largest_free_block(
                                     MALLOC_CAP_8BIT) / 1024);
        if (kb > 9999)
            kb = 9999;
        snprintf(det, sizeof(det), "err %d %uK", s_count, kb);
        draw_msg("scan failed", det);
        return -1;
    }
    resume_mark_ok();
    draw_list();
    return s_count;
}

void ui_blescan_drop(void) {
    if (s_devs) {
        free(s_devs);
        s_devs = NULL;
    }
    s_count = -2;
    s_top = 0;
    s_sel = 0;
    s_detail = 0;
}

int ui_blescan_key(hacku_key_t k) {
    if (s_detail) {
        switch (k) {
            case KEY_UP:
                if (s_sel > 0) {
                    s_sel--;
                    if (s_sel < s_top)
                        s_top = s_sel;
                    draw_detail();
                }
                return 0;
            case KEY_DOWN:
                if (s_sel + 1 < s_count) {
                    s_sel++;
                    if (s_sel >= s_top + ROWS)
                        s_top = s_sel - ROWS + 1;
                    draw_detail();
                }
                return 0;
            case KEY_OK:
            case KEY_LEFT:
            case KEY_BACK:
                s_detail = 0;
                draw_list();
                return 0;
            default:
                return 0;
        }
    }
    switch (k) {
        case KEY_UP:
            if (s_sel > 0) {
                s_sel--;
                if (s_sel < s_top)
                    s_top = s_sel;
                draw_list();
            }
            return 0;
        case KEY_DOWN:
            if (s_sel + 1 < s_count) {
                s_sel++;
                if (s_sel >= s_top + ROWS)
                    s_top = s_sel - ROWS + 1;
                draw_list();
            }
            return 0;
        case KEY_OK:
            if (s_count > 0) {
                s_detail = 1;
                draw_detail();
            }
            return 0;
        case KEY_BACK:
            return 1;
        default:
            return 0;
    }
}
