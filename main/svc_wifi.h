#pragma once

/* Minimal WiFi service: lazy STA init, one-shot AP scan, radio off on lock. */

#include <stdint.h>

#define WIFI_MAX_AP 32

typedef struct {
    char ssid[33];
    uint8_t bssid[6];
    int  rssi;
    int  channel;
    int  second;   /* wifi_second_chan_t: 0 = HT20, 1 = second above, 2 = below */
    int  auth;     /* wifi_auth_mode_t value */
    int  pairwise; /* wifi_cipher_type_t value */
    int  group;    /* wifi_cipher_type_t value */
    uint8_t phy_b, phy_g, phy_n, phy_lr, phy_ax;
    uint8_t wps, ftm_r, ftm_i;
    char country[4]; /* 2-letter code + NUL, "" if unset */
} wifi_ap_t;

/* Init + start STA (idempotent, lazy: first scan only). Returns 0 on ok. */
int svc_wifi_init(void);

/* Blocking AP scan (a few seconds). Returns AP count.
 * Negative stage codes (also shown on the failure screen):
 * -1 args, -2 init, -3 scan_start, -4 ap_num, -5 records/oom. */
int svc_wifi_scan(wifi_ap_t *out, int max);

/* Stop the radio (stealth + power). Safe to call when already stopped. */
void svc_wifi_stop(void);

/* Full teardown: stop + deinit driver + destroy netif, freeing radio heap
 * (lets BLE init afterwards on this RAM-tight chip). Next scan re-inits. */
void svc_wifi_teardown(void);
