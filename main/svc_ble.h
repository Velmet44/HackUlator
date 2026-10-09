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
