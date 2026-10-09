#include "svc_sniff.h"
#include "svc_wifi.h"
#include "esp_wifi.h"
#include "esp_wifi_types.h"
#include "esp_timer.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "svc_sniff";

/* Bumped by the WiFi-task callback, read by the UI task. */
static volatile uint32_t s_total;
static volatile uint32_t s_mgmt;
static volatile uint32_t s_data;
static volatile uint32_t s_other;
/* RSSI is always negative, so the "best so far" sentinel must start BELOW
 * any real reading (-128 dBm is the floor of the 8-bit signed field).
 * Starting at 0 would make every sample fail the `>` test forever. */
#define SNIFF_RSSI_NONE (-128)
static volatile int      s_best_rssi = SNIFF_RSSI_NONE;

static sniff_mode_t s_mode = SNIFF_OFF;
static uint8_t      s_filter[6];
static int          s_has_filter;
static int          s_channel = 1;

/* Rate window: frames counted in the current second. */
static volatile uint32_t s_win_frames;
static volatile uint32_t s_rate;
static int64_t s_win_us;

/* Beacon self-check. Each beacon states its own beacon interval, so the
 * expected rate follows from the air itself - no second receiver needed.
 * Stores only BSSID + interval + a count: never a payload. */
#define BEACON_TRACK_MAX 8
typedef struct {
    uint8_t  bssid[6];
    uint16_t interval_tu; /* 102.4 us units; 100 TU = ~9.77 beacons/s */
    uint32_t count;
} beacon_seen_t;

static beacon_seen_t s_bseen[BEACON_TRACK_MAX];
static int s_bseen_n;
static volatile uint32_t s_beacon_count;
static volatile uint32_t s_beacon_win;
static volatile uint32_t s_beacon_rate;
static volatile uint16_t s_beacon_interval;

/* Rate ticker, created on start and destroyed on stop. */
static esp_timer_handle_t s_rate_timer;

/* The callback runs on the WiFi task. IRAM_ATTR because that task can run
 * with flash cache issues during radio bring-up; it must not touch IRAM-
 * unsafe code, which is why it does nothing but bump counters. */
static void IRAM_ATTR rx_cb(void *buf, wifi_promiscuous_pkt_type_t type) {
    wifi_promiscuous_pkt_t *p = (wifi_promiscuous_pkt_t *)buf;

    /* Count the frame before anything else so the headline number tracks
     * real air traffic even when a filter drops it. */
    s_total++;

    switch (type) {
        case WIFI_PKT_MGMT:
            s_mgmt++;
            break;
        case WIFI_PKT_DATA:
            s_data++;
            break;
        default:
            s_other++;
            break;
    }

    /* rx_ctrl.rssi is signed; keep the strongest (closest to 0). */
    int rssi = (int)p->rx_ctrl.rssi;
    if (rssi > s_best_rssi) {
        s_best_rssi = rssi;
    }

    /* Tagged-frame filters need the MAC header. A valid 802.11 frame is at
     * least 24 bytes; anything shorter is a driver artefact, not air
     * traffic, and must not be indexed into. */
    if (p->rx_ctrl.sig_len < 24) {
        return;
    }

    /* Beacon self-check. In the FC byte the protocol version is bits 0-1,
     * TYPE is bits 2-3 and SUBTYPE is bits 4-7 - so a beacon is
     * type 0 / subtype 8, i.e. byte0 == 0x80. Masking the low nibble
     * instead matches 0x88 (QoS-Data) and counts data frames as beacons. */
    const uint8_t fc0 = p->payload[0];
    if ((fc0 & 0x0C) == 0x00 && ((fc0 >> 4) & 0x0F) == 0x08 &&
        p->rx_ctrl.sig_len >= 38) {
        const uint8_t *bs = p->payload + 16;
        uint16_t iv = (uint16_t)(p->payload[32] | (p->payload[33] << 8));
        s_beacon_count++;
        s_beacon_win++;
        /* Guard the interval: a real AP advertises tens to hundreds of TU.
         * Anything else means we mis-parsed the frame, and letting it into
         * the expected-rate maths would produce nonsense like 0.002/s. */
        if (iv >= 10 && iv <= 1000 && s_beacon_interval == 0) {
            s_beacon_interval = iv;
        }
        if (iv > 0) {
            s_beacon_interval = iv;
        }
        int slot = -1;
        for (int i = 0; i < s_bseen_n; i++) {
            if (memcmp(s_bseen[i].bssid, bs, 6) == 0) {
                slot = i;
                break;
            }
        }
        if (slot < 0 && s_bseen_n < BEACON_TRACK_MAX) {
            slot = s_bseen_n++;
            memcpy(s_bseen[slot].bssid, bs, 6);
            s_bseen[slot].count = 0;
            s_bseen[slot].interval_tu = (iv >= 10 && iv <= 1000) ? iv : 100;
        }
        if (slot >= 0) {
            s_bseen[slot].count++;
        }
    }

    switch (s_mode) {
        case SNIFF_TRAFFIC:
            if (type != WIFI_PKT_DATA) {
                break;
            }
            /* fall through to the BSSID match */
            if (s_has_filter) {
                const uint8_t *a = p->payload;
                if (memcmp(a + 4, s_filter, 6) != 0 &&
                    memcmp(a + 10, s_filter, 6) != 0 &&
                    memcmp(a + 16, s_filter, 6) != 0) {
                    break;
                }
            }
            s_win_frames++;
            break;
        case SNIFF_PROBE:
            if (type != WIFI_PKT_MGMT) {
                break;
            }
            s_win_frames++;
            break;
        case SNIFF_ALL:
            s_win_frames++;
            break;
        default:
            break;
    }
}

