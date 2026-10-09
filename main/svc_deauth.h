#pragma once

/* Targeted 802.11 deauth flood on the stored WiFi target (own lab).
 * Mechanism from Hydra-ESP (wsl_bypasser / attack_method): hand-built
 * broadcast deauth frames via esp_wifi_80211_tx on WIFI_IF_STA, 100 ms
 * periodic loop, channel pinned to the target, PS off. Runs until
 * stopped (BACK / relock / idle / radio switch). 802.11w targets resist
 * plain injection - that's the mechanism, not a bug. */

#include <stdint.h>

/* Attack modes. 0xC = deauth (works pre-auth), 0xA = disassociation
 * (only meaningful to an already-associated client). Combined sends both
 * every tick, which catches stacks that honour one and ignore the other.
 * Beacon spam broadcasts fake AP beacons with random SSIDs/BSSIDs (clutter
 * for nearby scanners). Frame layouts follow Hydra-ESP's wsl_bypasser
 * templates. */
typedef enum {
    DEAUTH_MODE_DEAUTH = 0,   /* 0xC only, reason 0x02 */
    DEAUTH_MODE_DISASSOC = 1, /* 0xA only, reason 0x01 */
    DEAUTH_MODE_COMBINED = 2, /* 0xC + 0xA per tick */
    DEAUTH_MODE_BEACON = 3,   /* pool of fake-AP beacons, channel-hopped */
    DEAUTH_MODE_COUNT
} deauth_mode_t;

/* 1 = this mode needs a session target and must be verified before it
 * runs; 0 = it invents its own targets (beacon spam). */
int svc_deauth_mode_needs_target(deauth_mode_t m);

/* Beacon-spam SSID sources (Hydra-ESP's sets, plus ALL = mixed pool). */
typedef enum {
    BEACON_NAMES_COMMON = 0,  /* vendor-plausible: "Netgear_WiFi" */
    BEACON_NAMES_GARBAGE,      /* random printable junk */
    BEACON_NAMES_RICKROLL,     /* rickroll bait */
    BEACON_NAMES_SECURITY,     /* scam / scare names */
    BEACON_NAMES_ALL,          /* one of the above per pool entry */
    BEACON_NAMES_COUNT
} beacon_name_mode_t;

/* Select the name source. Rebuys the pool at the next start (or now, when
 * idle). Stored across runs. */
void svc_deauth_set_name_mode(beacon_name_mode_t m);
beacon_name_mode_t svc_deauth_name_mode(void);
const char *svc_deauth_name_mode_name(beacon_name_mode_t m);

/* Fake APs kept live (all beacon every tick). */
uint32_t svc_deauth_beacon_pool(void);

/* Start on the current session WiFi target (must be set).
 * Brings STA up + PS_NONE + channel pin; 100 ms esp_timer TX loop.
 * Auto-stops after DEAUTH_TIMEOUT_S (see svc_deauth_expired()).
 * 0 ok; <0 stage error (-1 no target, -2 wifi init, -3 channel, -6 tx). */
int svc_deauth_start(deauth_mode_t mode);

/* Stop the TX loop. Safe when already stopped. Must run before any
 * WiFi/BLE teardown so the timer never touches a dead radio. */
void svc_deauth_stop(void);

int svc_deauth_running(void);
uint32_t svc_deauth_frames(void); /* frames TXed since start */
uint32_t svc_deauth_ticks(void);  /* TX callback invocations (timer alive) */
/* Beacon-spam counters: frames sent (frames), fake AP identities rolled
 * this run (fake_aps) - a scanner only sees a name once its dwell window
 * finishes, so fake_aps lags frames by one identity. */
uint32_t svc_deauth_beacons(void);
uint32_t svc_deauth_fake_aps(void);
int svc_deauth_tx_error(void);    /* esp_err of first TX failure, 0 = none */
int svc_deauth_beacon_ok(void);   /* 1 if the driver accepts beacon frames */

/* Auto-stop: 1 once DEAUTH_TIMEOUT_S elapsed. The TX callback only sets
 * this flag (a timer must not delete itself from inside its own callback);
 * the owner polls it and calls svc_deauth_stop(). */
int svc_deauth_expired(void);
uint32_t svc_deauth_fps(void);      /* smoothed frames/second */
uint32_t svc_deauth_elapsed_s(void);
uint32_t svc_deauth_remaining_s(void); /* 0 once expired */
deauth_mode_t svc_deauth_mode(void);   /* mode currently running */
const char *svc_deauth_mode_name(deauth_mode_t m); /* short UI label */
#define DEAUTH_TIMEOUT_S 180         /* 3 minutes */