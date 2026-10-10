#include "svc_ble.h"
#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_gap_ble_api.h"
#include "esp_log.h"
#include "esp_random.h"
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

/* ---- ADV flood state ----
 * Declared up here rather than with the flood code below because gap_cb()
 * touches s_flood_started and svc_ble_stop() touches s_flood_running, and
 * both appear earlier in this file.
 *
 * adv_int_min counts 0.625 ms units, NOT milliseconds - see the flood
 * section at the bottom for the full set of hardware limits.
 */
#define BLEADV_NAME_MAX 26
#define BLEADV_RAW_MAX  31
#define BLEADV_INTERVAL_UNITS 0x0020u   /* 20 ms -> 50 events/s */
#define BLEADV_INTERVAL_MS    20u

static int s_flood_running;
static volatile uint32_t s_flood_started;  /* real event count, 1 per run */
static int64_t s_flood_t0_us;
static int s_flood_start_err;
static char s_flood_name[BLEADV_NAME_MAX + 1];
static bleadv_name_mode_t s_name_mode = BLEADV_NAMES_COMMON;

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
    /* ADV lifecycle, before the scan-result early-out. This runs in the BT
     * callback context, so it ONLY counts and flags. It must never stop the
     * advertising or tear the controller down: esp_bluedroid_deinit() from
     * inside a GAP callback pulls the stack out from under the BT task, which
     * is the same class of fault as svc_deauth's orphan TX timer. The owner
     * polls svc_ble_flood_expired() and calls svc_ble_flood_stop().
     *
     * Bluedroid has NO per-advertising-event callback: these are the only two
     * advertising events that exist, and each fires once per run. That is
     * exactly why svc_ble_flood_est() has to be derived rather than counted. */
    if (event == ESP_GAP_BLE_ADV_DATA_SET_COMPLETE_EVT) {
        if (s_flood_running) {
            s_flood_started++; /* real confirmation the payload went on air */
        }
        return;
    }
    if (event == ESP_GAP_BLE_ADV_TERMINATED_EVT) {
        return;
    }
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
    /* Scanning and advertising share one controller and one radio. If a flood
     * is somehow still live (a screen that forgot to stop it), stand it down
     * rather than run both: a scanner that is also transmitting answers its
     * own SCAN_REQ and the scan results stop meaning anything. The UI already
     * guarantees this on every exit path; this is the backstop. */
    svc_ble_flood_stop();
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
    /* Advertising must be stopped explicitly: the controller keeps beaconing
     * after bluedroid is disabled otherwise, and a live ADV set is the BLE
     * equivalent of svc_deauth's orphan TX timer. Order matches the caller's
     * documented contract - stop the GAP activity, then unwind the stack. */
    s_flood_running = 0;
    esp_ble_gap_stop_advertising();
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
    svc_ble_flood_stop(); /* one radio: never listen and transmit at once */
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

/* ---- ADV flood ----
 *
 * Two hard limits shape this whole section, and both were read out of the
 * ESP-IDF headers rather than assumed:
 *
 *  1. esp_ble_adv_params_t.adv_int_min counts 0.625 ms units and its valid
 *     range starts at 0x0020, i.e. 20 ms. That caps a legal advertising event
 *     rate at 50/s. No config raises it.
 *  2. Classic ESP32's esp_bt.h has no ble_multi_adv_instances field (h2, h4,
 *     c2, c5 and c6 do). One advertising set, one identity. svc_deauth can
 *     run 20 fake APs because each is a separate esp_wifi_80211_tx() call;
 *     here the controller owns the cadence and reports no per-packet
 *     feedback at all.
 */

/* A legacy advertising payload is 31 bytes total. Flags cost 3 (length +
 * type + value) and a complete local name costs 2 + n, so 26 bytes of name
 * still fits beside the flags. Names are clamped rather than truncated
 * mid-element, because a name whose AD length byte disagrees with its payload
 * is malformed, not merely ugly. */

/* Name sources, mirroring svc_deauth's beacon wordlists. The name is chosen
 * once per start: with a single advertising set it cannot be rotated without
 * stopping and restarting, which costs far more than it buys. */
