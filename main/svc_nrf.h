#pragma once

/* 2.4 GHz spectrum analyser on an nRF24L01+, on SPI3 (VSPI).
 *
 * HOW THE MEASUREMENT WORKS, and why it is the only one available:
 * the nRF24L01+ has no RSSI register. The only power measurement it exposes
 * is RPD, a SINGLE BIT comparator in register 0x09 bit 0 that goes high when
 * the in-band power exceeds a fixed threshold. So a "level" here is a HIT
 * RATE over repeated samples, not a decibel reading, and the UI must say so
 * - presenting it as dBm would be inventing a number the chip never produced.
 *
 * THE TRAP THAT MAKES THE SAMPLE COUNT WORTH ANYTHING: RPD is LATCHED. The
 * datasheet is explicit - the status is latched when a packet arrives or
 * when the host pulls CE low. Reading it N times inside one RX period returns
 * the SAME bit N times, so the common "startListening() then loop N reads"
 * shape yields 0 or N and no gradation whatsoever. This driver re-enters RX
 * (CE low -> high) before every sample, waiting Tstby2a + Tdelay_AGC
 * (130 + 40 us) for the AGC to settle, which is what turns one bit into
 * N+1 usable levels. That is what costs ~190 us per sample.
 *
 * Threshold, datasheet 6.4: -64 dBm at 1/2 Mbps, -83 dBm at 250 kbps. The
 * rate is therefore not just a link parameter, it is the measurement
 * BANDWIDTH and it decides what the display is allowed to claim - see
 * nrf_rate_t. The rate is always shown on screen.
 *
 * Sequential, never concurrent: the ESP32's own Wi-Fi and BT are in the same
 * band and would light every channel of the trace. svc_nrf_start() is the
 * caller's contract to have torn them down first.
 *
 * Sweeps are INCREMENTAL (svc_nrf_step()) rather than one blocking pass.
 * A full 125-channel sweep at 8 samples is ~150 ms of solid SPI and busy
 * wait; doing that in one call would stall the key loop for the whole sweep.
 * Stepping a slice per tick keeps input responsive and, as a side effect,
 * gives the UI a live cursor showing where the sweep actually is.
 */

#include <stdint.h>

/* nRF24L01+ channels 0..124 = 2400..2524 MHz at 1 MHz spacing.
 * 125 is left out deliberately: it is 2525 MHz, above every Wi-Fi channel
 * (incl. ch14 at 2484) and above the BLE advertising bands, so it only ever
 * adds a column of noise to the plot. */
#define NRF_CH_COUNT  125
#define NRF_CH0_MHZ   2400

/* Receiver measurement bandwidth.
 *
 *   NRF_RATE_250K  narrowest, ~19 dB more sensitive (RPD trips at -83 dBm),
 *                  but sees only 250 kHz of a 2 MHz channel, so a Wi-Fi
 *                  signal's energy mostly falls outside the filter
 *   NRF_RATE_1M    the default: sees 1 MHz, the usual compromise
 *   NRF_RATE_2M    sees the whole 2 MHz channel, best for finding wideband
 *                  emitters, least sensitive
 *
 * It changes what the trace MEANS, so it is user-selectable and always
 * displayed. Do not change it silently and keep calling the result the
 * same thing. */
typedef enum {
    NRF_RATE_250K = 0,
    NRF_RATE_1M,
    NRF_RATE_2M,
    NRF_RATE_COUNT
} nrf_rate_t;

/* Bring up SPI3 + the CE/CSN pins and probe for the radio.
 * 0 = ok, <0 = fail (see the codes below). Refuses on low heap: the SPI
 * driver needs a contiguous internal DMA block, which this device does not
 * always have left once Wi-Fi and BT have been cycled. */
#define NRF_ERR_SPI   -1   /* bus/device init failed */
#define NRF_ERR_HEAP  -2   /* no room for the SPI DMA block */
#define NRF_ERR_ABSENT -3  /* bus works, radio does not answer */
int svc_nrf_init(void);

/* 1 if the last probe found a radio answering on the bus. */
int svc_nrf_present(void);

/* Power the radio down and release the SPI device. Safe when not started.
 * Must run before any Wi-Fi/BT bring-up, for the same reason svc_deauth_stop()
 * must: this driver owns the bus and the CE/CSN pins. */
void svc_nrf_stop(void);

void   svc_nrf_set_rate(nrf_rate_t r);
nrf_rate_t svc_nrf_rate(void);
const char *svc_nrf_rate_name(void);       /* "250k" / "1M" / "2M" */

/* Samples per channel = the resolution of the hit-rate scale. Each costs
 * ~190 us, so this sets sweep speed against gradation. Clamped 1..16. */
void svc_nrf_set_samples(int n);
int  svc_nrf_samples(void);
const char *svc_nrf_samples_name(void);   /* "fast" / "norm" / "fine" */

/* Peak hold: keep a decaying maximum per channel so a short burst stays
 * visible. It tracks the live level upward immediately and falls by one
 * step per completed sweep, which is slow enough to read and fast enough
 * that a signal which has gone away does not linger. */
void svc_nrf_set_peak_hold(int on);
int  svc_nrf_peak_hold_on(void);

/* Clear the live trace and the hold. */
void svc_nrf_reset(void);

/* Measure up to max_ch channels starting at the cursor, then advance it.
 * Returns how many were measured. Wraps to 0 and counts a sweep on wrap. */
int svc_nrf_step(int max_ch);

/* Channel the next svc_nrf_step() will measure first - the UI draws its
 * sweep cursor here. */
int svc_nrf_cursor(void);

uint8_t svc_nrf_level(int ch);      /* live hits, 0..svc_nrf_samples() */
uint8_t svc_nrf_peak(int ch);       /* held peak, same range */
int     svc_nrf_peak_ch(void);      /* strongest channel, -1 if all zero */
uint8_t svc_nrf_peak_level(void);
uint32_t svc_nrf_sweeps(void);

/* esp_err from the last SPI transaction, 0 = none. Non-zero here with
 * present()==1 means the bus is going bad, not that the radio is absent. */
int svc_nrf_error(void);