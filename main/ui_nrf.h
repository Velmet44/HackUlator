#pragma once

/* 2.4 GHz spectrum analyser screen (nRF24L01+ over SPI3).
 *
 * TWO VIEWS on one plot area, cycled with OK:
 *   SWEEP    live hit-rate bars with a decaying peak hold and a sweep cursor
 *   FALL     a waterfall, one bit-pixel per (channel, sweep) so the whole
 *            125 x 36 history costs 500 bytes of static RAM
 *
 * Every number drawn here is a HIT RATE out of svc_nrf_samples(), because
 * that is all the radio can measure - see svc_nrf.h. The scale is printed on
 * screen and never presented as dBm. */

#include "hal_input.h"

/* ui_nrf_key() result */
#define NRF_EXIT_MENU 1 /* BACK: leave to the top menu */

/* Cadence the main loop must drive ui_nrf_tick() at.
 *
 * Faster than every other live screen's 500 ms on purpose: the tick only
 * advances the sweep by a slice, so the trace has to be stepped often
 * enough for the eye to read it as one moving picture rather than a series
 * of jumps. A full band pass is ~150 ms of SPI, so this is fast enough to
 * look continuous and still leaves most of the tick for key handling. */
#define NRF_TICK_MS  200

/* Enter the screen: bring the radio up, mask the borrowed button pins,
 * show the key legend briefly, then drain stale input. */
void ui_nrf_run(void);

/* Returns 0 to stay, or NRF_EXIT_MENU to leave. */
int ui_nrf_key(hacku_key_t k);

/* Measure a slice of the band and repaint. Call on a ~200 ms cadence.
 * 1 = repainted. */
int ui_nrf_tick(void);