# HackUlator

ESP32 wardriving-style scanner disguised as a calculator. Boots into a working calculator; entering `4+6=` unlocks a hidden menu with **WiFi scan**, **BLE scan**, **WiFi attacks**, **BLE attacks**, **WiFi passive**, **BLE passive**, **Channel survey**, **Collateral** and **Audio hijack** (scrollable lists + detail screens).

Target: classic **ESP32-WROOM**, 0.96" **SSD1306 128x64 OLED** (I2C), 6 buttons. The mono UI is mirrored over USB serial to a PC viewer.

> **Legal / scope.** Educational lab tool for networks and devices you own or have written permission to test. Running 802.11 injection or BLE advertising attacks against third-party infrastructure is illegal in most jurisdictions (CFAA, UK Computer Misuse Act, IT Act 2000/2008, equivalents elsewhere). The attack modules here are for your own test gear.

## Features

- Working calculator disguise (shunting-yard evaluator, `C ( ) / 7 8 9 * 4 5 6 - 1 2 3 + 0 . < =`)
- Unlock with `4+6=`, relock with OK+BACK hold (2 s), 3x BACK, or 5-min idle
- **One-button mode** — hold BOOT (GPIO0) or spacebar 3 s to drop the disguise entirely and drive the device from a single input. One way: reboot is the only exit. See [Controls](#controls)
- **WiFi scan**: sorted by RSSI; detail shows MAC / RSSI / channel / auth / ciphers / PHY / WPS / FTM / country, plus a **2.5 s activity sample** — how busy the AP's channel was (frames/s) and how many frames came from that BSSID. The sample covers only the strongest AP's channel; networks on other channels report `not sampled` rather than a wrong number
- **BLE scan** (~5 s): deduped by MAC; detail shows name / MAC / RSSI / addr type / adv type / TX / flags / service UUID / manufacturer / **advert count** for the scan window. Advert rate is *not* a load figure — a phone and a fitness tracker advertise at similar rates whether idle or streaming — so no invented "busy %" is shown
- **Session attack targets**: pick a target from any scan detail view with RIGHT (footer flips to `selected`); stored in RAM only, cleared on reboot. Separate slots for WiFi (keyed by BSSID) and BLE (keyed by MAC).
- **BLE attacks** → **ADV flood**: broadcasts one phantom BLE device advertising as fast as the specification allows, so nearby scanners spend their time parsing adverts that lead nowhere. `OK` opens a **name-list picker** first, mirroring the WiFi beacon SSID picker:
  - `COMMON` — vendor-plausible device names (`Fitbit Charge 5`, `Galaxy Buds2`, `Tile Tracker`, …)
  - `GARBAGE` — random printable junk, generated on the fly so it never repeats
  - `RICKROLL` — bait names
  - `SECURITY` — scam / scare names
  - `ALL` — one of the above per run
  Live status shows the configured rate and time remaining; the counters band shows `est <n>` and the chosen list. Needs no session target. The phantom shows up in a scanner list under whatever name was drawn.
- **WiFi attacks** (UP/DOWN selects, OK runs, 100 ms cadence, 3-minute auto-stop):
  1. **Deauth** — subtype `0xC`, reason 2. Works pre-authentication.
  2. **Disassoc** — subtype `0xA`, reason 1. Only meaningful to an already-associated client.
  3. **Deauth+Disassoc** — both frames every tick, catching stacks that honour one and ignore the other.
  4. **Beacon spam** — broadcasts a pool of **20 fake APs** that all beacon on every tick, and **hops channels** (1→13, 300 ms each) so a channel-sweeping client meets the pool across the whole band instead of on one pinned channel. Pressing OK on it opens an **SSID-list picker** first:
  - `COMMON` — vendor-plausible names (`TP-Link_WiFi`, `Free Public WiFi`, …)
  - `GARBAGE` — random printable junk
  - `RICKROLL` — rickroll bait names
  - `SECURITY` — scam / scare names
  - `ALL` — one of the above per pool entry
  Rotating a single identity at a time leaves only one entry alive and scan lists expire the rest, so the pool stays fixed for the whole run. Needs no session target.
  The attacks screen shows live counters in the bottom band: frames TXed (`f…`) plus, for beacon spam, live fake APs and the chosen list (`f… a20 COMMON`).
  Live status shows the mode, frames/sec and time remaining. Works on **stock ESP-IDF** via a WSL bypass (`wsl_bypasser.*`: overrides the driver's private frame-type gate + `-Wl,-zmuldefs`), because stock `esp_wifi_80211_tx()` rejects management frames.
- Entry guards: choosing an attack that needs a target when none is set — or one that has vanished — shows a message and redirects to the scan page. The check runs when the attack is **launched**, not when the attacks screen is opened, so self-targeting modes (beacon spam) never trigger a scan
- **WiFi passive** → **RX monitor**: promiscuous mode, counts the 802.11 frames the chip overhears — live frames/sec, total, management vs data split, channel, strongest RSSI, APs heard. `UP`/`DOWN` hop the listener across channels 1-13 (counters reset per channel, so a rate always describes one band). Joins nothing, sends nothing, needs no target
- **BLE passive** → **Adv monitor**: counts BLE advertisements on a `BLE_SCAN_TYPE_PASSIVE` scan with a random own address, so it never answers a `SCAN_REQ` and the monitor itself is not identifiable. Ads/sec, total, strongest RSSI
- **Self-check** (`OK` on the RX monitor): the device validates its own counters. Beacons advertise their own transmit interval, so the expected rate follows from the air — no second receiver needed. Shows observed vs expected beacons/sec and the capture percentage. Compare *beacons to beacons*, never management frames to beacons
- Both passive screens are listen-only and hold no records — no pcap, no handshake capture, no probe harvesting
- **RX/TX coexistence**: `RIGHT` on the WiFi attacks screen toggles a victim-traffic meter that runs *while* an attack transmits — promiscuous RX filtered to the target's BSSID, counting **data frames only**. Shows victim frames/sec, cumulative total, channel + RSSI, and a beacon capture ratio so you can judge the measurement's own reliability. Press RIGHT again to stop. Because it works whether or not an attack is running, the useful flow is to take a baseline first and then watch the figure collapse
- **The victim metric is verified uncontaminated.** Measured against an idle network: 289 frames injected, and the meter moved by **zero** beyond the background. Every frame the device injects is management; the meter counts data only, so the attack cannot inflate the number it is judged by. Reproduce with `python tools/hk_probe_contam.py COM5` — use a *quiet* target, since traffic variance on a busy network swamps the measurement
- **Collateral** (menu item 8) → continuous wideband 2.4 GHz jamming on the WiFi radio, profiled as **BLE / Classic BT / WiFi**. The BLE controller is never touched: its only raw-TX mode is 2 MHz at +9 dBm, whereas one 22 MHz WiFi channel at +19.5 dBm covers eleven 2 MHz BLE channels at once, and a 1 Mbps 2346-byte frame (18.9 ms of airtime) is three times a Bluetooth DH5 packet, so no packet starting inside one can complete. Full/Hot/Burst/Lock/All/Sweep per profile, 3-minute auto-stop, live frame counter and modelled airtime duty
  - **Lock aims itself.** It surveys the band and parks on the busiest channel that carried a beacon — the beacon filter is what stops a transient data burst on an empty channel from winning. The chosen channel and how many channels held an AP are shown (`aim ch1 of 3 AP ch`), so a survey result can never be mistaken for a guess
  - **Measured on a victim's own link** (60 cm, AP on channel 1, 3 paired 15 s windows): p50 36-48 ms → 122-146 ms, p90 68 → 383 ms, packet loss 0.0% → 3.8%. Mode ranking puts **Lock** (110.7 ms mean) far clear of the rest; Full/Hot/Burst/All spread airtime over 4 or 13 channels, so the victim's single channel gets a fraction of it. Note the same build also gave 3.5× at 10 cm — six times the distance changed nothing, which a power-limited jammer should not do and which is still unexplained
  - Discovery and pairing are killable on both Bluetooth protocols with no defence to evade (AFH does not exist before a link). An **established BLE link will not drop** — LE has no ARQ and the supervision timeout needs 267 consecutive misses. An established **Classic** link does, because ACL retries indefinitely, so there is no middle state: audio cuts outright. **WiFi degrades latency; it never disconnects.**
- **Audio hijack** (menu item 9) → plays coded tunes on a Bluetooth speaker or headset by connecting to it as an **A2DP source**. Not BLE audio (see below) — car stereos, older/budget headphones, older TVs and soundbars, anything with a classic A2DP profile
  - **Tunes are RTTTL**, the Nokia ringtone text format: ~200 bytes per tune, plain text, monophonic note+duration+octave+tempo that maps 1:1 onto the oscillator. **Ten are built in, 3,038 bytes of flash total.** Adding one is: drop a `.rtttl` file into `tunes/` and run `python tools/gen_tunes.py`. [PICAXE ships a 10,000-tune zip](https://picaxe.com/rtttl-ringtones-for-tune-command/); [rtttl-hub.io](https://rtttl-hub.io/) has a browsable library and a MIDI→RTTTL converter. Existing Arduino melodies convert directly and exactly: `python tools/arduino2rtttl.py in.ino out.rtttl`
  - **No decoder and no audio files.** A 256-entry signed-byte sine table in `.rodata` (flash, so **zero RAM**) driven by a 32-bit fixed-point phase accumulator, with a 5 ms / 10 ms envelope so notes don't click. All integer: the ESP32 has no FPU and a `sin()` call would pull in libm
  - **The whole feature costs ~1.4 KB of RAM.** The ~41 KB A2DP stack is only allocated while a speaker is actually playing. Classic BT adds ~25 KB of *static* RAM that is linked in permanently — measured on this board: largest contiguous free block 110592 → 102400 with BTDM + Bluedroid + A2DP source + WiFi all up together, against a 16 KB floor
  - **LE audio is unreachable, and that is hardware.** The ESP32's radio is a BLE 4.2 controller with no LE Audio, no LC3 and no BAP, so true-wireless earbuds on the LE Audio path can never be reached. Silicon, not configuration
  - **Discovery is two techniques sharing ONE stack bring-up.** A BLE advert scan first (finds a speaker that is merely powered on — the advert's MAC field *is* the address A2DP needs), then classic inquiry filtered on the Class-of-Device "Rendering" bit (finds anything in pairing mode). Giving each half its own bring-up silently broke the inquiry half, because it was posting at a stack that had already been deinitialised
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

### One-button mode

Hold the module's own **BOOT button** (GPIO0) — or the viewer's **spacebar** — for 3 s and the calculator disguise is gone: the device boots straight to the menu, and the six other keys stop responding. This is **one way — only a reboot brings the calculator back**, which is deliberate: a mis-fired hold must never put the disguise back on screen.

| gesture | action |
|---|---|
| 1 tap | down / cycle a value |
| 2 taps | select / enter |
| 3 taps | back / leave |
| hold (700 ms) | up |

Every list wraps. `x` fires the 3 s hold directly, for clients that cannot hold a key. A progress bar fills while holding, so a 3 s wait is never silent.

## Tools (`tools/`)

All Python tools accept `--port` (auto-detected if omitted) and work on Windows / Linux / macOS.

| Tool | Purpose |
|---|---|
| `hacku_viewer.py` | Live display viewer (Tkinter + Pillow), remote key injection |
| `hk_test.py` | Headless end-to-end verifier (calculator, unlock, scans, relock) |
| `hk_test_deauth.py` | Deauth flow verifier (target select → run → counter → stop) |
| `hk_test_rx.py` | Passive verifiers (menu scroll → submenus → RX monitor → BACK → BLE adv monitor) |
| `hk_probe_contam.py` | Contamination probe: does RX count our own injected frames? |
| `hk_probe_victim.py` | **The only attack test that measures effect.** Watches the PC's own adapter and reports *time on target*. Run `--control` first — without it one observed drop proves nothing |
| `hk_test_advflood.py` | BLE ADV flood flow verifier (picker → run → counter → stop → relock) |
| `hk_test_powerkill.py` | PowerKill flow verifier (CSA-only mode, phase A→B escalation, victim traffic delta) |
| `hk_test_airtime.py` | Checks the airtime **estimate** against arithmetic under a known download — a model never compared to a load it should predict is just a number that looks like one |
| `hk_test_newui.py` | Screenshots the newer surfaces (menu scroll, attack list, CSA-only, RX panels, channel survey). Regression proves nothing broke; it does not prove new screens render |
| `hk_a2dhijack.py` | Audio-hijack flow verifier (menu → screen → discovery → connect path) |
| `hk_rtttl.py` | Host-compiles `tune_rtttl.c` + `tune_synth.c` and checks them against a corpus of **real** RTTTL strings — dot before *and* after the octave, lowercase, embedded spaces, `h` for B, octaves outside 4–7 — plus 7 reference pitches and malformed input. `--wav out.wav` renders a tune to listen to |
| `gen_tunes.py` | Regenerate `main/tune_data.h` from `tunes/*.rtttl` |
| `arduino2rtttl.py` | Convert an Arduino `melody[]` sketch to RTTTL. Reads the sketch's own `NOTE_*` table; the tempo arithmetic is identical between the two formats, so the conversion is exact |
| `hk_advtest.c` | Host-compiled check of the BLE advertising payload builder (AD lengths, 31-byte budget) |
| `hk_pkframe.c` | Host-compiled check of the PowerKill TPC/CSA beacon. Compiles `main/pk_frame.c` **directly**, so a pass says something about the bytes that actually go out |
| `hk_ocr.py` | Decode screen text from a captured PNG or a live frame (debugging aid, not a test oracle) |
| `hk_activity.py` | Walk both scan detail views to the new activity fields |
| `hk_validate_rx.py` | Counter-accuracy harness: scan for reference APs, hop to a channel, screenshot the self-check panel |
| `hk_log.py` | Capture one boot log over UART0 @ 115200 |
| `hk_wire.py` | Drive the UI and report which caster rects hit the wire |
| `hk_px.py` / `hk_anchor.py` / `hk_palette.py` | Framebuffer pixel inspection / colour anchors / palette dump |
| `gen_font.py` | Regenerate `main/font_oled.h` |

```sh
pip install pyserial pillow
python3 tools/hk_test.py --port /dev/ttyUSB0        # Windows: --port COM5
python3 tools/hk_test_deauth.py --port COM5
python3 tools/hk_log.py COM5 15
```

Screenshots land in `test_out/`, `test_a2dh/`, `test_deauth/` (gitignored — throwaway).

### Cross-platform

Everything under `tools/` runs on **Windows, Linux/Debian and macOS** — no
Windows-only paths, and generated files are written with explicit `newline="\n"`
so a Windows checkout does not produce CRLF in a C header.

| Requirement | Windows | Linux/Debian |
|---|---|---|
| Python | 3.9+ | 3.9+ |
| `pyserial`, `pillow` | for the viewer and the hardware verifiers | same |
| C compiler | MSVC Build Tools (`vswhere` + `vcvars64.bat`, found automatically) | `gcc` or `clang`; `cc` is tried first but **Debian often has no `cc` symlink**, so `gcc` is probed too |
| Extra | — | if `/tmp` is mounted `noexec`, set `TMPDIR` to a writable+executable dir |

`hk_rtttl.py` locates the host compiler itself — MSVC through `vswhere` with the
environment captured from `vcvars64.bat` (hand-assembling `INCLUDE` fails with a
misleading `cannot open include file: 'string.h'`), and POSIX by probing
`CC`, `cc`, `gcc`, `clang` with `--version`. Set `CC` to override.

`hk_a2dhijack.py` follows the repo port convention: auto-detected, `--port` to
override, `--list-ports` to see candidates.

`gen_tunes.py` and `arduino2rtttl.py` are pure Python with no dependencies.

## Repo layout

```text
main/            firmware: ui_* screens (incl. ui_wifipassive/ui_sniff/ui_blepassive/
                ui_wicoll/ui_a2dhijack), svc_wifi/svc_ble/svc_target/svc_deauth/
                svc_resume/svc_sniff/svc_wicoll/svc_a2dp, tune_rtttl/tune_synth,
                hal_oled/hal_input, caster, oled_gfx, font_oled
                main/a2dhijack.md  full notes on the A2DP feature (read before editing it)
tools/           viewer + headless verifiers + pixel/log probes + generators
tunes/           *.rtttl sources for the built-in melodies; tunes/sources/ keeps
                the original Arduino sketches they were converted from
                (both are compiled in via main/tune_data.h - generated)
firmware/        flashable pack (bootloader + partition-table + app + flash scripts)
CMakeLists.txt   partitions.csv   sdkconfig.defaults   dependencies.lock
AGENTS.md        repo conventions, build commands, hard constraints
```

## Known quirks

**"low mem XXK reboot".** The 1 KB mono framebuffer is static (no heap) but ESP32 WiFi/Bluedroid `deinit` still leaves ~10-15 KB of heap residue per radio switch (fragmentation of the *largest contiguous* block, not total free). After several WiFi↔BLE round-trips the largest block can drop under the 16 K bring-up floor, so the device cleanly reboots into the requested scan instead of asserting. The `XXK` on the `scanning...` screen is that meter.

**No logs while the caster runs.** The caster mirror is always on and silences the log bus (log bytes would corrupt the PKC rect stream), so the device looks silent on a 115200 monitor by design. Only early boot logs (before the caster starts) are visible. Use the viewer or on-OLED diagnostics.

**Passive counts are relative, not absolute.** The WiFi promiscuous callback in `svc_sniff.c` is deliberately count-only — it holds one of just six static RX buffers per frame, so queuing payloads is not an option, and the ESP-IDF driver gives no RX-drop counter. The BLE counter has the same caveat. Both are enough to see traffic or adverts appear and disappear, but neither is a claim about how much was captured.

**The RX monitor starts on channel 1, which is usually empty.** Before any scan the radio sits on the ESP32 default, so the first reading is normally near zero — a correct count of nothing. Use `UP`/`DOWN` to hop onto a band with traffic. Press `OK` for the self-check panel; "no beacons on this ch" tells you the same thing more clearly.

**Validating the counters without a second receiver.** A beacon states its own beacon interval, so the air itself is the reference: an AP advertising 100 TU must be heard ~9.8 times a second (a 802.11 time unit is 1024 µs). The self-check panel sums that expectation across the APs it heard and shows the capture ratio. Run it on a busy channel before trusting a rate.

Measured on a strong local AP (−25 dBm, 1 AP on ch6, 100 TU interval): **~86% of beacons captured.** That is good enough for the monitor's purpose — spotting a busy channel and watching traffic rise or fall — but it is not 100%, and **data-frame counts are the weakest number**: `CONFIG_ESP_WIFI_AMPDU_RX_ENABLED=y` aggregates them into bursts that a six-buffer pool sheds. Expect data rates to be understated by more than management rates.

**Passive is not invisible.** "Passive" here means the device does not transmit: no injected frames, no `SCAN_REQ`, no probe responses, no association. It is not anonymity — the ESP32 still radiates its own management frames, and the passive BLE scan uses a random address that changes per session but is still a radio that is visibly present. Do not rely on these modes to avoid detection.

**Scan activity is a sample, not a census.** The WiFi detail view reports a 2.5 s listen burst on the strongest AP's channel only. Networks on other channels read `sampled chN only` rather than a number that would not describe them. And unlike the RX monitor — continuous but lossy — a scan sample is brief but unbiased, so it can miss traffic that bursts outside its window.

**The BLE ADV flood is capped at 50 adverts/sec, by the spec.** `esp_ble_adv_params_t.adv_int_min` counts 0.625 ms units and its valid range starts at `0x0020` — 20 ms — so a legal advertising event rate is 50/s and no configuration raises it. Classic ESP32 also has no `ble_multi_adv_instances` field (only the h2/h4/c2/c5/c6 targets do), so there is exactly **one** advertising set: this is a *rate* flood with a **single fixed identity**, not a pool. The name is drawn once at start and cannot rotate without stopping and restarting, which would cost more than it buys. This is why the WiFi beacon pool's 20-identity architecture does not port across.

**The ADV flood's counter is an estimate, not a measurement.** Bluedroid exposes no per-advertising-event callback — the only advertising events are `ADV_DATA_SET_COMPLETE` (once per run) and `ADV_TERMINATED` (once). So `svc_ble_flood_est()` is `elapsed / interval`, which is why the UI labels it `est`. The honest real signal is `started`, which confirms the payload actually went on air. Never read the counter as proof of what reached the air or of any effect on another device; proving that needs a second receiver watching a victim's BLE discovery.

**Deauth is 2.4 GHz only.** The ESP32 cannot touch 5 GHz, so a client on `SSID-5G` is unaffected even while the flood runs. Target the 2.4 GHz SSID and confirm the client is on that band.

**Beacon spam is clutter, not capture.** It broadcasts invented beacons so scanners list phantom networks and some clients may try to connect — but it cannot hand you credentials: a fake AP is only useful if the victim also enters a password, and even then it goes to your capture, not the real network. It floods the whole channel, so every nearby device sees the bogus SSIDs too.

**Deauth floods everything on the channel.** The frames are broadcast, so unrelated devices on the same channel (including neighbours') will also be dropped. Keep runs short and test away from others.

**Deauth vs Disassoc.** Both are unauthenticated management frames — the ESP32 needs no connection and no password to send either. The difference is on the victim: deauth (`0xC`) also hits a client that is merely *trying* to associate, while disassociation (`0xA`) only means anything to a client already associated. Neither captures credentials; both are availability attacks.

## Credits

- Frame-level reference for the deauth template and 100 ms TX cadence: [Hydra-ESP](https://github.com/SameerAlSahab/Hydra-ESP) (GPL-3.0), itself derived from risinek's esp32-wifi-penetration-tool and inspired by spacehuhn's esp8266_deauther. HackUlator's implementation is its own code written against the public ESP-IDF `esp_wifi_80211_tx()` API.

## License

MIT (or your choice — update this line).