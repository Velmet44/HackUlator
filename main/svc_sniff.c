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

    /* Park on the target's channel when we have one: a listener is only
     * useful on a band someone is actually using. Falls back to ch 1. */
    uint8_t ch = 1;
    wifi_second_chan_t sec;
    if (esp_wifi_get_channel(&ch, &sec) == ESP_OK && ch >= 1 && ch <= 13) {
        s_channel = ch;
    } else {
        esp_wifi_set_channel(1, WIFI_SECOND_CHAN_NONE);
        s_channel = 1;
    }

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