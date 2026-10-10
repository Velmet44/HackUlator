#pragma once

/* Session-only attack targets (plain RAM: reboot clears, never persisted).
 * Separate slots for WiFi (keyed by BSSID) and BLE (keyed by MAC).
 * Set from the scan detail views, consumed by attacks. Selecting another
 * entry overwrites the slot. */

#include <stdint.h>

/* tgt_wifi_verify() results.
 *
 * These were a bare 1 / 0 / <0, and the single negative covered both "the
 * scan list had no room" and "the radio refused to scan" - two problems
 * with completely different fixes that the UI could only report as one
 * message. Distinct codes let the screen say which actually happened.
 * Appended, never renumbered. */
#define TGT_WIFI_PRESENT    1
#define TGT_WIFI_ABSENT     0
#define TGT_WIFI_NO_MEM   (-1)
#define TGT_WIFI_SCAN_ERR (-2)

/* WiFi target. ssid copied ("" when hidden), bssid 6 bytes, channel is
 * cached at select time and refreshed by verify. */
int           tgt_wifi_has(void);
void          tgt_wifi_set(const char *ssid, const uint8_t bssid[6],
                           int channel);
void          tgt_wifi_clear(void);
const char   *tgt_wifi_ssid(void);
const uint8_t *tgt_wifi_bssid(void);
int           tgt_wifi_channel(void);
/* Is the stored BSSID still here? 1 present, 0 absent, <0 error per the
 * TGT_WIFI_* codes. Caller must have the other radio down.
 *
 * Listens on the target's known channel first and only falls back to a full
 * scan if that finds nothing, because an AP rarely changes channel while a
 * client stays associated and the scan is both slower and subject to a
 * record cap. Pass force_scan to skip the fast path - required when the
 * question is "which channel is it on now", since listening on the old
 * channel cannot answer that. */
int           tgt_wifi_verify(void);
int           tgt_wifi_verify_ex(int force_scan);

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
