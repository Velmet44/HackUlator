#pragma once

/* Hackulator — board + protocol configuration (Phase 0).
 * Classic ESP32-WROOM, 2.8" ILI9341 240x320 portrait, 6 buttons. */

#include <stdint.h>

/* ---- Display ---- */
#define HACKU_DISP_W        240
#define HACKU_DISP_H        320

#define PIN_TFT_MOSI        23
#define PIN_TFT_SCK         18
#define PIN_TFT_MISO        19
#define PIN_TFT_CS          5
#define PIN_TFT_DC          2
#define PIN_TFT_RST         4
#define PIN_TFT_BL          27
#define TFT_SPI_HOST        SPI2_HOST
#define TFT_SPI_HZ          (20 * 1000 * 1000) /* 40M radiates into UART on
                                                  Dupont wiring: bit errors
                                                  both directions at 1Mbaud */

/* ---- Buttons (to GND, internal pull-ups, active low) ---- */
#define PIN_BTN_UP          32
#define PIN_BTN_DOWN        33
#define PIN_BTN_LEFT        25
#define PIN_BTN_RIGHT       26
#define PIN_BTN_OK          14
#define PIN_BTN_BACK        13

/* ---- Caster: PKC wire protocol (compatible with pikachu viewer) ----
 * UART0 @ 460800 baud, shared with the USB console cable.
 * (1 Mbaud proved error-prone next to 40 MHz TFT SPI on breadboard
 * wiring: single lost key bytes + torn frames. 460800 = 2x bit time.)
 * Frame: 7A A5 E1 'P' 'K' 'C' + x,y,w,h (u16 LE) + crc8(header[6:14]).
 * Rows: enc_len u16; enc_len == w*2 -> raw RGB565, else RLE [count:u16][px:u16].
 * Host->device keys: w/a/s/d = arrows, e/Enter = OK, b/Esc = BACK. */
#define CASTER_BAUD         460800
#define CASTER_SYNC0        0x7A
#define CASTER_SYNC1        0xA5
#define CASTER_SYNC2        0xE1
#define CASTER_TAG0         'P'
#define CASTER_TAG1         'K'
#define CASTER_TAG2         'C'

/* ---- UI ---- */
#define UI_FPS              30
#define IDLE_RELOCK_MS      (5 * 60 * 1000)   /* auto re-lock after 5 min */
#define RELOCK_HOLD_MS      2000              /* OK+BACK hold to re-lock */
