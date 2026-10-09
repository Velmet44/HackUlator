#pragma once

/* Passive promiscuous RX monitor (own lab).
 *
 * Puts the radio in promiscuous mode and COUNTS the 802.11 frames the chip
 * overhears, without joining any network. This is the missing half of the
 * scanner: svc_wifi.c can only see APs during an active scan, which samples
 * beacons and drops the data traffic in between.
 *
 * Deliberate design limits, because the callback runs on the WiFi task and
 * every frame it holds pins one of the few static RX buffers
 * (CONFIG_ESP_WIFI_STATIC_RX_BUFFER_NUM = 6 in sdkconfig):
 *   - count only, never queue or copy a payload;
 *   - no ESP_LOG* from the callback (it would stall the WiFi task);
 *   - no frame bodies are retained, so there is no pcap and no handshake
 *     capture. Seeing WHICH frames arrived is a separate, larger job.
 *
 * The ESP-IDF driver exposes no RX-drop counter, so s_rate/s_total are
 * RELATIVE figures: enough to see traffic appear and disappear, never a
 * claim about absolute capture completeness.
 */

#include <stdint.h>

/* What to listen for. SNIFF_ALL is the plain monitor; the filtered modes
 * exist so a later attack can measure one target's traffic specifically. */
typedef enum {
    SNIFF_OFF = 0,
    SNIFF_ALL,    /* every management + data frame */
    SNIFF_TRAFFIC, /* data frames only (what a client is really sending) */
    SNIFF_PROBE,   /* management frames only (beacons / probe requests) */
} sniff_mode_t;

/* Start promiscuous RX. filter_bssid may be NULL (ignored unless the mode
 * filters on it). Brings the STA up if it is down, leaves it in STA mode and
 * does NOT join or authenticate to anything.
 * 0 ok; <0 stage error (-1 wifi init, -2 promisc set, -3 filter, -4 cb). */
int svc_sniff_start(sniff_mode_t mode, const uint8_t *filter_bssid);

/* Stop promiscuous RX and clear the filter. MUST run before any WiFi/BLE
 * teardown: a live callback pointing into a deinitialised driver faults. */
void svc_sniff_stop(void);

int svc_sniff_running(void);
sniff_mode_t svc_sniff_mode(void);

/* Counters. All are since svc_sniff_start(). */
uint32_t svc_sniff_total(void);  /* every counted frame */
uint32_t svc_sniff_mgmt(void);   /* beacons, probes, auth, deauth */
uint32_t svc_sniff_data(void);   /* real traffic */
uint32_t svc_sniff_other(void);  /* control/misc, kept out of the headline */

/* Frames/sec over the last ~1 s window. The live proof-of-life number: it
 * climbs near traffic and falls to 0 when the device walks away. */
uint32_t svc_sniff_rate(void);

/* Strongest (least negative) RSSI seen this run, in dBm. Only meaningful
 * when svc_sniff_have_rssi() is 1 - the sentinel before the first frame is
 * -128, which is below any real reading. */
int svc_sniff_best_rssi(void);
int svc_sniff_have_rssi(void);

/* Channel the monitor is parked on. */
int svc_sniff_channel(void);

/* Frame counters are bumped from the WiFi-task callback, so a 32-bit load is
 * not atomic on ESP32. Read through these helpers. */
uint32_t svc_sniff_poll_total(void);