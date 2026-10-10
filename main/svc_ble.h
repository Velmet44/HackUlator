#pragma once

/* Minimal BLE service: lazy Bluedroid init, one-shot GAP scan, full
 * teardown on stop (frees RAM + kills the radio). */

#include <stdint.h>

#define BLE_MAX_DEV 32

typedef struct {
    char name[24]; /* adv name (complete, else short), or MAC fallback */
    char mac[18];  /* AA:BB:CC:DD:EE:FF */
    int  rssi;
    int  addr_type; /* esp_ble_addr_type_t: public/random/RPA-... */
    int  evt_type;  /* esp_ble_evt_type_t: ADV_IND/SCAN_RSP/... */
    int  tx_pwr;    /* dBm, valid if has_tx */
    uint8_t has_tx;
    uint8_t flags;  /* adv flags byte, valid if has_flags */
    uint8_t has_flags;
    uint16_t svc16; /* first 16-bit service UUID, valid if has_svc */
    uint8_t has_svc;
    uint16_t mfr;   /* manufacturer company id, valid if has_mfr */
    uint8_t has_mfr;
    uint32_t adv;   /* advert events seen from this MAC during the scan */
} ble_dev_t;

/* Blocking GAP scan (about `seconds`). Returns dev count, or negative
 * stage code: -1 args, -3 scan_start, -10..-15 bring-up (see ble_up). */
int svc_ble_scan(ble_dev_t *out, int max, int seconds);

/* Full radio teardown. Safe to call when already stopped. */
void svc_ble_stop(void);

/* ---- passive advert monitor ----
 * Listens without answering: BLE_SCAN_TYPE_PASSIVE sends no SCAN_REQ, and a
 * random own address keeps the monitor itself unidentifiable. Counters only
 * - no dedupe, no list, no records retained.
 * 0 ok; <0 stage code (-10..-15 bring-up, -16 scan params, -3 start). */
int  svc_ble_passive_start(void);
void svc_ble_passive_stop(void);
int  svc_ble_passive_running(void);
uint32_t svc_ble_adv_total(void);   /* adverts since start */
uint32_t svc_ble_adv_tick(void);    /* call ~1/s; returns adverts in last 1 s */
int  svc_ble_adv_best_rssi(void);   /* strongest dBm, 0 when nothing heard */

/* ---- ADV flood ----
 * Advertising-only load generator (own lab). Brings the radio up and
 * broadcasts one non-connectable advertising identity at the fastest rate
 * the BLE specification allows, to occupy nearby scanners with adverts they
 * must parse and discard.
 *
 * Hard ceiling, and it is a SPEC ceiling, not a configuration choice:
 * esp_ble_adv_params_t.adv_int_min is a count of 0.625 ms units whose valid
 * range starts at 0x0020. 0x0020 * 0.625 ms = 20 ms, so at most 50
 * advertising events per second are legal. Classic ESP32 also exposes NO
 * multi-advertising field (ble_multi_adv_instances exists only on h2/h4/
 * c2/c5/c6), so there is exactly ONE advertising set: this is a RATE flood
 * with a single fixed identity, unlike svc_deauth's 20-AP WiFi pool.
 *
 * Because the identity cannot be rotated without stopping and restarting the
 * advertising (which costs far more than it buys), the name is chosen once at
 * svc_ble_flood_start() and then held for the whole run.
 */

typedef enum {
    BLEADV_NAMES_COMMON = 0,  /* vendor-plausible device names */
    BLEADV_NAMES_GARBAGE,     /* random printable junk */
    BLEADV_NAMES_RICKROLL,    /* bait names */
    BLEADV_NAMES_SECURITY,    /* scam / scare names */
    BLEADV_NAMES_ALL,         /* one of the above, chosen per start */
    BLEADV_NAMES_COUNT
} bleadv_name_mode_t;

/* Select the name source. Rebuys the payload at the next start. Stored
 * across runs. Out-of-range values clamp to COMMON. */
void svc_ble_flood_set_name_mode(bleadv_name_mode_t m);
bleadv_name_mode_t svc_ble_flood_name_mode(void);
const char *svc_ble_flood_name_mode_name(bleadv_name_mode_t m);

/* 0 ok; <0 stage error (-1 bad mode, -10..-15 bring-up, -17 adv data,
 * -18 start). Self-targeting: no session BLE target is required, the same
 * rule beacon spam follows on the WiFi side. */
int svc_ble_flood_start(void);

/* Stop advertising. Must run before any radio teardown - see the note on
 * svc_ble_flood_expired(). Safe when already stopped. */
void svc_ble_flood_stop(void);

int svc_ble_flood_running(void);
const char *svc_ble_flood_name(void); /* name in the live payload */

/* MEASURED: how many times Bluedroid confirmed the advertising payload was
 * accepted and went on air (ESP_GAP_BLE_ADV_DATA_SET_COMPLETE_EVT). This is
 * real stack feedback and is 1 for a normal run - Bluedroid has no
 * per-advertising-event callback, so it is emphatically NOT a packet count.
 * It is the honest "the radio really started" signal. */
uint32_t svc_ble_flood_started(void);

/* DERIVED, not measured. Bluedroid reports no per-packet advertising
 * feedback, so this is elapsed / the configured interval: an estimate of how
 * many events the controller has been asked to send. Never present it as a
 * capture or a proof of effect. */
uint32_t svc_ble_flood_est(void);

/* Configured events per second (1000 / interval). Derived from config. */
uint32_t svc_ble_flood_rate(void);
uint32_t svc_ble_flood_elapsed_s(void);
uint32_t svc_ble_flood_remaining_s(void); /* 0 once expired */
int      svc_ble_flood_expired(void);     /* flag only; owner must stop */

/* esp_err from the start attempt, 0 if none. */
int svc_ble_flood_start_error(void);

#define BLEADV_TIMEOUT_S 180             /* auto-stop, mirrors DEAUTH_TIMEOUT_S */
