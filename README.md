# HackUlator

ESP32 wardriving-style scanner disguised as a calculator. Boots into a working calculator; entering `4+6=` unlocks a hidden menu with **WiFi scan**, **BLE scan**, **WiFi attacks** and **BLE attacks** (scrollable lists + detail screens).

Target: classic **ESP32-WROOM**, 2.8" **ILI9341 240x320** portrait TFT, 6 buttons. Runs headless too — the display is streamed over USB serial to a PC viewer.

> **Legal / scope.** Educational lab tool for networks and devices you own or have written permission to test. Running 802.11 injection or BLE advertising attacks against third-party infrastructure is illegal in most jurisdictions (CFAA, UK Computer Misuse Act, IT Act 2000/2008, equivalents elsewhere). The attack modules here are for your own test gear.

## Features

- Working calculator disguise (shunting-yard evaluator, `C ( ) / 7 8 9 * 4 5 6 - 1 2 3 + 0 . < =`)
- Unlock with `4+6=`, relock with OK+BACK hold (2 s), 3x BACK, or 5-min idle
- **WiFi scan**: sorted by RSSI; detail shows MAC / RSSI / channel / auth / ciphers / PHY / WPS / FTM / country
- **BLE scan** (~5 s): deduped by MAC; detail shows name / MAC / RSSI / addr type / adv type / TX / flags / service UUID / manufacturer
- **Session attack targets**: pick a target from any scan detail view with RIGHT (button flips to green `selected`); stored in RAM only, cleared on reboot. Separate slots for WiFi (keyed by BSSID) and BLE (keyed by MAC).
- **WiFi attacks → 1. Deauth**: raw broadcast 802.11 deauth flood against the stored target at 100 ms cadence, with a live frame counter. Ported from Hydra-ESP's `wsl_bypasser` frame template (see Credits).
- Entry guards: choosing an attack with no target — or a target that has vanished — shows a message and redirects to the scan page
- One radio at a time (WiFi torn down before BLE and vice versa); full teardown on lock (stealth + power)
- ILI9341 auto-detect (RDDID); falls back to **caster mode** when no TFT answers
- PKC dirty-rect caster: framebuffer regions over UART0 @ 460800 baud + remote keys from viewer
- Shared bottom status bar: free heap KB plus WiFi/BLE target prefixes (`34K W:abc B:def`)
- Boot-cause footer on menu (`PWRON/EXT/SW/PANIC/BROWN/...`)
- Low-heap guard: persists the requested scan across a reboot to pristine heap (`RTC_NOINIT_ATTR`) instead of crashing in the radio bring-up

## Hardware

| Part | Pins |
|---|---|
| TFT ILI9341 SPI (SPI2, 20 MHz) | MOSI 23, SCK 18, MISO 19, CS 5, DC 2, RST 4, BL 27 (LEDC PWM) |
| Buttons to GND, internal pull-ups, active low | UP 32, DOWN 33, LEFT 25, RIGHT 26, OK 14, BACK 13 |
| USB console / caster | UART0 @ 460800 baud |

2 MB flash layout (`partitions.csv`): factory-only, no OTA.

## Quick start

**Requirements:** ESP-IDF 5.x (build only), Python 3 with `pyserial` + `pillow` (viewer/tests), `esptool` (firmware pack flashing).

> Windows: system Python 3.14 has no IDF env. Either use the bundled scripts (they force IDF Python 3.11), or run:
> `cmd /c "set PATH=C:\Espressif\tools\idf-python\3.11.2;%PATH% && call C:\Espressif\frameworks\esp-idf-v5.5.5\export.bat && idf.py build"`

Windows (double-click friendly):

```bat
build_flash.bat      :: build + flash (auto-sources ESP-IDF, prompts for COM port)
open_caster.bat      :: open the display viewer
export_fw.bat        :: build + copy bootloader/partition-table/app into firmware\
```

Linux / macOS:

```sh
./build_flash.sh                      # needs a sourced ESP-IDF environment
python3 tools/hacku_viewer.py --port /dev/ttyUSB0
./export_fw.sh
```

No ESP-IDF needed to flash a release: see `firmware/README.txt` (esptool only).

> Windows: close Arduino IDE / PuTTY / `idf.py monitor` before opening the viewer — only one program can hold the COM port.

## Controls

- Physical: 6 buttons navigate the keypad cursor, OK selects, BACK deletes / goes back.
- Remote (viewer focused): `W/A/S/D` = arrows, `E`/Enter = OK, `B`/Esc = BACK.

## Tools (`tools/`)

All Python tools accept `--port` (auto-detected if omitted) and work on Windows / Linux / macOS.

| Tool | Purpose |
|---|---|
| `hacku_viewer.py` | Live display viewer (Tkinter + Pillow), remote key injection |
| `hk_test.py` | Headless end-to-end verifier (calculator, unlock, scans, relock) |
| `hk_test_deauth.py` | Deauth flow verifier (target select → run → counter → stop) |
| `hk_log.py` | Capture one boot log over UART0 @ 115200 |
| `hk_wire.py` | Drive the UI and report which caster rects hit the wire |
| `hk_px.py` / `hk_anchor.py` / `hk_palette.py` | Framebuffer pixel inspection / colour anchors / palette dump |
| `gen_font.py` | Regenerate `main/font.h` |

```sh
pip install pyserial pillow
python3 tools/hk_test.py --port /dev/ttyUSB0        # Windows: --port COM5
python3 tools/hk_test_deauth.py --port COM5
python3 tools/hk_log.py COM5 15
```

Screenshots land in `test_out/`, `test_deauth/` (gitignored — throwaway).

## Repo layout

```text
main/            firmware: ui_* screens, svc_wifi/svc_ble/svc_target/svc_deauth/svc_resume,
                hal_display/hal_input, caster, gfx, font
tools/           viewer + headless verifiers + pixel/log probes
firmware/        flashable pack (bootloader + partition-table + app + flash scripts)
CMakeLists.txt   partitions.csv   sdkconfig.defaults   dependencies.lock
AGENTS.md        repo conventions, build commands, hard constraints
```

## Known quirks

**"low mem XXK reboot".** The 150 KB RGB565 framebuffer stays resident and ESP32 WiFi/Bluedroid `deinit` leaves ~10-15 KB of heap residue per radio switch (fragmentation of the *largest contiguous* block, not total free). Pristine boot shows ~34K largest free; after several WiFi↔BLE round-trips it drops under the 16 K bring-up floor, so the device cleanly reboots into the requested scan instead of asserting. The `XXK` on the `scanning...` screen is that meter.

**Headless units have no logs.** Caster mode silences the log bus (log bytes would corrupt the PKC rect stream), so a headless device looks silent on a 115200 monitor by design. Use the TFT for on-device log output, or add targeted pixel-visible diagnostics.

## Credits

- Frame-level reference for the deauth template and 100 ms TX cadence: [Hydra-ESP](https://github.com/SameerAlSahab/Hydra-ESP) (GPL-3.0), itself derived from risinek's esp32-wifi-penetration-tool and inspired by spacehuhn's esp8266_deauther. HackUlator's implementation is its own code written against the public ESP-IDF `esp_wifi_80211_tx()` API.

## License

MIT (or your choice — update this line).