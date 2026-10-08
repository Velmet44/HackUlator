#include "svc_target.h"
#include "svc_wifi.h"
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

int tgt_wifi_verify(void) {
    if (!s_wifi.has)
        return 0;
    wifi_ap_t *aps = malloc(sizeof(wifi_ap_t) * WIFI_MAX_AP);
    if (!aps)
        return -1;
    int n = svc_wifi_scan(aps, WIFI_MAX_AP);
    int rc = -1;
    if (n >= 0) {
        rc = 0;
        for (int i = 0; i < n; i++) {
            if (!memcmp(aps[i].bssid, s_wifi.bssid, 6)) {
                s_wifi.channel = aps[i].channel; /* AP may have moved */
                rc = 1;
                break;
            }
        }
    }
    free(aps);
    return rc;
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
