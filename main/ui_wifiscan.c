#include "ui_wifiscan.h"
#include "app_config.h"
#include "hal_oled.h"
#include "hal_input.h"
#include "oled_gfx.h"
#include "font_oled.h"
#include "svc_wifi.h"
#include "svc_sniff.h"
#include "esp_wifi_types.h"
#include "esp_heap_caps.h"
#include "svc_resume.h"
#include "svc_target.h"
#include "ui_status.h"
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
static int s_page = 0;   /* detail field page */

#define ROWS 4
#define ROW_PX 11
#define LIST_Y0 12
#define FIELDS_PER_PAGE 4
#define WIFI_NFIELDS 12
/* Post-scan listen sample. Only the strongest AP's channel is measured -
 * one burst keeps the scan quick, and most networks cluster on 1/6/11.
 * APs on other channels report "not sampled" rather than a wrong number. */
#define BURST_MS 2500
static sniff_burst_t s_burst;
static int s_burst_ok;

static const sniff_ap_t *burst_find(const uint8_t *bssid) {
    if (!s_burst_ok) {
        return NULL;
    }
    for (int i = 0; i < s_burst.n && i < SNIFF_AP_TRACK; i++) {
        if (memcmp(s_burst.ap[i].bssid, bssid, 6) == 0) {
            return &s_burst.ap[i];
        }
    }
    return NULL;
}

static const char *auth_label(int a) {
    switch (a) {
        case WIFI_AUTH_OPEN:
            return "OPEN";
        case WIFI_AUTH_WEP:
            return "WEP";
        case WIFI_AUTH_WPA_PSK:
            return "WPA";
        case WIFI_AUTH_WPA2_PSK:
            return "WPA2";
        case WIFI_AUTH_WPA_WPA2_PSK:
            return "WPA*";
        case WIFI_AUTH_ENTERPRISE:
            return "WPA2-E";
        case WIFI_AUTH_WPA3_PSK:
            return "WPA3";
        case WIFI_AUTH_WPA2_WPA3_PSK:
            return "WPA2/3";
        case WIFI_AUTH_WAPI_PSK:
            return "WAPI";
        case WIFI_AUTH_OWE:
            return "OWE";
        case WIFI_AUTH_WPA3_ENT_192:
            return "WPA3-E";
        case WIFI_AUTH_WPA3_EXT_PSK:
            return "WPA3";
        case WIFI_AUTH_WPA3_EXT_PSK_MIXED_MODE:
            return "WPA3";
        case WIFI_AUTH_DPP:
            return "DPP";
        case WIFI_AUTH_WPA3_ENTERPRISE:
            return "WPA3-E";
        default:
            return "???";
    }
}

