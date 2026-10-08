#include "ui_wifiscan.h"
#include "app_config.h"
#include "hal_display.h"
#include "hal_input.h"
#include "gfx.h"
#include "font.h"
#include "svc_wifi.h"
#include "esp_wifi_types.h"
#include "esp_heap_caps.h"
#include "svc_resume.h"
#include "ui_blescan.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static wifi_ap_t *s_aps = NULL; /* heap, not static: protects the FB block */
static int s_count = -2; /* -2 never scanned, -1 error, >=0 count */
static int s_top = 0;    /* first visible row */
static int s_sel = 0;    /* highlighted entry */
static int s_detail = 0; /* 1 = detail screen */

#define ROWS 5
#define ROW_PX 48
#define LIST_Y0 52

static const char *auth_label(int a) {
    switch (a) {
        case WIFI_AUTH_OPEN:            return "OPEN";
        case WIFI_AUTH_WEP:             return "WEP";
        case WIFI_AUTH_WPA_PSK:         return "WPA";
        case WIFI_AUTH_WPA2_PSK:        return "WPA2";
        case WIFI_AUTH_WPA_WPA2_PSK:    return "WPA*";
        case WIFI_AUTH_ENTERPRISE:      return "WPA2-E";
        case WIFI_AUTH_WPA3_PSK:        return "WPA3";
        case WIFI_AUTH_WPA2_WPA3_PSK:   return "WPA2/3";
        case WIFI_AUTH_WAPI_PSK:        return "WAPI";
        case WIFI_AUTH_OWE:             return "OWE";
        case WIFI_AUTH_WPA3_ENT_192:    return "WPA3-E";
        case WIFI_AUTH_WPA3_EXT_PSK:
        case WIFI_AUTH_WPA3_EXT_PSK_MIXED_MODE:
                                        return "WPA3";
        case WIFI_AUTH_DPP:             return "DPP";
        case WIFI_AUTH_WPA3_ENTERPRISE: return "WPA3-E";
        default:                        return "???";
    }
}

static const char *cipher_label(int c) {
    switch (c) {
        case WIFI_CIPHER_TYPE_NONE:      return "NONE";
        case WIFI_CIPHER_TYPE_WEP40:
        case WIFI_CIPHER_TYPE_WEP104:    return "WEP";
        case WIFI_CIPHER_TYPE_TKIP:      return "TKIP";
        case WIFI_CIPHER_TYPE_CCMP:      return "CCMP";
        case WIFI_CIPHER_TYPE_TKIP_CCMP: return "T+C";
        case WIFI_CIPHER_TYPE_SMS4:      return "SMS4";
        case WIFI_CIPHER_TYPE_GCMP:
        case WIFI_CIPHER_TYPE_GCMP256:   return "GCMP";
        default:                         return "?";
    }
}

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

