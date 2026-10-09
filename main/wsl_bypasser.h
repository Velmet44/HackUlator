#pragma once

/* Wi-Fi Stack Libraries (WSL) frame-type bypass for raw 802.11 injection.
 * Stock ESP-IDF's esp_wifi_80211_tx() refuses management frames other than
 * beacon/probe/action (see esp_wifi.h: "Currently only support for sending
 * beacon/probe request/probe response/action and non-QoS data frame"),
 * returning ESP_ERR_INVALID_ARG for deauth/disassoc.
 *
 * The check lives inside the closed Wi-Fi binary as
 * ieee80211_raw_frame_sanity_check() (recovered via Ghidra by
 * GANESH-ICMC/esp32-deauther). Defining our own symbol with the same name
 * and linking with -Wl,-zmuldefs lets ours win, so arbitrary 802.11 frames
 * can be injected on a stock ESP-IDF toolchain.
 *
 * Same technique as Hydra-ESP / risinek esp32-wifi-penetration-tool. The
 * component REQUIRES the -Wl,-zmuldefs link flag (see CMakeLists.txt) or the
 * duplicate symbol is dropped and injection keeps failing.
 */

#include <stdint.h>
#include "esp_err.h"

/* Deauth (subtype 0xC) frame: broadcast dst, BSSID patched into addr2/addr3,
 * reason 0x0002 PREV_AUTH_NOT_VALID. */
void wsl_send_deauth(const uint8_t bssid[6]);

/* Disassoc (subtype 0xA), reason 0x0001 UNSPECIFIED. */
void wsl_send_disassoc(const uint8_t bssid[6]);

/* Beacon frame advertising `ssid` from `bssid` on `channel`. */
void wsl_send_beacon(const uint8_t bssid[6], const char *ssid, int channel);

/* Raw passthrough: returns the driver error (ESP_OK = transmitted). */
esp_err_t wsl_send_raw(const uint8_t *frame, int len);