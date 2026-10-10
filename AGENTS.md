# AGENTS.md — HackUlator

ESP-IDF (v5.5.5, target `esp32` classic) wardriving-style scanner disguised
as a calculator. Boots to a working calculator; `4+6=` unlocks WiFi/BLE
scan + attack menus. SSD1306 128x64 OLED (I2C) + always-on caster mirror
over UART0.

## Build / flash (Windows)

System Python is 3.14 and has **no** IDF env — always force IDF Python 3.11:

```bat
cmd /c "set PATH=C:\Espressif\tools\idf-python\3.11.2;%PATH% && call C:\Espressif\frameworks\esp-idf-v5.5.5\export.bat && idf.py build"
```

* Do NOT use `export.ps1` (picks the missing 3.14 venv and fails).
* Interactive scripts: `build_flash.bat` (build+flash), `open_caster.bat`
  (viewer), `export_fw.bat` (rebuild + copy bins into `firmware/`).
* `idf.py -p COMx flash` to flash. No-IDF flashing: `firmware/` pack +
  esptool (see `firmware/README.txt`).

## Source layout (`main/`)

* `main.c` — screen state machine (`SCR_*`), menu dispatch, relock paths.
* `ui_calc.c` — calculator disguise (unlock is `4+6=`).
* `ui_menu.c` — top menu. `ui_wifiscan.*` / `ui_blescan.*` — scan + detail.
* `ui_wifiatk.*` / `ui_bleatk.*` — attack screens. Verification is **not**
  an entry guard: `main.c` opens the list immediately, and
  `ui_wifiatk_key()` verifies only when a mode that needs a target is
  launched, returning `WIFATK_EXIT_SCAN` so the caller opens the scan
  page. Beacon spam skips it and opens the SSID-list picker instead.
  `svc_deauth.*` — WiFi deauth-family flood (100 ms esp_timer TX loop,
  `esp_wifi_80211_tx`). Four modes via `deauth_mode_t`: DEAUTH (0xC),
  DISASSOC (0xA), COMBINED (both per tick, so the fps is ~20), BEACON
  (pool of `BEACON_POOL_DEFAULT`=20 fake APs, every one beaconing on
  every tick - Hydra-ESP's model; rotating a single identity instead
  leaves only one alive and clients expire the rest). Channel-hopped
  across 1..13 every `BEACON_HOP_TICKS`=3 ticks. SSID sources
  `beacon_name_mode_t` (COMMON/GARBAGE/RICKROLL/SECURITY/ALL) chosen in
  the picker; BSSIDs locally administered, monotonic TSF.
  The attacks screen keeps a counters band
  (`f<frames> a<live fake APs> <list>`) under the status line -
  `svc_deauth_frames()` / `svc_deauth_fake_aps()`.
  `ui_wifiatk_tick()` repaints only the status band; main loop calls it
  every 500 ms. It is also where the auto-stop is
  honoured: the TX callback only sets `svc_deauth_expired()` (a timer must
  not delete itself from its own callback), the tick calls
  `svc_deauth_stop()`. Timeout is `DEAUTH_TIMEOUT_S` (180 s).
  `main.c:enter_menu_item()` owns every menu transition and MUST call
  `svc_deauth_stop()`, `svc_sniff_stop()` and `svc_ble_passive_stop()`
  before any radio teardown (an orphan TX timer breaks the next verify
  scan; a live promiscuous callback faults on a deinitialised driver; a
  live BT scan leaks the controller). The relock and idle paths in
  `app_main()` must do the same.
* **RX callback is count-only.** `svc_sniff.c`'s `rx_cb` runs on the WiFi
  task and holds one of only `CONFIG_ESP_WIFI_STATIC_RX_BUFFER_NUM` (6) RX
  buffers per frame. Never malloc, queue, or copy a payload there, and never
  call `ESP_LOG*` (it stalls the WiFi task). The IDF driver exposes no
  RX-drop counter, so the displayed rate is a **relative** figure — enough
  for "traffic appeared / disappeared", never a completeness claim.
  `svc_sniff_set_channel()` (UP/DOWN) resets the counters per hop so a rate
  always describes one band.
* **The victim-traffic metric counts DATA frames only.** `svc_sniff_hits()`
  counts frames that passed the BSSID filter in `SNIFF_TRAFFIC` mode, and
  that mode rejects everything that is not `WIFI_PKT_DATA`. Every frame the
  device INJECTS is management (deauth `0xC0`, disassoc `0xA0`, beacon
  `0x80`), so the attack's own output cannot inflate the figure the attack
  is judged by. Never widen this to "all frames" without re-proving it —
  `tools/hk_probe_contam.py` is the test.
* **`ui_wifiscan_run()` blocks ~2.5 s longer than it used to.** After the
  scan it runs `svc_sniff_burst()` on the strongest AP's channel. Headless
  tests that wait a fixed time for the scan list must allow for that, and
  the burst channel comes from `s_aps[0].channel` — never a hardcoded 1,
  which silently sampled an empty band whenever the loop body never ran.
