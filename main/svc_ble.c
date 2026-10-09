#include "svc_ble.h"
#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_gap_ble_api.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include <string.h>
#include <stddef.h>

static const char *TAG = "svc_ble";
static int s_enabled = 0;

static ble_dev_t *s_out;
static int s_max, s_n;
static char s_addrs[BLE_MAX_DEV][18]; /* MAC per entry, for dedupe */

/* Passive mode: count adverts instead of filling a list. Set by
 * svc_ble_passive_start(); when on, gap_cb does no dedupe work and writes
 * no records, because nothing is going to read them. */
static volatile int s_passive;
static volatile uint32_t s_adv_total;
static volatile uint32_t s_adv_win;
static volatile uint32_t s_adv_rate;
/* RSSI is always negative, so "best so far" must start BELOW any real
 * reading. Starting at 0 makes every sample fail the `>` test forever and
 * the UI then claims nothing is in range while counting adverts. */
#define BLE_RSSI_NONE (-128)
static int s_adv_best_rssi;
static int64_t s_adv_win_us;

/* Look up one AD type in the adv payload, then the scan-response payload. */
static uint8_t *adv_lookup(esp_ble_gap_cb_param_t *p, int type, uint8_t *len) {
    uint8_t *v = esp_ble_resolve_adv_data(p->scan_rst.ble_adv, type, len);
    if ((!v || !*len) && p->scan_rst.scan_rsp_len)
        v = esp_ble_resolve_adv_data(
            p->scan_rst.ble_adv + p->scan_rst.adv_data_len, type, len);
    return (*len) ? v : NULL;
}

static void fill_opt(ble_dev_t *dv, esp_ble_gap_cb_param_t *p) {
    uint8_t n = 0;
    uint8_t *v;
    if (!dv->has_tx &&
        (v = adv_lookup(p, ESP_BLE_AD_TYPE_TX_PWR, &n)) && n >= 1) {
        dv->tx_pwr = (int8_t)v[0];
        dv->has_tx = 1;
    }
    if (!dv->has_flags &&
        (v = adv_lookup(p, ESP_BLE_AD_TYPE_FLAG, &n)) && n >= 1) {
        dv->flags = v[0];
        dv->has_flags = 1;
    }
    if (!dv->has_svc &&
        ((v = adv_lookup(p, ESP_BLE_AD_TYPE_16SRV_CMPL, &n)) ||
         (v = adv_lookup(p, ESP_BLE_AD_TYPE_16SRV_PART, &n))) && n >= 2) {
        dv->svc16 = (uint16_t)(v[0] | (v[1] << 8));
        dv->has_svc = 1;
    }
    if (!dv->has_mfr &&
        (v = adv_lookup(p, ESP_BLE_AD_MANUFACTURER_SPECIFIC_TYPE, &n)) &&
        n >= 2) {
        dv->mfr = (uint16_t)(v[0] | (v[1] << 8));
        dv->has_mfr = 1;
    }
}

static void fill_name(ble_dev_t *dv, esp_ble_gap_cb_param_t *p) {
    uint8_t nlen = 0;
    uint8_t *nm = adv_lookup(p, ESP_BLE_AD_TYPE_NAME_CMPL, &nlen);
    if (!nm || !nlen)
        nm = adv_lookup(p, ESP_BLE_AD_TYPE_NAME_SHORT, &nlen);
    if (nm && nlen) {
        size_t cn = nlen > sizeof(dv->name) - 1 ? sizeof(dv->name) - 1 : nlen;
        memcpy(dv->name, nm, cn);
        dv->name[cn] = 0;
    }
}

static void gap_cb(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *p) {
    if (event != ESP_GAP_BLE_SCAN_RESULT_EVT)
        return;
    if (p->scan_rst.search_evt != ESP_GAP_SEARCH_INQ_RES_EVT)
        return;

    /* Passive monitor: count and return before any list bookkeeping. Runs
     * in the BT callback context, so it stays arithmetic-only. */
    if (s_passive) {
        s_adv_total++;
        s_adv_win++;
        if ((int)p->scan_rst.rssi > s_adv_best_rssi)
            s_adv_best_rssi = (int)p->scan_rst.rssi;
        return;
    }

    if (!s_out || s_n >= s_max)
        return;

    char addr[18];
    snprintf(addr, sizeof(addr), "%02X:%02X:%02X:%02X:%02X:%02X",
             p->scan_rst.bda[0], p->scan_rst.bda[1], p->scan_rst.bda[2],
             p->scan_rst.bda[3], p->scan_rst.bda[4], p->scan_rst.bda[5]);

    /* Seen before: refresh name (first sighting may be nameless), fill any
     * optional fields missed earlier, keep the strongest RSSI. */
    for (int i = 0; i < s_n; i++) {
        if (!strcmp(s_addrs[i], addr)) {
            if (!strcmp(s_out[i].name, s_addrs[i]))
                fill_name(&s_out[i], p);
            fill_opt(&s_out[i], p);
            if (p->scan_rst.rssi > s_out[i].rssi)
                s_out[i].rssi = p->scan_rst.rssi;
            s_out[i].adv++;   /* activity sample for the detail view */
            return;
        }
    }
    /* New device. */
    if (s_n >= s_max)
        return;
    memset(&s_out[s_n], 0, sizeof(s_out[s_n]));
    memcpy(s_addrs[s_n], addr, sizeof(addr));
    memcpy(s_out[s_n].mac, addr, sizeof(addr));
    memcpy(s_out[s_n].name, addr, sizeof(addr));
    fill_name(&s_out[s_n], p);
    fill_opt(&s_out[s_n], p);
    s_out[s_n].addr_type = p->scan_rst.ble_addr_type;
    s_out[s_n].evt_type = p->scan_rst.ble_evt_type;
    s_out[s_n].rssi = p->scan_rst.rssi;
    s_out[s_n].adv = 1;
    s_n++;
}