static void rate_tick(void *arg) {
    (void)arg;
    /* Snapshot the window and restart it. Called from the esp_timer task, so
     * the read-modify-write of s_win_frames can race the callback; a lost
     * frame in one second only shifts the rate slightly, which is fine for
     * a display. */
    int64_t now = esp_timer_get_time();
    if (s_win_us == 0) {
        s_win_us = now;
    }
    if (now - s_win_us >= 1000000) {
        s_rate = s_win_frames;
        s_win_frames = 0;
        /* Beacon rate is tracked separately from the all-management rate so
         * the self-check compares like with like: observed BEACONS against
         * expected BEACONS. Mixing in probes and auth would make the ratio
         * meaningless. */
        s_beacon_rate = s_beacon_win;
        s_beacon_win = 0;
        s_win_us = now;
    }
}

int svc_sniff_start(sniff_mode_t mode, const uint8_t *filter_bssid) {
    if (mode == SNIFF_OFF || mode > SNIFF_PROBE) {
        return -1;
    }
    if (svc_sniff_running()) {
        svc_sniff_stop();
    }
    if (svc_wifi_init() != 0) {
        return -1;
    }

    s_mode = mode;
    s_has_filter = 0;
    if (filter_bssid) {
        memcpy(s_filter, filter_bssid, 6);
        s_has_filter = 1;
    }

    s_total = 0;
    s_mgmt = 0;
    s_data = 0;
    s_other = 0;
    s_best_rssi = SNIFF_RSSI_NONE;
    s_win_frames = 0;
    s_rate = 0;
    s_win_us = esp_timer_get_time();
    s_bseen_n = 0;
    memset(s_bseen, 0, sizeof(s_bseen));
    s_beacon_count = 0;
    s_beacon_win = 0;
    s_beacon_rate = 0;
    s_beacon_interval = 0;

    if (esp_wifi_set_promiscuous(true) != ESP_OK) {
        s_mode = SNIFF_OFF;
        return -2;
    }

    wifi_promiscuous_filter_t f = {
        .filter_mask = WIFI_PROMIS_FILTER_MASK_MGMT | WIFI_PROMIS_FILTER_MASK_DATA,
    };
    if (esp_wifi_set_promiscuous_filter(&f) != ESP_OK) {
        esp_wifi_set_promiscuous(false);
        s_mode = SNIFF_OFF;
        return -3;
    }

    if (esp_wifi_set_promiscuous_rx_cb(rx_cb) != ESP_OK) {
        esp_wifi_set_promiscuous(false);
        s_mode = SNIFF_OFF;
        return -4;
    }

    /* Start on whatever channel the radio already sits on. NOTE this is the
     * ESP32 default (channel 1) when no scan has run yet, which is usually
     * an EMPTY channel - the counter will read near zero until the operator
     * hops UP/DOWN onto a band with traffic. That is a correct count of
     * nothing, not a fault. */
    uint8_t ch = 1;
    wifi_second_chan_t sec;
    if (esp_wifi_get_channel(&ch, &sec) != ESP_OK || ch < 1 || ch > 13) {
        ch = 1;
    }
    esp_wifi_set_channel(ch, WIFI_SECOND_CHAN_NONE);
    s_channel = ch;

    esp_timer_create_args_t a = {
        .callback = rate_tick,
        .name = "sniff_rate",
    };
    if (esp_timer_create(&a, &s_rate_timer) != ESP_OK || s_rate_timer == NULL) {
        s_rate_timer = NULL; /* monitor still counts, just without a rate */
        ESP_LOGW(TAG, "rate timer failed");
    } else if (esp_timer_start_periodic(s_rate_timer, 200000) != ESP_OK) {
        esp_timer_delete(s_rate_timer);
        s_rate_timer = NULL;
    }

    ESP_LOGI(TAG, "promiscuous RX on ch %d (mode %d)", s_channel, (int)mode);
    return 0;
}