* **802.11 field layout, two traps.** In the FC byte, TYPE is bits 2-3 and
  SUBTYPE is bits **4-7** — a beacon is `fc0 & 0x0C == 0` **and**
  `(fc0 >> 4) & 0x0F == 8`. Masking the low nibble matches `0x88` (QoS-Data)
  and silently counts data frames as beacons. Second: RSSI is always
  negative, so a "best seen" sentinel must start below any real reading
  (`-128`), never at 0.
* **The self-check compares beacons to beacons.** `svc_sniff.c` derives the
  expected rate from the beacon interval the frame itself advertises, so no
  second receiver is needed to check the counters. Never compare the
  all-management rate against it — management traffic also contains probes,
  auth and deauth, which inflates the ratio past any meaningful value.
  `ui_menu.c` `LIST_Y0` is capped by this: rows fill y=10..54, so
  `LIST_VISIBLE` is 5 at ROW_H 9 and the top menu (6 items) scrolls via
  `scroll_to_sel()`. Adding a 7th menu entry needs no layout change, but
  verify the scroll window reaches the last row.
* **`svc_ble.c` ADV flood: the ceiling is the BLE spec, not a config.**
  `adv_int_min`/`adv_int_max` are counts of **0.625 ms units**, not
  milliseconds, and their range starts at `0x0020` (= 20 ms), so the fastest
  legal advertising event rate is **50/s**. Classic ESP32's `esp_bt.h` has no
  `ble_multi_adv_instances` field (h2/h4/c2/c5/c6 do), so there is ONE
  advertising set and ONE identity: a rate flood, not a pool. The name is
  picked once in `flood_build_raw()` and cannot be rotated without
  stop/restart. Do not "optimise" the interval below `0x0020` — the controller
  will clamp it and any claim of a higher rate is false.
* **The ADV flood counter is DERIVED; never present it as measured.**
  Bluedroid has no per-advertising-event callback — only
  `ESP_GAP_BLE_ADV_DATA_SET_COMPLETE_EVT` (once per run) and
  `ESP_GAP_BLE_ADV_TERMINATED_EVT`. So `svc_ble_flood_est()` is
  `elapsed / interval` and the UI prints `est` deliberately.
  `svc_ble_flood_started()` is the only real feedback (it goes to 1 when the
  payload is on air). Widening `est` into a packet count, or dropping the
  `est` label, would be a lie in the same way as presenting a relative sniff
  rate as an absolute one.
* **GAP callback never stops advertising.** `gap_cb()` counts and flags only.
  `ui_bleatk_tick()` polls `svc_ble_flood_expired()` and calls
  `svc_ble_flood_stop()` — the same two-phase rule as `svc_deauth`'s TX timer,
  for the same reason: `esp_bluedroid_deinit()` from inside a BT callback
  pulls the stack out from under the BT task. `svc_ble_stop()` must also stop
  advertising (it does) before unwinding the controller.
* **BLE is one radio: scan, listen and advertise are mutually exclusive.**
  `svc_ble_flood_start()` stands down the passive monitor, and
  `svc_ble_scan()` / `svc_ble_passive_start()` stand the flood down. The UI
  guarantees this on every exit path; the service calls are the backstop. A
  radio that scans while advertising answers its own `SCAN_REQ`, which
  quietly ruins both jobs.
* **The ADV flood must never call `ui_bleatk_require()`.** It is
  self-targeting, exactly like WiFi beacon spam. `main.c` opens the attacks
  screen unconditionally and `BLEATK_EXIT_SCAN` stays dead code until a
  genuinely targeted BLE mode exists.
* `tools/hk_advtest.c` compiles `flood_pick_name()` / `flood_build_raw()` on
  a PC against stub RNG and length helpers, and checks the AD structure and
  the 31-byte legacy budget. **The two copies are kept in sync by hand** — if
  you change the builder in `svc_ble.c`, change it there too. The alternative
  (compiling `svc_ble.c` for the host) drags in `esp_bt.h`.
* **`ui_bleatk.c` return codes are load-bearing: `0` means STAY.** A sub-handler
  (e.g. `picker_key()`) must return `0`, never `1`. `BLEATK_EXIT_MENU` is
  literally `1`, so returning `1` from anywhere inside the screen makes
  `main.c` treat the keypress as "leave to menu" and eject the user — the next
  keypress then lands on the top menu and navigates somewhere unrelated. This
  presents as "the attack silently did nothing and a random screen appeared",
  with no crash and no log. Same trap as `WIFATK_EXIT_*` / `BLEATK_EXIT_*`;
  `ui_wifiatk.c` gets it right, so copy that file's convention.