static void draw_list(void) {
    hacku_fb_t *fb = hacku_display_fb();
    gfx_fill_rect(fb, 0, 0, HACKU_DISP_W, HACKU_DISP_H, C_BLACK);
    char head[32];
    snprintf(head, sizeof(head), s_count < 0 ? "scan failed" : "%d networks",
             s_count < 0 ? 0 : s_count);
    int hw = gfx_text_w(&hacku_font, head, 1);
    gfx_text(fb, &hacku_font, (HACKU_DISP_W - hw) / 2, 8,
             head, C_ORANGE, -1, 1);
    gfx_hline(fb, 0, 36, HACKU_DISP_W, C_GRAY25);
    if (s_count > 0) {
        for (int i = 0; i < ROWS && s_top + i < s_count; i++) {
            int idx = s_top + i;
            wifi_ap_t *ap = &s_aps[idx];
            int y = LIST_Y0 + i * ROW_PX;
            if (idx == s_sel)
                gfx_fill_rect(fb, 2, y - 2, HACKU_DISP_W - 4, ROW_PX - 2,
                              C_GRAY25);
            int ch = ap->channel;
            if (ch < 0) ch = 0;
            if (ch > 99) ch = 99;
            int rs = ap->rssi;
            if (rs < -999) rs = -999;
            if (rs > 999) rs = 999;
            char chb[12], rsb[12], det[40]; /* worst-case int-proof */
            snprintf(chb, sizeof(chb), "%2d", ch);
            snprintf(rsb, sizeof(rsb), "%4d", rs);
            snprintf(det, sizeof(det), "ch %.2s %.4sdBm %.6s", chb, rsb,
                     auth_label(ap->auth));
            char name[20];
            snprintf(name, sizeof(name), "%.18s",
                     ap->ssid[0] ? ap->ssid : "<hidden>");
            gfx_text(fb, &hacku_font, 6, LIST_Y0 + i * ROW_PX,
                     name, C_WHITE, -1, 1);
            gfx_text(fb, &hacku_font, 6, LIST_Y0 + i * ROW_PX + 22,
                     det, CALC_DIM, -1, 1);
        }
        if (s_count > ROWS) {
            char more[24]; /* 2x worst-case int + '/' + NUL */
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
    const char *head = "WiFi detail";
    int hw = gfx_text_w(&hacku_font, head, 1);
    gfx_text(fb, &hacku_font, (HACKU_DISP_W - hw) / 2, 8,
             head, C_ORANGE, -1, 1);
    gfx_hline(fb, 0, 36, HACKU_DISP_W, C_GRAY25);
    wifi_ap_t *ap = &s_aps[s_sel];
    int y = 44;
    char bssid[18], rsb[16], ch[8], ci[16], phy[16], ftm[8], cc[4];
    snprintf(bssid, sizeof(bssid), "%02X:%02X:%02X:%02X:%02X:%02X",
             ap->bssid[0], ap->bssid[1], ap->bssid[2],
             ap->bssid[3], ap->bssid[4], ap->bssid[5]);
    int rs = ap->rssi < -999 ? -999 : ap->rssi > 999 ? 999 : ap->rssi;
    snprintf(rsb, sizeof(rsb), "%d dBm", rs);
    int c = ap->channel < 0 ? 0 : ap->channel > 99 ? 99 : ap->channel;
    snprintf(ch, sizeof(ch), ap->second == 1 ? "%d+" :
                             ap->second == 2 ? "%d-" : "%d", c);
    snprintf(ci, sizeof(ci), "P:%.4s G:%.4s",
             cipher_label(ap->pairwise), cipher_label(ap->group));
    snprintf(phy, sizeof(phy), "%s%s%s%s%s",
             ap->phy_b ? "b" : "", ap->phy_g ? "/g" : "",
             ap->phy_n ? "/n" : "", ap->phy_lr ? "/lr" : "",
             ap->phy_ax ? "/ax" : "");
    if (!phy[0])
        snprintf(phy, sizeof(phy), "-");
    else if (phy[0] == '/')
        memmove(phy, phy + 1, strlen(phy));
    snprintf(ftm, sizeof(ftm), "%s%s%s",
             ap->ftm_r ? "R" : "", ap->ftm_i ? "I" : "",
             !ap->ftm_r && !ap->ftm_i ? "-" : "");
    snprintf(cc, sizeof(cc), "%.2s", ap->country);
    if (cc[0] < ' ' || cc[1] < ' ')
        snprintf(cc, sizeof(cc), "-");
    det_field(fb, &y, "SSID", ap->ssid[0] ? ap->ssid : "<hidden>");
    det_field(fb, &y, "MAC", bssid);
    det_field(fb, &y, "RSSI", rsb);
    det_field(fb, &y, "CHAN", ch);
    det_field(fb, &y, "AUTH", auth_label(ap->auth));
    det_field(fb, &y, "CIPH", ci);
    det_field(fb, &y, "PHY", phy[0] == '/' ? phy + 1 : phy);
    det_field(fb, &y, "WPS", ap->wps ? "yes" : "no");
    det_field(fb, &y, "FTM", ftm);
    det_field(fb, &y, "CC", cc);
    hacku_display_flush_all();
}

/* Heap floor for a radio bring-up (pristine boot shows ~34K; radio cycles
 * leak ~10-15K each, so the floor is reached after a few switches). Below
 * this, IDF's own bring-up unwinds hit NULL allocs and assert - so persist
 * the requested scan and reboot to pristine heap instead of crashing.
 * Two strikes in a row (no successful scan between) parks on a safe
 * screen: something is structurally wrong, don't reboot-loop. */
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
    resume_request(RESUME_WIFI);
    return 0;
}

int ui_wifiscan_run(void) {
    ui_blescan_drop(); /* the other radio's list is dead weight now */
    if (!heap_ok_or_reboot())
        return -1;
    char sl2[24];
    snprintf(sl2, sizeof(sl2), "2.4G %uK",
             (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT)
                        / 1024));
    draw_msg("scanning...", sl2);
    if (!s_aps) {
        s_aps = heap_caps_malloc(sizeof(wifi_ap_t) * WIFI_MAX_AP,
                                 MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL);
        if (!s_aps) {
            draw_msg("out of memory", "BACK to return");
            return -1;
        }
    }
    s_count = svc_wifi_scan(s_aps, WIFI_MAX_AP);
    s_top = 0;
    s_sel = 0;
    s_detail = 0;
    hacku_input_drain(); /* drop keys pressed mid-scan */
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

void ui_wifiscan_drop(void) {
    if (s_aps) {
        free(s_aps);
        s_aps = NULL;
    }
    s_count = -2;
    s_top = 0;
    s_sel = 0;
    s_detail = 0;
}

int ui_wifiscan_key(hacku_key_t k) {
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
