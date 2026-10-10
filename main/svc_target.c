#include "svc_target.h"
#include "svc_wifi.h"
#include "svc_sniff.h"
#include "svc_ble.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

static struct {
    char ssid[33];
    uint8_t bssid[6];
    int channel;
    int has;
} s_wifi;

static struct {
    char mac[18];
    char name[24];
    int addr_type;
    int has;
} s_ble;

int tgt_wifi_has(void) { return s_wifi.has; }

void tgt_wifi_set(const char *ssid, const uint8_t bssid[6], int channel) {
    snprintf(s_wifi.ssid, sizeof(s_wifi.ssid), "%s", ssid ? ssid : "");
    memcpy(s_wifi.bssid, bssid, 6);
    s_wifi.channel = channel;
    s_wifi.has = 1;
}

void tgt_wifi_clear(void) { s_wifi.has = 0; }

const char *tgt_wifi_ssid(void) { return s_wifi.ssid; }

const uint8_t *tgt_wifi_bssid(void) { return s_wifi.bssid; }

int tgt_wifi_channel(void) { return s_wifi.channel; }

/* Listen window for the fast path. At the standard 100 TU interval a real
 * AP emits ~9.8 beacons/s, so 1200 ms is ~12 expected beacons; the measured
 * capture ratio near 86% still leaves ~10 to match on. Short enough that a
 * genuinely-moved target only adds ~1.2 s to the fallback scan. */
#define VERIFY_LISTEN_MS 1200

static int verify_burst(void) {
    sniff_burst_t b;
    int ch = s_wifi.channel;
    if (ch < 1 || ch > 13)
        ch = 1;
    if (svc_sniff_burst(ch, VERIFY_LISTEN_MS, &b) < 0)
        return -1;
    for (int i = 0; i < b.n; i++) {
        /* Beacon frames specifically: this is proof the AP is alive and
         * transmitting under this BSSID, not just some frame nearby. */
        if (b.ap[i].beacons > 0 && !memcmp(b.ap[i].bssid, s_wifi.bssid, 6)) {
            return 1;
        }
    }
    return 0;
}

static int verify_scan(void) {
    wifi_ap_t *aps = malloc(sizeof(wifi_ap_t) * WIFI_MAX_AP);
    if (!aps)
        return TGT_WIFI_NO_MEM;
    int n = svc_wifi_scan(aps, WIFI_MAX_AP);
    int rc = TGT_WIFI_SCAN_ERR;
    if (n >= 0) {
        rc = TGT_WIFI_ABSENT;
        for (int i = 0; i < n; i++) {
            if (!memcmp(aps[i].bssid, s_wifi.bssid, 6)) {
                s_wifi.channel = aps[i].channel; /* AP may have moved */
                rc = TGT_WIFI_PRESENT;
                break;
            }
        }
    }
    free(aps);
    return rc;
}

int tgt_wifi_verify_ex(int force_scan) {
    if (!s_wifi.has)
        return TGT_WIFI_ABSENT;
    if (!force_scan) {
        int b = verify_burst();
        if (b > 0)
            return TGT_WIFI_PRESENT;
        /* Only fall through on "not heard". An error here is not evidence
         * the AP is gone, and the scan below gives a real answer. */
    }
    return verify_scan();
}

int tgt_wifi_verify(void) {
    return tgt_wifi_verify_ex(0);
}

int tgt_ble_has(void) { return s_ble.has; }

void tgt_ble_set(const char *mac, const char *name, int addr_type) {
    snprintf(s_ble.mac, sizeof(s_ble.mac), "%s", mac ? mac : "");
    snprintf(s_ble.name, sizeof(s_ble.name), "%s", name ? name : "");
    s_ble.addr_type = addr_type;
    s_ble.has = 1;
}

void tgt_ble_clear(void) { s_ble.has = 0; }

const char *tgt_ble_mac(void) { return s_ble.mac; }

const char *tgt_ble_name(void) { return s_ble.name; }

int tgt_ble_addr_type(void) { return s_ble.addr_type; }

#define VERIFY_SECS 5

int tgt_ble_verify(void) {
    if (!s_ble.has)
        return 0;
    ble_dev_t *devs = malloc(sizeof(ble_dev_t) * BLE_MAX_DEV);
    if (!devs)
        return -1;
    int n = svc_ble_scan(devs, BLE_MAX_DEV, VERIFY_SECS);
    int rc = -1;
    if (n >= 0) {
        rc = 0;
        for (int i = 0; i < n; i++) {
            if (!strcmp(devs[i].mac, s_ble.mac)) {
                rc = 1;
                break;
            }
        }
    }
    free(devs);
    return rc;
}