/* Bring up controller + Bluedroid. 0 ok, negative stage on failure:
 * -10 controller_init, -11 controller_enable, -12 bluedroid_init,
 * -13 bluedroid_enable, -14 gap_cb, -15 scan_params. */
static int ble_up(void) {
    if (s_enabled)
        return 0;
    esp_bt_controller_config_t cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    if (esp_bt_controller_init(&cfg) != ESP_OK)
        return -10;
    if (esp_bt_controller_enable(ESP_BT_MODE_BLE) != ESP_OK) {
        esp_bt_controller_deinit();
        return -11;
    }
    if (esp_bluedroid_init() != ESP_OK) {
        esp_bt_controller_disable();
        esp_bt_controller_deinit();
        return -12;
    }
    if (esp_bluedroid_enable() != ESP_OK) {
        esp_bt_controller_disable();
        esp_bt_controller_deinit();
        return -13;
    }
    if (esp_ble_gap_register_callback(gap_cb) != ESP_OK)
        return -14;
    /* NB: non-const (IDF >=5.4 takes esp_ble_scan_params_t *). The API
     * only reads it; static so it isn't stack garbage if GAP holds it. */
    static esp_ble_scan_params_t params = {
        .scan_type              = BLE_SCAN_TYPE_ACTIVE,
        .own_addr_type          = BLE_ADDR_TYPE_PUBLIC,
        .scan_filter_policy     = BLE_SCAN_FILTER_ALLOW_ALL,
        .scan_interval          = 0x50,
        .scan_window            = 0x30,
        .scan_duplicate         = BLE_SCAN_DUPLICATE_DISABLE,
    };
    if (esp_ble_gap_set_scan_params(&params) != ESP_OK)
        return -15;
    s_enabled = 1;
    ESP_LOGI(TAG, "bluedroid up");
    return 0;
}

int svc_ble_scan(ble_dev_t *out, int max, int seconds) {
    if (!out || max <= 0 || max > BLE_MAX_DEV || seconds <= 0)
        return -1;
    int up = ble_up();
    if (up != 0)
        return up; /* -10..-15: see ble_up */
    s_out = out;
    s_max = max;
    s_n = 0;
    if (esp_ble_gap_start_scanning((uint32_t)seconds) != ESP_OK)
        return -3;
    vTaskDelay(pdMS_TO_TICKS(seconds * 1000 + 800)); /* collect + settle */
    esp_ble_gap_stop_scanning();
    ESP_LOGI(TAG, "scan done: %d devs", s_n);
    s_out = NULL;
    return s_n;
}

void svc_ble_stop(void) {
    if (!s_enabled)
        return;
    esp_ble_gap_stop_scanning();
    esp_bluedroid_disable();
    esp_bluedroid_deinit();
    esp_bt_controller_disable();
    esp_bt_controller_deinit();
    s_enabled = 0;
    s_passive = 0;
    ESP_LOGI(TAG, "radio off");
}

/* ---- passive advert monitor ---- */

int svc_ble_passive_start(void) {
    if (s_passive)
        return 0;
    /* A passive scan must not send SCAN_REQ or SCAN_RSP: scanning actively
     * would make the radio reply, which is the opposite of listening. The
     * own address is random so a passive monitor is not identifiable. */
    int up = ble_up();
    if (up != 0)
        return up;
    esp_ble_scan_params_t p = {
        .scan_type              = BLE_SCAN_TYPE_PASSIVE,
        .own_addr_type          = BLE_ADDR_TYPE_RANDOM,
        .scan_filter_policy     = BLE_SCAN_FILTER_ALLOW_ALL,
        .scan_interval          = 0x50,
        .scan_window            = 0x30,
        .scan_duplicate         = BLE_SCAN_DUPLICATE_DISABLE,
    };
    if (esp_ble_gap_set_scan_params(&p) != ESP_OK)
        return -16;
    s_adv_total = 0;
    s_adv_win = 0;
    s_adv_rate = 0;
    s_adv_best_rssi = BLE_RSSI_NONE;
    s_adv_win_us = esp_timer_get_time();
    s_passive = 1;
    if (esp_ble_gap_start_scanning(0) != ESP_OK) { /* 0 = scan until stopped */
        s_passive = 0;
        return -3;
    }
    ESP_LOGI(TAG, "passive advert monitor up");
    return 0;
}

void svc_ble_passive_stop(void) {
    if (!s_passive)
        return;
    esp_ble_gap_stop_scanning();
    s_passive = 0;
    ESP_LOGI(TAG, "passive monitor off, %u adverts",
             (unsigned)s_adv_total);
}

int svc_ble_passive_running(void) { return s_passive; }

uint32_t svc_ble_adv_total(void) { return s_adv_total; }

/* Called from the UI tick so the window flip does not need a timer task. */
uint32_t svc_ble_adv_tick(void) {
    if (!s_passive)
        return s_adv_rate;
    int64_t now = esp_timer_get_time();
    if (now - s_adv_win_us >= 1000000) {
        s_adv_rate = s_adv_win;
        s_adv_win = 0;
        s_adv_win_us = now;
    }
    return s_adv_rate;
}

int svc_ble_adv_best_rssi(void) { return s_adv_best_rssi; }
