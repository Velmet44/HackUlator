# AGENTS.md — HackUlator

ESP-IDF (v5.5.5, target `esp32` classic) wardriving-style scanner disguised
as a calculator. Boots to a working calculator; `4+6=` unlocks WiFi/BLE
scan + attack menus. ILI9341 240x320 TFT or headless caster mode over UART0.

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
* `ui_wifiatk.*` / `ui_bleatk.*` — attack screens + `ui_*_require()` entry
  guards (no target / target gone → caller must show scan page).
  `svc_deauth.*` — WiFi deauth flood (100 ms esp_timer TX loop,
  `esp_wifi_80211_tx`). `ui_wifiatk_tick()` repaints only the status band;
  main loop calls it every 500 ms. It is also where the auto-stop is
  honoured: the TX callback only sets `svc_deauth_expired()` (a timer must
  not delete itself from its own callback), the tick calls
  `svc_deauth_stop()`. Timeout is `DEAUTH_TIMEOUT_S` (180 s).
  `main.c:enter_menu_item()` owns every menu transition and MUST call
  `svc_deauth_stop()` before any radio teardown (an orphan TX timer breaks
  the next verify scan).
* `ui_status.*` — bottom bar `"34K W:abc B:def"` (free KB + targets).
  Calculator has no bar (disguise); detail views keep action footers.
* `svc_wifi.c` / `svc_ble.c` — lazy radio bring-up, full teardown on stop.
  One radio at a time. `svc_target.*` — session-only (RAM, reboot clears)
  attack targets: WiFi keyed by BSSID, BLE by MAC, `tgt_*_verify()` rescans.
* `svc_resume.c` — persists a pending scan across `esp_restart()` (RTC
  NOINIT) for the low-heap reboot path.
* `hal_display.*` (banked 2x75KB FB, TFT+caster flush), `hal_input.*`
  (6 buttons + caster keys, `KEY_RELOCK` on OK+BACK hold / 3x BACK),
  `caster.*` (PKC rect protocol), `gfx.*`, `font.h` (generated, don't edit).

## Hard constraints

* **Injection**: `wsl_bypasser.*` overrides the driver's private
  `ieee80211_raw_frame_sanity_check()` (which lives in the closed Wi-Fi blob
  and rejects management subtypes) and `main/CMakeLists.txt` links with
  `-Wl,-zmuldefs`. **Removing that link flag silently reinstates
  `ESP_ERR_INVALID_ARG` on every injected frame** — deauth will report
  `tx err 258` and the counter stays 0. Same technique as Hydra-ESP /
  risinek, credit them if this is ever redistributed.
* **Heap**: 150KB FB is permanently resident; largest-free-block is the
  binding metric (pristine ~34K). `heap_ok_or_reboot()` in `ui_*scan.c`
  reboots below 16K — never lower it without on-device proof. Free the
  other radio's list before bring-up (`ui_*_drop()`); scan buffers are
  heap (not static) to protect the FB block. BLE-only: Classic-BT
  controller RAM is released at boot (`main.c`).
* **`-Werror=all`**: `snprintf` into small buffers needs clamped ints
  first (see footer code in `ui_*scan.c`); one statement per line (the
  misleading-indentation check fires on `if (a) x; if (b) y;`).
* **UI protocol**: each screen draws then `hacku_display_flush_all()`;
  `*_run()` drains stale input; `*_key()` returns 1 only to leave.
  Detail: UP/DOWN moves, RIGHT selects target, OK/LEFT/BACK exits.
* **Config**: edit `sdkconfig.defaults`, never `sdkconfig` (generated,
  gitignored). Same for `build/` and `managed_components/` (IDF component
  cache). Register new sources in `main/CMakeLists.txt`.

## Test

* `tools/hacku_viewer.py --port COMx` — live display viewer + remote keys
  (`wasd`/`e`/`b`). `tools/hk_test.py --port COMx [--cycles N]` —
  headless pass/fail incl. multi-cycle heap screenshots.
  `tools/hk_test_deauth.py` — deauth flow. `tools/hk_log.py` — boot log
  over UART0 @ 115200 (**pointless on caster units**: logs are silenced so
  they cannot corrupt the PKC stream). `tools/hk_wire.py`,
  `tools/hk_px.py`, `tools/hk_anchor.py`, `tools/hk_palette.py` —
  caster-wire and pixel probes. All accept `--port`, auto-detect if
  omitted, and run on Windows/Linux/macOS.
* Test PNGs (`test_mem*`, `test_out`, `test_deauth`, `tools/probe_*`) are
  throwaway and gitignored — never commit them.

## Git

GitHub `Velmet44/HackUlator`, branch `main`. Commit/push and cut
releases only when explicitly asked. `firmware/*.bin` are tracked release
artifacts — refresh from a verified build before a release commit.
