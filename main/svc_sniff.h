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

/* Park the listener on a specific channel (1..13) and reset the counters, so
 * a displayed rate always belongs to exactly one channel.
 *
 * Why this exists: at start-up the radio sits on the ESP32 default of
 * channel 1, which in most environments is EMPTY. A monitor counting an
 * empty channel reads near zero and looks broken. That is a correct count
 * of nothing. Hop to a channel with traffic before trusting the rate.
 * 0 ok, <0 refused (out of range, or the driver rejected it). */
int svc_sniff_set_channel(int ch);

/* ---- listen burst ----
 * A short blocking listen on one channel, used to sample a scanned
 * network's activity. Unlike the live monitor this is deliberately a
 * SAMPLE: it answers "how busy was this band for 2.5 s", not the same
 * thing as a continuous count.
 *
 * Must not be called while svc_sniff_running() - it takes over the
 * promiscuous callback itself. */
#define SNIFF_AP_TRACK 8
typedef struct {
    uint8_t  bssid[6];
    uint32_t frames;   /* all frames seen from this BSSID */
    uint32_t beacons;  /* subset of `frames` that were beacons */
    int      rssi;     /* strongest seen, negative dBm */
} sniff_ap_t;

typedef struct {
    int      channel;
    uint32_t total, mgmt, data;
    int      elapsed_ms;
    int      n;                  /* valid entries in ap[] */
    sniff_ap_t ap[SNIFF_AP_TRACK];
} sniff_burst_t;

/* Listen on `channel` for `ms`, then stop. Fills *out (zeroed first).
 * Returns the total frame count, or <0 on failure. */
int svc_sniff_burst(int channel, int ms, sniff_burst_t *out);

/* Frame counters are bumped from the WiFi-task callback, so a 32-bit load is
 * not atomic on ESP32. Read through these helpers. */
uint32_t svc_sniff_poll_total(void);

/* ---- beacon self-check ----
 * A beacon states its own beacon interval, so the air itself is the
 * reference: an AP that advertises 100 TU must be heard ~9.8 times a
 * second. Counting those beacons gives an EXPECTED rate with no second
 * receiver, and comparing it to the observed rate yields a capture ratio -
 * the honest answer to "is my counter dropping frames?".
 * Only 8 BSSIDs are tracked (96 bytes of .bss); no payload is retained. */
int      svc_sniff_beacon_aps(void);      /* distinct BSSIDs heard beaconing */
uint32_t svc_sniff_beacons(void);         /* total beacons counted */
uint32_t svc_sniff_beacon_rate(void);     /* beacons in the last 1 s window */
uint32_t svc_sniff_beacon_exp_hz_milli(void); /* expected beacons/s x1000 */
uint16_t svc_sniff_beacon_interval(void); /* first-seen interval, in TU */