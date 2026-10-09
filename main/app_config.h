#pragma once

/* Hackulator — board + protocol configuration.
 * Classic ESP32-WROOM, 0.96" 128x64 SSD1306 OLED (I2C), 6 buttons. */

#include <stdint.h>

/* ---- Display (SSD1306 128x64, I2C) ---- */
#define HACKU_DISP_W        128
#define HACKU_DISP_H        64

#define PIN_OLED_SDA        21
#define PIN_OLED_SCL        22
#define OLED_I2C_PORT       0
#define OLED_I2C_HZ         400000
#define OLED_ADDR           0x3C

/* ---- Buttons (to GND, internal pull-ups, active low) ---- */
#define PIN_BTN_UP          32
#define PIN_BTN_DOWN        33
#define PIN_BTN_LEFT        25
#define PIN_BTN_RIGHT       26
#define PIN_BTN_OK          14
#define PIN_BTN_BACK        13

/* ---- Caster: PKC wire protocol (compatible with pikachu viewer) ----
 * UART0 @ 460800 baud, shared with the USB console cable.
 * The OLED framebuffer is mono; the caster mirror expands it to RGB565
 * (white/black) so the wire format is unchanged (viewer: 128x64).
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
