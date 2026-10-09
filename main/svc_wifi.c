#include "svc_wifi.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_err.h"
#include <string.h>

static const char *TAG = "svc_wifi";
static int s_started = 0;
static int s_inited = 0;

/* NOTE: deliberately no esp_netif here. This device only ever scans —
 * no station connection, no DHCP, no sockets — so lwIP/TCP-IP (~15-25 KB
 * permanently resident once esp_netif_init runs) is dead weight that breaks
 * the later BT bring-up on this RAM-tight chip. WiFi events come through
 * esp_event; scan results come through the WiFi driver API. */
int svc_wifi_init(void) {
    if (s_inited)
        goto start;
    if (esp_event_loop_create_default() != ESP_OK &&
        esp_event_loop_create_default() != ESP_ERR_INVALID_STATE) {
        /* already exists is fine */
    }
    {
        wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
        if (esp_wifi_init(&cfg) != ESP_OK)
            return -1;
    }
    s_inited = 1;
start:
    if (!s_started) {
        esp_wifi_set_mode(WIFI_MODE_STA);
        if (esp_wifi_start() != ESP_OK)
            return -1;
        s_started = 1;
        ESP_LOGI(TAG, "STA started");
    }
    return 0;
}

static int cmp_ap(const void *a, const void *b) {
    return ((const wifi_ap_t *)b)->rssi - ((const wifi_ap_t *)a)->rssi;
}

#include <stdlib.h>

int svc_wifi_scan(wifi_ap_t *out, int max) {
    if (!out || max <= 0)
        return -1;
    if (svc_wifi_init() != 0)
        return -2;

    wifi_scan_config_t cfg = {
        .ssid = NULL,
        .bssid = NULL,
        .channel = 0, /* all channels */
        .show_hidden = true,
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
        .scan_time = { .active = { .min = 100, .max = 250 } },
    };
    if (esp_wifi_scan_start(&cfg, true) != ESP_OK)
        return -3;

    uint16_t n = 0;
    if (esp_wifi_scan_get_ap_num(&n) != ESP_OK)
        return -4;
    if (n > (uint16_t)max)
        n = (uint16_t)max;

    /* Cap the record array: the driver mallocs its own contiguous block
     * internally, so a huge request here just fragments the heap. */
    if (n > 8)
        n = 8;
    ESP_LOGI(TAG, "scan: %u aps, rec %u B, largest %u",
             (unsigned)n, (unsigned)(sizeof(wifi_ap_record_t) * n),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
    wifi_ap_record_t *rec = malloc(sizeof(wifi_ap_record_t) * n);
    if (!rec) {
        ESP_LOGW(TAG, "rec malloc failed (%u B, largest %u)",
                 (unsigned)(sizeof(wifi_ap_record_t) * n),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
        return -5;
    }
    uint16_t got = n;
    int rc = -1;
    esp_err_t err = esp_wifi_scan_get_ap_records(&got, rec);
    if (err != ESP_OK)
        ESP_LOGW(TAG, "get_ap_records failed: %s", esp_err_to_name(err));
    if (err == ESP_OK) {
        for (int i = 0; i < got && i < max; i++) {
            size_t sl = strnlen((const char *)rec[i].ssid, 32);
            memcpy(out[i].ssid, rec[i].ssid, sl);
            out[i].ssid[sl] = 0;
            memcpy(out[i].bssid, rec[i].bssid, 6);
            out[i].rssi = rec[i].rssi;
            out[i].channel = rec[i].primary;
            out[i].second = rec[i].second;
            out[i].auth = rec[i].authmode;
            out[i].pairwise = rec[i].pairwise_cipher;
            out[i].group = rec[i].group_cipher;
            out[i].phy_b = rec[i].phy_11b ? 1 : 0;
            out[i].phy_g = rec[i].phy_11g ? 1 : 0;
            out[i].phy_n = rec[i].phy_11n ? 1 : 0;
            out[i].phy_lr = rec[i].phy_lr ? 1 : 0;
            out[i].phy_ax = rec[i].phy_11ax ? 1 : 0;
            out[i].wps = rec[i].wps ? 1 : 0;
            out[i].ftm_r = rec[i].ftm_responder ? 1 : 0;
            out[i].ftm_i = rec[i].ftm_initiator ? 1 : 0;
            out[i].country[0] = rec[i].country.cc[0];
            out[i].country[1] = rec[i].country.cc[1];
            out[i].country[2] = 0;
            out[i].country[3] = 0;
        }
        qsort(out, got, sizeof(wifi_ap_t), cmp_ap);
        rc = got;
    }
    free(rec);
    ESP_LOGI(TAG, "scan done: %d APs", rc);
    return rc;
}

void svc_wifi_stop(void) {
    if (s_started) {
        esp_wifi_stop();
        s_started = 0;
        ESP_LOGI(TAG, "radio off");
    }
}

void svc_wifi_teardown(void) {
    svc_wifi_stop();
    if (s_inited) {
        esp_wifi_deinit();
        s_inited = 0;
        ESP_LOGI(TAG, "driver deinit, heap freed");
    }
}
