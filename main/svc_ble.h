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
} ble_dev_t;

/* Blocking GAP scan (about `seconds`). Returns dev count, or negative
 * stage code: -1 args, -3 scan_start, -10..-15 bring-up (see ble_up). */
int svc_ble_scan(ble_dev_t *out, int max, int seconds);

/* Full radio teardown. Safe to call when already stopped. */
void svc_ble_stop(void);
