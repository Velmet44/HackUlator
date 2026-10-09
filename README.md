# HackUlator

ESP32 wardriving-style scanner disguised as a calculator. Boots into a working calculator; entering `4+6=` unlocks a hidden menu with **WiFi scan**, **BLE scan**, **WiFi attacks** and **BLE attacks** (scrollable lists + detail screens).

Target: classic **ESP32-WROOM**, 0.96" **SSD1306 128x64 OLED** (I2C), 6 buttons. The mono UI is mirrored over USB serial to a PC viewer.

> **Legal / scope.** Educational lab tool for networks and devices you own or have written permission to test. Running 802.11 injection or BLE advertising attacks against third-party infrastructure is illegal in most jurisdictions (CFAA, UK Computer Misuse Act, IT Act 2000/2008, equivalents elsewhere). The attack modules here are for your own test gear.

## Features

- Working calculator disguise (shunting-yard evaluator, `C ( ) / 7 8 9 * 4 5 6 - 1 2 3 + 0 . < =`)
- Unlock with `4+6=`, relock with OK+BACK hold (2 s), 3x BACK, or 5-min idle
- **WiFi scan**: sorted by RSSI; detail shows MAC / RSSI / channel / auth / ciphers / PHY / WPS / FTM / country
- **BLE scan** (~5 s): deduped by MAC; detail shows name / MAC / RSSI / addr type / adv type / TX / flags / service UUID / manufacturer
- **Session attack targets**: pick a target from any scan detail view with RIGHT (footer flips to `selected`); stored in RAM only, cleared on reboot. Separate slots for WiFi (keyed by BSSID) and BLE (keyed by MAC).
- **WiFi attacks** (UP/DOWN selects, OK runs, 100 ms cadence, 3-minute auto-stop):
  1. **Deauth** — subtype `0xC`, reason 2. Works pre-authentication.
  2. **Disassoc** — subtype `0xA`, reason 1. Only meaningful to an already-associated client.
  3. **Deauth+Disassoc** — both frames every tick, catching stacks that honour one and ignore the other.
  4. **Beacon spam** — broadcasts a pool of **12 fake APs simultaneously**: each one has a locally-administered BSSID and a vendor-plausible name (`TP-Link_WiFi`, `Free Public WiFi`, …) and every entry beacons on every 100 ms tick, holding its identity for the whole run. Rotating a single identity at a time leaves only one entry alive at any moment and scan lists expire the rest within seconds — this pool model is why all of them stay listed. No session target required; runs on channel 1 unless a target's channel is set.
  The attacks screen shows live counters in the bottom band: frames TXed (`f…`) plus, for beacon spam, how many fake APs are live (`a…`).
  Live status shows the mode, frames/sec and time remaining. Works on **stock ESP-IDF** via a WSL bypass (`wsl_bypasser.*`: overrides the driver's private frame-type gate + `-Wl,-zmuldefs`), because stock `esp_wifi_80211_tx()` rejects management frames.
- Entry guards: choosing an attack with no target — or a target that has vanished — shows a message and redirects to the scan page
- One radio at a time (WiFi torn down before BLE and vice versa); full teardown on lock (stealth + power)
- SSD1306 probe at boot (I2C 0x3C/0x3D); without an OLED the device still runs caster-only
- PKC dirty-rect caster mirror (always on): mono OLED pixels expanded to RGB565 white/black over UART0 @ 460800 baud + remote keys from viewer
- Menu footer combines free heap, targets and boot cause (`34K W:abc B:def PWRON/...`)
- Low-heap guard: persists the requested scan across a reboot to pristine heap (`RTC_NOINIT_ATTR`) instead of crashing in the radio bring-up

## Hardware

| Part | Pins |
|---|---|
| OLED SSD1306 I2C | SDA 21, SCL 22, 400 kHz, addr 0x3C (0x3D retry) |
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
                hal_oled/hal_input, caster, oled_gfx, font_oled
tools/           viewer + headless verifiers + pixel/log probes
firmware/        flashable pack (bootloader + partition-table + app + flash scripts)
CMakeLists.txt   partitions.csv   sdkconfig.defaults   dependencies.lock
AGENTS.md        repo conventions, build commands, hard constraints
```

## Known quirks

**"low mem XXK reboot".** The 1 KB mono framebuffer is static (no heap) but ESP32 WiFi/Bluedroid `deinit` still leaves ~10-15 KB of heap residue per radio switch (fragmentation of the *largest contiguous* block, not total free). After several WiFi↔BLE round-trips the largest block can drop under the 16 K bring-up floor, so the device cleanly reboots into the requested scan instead of asserting. The `XXK` on the `scanning...` screen is that meter.

**No logs while the caster runs.** The caster mirror is always on and silences the log bus (log bytes would corrupt the PKC rect stream), so the device looks silent on a 115200 monitor by design. Only early boot logs (before the caster starts) are visible. Use the viewer or on-OLED diagnostics.

**Deauth is 2.4 GHz only.** The ESP32 cannot touch 5 GHz, so a client on `SSID-5G` is unaffected even while the flood runs. Target the 2.4 GHz SSID and confirm the client is on that band.

**Beacon spam is clutter, not capture.** It broadcasts invented beacons so scanners list phantom networks and some clients may try to connect — but it cannot hand you credentials: a fake AP is only useful if the victim also enters a password, and even then it goes to your capture, not the real network. It floods the whole channel, so every nearby device sees the bogus SSIDs too.

**Deauth floods everything on the channel.** The frames are broadcast, so unrelated devices on the same channel (including neighbours') will also be dropped. Keep runs short and test away from others.

**Deauth vs Disassoc.** Both are unauthenticated management frames — the ESP32 needs no connection and no password to send either. The difference is on the victim: deauth (`0xC`) also hits a client that is merely *trying* to associate, while disassociation (`0xA`) only means anything to a client already associated. Neither captures credentials; both are availability attacks.

## Credits

- Frame-level reference for the deauth template and 100 ms TX cadence: [Hydra-ESP](https://github.com/SameerAlSahab/Hydra-ESP) (GPL-3.0), itself derived from risinek's esp32-wifi-penetration-tool and inspired by spacehuhn's esp8266_deauther. HackUlator's implementation is its own code written against the public ESP-IDF `esp_wifi_80211_tx()` API.

## License

MIT (or your choice — update this line).