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
 * 0 ok; <0 stage error (-1 no target, -2 wifi init, -3 channel). */
int svc_deauth_start(void);

/* Stop the TX loop. Safe when already stopped. Must run before any
 * WiFi/BLE teardown so the timer never touches a dead radio. */
void svc_deauth_stop(void);

int svc_deauth_running(void);
uint32_t svc_deauth_frames(void); /* frames TXed since start */
int svc_deauth_tx_error(void);    /* esp_err of first TX failure, 0 = none */