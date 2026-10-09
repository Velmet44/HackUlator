#include "ui_blescan.h"
#include "app_config.h"
#include "hal_oled.h"
#include "hal_input.h"
#include "oled_gfx.h"
#include "font_oled.h"
#include "svc_ble.h"
#include "svc_wifi.h"
#include "esp_heap_caps.h"
#include "esp_bt_defs.h"
#include "esp_gap_ble_api.h"
#include "svc_resume.h"
#include "svc_target.h"
#include "ui_status.h"
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
static int s_page = 0;

#define ROWS 4
#define ROW_PX 11
#define LIST_Y0 12
#define SCAN_SECS 5
#define FIELDS_PER_PAGE 4
#define BLE_NFIELDS 10

static const char *addr_label(int a) {
    switch (a) {
        case BLE_ADDR_TYPE_PUBLIC:
            return "PUBLIC";
        case BLE_ADDR_TYPE_RANDOM:
            return "RANDOM";
        case BLE_ADDR_TYPE_RPA_PUBLIC:
            return "RPA-PUB";
        case BLE_ADDR_TYPE_RPA_RANDOM:
            return "RPA-RND";
        default:
            return "?";
    }
}

static const char *evt_label(int e) {
    switch (e) {
        case ESP_BLE_EVT_CONN_ADV:
            return "ADV_IND";
        case ESP_BLE_EVT_CONN_DIR_ADV:
            return "ADV_DIR";
        case ESP_BLE_EVT_DISC_ADV:
            return "SCAN_IND";
        case ESP_BLE_EVT_NON_CONN_ADV:
            return "ADV_NON";
        case ESP_BLE_EVT_SCAN_RSP:
            return "SCAN_RSP";
        default:
            return "?";
    }
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

static void draw_row(oled_fb_t *fb, int y, ble_dev_t *dv, int sel) {
    char row[36];
    char nm[20];
    snprintf(nm, sizeof(nm), "%.18s", dv->name);
    int rs = dv->rssi;
    if (rs < -999) {
        rs = -999;
    }
    if (rs > 999) {
        rs = 999;
    }
    snprintf(row, sizeof(row), "%-18.18s %4d", nm, rs);
    row[32] = 0;
    if (sel) {
        oled_fill_rect(fb, 0, y, OLED_W, ROW_PX, 1);
    }
    oled_text(fb, &oled_font, 0, y + 1, row, sel ? 0 : 1);
}

static void draw_list(void) {
    oled_fb_t *fb = hal_oled_fb();
    oled_clear(fb, 0);
    char head[24];
    snprintf(head, sizeof(head), s_count < 0 ? "scan failed" : "%d devices",
             s_count < 0 ? 0 : s_count);
    int hw = oled_text_w(&oled_font, head);
    oled_text(fb, &oled_font, (OLED_W - hw) / 2, 0, head, 1);
    oled_hline(fb, 0, 10, OLED_W, 1);
    if (s_count > 0) {
        for (int i = 0; i < ROWS && s_top + i < s_count; i++) {
            int idx = s_top + i;
            draw_row(fb, LIST_Y0 + i * ROW_PX, &s_devs[idx], idx == s_sel);
        }
        {
            char left[16], right[16];
            int a = s_top + 1, b = s_count; /* clamp: int-proof buffer */
            if (a < 0) {
                a = 0;
            }
            if (a > 99) {
                a = 99;
            }
            if (b < 0) {
                b = 0;
            }
            if (b > 99) {
                b = 99;
            }
            if (s_count > ROWS) {
                snprintf(left, sizeof(left), "%d/%d %uK", a, b,
                         ui_status_memk());
            } else {
                snprintf(left, sizeof(left), "%uK", ui_status_memk());
            }
            oled_text(fb, &oled_font, 0, OLED_H - 9, left, 1);
            if (tgt_wifi_has() || tgt_ble_has()) {
                ui_status_targets(right, sizeof(right));
            } else {
                snprintf(right, sizeof(right), "OK");
            }
            int rw = oled_text_w(&oled_font, right);
            oled_text(fb, &oled_font, OLED_W - rw, OLED_H - 9, right, 1);
        }
    }
    hal_oled_flush_all();
}

/* i-th detail field of one device, as "LBL val" truncated to 32 cells. */
static void ble_field(ble_dev_t *dv, int i, char *out, size_t n) {
    char rsb[16], tx[16], fl[12], svc[8], mfr[8];
    switch (i) {
        case 0:
            snprintf(out, n, "NAME %s", dv->name);
            break;
        case 1:
            snprintf(out, n, "MAC %s", dv->mac);
            break;
        case 2: {
            int rs = dv->rssi < -999 ? -999 : dv->rssi > 999 ? 999 : dv->rssi;
            snprintf(rsb, sizeof(rsb), "%d dBm", rs);
            snprintf(out, n, "RSSI %s", rsb);
            break;
        }
        case 3:
            snprintf(out, n, "ADDR %s", addr_label(dv->addr_type));
            break;
        case 4:
            snprintf(out, n, "TYPE %s", evt_label(dv->evt_type));
            break;
        case 5:
            if (dv->has_tx) {
                snprintf(tx, sizeof(tx), "%d dBm", dv->tx_pwr);
            } else {
                snprintf(tx, sizeof(tx), "-");
            }
            snprintf(out, n, "TX %s", tx);
            break;
        case 6:
            if (dv->has_flags) {
                snprintf(fl, sizeof(fl), "0x%02X%s%s", dv->flags,
                         dv->flags & 0x01 ? " L" : "",
                         dv->flags & 0x02 ? " G" : "");
            } else {
                snprintf(fl, sizeof(fl), "-");
            }
            snprintf(out, n, "FLAGS %s", fl);
            break;
        case 7:
            if (dv->has_svc) {
                snprintf(svc, sizeof(svc), "0x%04X", dv->svc16);
            } else {
                snprintf(svc, sizeof(svc), "-");
            }
            snprintf(out, n, "SVC %s", svc);
            break;
        case 8:
            /* Advert events seen during the scan window. This is a count
             * of how OFTEN a device announces itself - NOT a load figure.
             * A phone and a fitness tracker advertise at similar rates
             * whether idle or streaming, so there is no honest "busy %"
             * to show here. */
            snprintf(out, n, "adv %lu in 5s",
                     (unsigned long)(dv->adv > 99999UL ? 99999UL : dv->adv));
            break;
        default:
            if (dv->has_mfr) {
                snprintf(mfr, sizeof(mfr), "0x%04X", dv->mfr);
            } else {
                snprintf(mfr, sizeof(mfr), "-");
            }
            snprintf(out, n, "MFR %s", mfr);
            break;
    }
    out[n - 1] = 0;
}

static void draw_detail(void) {
    oled_fb_t *fb = hal_oled_fb();
    oled_clear(fb, 0);
    const char *head = "BLE detail";
    int hw = oled_text_w(&oled_font, head);
    oled_text(fb, &oled_font, (OLED_W - hw) / 2, 0, head, 1);
    oled_hline(fb, 0, 10, OLED_W, 1);
    ble_dev_t *dv = &s_devs[s_sel];
    for (int i = 0; i < FIELDS_PER_PAGE; i++) {
        int fi = s_page * FIELDS_PER_PAGE + i;
        if (fi >= BLE_NFIELDS) {
            break;
        }
        char line[36];
        ble_field(dv, fi, line, sizeof(line));
        line[32] = 0;
        oled_text(fb, &oled_font, 0, LIST_Y0 + i * ROW_PX + 1, line, 1);
    }
    {
        int is_sel = tgt_ble_has() &&
            !strcmp(s_devs[s_sel].mac, tgt_ble_mac());
        char pg[12];
        int npg = (BLE_NFIELDS + FIELDS_PER_PAGE - 1) / FIELDS_PER_PAGE;
        int pg0 = s_page + 1;
        if (pg0 < 0) {
            pg0 = 0;
        }
        if (pg0 > 99) {
            pg0 = 99;
        }
        if (npg < 0) {
            npg = 0;
        }
        if (npg > 99) {
            npg = 99;
        }
        snprintf(pg, sizeof(pg), "%d/%d", pg0, npg);
        oled_text(fb, &oled_font, 0, OLED_H - 9,
                  is_sel ? "selected" : "R=sel", 1);
        int pw = oled_text_w(&oled_font, pg);
        oled_text(fb, &oled_font, OLED_W - pw, OLED_H - 9, pg, 1);
    }
    hal_oled_flush_all();
}

/* Heap floor for a radio bring-up (see ui_wifiscan.c). */
#define BRINGUP_MIN_KB 16

/* 1 when heap can take a bring-up, else resume-or-park (returns 0). */
static int heap_ok_or_reboot(void) {
    unsigned kb = (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT)
                             / 1024);
    if (kb >= BRINGUP_MIN_KB) {
        return 1;
    }
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
    if (!heap_ok_or_reboot()) {
        return -1;
    }
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
    s_page = 0;
    hacku_input_drain();
    if (s_count < 0) {
        char det[24];
        unsigned kb = (unsigned)(heap_caps_get_largest_free_block(
                                     MALLOC_CAP_8BIT) / 1024);
        if (kb > 9999) {
            kb = 9999;
        }
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
    s_page = 0;
}

int ui_blescan_key(hacku_key_t k) {
    int npg = (BLE_NFIELDS + FIELDS_PER_PAGE - 1) / FIELDS_PER_PAGE;
    if (s_detail) {
        switch (k) {
            case KEY_UP:
                if (s_page > 0) {
                    s_page--;
                    draw_detail();
                }
                return 0;
            case KEY_DOWN:
                if (s_page + 1 < npg) {
                    s_page++;
                    draw_detail();
                }
                return 0;
            case KEY_OK:
            case KEY_LEFT:
            case KEY_BACK:
                s_detail = 0;
                s_page = 0;
                draw_list();
                return 0;
            case KEY_RIGHT:
                tgt_ble_set(s_devs[s_sel].mac, s_devs[s_sel].name,
                            s_devs[s_sel].addr_type);
                draw_detail();
                return 0;
            default:
                return 0;
        }
    }
    switch (k) {
        case KEY_UP:
            if (s_sel > 0) {
                s_sel--;
                if (s_sel < s_top) {
                    s_top = s_sel;
                }
                draw_list();
            }
            return 0;
        case KEY_DOWN:
            if (s_sel + 1 < s_count) {
                s_sel++;
                if (s_sel >= s_top + ROWS) {
                    s_top = s_sel - ROWS + 1;
                }
                draw_list();
            }
            return 0;
        case KEY_OK:
            if (s_count > 0) {
                s_detail = 1;
                s_page = 0;
                draw_detail();
            }
            return 0;
        case KEY_BACK:
            return 1;
        default:
            return 0;
    }
}