void svc_sniff_stop(void) {
    if (s_mode == SNIFF_OFF) {
        return;
    }
    /* Order matters: stop the source of callbacks BEFORE taking the radio
     * down, or a frame in flight lands on a dead driver. */
    esp_wifi_set_promiscuous_rx_cb(NULL);
    esp_wifi_set_promiscuous(false);

    if (s_rate_timer) {
        esp_timer_stop(s_rate_timer);
        esp_timer_delete(s_rate_timer);
        s_rate_timer = NULL;
    }

    s_mode = SNIFF_OFF;
    s_has_filter = 0;
    ESP_LOGI(TAG, "RX monitor off, %u frames", (unsigned)s_total);
}

int svc_sniff_running(void) { return s_mode != SNIFF_OFF; }

sniff_mode_t svc_sniff_mode(void) { return s_mode; }

uint32_t svc_sniff_total(void) { return s_total; }

uint32_t svc_sniff_mgmt(void) { return s_mgmt; }

uint32_t svc_sniff_data(void) { return s_data; }

uint32_t svc_sniff_other(void) { return s_other; }

uint32_t svc_sniff_poll_total(void) { return s_total; }

uint32_t svc_sniff_rate(void) { return s_rate; }

int svc_sniff_best_rssi(void) { return s_best_rssi; }

int svc_sniff_have_rssi(void) { return s_best_rssi != SNIFF_RSSI_NONE; }

int svc_sniff_channel(void) { return s_channel; }

int svc_sniff_set_channel(int ch) {
    if (ch < 1 || ch > 13) {
        return -1;
    }
    if (!svc_sniff_running()) {
        return -2;
    }
    if (esp_wifi_set_channel((uint8_t)ch, WIFI_SECOND_CHAN_NONE) != ESP_OK) {
        return -3;
    }
    s_channel = ch;
    /* Reset so the displayed rate and total describe this one channel.
     * Carrying counts across a hop would make "total" meaningless. */
    s_total = 0;
    s_mgmt = 0;
    s_data = 0;
    s_other = 0;
    s_best_rssi = SNIFF_RSSI_NONE;
    s_win_frames = 0;
    s_rate = 0;
    s_win_us = esp_timer_get_time();
    s_bseen_n = 0;
    memset(s_bseen, 0, sizeof(s_bseen));
    s_beacon_count = 0;
    s_beacon_win = 0;
    s_beacon_rate = 0;
    s_beacon_interval = 0;
    ESP_LOGI(TAG, "hopped to ch %d", ch);
    return 0;
}

int svc_sniff_beacon_aps(void) { return s_bseen_n; }

uint32_t svc_sniff_beacons(void) { return s_beacon_count; }

uint32_t svc_sniff_beacon_rate(void) { return s_beacon_rate; }

uint16_t svc_sniff_beacon_interval(void) { return s_beacon_interval; }

/* Expected beacon rate from the advertised intervals, in milli-Hz.
 *
 * An 802.11 time unit is 1024 us (NOT 102.4 us - that is the OFDM symbol
 * duration), so an AP advertising 100 TU beacons every 102.4 ms, i.e.
 * ~9.77 beacons/s. Getting this factor wrong by 10x quietly turns a
 * perfect capture into a 10% one. */
uint32_t svc_sniff_beacon_exp_hz_milli(void) {
    uint64_t milli = 0;
    for (int i = 0; i < s_bseen_n; i++) {
        uint32_t iv = s_bseen[i].interval_tu;
        if (iv == 0) {
            iv = 100; /* unannounced: assume the near-universal default */
        }
        milli += 1000000000ULL / ((uint64_t)iv * 1024ULL);
    }
    return (uint32_t)milli;
}