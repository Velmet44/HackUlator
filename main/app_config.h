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

/* ---- One-button gesture ----
 * GPIO0 is the module's own BOOT button, not one of the six UI keys, so it
 * is free. A 3 s hold toggles "direct" mode, which drops the calculator
 * disguise and goes straight to the menu; it lives in RAM only, so a reboot
 * always comes back disguised.
 *
 * Read as a plain input AFTER boot, never as a strap: GPIO0 low at reset
 * selects the download bootloader, so this must not be used to decide
 * anything during startup. */
#define PIN_BTN_ONE         0

/* ---- 2.4 GHz spectrum analyser: nRF24L01+ ----
 * MOSI/MISO/SCK are 23/19/18, which are the classic ESP32's SPI3 (VSPI)
 * IO_MUX pins, so this runs at native speed rather than through the GPIO
 * matrix. Wiring the nRF to HSPI's 13/12/14 instead would halve it.
 *
 * ============================ READ THIS FIRST ============================
 * CSN = 14 and CE = 26 ARE PIN_BTN_OK and PIN_BTN_RIGHT. Those two buttons
 * are electrically repurposed by the analyser and the board cannot have
 * both. Consequences, all handled in code but none of them free:
 *
 *   - hacku_input_init() makes GPIO14 an INPUT with a pull-up at boot. The
 *     radio would then be deselected for ever, so svc_nrf_init() reclaims
 *     the pin as an output before it uses the bus. This means the analyser
 *     cannot be initialised until after hal_input_init(), and it must be
 *     stopped before anything re-runs that init.
 *   - GPIO14 and GPIO26 are put in a BUTTON MASK while the analyser screen
 *     is live, so they are neither sampled as keys nor able to drag CSN to
 *     ground mid-transaction. Those two buttons are inert on that screen -
 *     physically unavoidable, since pressing OK would otherwise put the
 *     output into a dead short through the button.
 *   - The analyser still uses OK (cycle view) and RIGHT (sample count) -
 *     via the caster, which is an independent path. On the six-key keypad
 *     they simply do not exist while the screen is open.
 *
 * To fix this properly, move the nRF's CSN off 14 and its CE off 26 (or
 * move the keypad's OK and RIGHT). Nothing in the code depends on these
 * numbers beyond app_config.h.
 * ======================================================================== */
#define PIN_NRF_CE           26
#define PIN_NRF_CSN          14
#define PIN_NRF_MOSI         23
#define PIN_NRF_MISO         19
#define PIN_NRF_SCK          18

/* Smallest largest-free-block that will accept an SPI3 bring-up. Below this
 * the DMA descriptor allocation cannot be satisfied and the driver faults
 * rather than failing, so the service refuses and says so. */
#define NRF_MIN_HEAP_BYTES  (24 * 1024)

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
