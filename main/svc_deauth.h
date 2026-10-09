#pragma once

/* Targeted 802.11 deauth flood on the stored WiFi target (own lab).
 * Mechanism from Hydra-ESP (wsl_bypasser / attack_method): hand-built
 * broadcast deauth frames via esp_wifi_80211_tx on WIFI_IF_STA, 100 ms
 * periodic loop, channel pinned to the target, PS off. Runs until
 * stopped (BACK / relock / idle / radio switch). 802.11w targets resist
 * plain injection - that's the mechanism, not a bug. */

#include <stdint.h>

/* Start on the current session WiFi target (must be set).
 * Brings STA up + PS_NONE + channel pin; 100 ms esp_timer TX loop.
 * Auto-stops after DEAUTH_TIMEOUT_S (see svc_deauth_expired()).
 * 0 ok; <0 stage error (-1 no target, -2 wifi init, -3 channel, -6 tx). */
int svc_deauth_start(void);

/* Stop the TX loop. Safe when already stopped. Must run before any
 * WiFi/BLE teardown so the timer never touches a dead radio. */
void svc_deauth_stop(void);

int svc_deauth_running(void);
uint32_t svc_deauth_frames(void); /* frames TXed since start */
uint32_t svc_deauth_ticks(void);  /* TX callback invocations (timer alive) */
int svc_deauth_tx_error(void);    /* esp_err of first TX failure, 0 = none */
int svc_deauth_beacon_ok(void);   /* 1 if the driver accepts beacon frames */

/* Auto-stop: 1 once DEAUTH_TIMEOUT_S elapsed. The TX callback only sets
 * this flag (a timer must not delete itself from inside its own callback);
 * the owner polls it and calls svc_deauth_stop(). */
int svc_deauth_expired(void);
uint32_t svc_deauth_fps(void);      /* smoothed frames/second */
uint32_t svc_deauth_elapsed_s(void);
uint32_t svc_deauth_remaining_s(void); /* 0 once expired */
#define DEAUTH_TIMEOUT_S 180         /* 3 minutes */