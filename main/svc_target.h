#pragma once

/* Session-only attack targets (plain RAM: reboot clears, never persisted).
 * Separate slots for WiFi (keyed by BSSID) and BLE (keyed by MAC).
 * Set from the scan detail views, consumed by attacks. Selecting another
 * entry overwrites the slot. */

#include <stdint.h>

/* WiFi target. ssid copied ("" when hidden), bssid 6 bytes, channel is
 * cached at select time and refreshed by verify. */
int           tgt_wifi_has(void);
void          tgt_wifi_set(const char *ssid, const uint8_t bssid[6],
                           int channel);
void          tgt_wifi_clear(void);
const char   *tgt_wifi_ssid(void);
const uint8_t *tgt_wifi_bssid(void);
int           tgt_wifi_channel(void);
/* Fresh scan for the stored BSSID: 1 present (channel refreshed),
 * 0 absent, <0 scan/alloc error. Caller must have the other radio down. */
int           tgt_wifi_verify(void);

/* BLE target. mac "AA:BB:CC:DD:EE:FF", name copied, addr_type cached. */
int         tgt_ble_has(void);
void        tgt_ble_set(const char *mac, const char *name, int addr_type);
void        tgt_ble_clear(void);
const char *tgt_ble_mac(void);
const char *tgt_ble_name(void);
int         tgt_ble_addr_type(void);
/* Fresh GAP scan for the stored MAC: 1 present, 0 absent, <0 error.
 * Caller must have WiFi torn down first. */
int         tgt_ble_verify(void);