* `tools/hk_ocr.py` decodes screen text straight from `main/font_oled.h` (4x9
  fixed cell, one byte per row). Text is drawn at arbitrary y AND centred text
  starts at an arbitrary x (`draw_msg` puts a 13-char line at x=38), so it
  brute-forces both offsets and scores every glyph; rows reading at avg 36.00
  are exact. Debugging aid only — it is not a test oracle, and a captured
  frame can be torn because the caster drops frames rather than queueing.
* `ui_status.*` — bottom bar `"34K W:abc B:def"` (free KB + targets).
  Calculator has no bar (disguise); detail views keep action footers.
* `svc_wifi.c` / `svc_ble.c` — lazy radio bring-up, full teardown on stop.
  One radio at a time. `svc_target.*` — session-only (RAM, reboot clears)
  attack targets: WiFi keyed by BSSID, BLE by MAC, `tgt_*_verify()` rescans.
* `svc_resume.c` — persists a pending scan across `esp_restart()` (RTC
  NOINIT) for the low-heap reboot path.
* `svc_sniff.*` — passive promiscuous RX (count only, see hard constraints).
  Also `svc_sniff_burst()`: a short blocking listen used by the WiFi scan
  detail view to sample one channel's activity. It takes over the
  promiscuous callback itself, so the live monitor must not be running.
  `ui_sniff.*` — the RX monitor screen, reached through `ui_wifipassive.*`
  (the "WiFi passive" submenu — deliberately its own widget, not a nested
  `ui_menu_enter()`, whose single module-level cursor the submenu would
  clobber on the way back). `ui_blepassive.*` is the BLE counterpart,
  counting adverts via `svc_ble_passive_*` (`BLE_SCAN_TYPE_PASSIVE` +
  random own address, so it never answers a SCAN_REQ and is not
  identifiable).
* `hal_oled.*` (1KB static mono page FB, SSD1306+caster flush),
  `hal_input.*`
  (6 buttons + caster keys, `KEY_RELOCK` on OK+BACK hold / 3x BACK),
  `caster.*` (PKC rect protocol, mono expanded to RGB565 white/black),
  `oled_gfx.*`, `font_oled.h` (generated via `tools/gen_font.py`, don't edit).

## Hard constraints

* **Injection**: `wsl_bypasser.*` overrides the driver's private
  `ieee80211_raw_frame_sanity_check()` (which lives in the closed Wi-Fi blob
  and rejects management subtypes) and `main/CMakeLists.txt` links with
  `-Wl,-zmuldefs`. **Removing that link flag silently reinstates
  `ESP_ERR_INVALID_ARG` on every injected frame** — deauth will report
  `tx err 258` and the counter stays 0. Same technique as Hydra-ESP /
  risinek, credit them if this is ever redistributed.
* **Heap**: 1KB mono FB is static (no heap); largest-free-block is the
  binding metric. `heap_ok_or_reboot()` in `ui_*scan.c`
  reboots below 16K — never lower it without on-device proof. Free the
  other radio's list before bring-up (`ui_*_drop()`); scan buffers are
  heap (not static). BLE-only: Classic-BT
  controller RAM is released at boot (`main.c`).
* **`-Werror=all`**: `snprintf` into small buffers needs clamped ints
  first (see footer code in `ui_*scan.c`); one statement per line (the
  misleading-indentation check fires on `if (a) x; if (b) y;`).
* **UI protocol**: each screen draws then `hal_oled_flush_all()`;
  `*_run()` drains stale input; `*_key()` returns 1 only to leave.
  Detail: UP/DOWN pages fields, RIGHT selects target, OK/LEFT/BACK exits.
* **Config**: edit `sdkconfig.defaults`, never `sdkconfig` (generated,
  gitignored). Same for `build/` and `managed_components/` (IDF component
  cache). Register new sources in `main/CMakeLists.txt`.

## Test

* `tools/hacku_viewer.py --port COMx` — live display viewer + remote keys
  (`wasd`/`e`/`b`). `tools/hk_test.py --port COMx [--cycles N]` —
  headless pass/fail incl. multi-cycle heap screenshots.
  `tools/hk_test_deauth.py` — deauth flow. `tools/hk_log.py` — boot log
  over UART0 @ 115200 (**early boot only**: once the always-on caster
  starts, logs are silenced so they cannot corrupt the PKC stream). `tools/hk_wire.py`,
  `tools/hk_px.py`, `tools/hk_anchor.py`, `tools/hk_palette.py` —
  caster-wire and pixel probes. All accept `--port`, auto-detect if
  omitted, and run on Windows/Linux/macOS.
* Test PNGs (`test_mem*`, `test_out`, `test_deauth`, `tools/probe_*`) are
  throwaway and gitignored — never commit them.

## Git

GitHub `Velmet44/HackUlator`, branch `main`. Commit/push and cut
releases only when explicitly asked. `firmware/*.bin` are tracked release
artifacts — refresh from a verified build before a release commit.