static const char *const s_names_common[] = {
    "HUAWEI Watch GT 3", "Fitbit Charge 5", "Galaxy Buds2", "AirPods Pro",
    "Tile Tracker", "Govee Sensor", "Sony WF-1000XM4", "Beats Studio Buds",
    "Yale Linus", "SwitchBot Meter", "Echo Dot", "Nest Mini",
};
static const char *const s_names_rickroll[] = {
    "Never Gonna Give You Up", "Together Forever", "Rickroll",
    "Never Gonna Let You Down",
};
static const char *const s_names_security[] = {
    "Free Public WiFi", "FBI Surveillance Van", "Update Your iPhone",
    "Microsoft Support", "IT Helpdesk", "Your Account Hacked",
};
#define NEL(a) (sizeof(a) / sizeof((a)[0]))

/* Pick a name for the active wordlist. GARBAGE is generated, so it never
 * repeats and costs no flash. */
static void flood_pick_name(char *out, size_t out_sz) {
    bleadv_name_mode_t m = s_name_mode;
    if (m == BLEADV_NAMES_ALL) {
        m = (bleadv_name_mode_t)(esp_random() % BLEADV_NAMES_COUNT);
    }
    if (m == BLEADV_NAMES_GARBAGE) {
        /* Printable but meaningless. 0x00 is excluded because it would
         * truncate the name the UI logs. */
        static const char alpha[] =
            "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_";
        size_t n = 4u + (size_t)(esp_random() % (BLEADV_NAME_MAX - 4u));
        if (n > out_sz - 1) {
            n = out_sz - 1;
        }
        for (size_t i = 0; i < n; i++) {
            out[i] = alpha[esp_random() % (sizeof(alpha) - 1)];
        }
        out[n] = 0;
        return;
    }
    const char *const *tbl;
    size_t n;
    if (m == BLEADV_NAMES_RICKROLL) {
        tbl = s_names_rickroll;
        n = NEL(s_names_rickroll);
    } else if (m == BLEADV_NAMES_SECURITY) {
        tbl = s_names_security;
        n = NEL(s_names_security);
    } else {
        tbl = s_names_common;
        n = NEL(s_names_common);
    }
    snprintf(out, out_sz, "%s", tbl[esp_random() % n]);
}

/* Build the raw payload: flags + one complete local name. Returns the byte
 * length, or <0 if the name would not fit. */
static int flood_build_raw(uint8_t *raw, size_t raw_sz) {
    char name[BLEADV_NAME_MAX + 1];
    flood_pick_name(name, sizeof(name));
    size_t nl = strlen(name);
    if (nl == 0 || nl > BLEADV_NAME_MAX) {
        return -1;
    }
    if (raw_sz < nl + 5u) {
        return -1;
    }
    size_t i = 0;
    /* LE General Discoverable (0x02) | BR/EDR not supported (0x04). Without
     * 0x04 some scanners treat the phantom as a classic device. */
    raw[i++] = 2;
    raw[i++] = ESP_BLE_AD_TYPE_FLAG;
    raw[i++] = 0x06;
    raw[i++] = (uint8_t)(nl + 1); /* AD length counts type + payload */
    raw[i++] = ESP_BLE_AD_TYPE_NAME_CMPL;
    memcpy(raw + i, name, nl);
    i += nl;
    snprintf(s_flood_name, sizeof(s_flood_name), "%s", name);
    return (int)i;
}