static const char *cipher_label(int c) {
    switch (c) {
        case WIFI_CIPHER_TYPE_NONE:
            return "NONE";
        case WIFI_CIPHER_TYPE_WEP40:
            return "WEP";
        case WIFI_CIPHER_TYPE_WEP104:
            return "WEP";
        case WIFI_CIPHER_TYPE_TKIP:
            return "TKIP";
        case WIFI_CIPHER_TYPE_CCMP:
            return "CCMP";
        case WIFI_CIPHER_TYPE_TKIP_CCMP:
            return "T+C";
        case WIFI_CIPHER_TYPE_SMS4:
            return "SMS4";
        case WIFI_CIPHER_TYPE_GCMP:
            return "GCMP";
        case WIFI_CIPHER_TYPE_GCMP256:
            return "GCMP";
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

/* One list row: "ssid ch rssi", truncated to 32 cells. */
static void draw_row(oled_fb_t *fb, int y, wifi_ap_t *ap, int sel) {
    char row[36];
    char nm[20];
    snprintf(nm, sizeof(nm), "%.18s", ap->ssid[0] ? ap->ssid : "<hid>");
    int ch = ap->channel;
    if (ch < 0) {
        ch = 0;
    }
    if (ch > 99) {
        ch = 99;
    }
    int rs = ap->rssi;
    if (rs < -999) {
        rs = -999;
    }
    if (rs > 999) {
        rs = 999;
    }
    snprintf(row, sizeof(row), "%-18.18s %2d %4d", nm, ch, rs);
    row[32] = 0;
    if (sel) {
        oled_fill_rect(fb, 0, y, OLED_W, ROW_PX, 1);
    }
    oled_text(fb, &oled_font, 0, y + 1, row, sel ? 0 : 1);
}

static void draw_list(void) {
    oled_fb_t *fb = hal_oled_fb();
    oled_clear(fb, 0);
    char head[32];
    snprintf(head, sizeof(head), s_count < 0 ? "scan failed" : "%d nets",
             s_count < 0 ? 0 : s_count);
    int hw = oled_text_w(&oled_font, head);
    oled_text(fb, &oled_font, (OLED_W - hw) / 2, 0, head, 1);
    oled_hline(fb, 0, 10, OLED_W, 1);
    if (s_count > 0) {
        for (int i = 0; i < ROWS && s_top + i < s_count; i++) {
            int idx = s_top + i;
            draw_row(fb, LIST_Y0 + i * ROW_PX, &s_aps[idx], idx == s_sel);
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

/* i-th detail field of one AP, as "LBL val" truncated to 32 cells. */
static void wifi_field(wifi_ap_t *ap, int i, char *out, size_t n) {
    char bssid[18], rsb[16], ch[8], ci[14], phy[14], ftm[6], cc[4];
    switch (i) {
        case 0:
            snprintf(out, n, "SSID %.30s", ap->ssid[0] ? ap->ssid : "<hid>");
            break;
        case 1:
            snprintf(bssid, sizeof(bssid), "%02X:%02X:%02X:%02X:%02X:%02X",
                     ap->bssid[0], ap->bssid[1], ap->bssid[2],
                     ap->bssid[3], ap->bssid[4], ap->bssid[5]);
            snprintf(out, n, "MAC %s", bssid);
            break;
        case 2: {
            int rs = ap->rssi < -999 ? -999 : ap->rssi > 999 ? 999 : ap->rssi;
            snprintf(rsb, sizeof(rsb), "%d dBm", rs);
            snprintf(out, n, "RSSI %s", rsb);
            break;
        }
        case 3: {
            int c = ap->channel < 0 ? 0 : ap->channel > 99 ? 99 : ap->channel;
            snprintf(ch, sizeof(ch), ap->second == 1 ? "%d+" :
                                     ap->second == 2 ? "%d-" : "%d", c);
            snprintf(out, n, "CHAN %s", ch);
            break;
        }
        case 4:
            snprintf(out, n, "AUTH %s", auth_label(ap->auth));
            break;
        case 5:
            snprintf(ci, sizeof(ci), "P:%.4s G:%.4s",
                     cipher_label(ap->pairwise), cipher_label(ap->group));
            snprintf(out, n, "CIPH %s", ci);
            break;
        case 6:
            snprintf(phy, sizeof(phy), "%s%s%s%s%s",
                     ap->phy_b ? "b" : "", ap->phy_g ? "/g" : "",
                     ap->phy_n ? "/n" : "", ap->phy_lr ? "/lr" : "",
                     ap->phy_ax ? "/ax" : "");
            if (!phy[0]) {
                snprintf(phy, sizeof(phy), "-");
            } else if (phy[0] == '/') {
                memmove(phy, phy + 1, strlen(phy));
            }
            snprintf(out, n, "PHY %s", phy[0] == '/' ? phy + 1 : phy);
            break;
        case 7:
            snprintf(out, n, "WPS %s", ap->wps ? "yes" : "no");
            break;
        case 8:
            snprintf(ftm, sizeof(ftm), "%s%s%s",
                     ap->ftm_r ? "R" : "", ap->ftm_i ? "I" : "",
                     !ap->ftm_r && !ap->ftm_i ? "-" : "");
            snprintf(out, n, "FTM %s", ftm);
            break;
        case 10: {
            /* Activity sample: how busy this AP's channel was during the
             * post-scan listen burst. Only one channel is sampled, so
             * anything else says which channel was measured instead of
             * printing a number that does not describe this network. */
            if (!s_burst_ok) {
                snprintf(out, n, "busy  sample failed");
            } else if (ap->channel != s_burst.channel) {
                snprintf(out, n, "busy  sampled ch%d only",
                         s_burst.channel);
            } else {
                unsigned hz = s_burst.elapsed_ms > 0
                    ? (unsigned)((uint64_t)s_burst.total * 1000u /
                                 (uint32_t)s_burst.elapsed_ms) : 0u;
                snprintf(out, n, "ch%d busy %u f/s", ap->channel,
                         hz > 9999u ? 9999u : hz);
            }
            break;
        }
        case 11: {
            const sniff_ap_t *sa = burst_find(ap->bssid);
            if (!s_burst_ok || ap->channel != s_burst.channel || !sa) {
                snprintf(out, n, "frames -");
            } else {
                snprintf(out, n, "heard %lu in %ds",
                         (unsigned long)(sa->frames > 9999u
                             ? 9999u : sa->frames),
                         (int)(s_burst.elapsed_ms / 1000));
            }
            break;
        }
        default:
            snprintf(cc, sizeof(cc), "%.2s", ap->country);
            if (cc[0] < ' ' || cc[1] < ' ') {
                snprintf(cc, sizeof(cc), "-");
            }
            snprintf(out, n, "CC %s", cc);
            break;
    }
    out[n - 1] = 0;
}

static void draw_detail(void) {
    oled_fb_t *fb = hal_oled_fb();
    oled_clear(fb, 0);
    const char *head = "WiFi detail";
    int hw = oled_text_w(&oled_font, head);
    oled_text(fb, &oled_font, (OLED_W - hw) / 2, 0, head, 1);
    oled_hline(fb, 0, 10, OLED_W, 1);
    wifi_ap_t *ap = &s_aps[s_sel];
    for (int i = 0; i < FIELDS_PER_PAGE; i++) {
        int fi = s_page * FIELDS_PER_PAGE + i;
        if (fi >= WIFI_NFIELDS) {
            break;
        }
        char line[36];
        wifi_field(ap, fi, line, sizeof(line));
        line[32] = 0;
        oled_text(fb, &oled_font, 0, LIST_Y0 + i * ROW_PX + 1, line, 1);
    }
    {
        int is_sel = tgt_wifi_has() &&
            !memcmp(ap->bssid, tgt_wifi_bssid(), 6);
        char pg[12];
        int npg = (WIFI_NFIELDS + FIELDS_PER_PAGE - 1) / FIELDS_PER_PAGE;
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

/* Heap floor for a radio bring-up. The 1 KB OLED framebuffer leaves far
 * more headroom than the old 150 KB TFT one, but radio cycles still leak
 * ~10-15K each: keep the reboot-to-pristine-heap guard. Two strikes in a
 * row parks on a safe screen instead of reboot-looping. */
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
    resume_request(RESUME_WIFI);
    return 0;
}

int ui_wifiscan_run(void) {
    ui_blescan_drop(); /* the other radio's list is dead weight now */
    if (!heap_ok_or_reboot()) {
        return -1;
    }
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
    s_burst_ok = 0;
    s_top = 0;
    s_sel = 0;
    s_detail = 0;
    s_page = 0;
    hacku_input_drain(); /* drop keys pressed mid-scan */
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
    /* Sample activity on the strongest AP's channel so the detail view can
     * report real traffic rather than guessing from a beacon count. */
    if (s_count > 0) {
        int ch = s_aps[0].channel;   /* the strongest, since the list is RSSI-sorted */
        int best = s_aps[0].rssi;
        for (int i = 1; i < s_count; i++) {
            if (s_aps[i].rssi > best) {
                best = s_aps[i].rssi;
                ch = s_aps[i].channel;
            }
        }
        draw_msg("sampling band", "");
        s_burst_ok = svc_sniff_burst(ch, BURST_MS, &s_burst) >= 0;
    }
    draw_list();
    return s_count;
}

void ui_wifiscan_drop(void) {
    s_burst_ok = 0;
    if (s_aps) {
        free(s_aps);
        s_aps = NULL;
    }
    s_count = -2;
    s_top = 0;
    s_sel = 0;
    s_detail = 0;
    s_page = 0;
}

int ui_wifiscan_key(hacku_key_t k) {
    int npg = (WIFI_NFIELDS + FIELDS_PER_PAGE - 1) / FIELDS_PER_PAGE;
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
                tgt_wifi_set(s_aps[s_sel].ssid, s_aps[s_sel].bssid,
                             s_aps[s_sel].channel);
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
