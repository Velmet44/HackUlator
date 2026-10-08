# HackUlator

ESP32 wardriving-style scanner disguised as a calculator. Boots into a working calculator; entering `4+6=` unlocks a hidden menu with **WiFi scan** and **BLE scan** (scrollable lists + detail screens).

Target: classic **ESP32-WROOM**, 2.8" **ILI9341 240x320** portrait TFT, 6 buttons. Runs headless too — the display is streamed over USB serial to a PC viewer.

## Features

- Working calculator disguise (shunting-yard evaluator, `C ( ) / 7 8 9 * 4 5 6 - 1 2 3 + 0 . < =`)
- Unlock with `4+6=`, relock with OK+BACK hold (2 s), 3x BACK, or 5-min idle
- WiFi AP scan: sorted by RSSI, detail shows MAC / RSSI / channel / auth / ciphers / PHY / WPS / FTM / country
- BLE GAP scan (~5 s): deduped by MAC, detail shows name / MAC / RSSI / addr type / adv type / TX / flags / service UUID / manufacturer
- One radio at a time (WiFi torn down before BLE and vice versa); full teardown on lock (stealth + power)
- ILI9341 auto-detect (RDDID); falls back to **caster mode** when no TFT answers
- PKC dirty-rect caster: framebuffer regions over UART0 @ 460800 baud + remote keys from viewer
- Boot-cause footer on menu (`PWRON/EXT/SW/PANIC/BROWN/...`) for telling brownouts apart
- Low-heap guard: persists the requested scan across a reboot to pristine heap (`RTC_NOINIT_ATTR`) instead of crashing in the radio bring-up

## Hardware

| Part | Pins |
|---|---|
| TFT ILI9341 SPI (SPI2, 20 MHz) | MOSI 23, SCK 18, MISO 19, CS 5, DC 2, RST 4, BL 27 (LEDC PWM) |
| Buttons to GND, internal pull-ups, active low | UP 32, DOWN 33, LEFT 25, RIGHT 26, OK 14, BACK 13 |
| USB console / caster | UART0 @ 460800 baud |

2 MB flash layout (`partitions.csv`): factory-only, no OTA yet.

## Quick start

Windows (double-click friendly):

```bat
build_flash.bat      :: build + flash (auto-sources ESP-IDF, prompts for COM port)
open_caster.bat      :: open the display viewer
export_fw.bat        :: build + copy bootloader/partition-table/app into firmware\
```

Manual / Linux/macOS:

```sh
./build_flash.sh
python3 tools/hacku_viewer.py --port /dev/ttyUSB0   # Windows: --port COM5
./export_fw.sh
```

No ESP-IDF needed to flash a release: see `firmware/README.txt` (esptool only).

Requirements: ESP-IDF 5.x, Python 3 + `pyserial pillow` for the viewer, `esptool` for the firmware pack.

## Controls

- Physical: 6 buttons navigate the keypad cursor, OK selects, BACK deletes / goes back.
- Remote (viewer focused): `W/A/S/D` = arrows, `E`/Enter = OK, `B`/Esc = BACK.

## Repo layout

```text
main/            firmware: ui_* screens, svc_wifi/svc_ble/svc_resume, hal_display/hal_input, gfx, font
tools/           hacku_viewer.py (caster viewer), hk_test.py (headless verifier), gen_font.py
firmware/        flashable pack (bootloader + partition-table + app + flash scripts)
CMakeLists.txt   partitions.csv   sdkconfig.defaults   dependencies.lock
```

## Known quirk: "low mem XXK reboot"

The 150 KB RGB565 framebuffer stays resident, and ESP32 WiFi/Bluedroid `deinit` leaves ~10-15 KB of heap residue per radio switch (fragmentation of the *largest contiguous* block, not total free). Pristine boot shows ~34K largest free; after a WiFi+BLE round-trip it drops under the 16K bring-up floor, so the device cleanly reboots into the requested scan instead of asserting. Watch the `XXK` number on the `scanning...` screen — that's the meter.

## License

MIT (or your choice — update this line).