int svc_ble_flood_start(void) {
    int up = ble_up();
    if (up != 0) {
        return up; /* -10..-15: see ble_up */
    }
    /* The flood and the passive monitor both claim the controller; only one
     * may hold it. Starting the flood stands the monitor down. */
    svc_ble_passive_stop();

    uint8_t raw[BLEADV_RAW_MAX];
    int n = flood_build_raw(raw, sizeof(raw));
    if (n < 0) {
        s_flood_start_err = -17;
        return s_flood_start_err;
    }
    if (esp_ble_gap_config_adv_data_raw(raw, (uint32_t)n) != ESP_OK) {
        s_flood_start_err = -17;
        return s_flood_start_err;
    }

    /* NONCONN_IND, not ADV_TYPE_IND: a connectable advertisement makes the
     * controller honour SCAN_REQ and accept connections, spending airtime on
     * replies and inviting state the flood does not want. Pure broadcast is
     * strictly more adverts per second for the same air budget. */
    esp_ble_adv_params_t p = {
        .adv_int_min        = BLEADV_INTERVAL_UNITS,
        .adv_int_max        = BLEADV_INTERVAL_UNITS,
        .adv_type           = ADV_TYPE_NONCONN_IND,
        .own_addr_type      = BLE_ADDR_TYPE_PUBLIC,
        .peer_addr          = {0},
        .peer_addr_type     = BLE_ADDR_TYPE_PUBLIC,
        .channel_map        = ADV_CHNL_ALL, /* 37 + 38 + 39 */
        .adv_filter_policy  = ADV_FILTER_ALLOW_SCAN_ANY_CON_ANY,
    };
    if (esp_ble_gap_start_advertising(&p) != ESP_OK) {
        s_flood_start_err = -18;
        return s_flood_start_err;
    }
    s_flood_running = 1;
    s_flood_started = 0;
    s_flood_start_err = 0;
    s_flood_t0_us = esp_timer_get_time();
    ESP_LOGI(TAG, "adv flood up: \"%s\" at %u/s",
             s_flood_name, (unsigned)svc_ble_flood_rate());
    return 0;
}

void svc_ble_flood_stop(void) {
    if (!s_flood_running) {
        return;
    }
    /* Only the owner calls this (main loop / ui_bleatk_tick), never from the
     * GAP callback - see the svc_ble_flood_expired() note in the header. */
    esp_ble_gap_stop_advertising();
    s_flood_running = 0;
    ESP_LOGI(TAG, "adv flood off after %u s",
             (unsigned)svc_ble_flood_elapsed_s());
}

int svc_ble_flood_running(void) { return s_flood_running; }

const char *svc_ble_flood_name(void) { return s_flood_name; }

uint32_t svc_ble_flood_started(void) { return s_flood_started; }

uint32_t svc_ble_flood_rate(void) { return 1000u / BLEADV_INTERVAL_MS; }

/* DERIVED, not measured. See the header: Bluedroid reports no per-packet
 * advertising feedback, so this is elapsed / interval. */
uint32_t svc_ble_flood_est(void) {
    if (!s_flood_running) {
        return 0;
    }
    int64_t ms = (esp_timer_get_time() - s_flood_t0_us) / 1000;
    if (ms < 0) {
        return 0;
    }
    return (uint32_t)ms / BLEADV_INTERVAL_MS;
}

uint32_t svc_ble_flood_elapsed_s(void) {
    return (uint32_t)((esp_timer_get_time() - s_flood_t0_us) / 1000000);
}

uint32_t svc_ble_flood_remaining_s(void) {
    if (svc_ble_flood_expired()) {
        return 0;
    }
    uint32_t e = svc_ble_flood_elapsed_s();
    return e >= BLEADV_TIMEOUT_S ? 0 : (BLEADV_TIMEOUT_S - e);
}

int svc_ble_flood_expired(void) {
    return s_flood_running && svc_ble_flood_elapsed_s() >= BLEADV_TIMEOUT_S;
}

int svc_ble_flood_start_error(void) { return s_flood_start_err; }

void svc_ble_flood_set_name_mode(bleadv_name_mode_t m) {
    if ((int)m < 0 || m >= BLEADV_NAMES_COUNT) {
        m = BLEADV_NAMES_COMMON;
    }
    s_name_mode = m;
}

bleadv_name_mode_t svc_ble_flood_name_mode(void) { return s_name_mode; }

const char *svc_ble_flood_name_mode_name(bleadv_name_mode_t m) {
    switch (m) {
    case BLEADV_NAMES_COMMON:  return "COMMON";
    case BLEADV_NAMES_GARBAGE: return "GARBAGE";
    case BLEADV_NAMES_RICKROLL: return "RICKROLL";
    case BLEADV_NAMES_SECURITY: return "SECURITY";
    case BLEADV_NAMES_ALL:     return "ALL";
    default:                   return "COMMON";
    }
}
