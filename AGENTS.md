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
  **VERIFIED**: against JioFibre 2.4 GHz (ch1, -58 dBm, idle network),
  289 frames were injected and dB - dA came out at **-1** — every one of
  them excluded. The probe only resolves against a near-idle target: on a
  busy network natural variance (~±40 f/s) swamps a 10 f/s injection, so
  the A/B says nothing. That is a limit of the experiment, not of the
  metric, and it is why earlier attempts here were inconclusive.
* **PowerKill (`DEAUTH_MODE_POWERKILL`) is the TPC/CSA beacon under the
  target's OWN BSSID**, ported from `ESP32WifiPhisher`'s
  `wifi_attack_deauth_client_negative_tx_power()`. Beacons carry no 802.11w
  protection, so it is meant to reach PMF clients that discard forged deauth.
  Phase A (20 s, `POWERKILL_PHASE_A_S`) sends the beacon alone; phase B adds
  deauth + disassoc. The TX buffer had to go `f[80]` -> `f[160]` first: the
  frame is ~107 B with a 32-char SSID and RSN, and 80 truncated it silently.
  **MEASURED**: 3 drops / 0 control, all three in **phase B**. Zero drops
  across phase A. Against a WPA2-PSK client the CSA half did nothing - plain
  deauth did all the work, which is expected, because the case PowerKill
  exists for is a WPA3/PMF client and none was available to test. Do not
  claim it beats deauth without a PMF victim; it is still an unproven 20 s
  of dead time on any client that honours deauth.
* **`svc_wifi_scan()`'s record cap was a correctness bug, not just a
  heap guard.** It was a flat `if (n > 8) n = 8;`, and records come back
  RSSI-ordered - so every scan was silently truncated to the 8 STRONGEST
  networks, and `tgt_wifi_verify()` searches that list by BSSID. An armed AP
  weaker than 8th place reported "target not found" while beaconing. Now
  heap-adaptive (`SCAN_HEAP_RESERVE`), and at ~112 B/record the old cap spent
  under 1 KB to discard most of the map. **Note**: this was NOT the cause of
  the failure it was first blamed on - that environment had 5 APs, all under
  the cap. A latent bug for dense settings, still worth fixing, but do not
  cite it as an explanation without checking the AP count first.
* **`tgt_wifi_verify()` listens before it scans.** `svc_sniff_burst()` on the
  target's known channel (~1200 ms, ~12 expected beacons) proves presence
  without a 2.4 GHz sweep or a 3.2 KB scan buffer. Falls back to a scan only
  when unheard. `tgt_wifi_verify_ex(force_scan=1)` skips the fast path -
  **required** for channel-following, because listening on the old channel
  cannot discover a move by construction.
* **Channel-following is skipped while the meter is live.** Promiscuous RX
  and scanning share the receiver and running both makes the scan fail.
  Tearing the meter down to check would reset its counters and destroy the
  reading in progress, so measuring wins. `ui_wifiatk_tick()` re-verifies
  every `FOLLOW_PERIOD_S` and calls `svc_deauth_follow_channel()`
  **unconditionally** after a present check - the scan leaves the radio
  wherever it finished, so skipping the call strands the attack off-channel
  even when the channel never changed.
* **A band can have TWO painters.** `draw()` and `ui_wifiatk_tick()` both
  paint the attack status band, and the tick runs every 500 ms during an
  attack. They each had their own copy of the text and the tick silently won,
  hiding the PowerKill phase tag for an entire run - screenshots read a bare
  "30/s" while the attack sat in phase B. Both now call one `status_text()`.
  Same trap as the dual `*_key()` return codes: a duplicated code path will
  drift, and the one that is harder to notice is the one that wins.
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
* **`svc_nrf.*` / `ui_nrf.*`** — "2.4G spectrum" (menu item 10), the
  external nRF24L01+ analyser. See the full note under Hard constraints; the
  short version is that RPD is one bit, it is latched, and two of those GPIOs
  are also keypad buttons.
* `hal_oled.*` (1KB static mono page FB, SSD1306+caster flush),
  `hal_input.*`
  (6 buttons + caster keys, `KEY_RELOCK` on OK+BACK hold / 3x BACK,
  (`KEY_ONEBTN` tap / `KEY_ONEBTN_HOLD` 3 s hold, GPIO0 + caster space/x)),
  `caster.*` (PKC rect protocol, mono expanded to RGB565 white/black),
  `oled_gfx.*`, `font_oled.h` (generated via `tools/gen_font.py`, don't edit).
* **"Direct mode" (`main.c`) removes the calculator disguise AND the other
  six keys, ONE WAY.** `s_direct` makes the device boot and idle straight to
  the menu with a single input: the module's own BOOT button on GPIO0, or the
  caster's **spacebar**. A 3 s hold (`ONEBTN_HOLD_MS` = 3000) enters it
  **once**; **only a reboot brings the calculator back**. There is deliberately
  no gesture that exits it. `hacku_input_set_onebutton()` is the only thing
  that switches the HAL over, and `go_home()` owns calling it, so the mode and
  the input layer can never disagree about which keys are alive.
  - **Why one-way.** The disguise is the whole point of the device, and a
    single mis-fired long press must never put a calculator back on screen.
    Every press either advances or does nothing; no input path re-reveals the
    disguise by accident. RAM-only already guaranteed a reboot clears it —
    this removes the in-session path too.
  - **RAM-only, deliberately.** A reboot always comes back disguised. Do not
    move it to NVS or RTC NOINIT — `svc_resume.c` already owns RTC NOINIT.
  - **`unlocked` is derived, not chosen.** `go_home()` sets
    `unlocked = s_direct`. Zeroing it while the menu is on screen is
    *silent*: the key loop hands presses to `ui_calc_key()` and the menu
    ignores the keypad — no crash, no log. Same class of bug as the
    collateral profile desync.
  - **The six keys are refused in the HAL, not by the UI.** One button has
    to express three actions, so it speaks in taps
    (`ONEBTN_TAP_WINDOW_MS` = 400): **1 tap = DOWN, 2 = OK, 3 = BACK**. The
    HAL synthesises those normal keys, so no screen needed rewriting — which
    is the only reason this stayed a small change. A tap is emitted when the
    window *closes*, not on release, so one gesture delivers one key instead
    of a DOWN that is corrected a moment later. Every action costs up to
    400 ms; that is the price of not having four more buttons.
  - **The caster cannot express a hold, so tap and hold are different
    bytes.** Space is `KEY_ONEBTN` (a tap) and `x` is `KEY_ONEBTN_HOLD`.
    Merging them made `x` increment a tap counter, so a remote long press
    could *never* reach the toggle — and the check still went green, because
    it was measuring a screen the press was not supposed to change. A test
    that passes on the wrong screen is worse than no test.
  - **The 3 s hold must be reachable from BOTH home screens.** It is how
    one-button mode is entered in the first place. Gating it on `SCR_MENU`
    alone made the gesture dead on a cold boot — a disguised device sits on
    `SCR_CALC`, so the hold fell into "go home" and just re-drew the
    calculator. Nothing appeared to happen, and the whole test suite passed
    anyway (14/14) because every check started from the menu, the one screen
    where the bug cannot show. **Always test an entry path from the state a
    user is actually in when they first touch it.** Inside any other screen
    the hold is still the escape hatch ("go home", tearing down any running
    attack) — it just cannot change modes.
  - **A held caster key is auto-repeat, not a burst of taps.** Holding
    spacebar delivers ~30 bytes/second; the tap counter hit 3 instantly,
    fired `KEY_BACK`, reset, and repeated — "sort of spams it", and the hold
    never completed. Bytes closer together than `ONEBTN_REPEAT_GAP_MS`
    (90 ms) are now one continuous press; the first repeat cancels any
    pending tap run so a tap-then-hold cannot deliver a phantom `KEY_OK`.
    `hacku_viewer.py` also suppresses repeat for these keys at the source.
  - **A toggle must be driven by a COMPLETED hold, never by the start of
    one.** The first version pushed the toggle on the *first* byte of a
    press. Auto-repeat has gaps longer than the repeat threshold all the
    time, every gap cleared the burst, and the next byte then looked like a
    fresh press and toggled again — measured: one 8 s hold toggled at 3.6 s,
    5.5 s and again after the test ended, finishing exactly where it
    started. Space now means the same in both modes: a tap only navigates
    (and only in one-button mode, where there is a cursor to move); the
    disguise toggles on a completed 3 s hold. Identical to the physical
    button, which is the point.
  - **Keep the tap window and the repeat gap far apart.** They were 400 ms
    and 120 ms, so taps 120 ms apart — an ordinary double-tap — were
    classified as a held press, their tap runs were cancelled, and the
    2-tap/3-tap gestures silently did nothing. They are now 500 ms and
    90 ms, which clears a ~200 ms human double-tap on both sides.
  - **The HAL must be able to emit UP.** It originally produced only
    DOWN/OK/BACK, so every list in the UI was one-way and reaching an earlier
    row meant cycling the whole list the long way round — worst on the scan
    lists, which are long. The hold was free once the mode went one-way (the
    toggle is only needed to get IN), so it became `KEY_UP` at
    `ONEBTN_UP_HOLD_MS` = 700. Shorter than the 3 s entry hold on purpose:
    that one is deliberately hard to trigger by accident and cannot be undone,
    while "up" is used constantly. `hacku_input_onebtn_hold_target_ms()` is
    what the progress bar divides by, so the bar always fills to the moment
    the gesture actually fires.
  - **Lists wrap, and this was not true everywhere.** Five of the seven lists
    cycle (`(s_sel + 1) % n`); `ui_wifiscan.c` and `ui_blescan.c` clamped at
    both ends (`if (s_sel > 0)` / `if (s_sel + 1 < s_count)`). With no UP
    gesture that made the two scan lists strictly downward-only. Both wrap now,
    and `s_top` is corrected for the wrap in both directions. **When adding a
    list, wrap it** — a clamped list is a trap once one button has to drive it.
  - **One button must be able to ARM A TARGET, and could not.** Six keys and
    the caster arrows are both dead in this mode, and the only thing that armed
    a target was `KEY_RIGHT` in the scan *detail* view: `LIST -> OK -> detail ->
    RIGHT`. That last press can never happen, so **a target could not be set at
    all** and the whole WiFi/BLE attack path was unreachable. The fix is in
    `ui_wifiscan.c` / `ui_blescan.c`: in one-button mode `OK` on the LIST (the
    two-tap gesture) calls `arm_selected()` instead of opening the detail view,
    and the armed row is marked `*` on the row itself rather than in a
    transient message — a selection that vanishes after a second has to be
    remembered, one that is on the row is still there when you scroll back.
    The header gains `2x=SET`, because a gesture that is not advertised is a
    gesture nobody finds. **Trade-off, deliberate:** the detail browser is a
    six-button feature; on the keypad nothing changed (OK opens detail, RIGHT
    arms). Do not "fix" this by putting the detail view back on two taps — that
    is what made the mode useless in the first place.
  - **The hold draws a filling bar** (`draw_hold_progress()`, home screens
    only). That bar is the only thing that separates "counting down" from
    "this pin is not wired to anything" — a dead button and a working one look
    identical from the outside. It is why a real wiring fault is now
    diagnosable in one press instead of by argument.
  - **The confirmation must be transient.** `direct_notice()` paints over
    the home screen; `s_notice_until` repaints ~1.5 s later and any keypress
    clears it early. Without the repaint, toggling OFF leaves the device
    sitting on a dead message.
  - **GPIO0 is a strap pin.** Configure it as a plain input AFTER boot;
    holding it low at reset drops the chip into the download bootloader, so
    it must not decide anything during startup.
  - `hacku_input_set_onebutton()` is the only thing that switches the HAL
    over, and `go_home()` owns calling it, so the mode and the input layer
    can never disagree about which keys are alive.
* **`svc_wicoll.*` / `ui_wicoll.*` — "Collateral" (menu item 8)**, wideband
  2.4 GHz jamming on the WiFi radio, profiled as BLE / Classic BT / WiFi.
  Full provenance in the `svc_wicoll.h` header; the parts that bite:
  - **The UI's profile and the service's profile were separate, and that
    killed the device.** `wicoll_mode_at()` (used to turn a pressed row into
    a mode) reads the profile held in `svc_wicoll`, while `wicoll_start()`
    took the UI's. Selecting WiFi before ever running one left the service on
    BLE, so WiFi row 4 resolved to `DISCOVER` and
    `CFG[WiFi][DISCOVER]` is `{NULL, 0, NULL}` — `tx_task` fell through to
    the sweep branch and dereferenced `c->ch[0]`. Measured: `LoadProhibited`,
    `A0 = 0`. The UI now calls **`wicoll_set_profile()`** on every change,
    and `wicoll_start()` **refuses (`-8`) a mode the profile does not offer**.
  - **LOCK surveys now.** It used to park on `pinned_ch`, i.e. channel 1
    unless something else set it — so the one mode that is supposed to be
    precise was the one attacking a fixed guess. `run_lock_survey()` picks
    the busiest channel that carried a **beacon**; the beacon filter is what
    stops a transient data burst on an otherwise empty channel from winning.
    Aims and AP-channel count are shown on screen (`aim ch1 of 3 AP ch`) —
    a survey result that is not displayed cannot be told apart from a guess.
  - **MEASURED on a victim, 60 cm, AP on ch1, 3 paired 15 s windows:**
    p50 36-48 ms -> 122-146 ms (**3.4x**), p90 68 -> 383 ms (5.6x), loss
    0.0% -> 3.8%. But the same build gave 3.54x at **10 cm**, and 6x the
    distance changed nothing — which is not how a power-limited jammer
    behaves, and is unexplained. The victim's RSSI is only a percentage, so
    the C/I was never actually measured. Do not quote a range from this.
  - **WiFi mode ranking, measured the same way (jam p50, control band
    36-48 ms):** Lock **110.7** (n=3, 87-141, 4.2% loss), Sweep 68.3 (n=3,
    66-71), Full 63.0 (n=4), All 53.0 (n=3), Hot 51.5 (n=4), Burst 46.7
    (n=3). **Only Lock clearly beats the control band, and it is the only
    mode that ever produced packet loss.** That is the expected ordering and
    it is the whole argument for concentrating duty: Full/Hot/Burst/All
    spread airtime over 4 or 13 channels, so the victim's single channel
    gets a fraction of it and the result is indistinguishable from noise.
    Lock is the mode to reach for against one known AP.
  - **The ranking above supersedes the old one**, which was taken on the
    build that could panic mid-run; see the profile desync note above. It is
    still only 3-4 windows per mode — good enough to separate Lock from the
    rest, not enough to rank the middle four against each other.
  - **The control link is noisy**: one control window produced a 694 ms
    outlier on a 35 ms median. Only distributions are meaningful here; a
    mean over a handful of samples on this link is noise.
  - **`Duration/ID` was A/B'd and made no detectable difference** (max NAV
    vs zero, 3 windows each, distributions overlapping). The degradation is
    raw energy on the channel, not virtual carrier sense. Kept at zero.
  - **Do not call `esp_wifi_set_max_tx_power(78)`** — on this driver the call
    itself drops the reported power to 58-59 (14.7 dBm) from 78 (19.5 dBm).
    `svc_wicoll`'s `apply_tx_power()` reads first and writes only if low.
    `svc_deauth.c` still calls it and is losing that 4.8 dB.

## Hard constraints

* **`svc_a2dp.*` / `ui_a2dhijack.*` / `tune_rtttl.*` / `tune_synth.*` —
  "Audio Device Hijacker" (menu item 9)**, A2DP **source**: connect to a
  Bluetooth audio sink and play coded tunes on it. Own gear.
  **FULL NOTES: `main/a2dhijack.md`** — RAM measurements, every bug found,
  the verified/unverified split, and the dead ends. Read it before changing
  any of this.
  Parts that bite:
  - **Classic BT only. There is no BLE audio and there never will be.** The
    radio is a BLE 4.2 controller with no LE Audio / LC3 / BAP. TWS earbuds
    on the LE Audio path are unreachable; that is silicon, not
    configuration, and no amount of work changes it.
  - **`esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT)` is GONE from
    `app_main()` and must not come back.** It is a one-way door: once it runs
    the controller can never give that RAM back, which makes A2DP impossible.
    Costs ~25 KB static. Measured 110592 -> 102400 largest free block with
    BTDM + Bluedroid + A2DP source + WiFi all up; floor is 16 KB. The
    ~41 KB runtime cost is only paid while a speaker is playing.
  - **`sdkconfig.defaults` is ONLY read on a fresh configure.** Delete
    `sdkconfig` (gitignored) or an existing one silently overrides it. That
    cost a whole build: every Classic-BT symbol failed to link with no error
    pointing anywhere near the cause.
  - **Discovery is two techniques and they MUST share ONE stack bring-up.**
    BLE advert scan (a powered-on speaker is invisible to inquiry, but its
    advert's MAC field *is* the A2DP address) then classic inquiry filtered
    on the COD "Rendering" bit. The first version gave each half its own
    bring-up, so `svc_a2dp_find_by_ble()` ended with
    `esp_bt_controller_deinit()` and the inquiry that followed was posting at
    a dead stack. It looked exactly like a device that was not discoverable —
    same bug shape as the collateral profile desync.
  - **`esp_a2d_source_audio_data_send()` does NOT copy the buffer.**
    `btc_av_audio_buff_alloc()` hands out a `BT_HDR` with the payload
    *inside* that allocation and the caller's `esp_a2d_audio_buff_t`
    overlays it; on `ESP_OK` the **stack owns it**. A static buffer corrupts
    the BT queue and presents as audio corruption, not a crash. Allocate per
    send, hand over on success, `esp_a2d_audio_buff_free()` on refusal.
  - **The producer MUST be paced against the sample clock.** Sending as fast
    as the stack accepts starves the idle task and trips the task WDT inside
    `feed_task`. Count frames against a 10 ms budget (441 samples) and
    `vTaskDelay(1)` per window. An A2DP source is a real-time producer.
  - **Do NOT gate `start_feed()` on `s_media_ready`.** Media starts
    asynchronously (connection -> CHECK_SRC_RDY -> START -> AUDIO_STATE_STARTED)
    so it is normally not ready when the user presses play. The task already
    waits internally; gating the start on it meant the tune never began and
    never retried, with no error anywhere.
  - **Two-phase stop, and the disconnect is asynchronous.**
    `esp_a2d_source_disconnect()` only posts a request; the reply lands in
    the profile being deinit'd. Wait (bounded) for `s_connected` to clear.
  - **Return-code trap:** `A2DH_EXIT_MENU` is 1 and **0 means STAY**. Every
    path in `ui_a2dhijack_key()` returns 0 unless it means to leave.
  - **A state machine that gets ONE chance is a silent-failure machine.** This
    is the single lesson of the "connects but does not play sound" pass, and it
    generalises to every other service here. Three separate give-ups produced
    the identical symptom — link up, screen cheerful, zero audio, no diagnostic
    (the always-on caster suppresses logs by design): `CHECK_SRC_RDY` sent once;
    `START` acked with `STARTED` never arriving; and a producer that `break`s
    on an alloc failure or a refused send. **`svc_a2dp_pump()` is the fix and it
    must stay.** It re-asks the sink, times out a connect that was never
    answered, and watchdog-times a start that never lands. It rides the UI's
    500 ms tick rather than owning a timer, so there is nothing to leak.
  - **The pump must be BOUNDED, or it fights the user.** Re-asking forever
    restarts a speaker that suspended because somebody pressed pause *on the
    speaker*. It is insistent only within `MEDIA_GRACE_MS` of the link coming up,
    or while the user has asked for audio and it has not arrived.
  - **`ESP_BT_GAP_CFM_REQ_EVT` is not optional.** SSP raises a user-confirmation
    request that the application *must* answer with
    `esp_bt_gap_ssp_confirm_reply()`; there is no default and no log near the
    cause. Handling `PIN_REQ` and `KEY_REQ` but not `CFM_REQ` is what makes
    pairing hang on every headset from the last decade. Pair with
    `ESP_BT_SP_IOCAP_MODE` = `ESP_BT_IO_CAP_NONE` (this device has no display
    and no keypad, so it must say so) and an `esp_bt_gap_set_pin()` default.
  - **`esp_bt_gap_start_discovery()` does NOT make you discoverable.** It only
    issues the HCI_Inquiry. The window must be opened with
    `esp_bt_gap_set_scan_mode(CONNECTABLE, GENERAL_DISCOVERABLE)` first and
    closed on `DISCOVERY_STOPPED`, or the classic half of discovery finds
    nothing — which reads as "that device is not discoverable".
  - **`cfg.mode` must equal what you pass to `esp_bt_controller_enable()`.** The
    IDF example relies on sdkconfig agreeing between the two calls; ESP32
    rejects the enable when they disagree (`btdm_controller_get_mode()` in
    `components/bt/controller`). Set the field explicitly.
  - **AVRCP is not a luxury for a source.** A TV, soundbar or Android device in
    A2DP-source mode routinely sits "connected, streaming, silent" until it is
    sent a PLAY passthrough. `esp_avrc_ct_init()` must run **before**
    `esp_a2d_source_init()` and `esp_avrc_ct_deinit()` **after**
    `esp_a2d_source_deinit()` — the IDF documents that order. Optional: a failure
    there is logged and stepped over, never load-bearing.
  - **Sends must stay under the MTU, not at it.** The stack adds its own AVDU
    media header on top of the PCM (`A2DPEncoder.h`: `payload_header_size()` is
    1 for SBC), so `data_len == mtu` asks for one byte more than the L2CAP
    channel was negotiated for. `A2DP_SEND_GUARD` is 8. And
    `ESP_ERR_INVALID_SIZE` is refused *every* time, so an overshoot must shrink
    the MTU and continue — the old code gave up on the first one.
  - **`s_tune_done` is sticky, and the tick acts on it by MOVING THE SCREEN.**
    It is only cleared by `start_feed()`. So acting on it from every screen
    meant BACK from the tune list was undone 500 ms later and the user could not
    leave at all — reported as *"it flashes into another screen and comes back
    into the audio selection"*. It is now cleared by
    `svc_a2dp_clear_tune_done()` as soon as the UI acts, and only acted on while
    the PLAY screen is up. **Any sticky flag a tick acts on by navigating must
    be cleared the moment it is consumed.**
  - **Six screen states, not two.** `svc_a2dp_state_name()` used to say "linked"
    for anything merely CONNECTED, which is exactly what a sink that has
    refused to start its media stream looks like. `A2DP_LINK_*` now separates
    down / idle / wait / media / streaming / failed, and a named fault
    (`tx stalled`, `mtu`, `bad handle`) overrides the generic wording. A
    failure that cannot be read on the screen cannot be debugged when the log
    bus is off.
  - **Producer pacing lives in `main/a2dp_pace.h`, shared with its test.**
    `tools/hk_a2dp_pace.c` compiles that header, so the schedule is measured
    rather than asserted. Two arithmetic facts it caught, both invisible from
    outside: 147 frames is 3333.33 µs, so summing TRUNCATED per-chunk durations
    loses 1 µs every three chunks (441735 frames handed over where 441000 is
    real time, in a simulated 10 s) — the due time must come from the
    **cumulative** frame count; and the chunk size only works because 3 × 147 =
    441 = exactly one 10 ms tick at 44.1 kHz, where the old 168 needed three
    sends to cover 504 frames and ran ~4 % fast. **Do not "tidy" these numbers.**
  - **RTTTL violates its own spec constantly.** The parser accepts the dotted
    '.' **before or after** the octave (most real files use before), lowercase,
    embedded spaces, `h`/`H` for B, `p` for rest, octaves outside 4-7 and
    names over 10 chars. A spec-strict parser fails on the first real tune.
    `tools/hk_rtttl.py` exists to test against real strings, not tidy ones.
  - **The PCM staging buffer is `int16_t[]`, not `uint8_t[]`.** Xtensa faults
    on an unaligned halfword load and a `uint8_t` array is only 1-byte aligned.
  - **`tune_tables.h` and `tune_data.h` are generated** by
    `tools/gen_tunes.py` from `tunes/*.rtttl`. Never hand-edit, like
    `font_oled.h`.
* **`svc_nrf.*` / `ui_nrf.*` — 2.4 GHz spectrum analyser (menu item 10)**,
  an external nRF24L01+ on SPI3. Wired CE=26, CSN=14, MOSI=23, MISO=19,
  SCK=18. Reference project: `ESP32-RF-Spectrum-Scanner`
  (dkydivyansh, GPL-3.0); register-level behaviour cross-checked against
  Nordic's datasheet and the `nRF24/RF24` driver source. Parts that bite:
  - **NOT RUN ON HARDWARE YET.** The driver is register-correct and builds
    clean, but no nRF24 has been attached. Treat the first on-device run as
    bring-up, not as a regression test, and expect to fix pin or timing
    details before trusting any trace.
  - **RPD IS ONE BIT, NOT dBm.** The nRF24L01+ has no RSSI register. The
    only power measurement it exposes is the Received Power Detector:
    register **0x09 bit 0**, a single comparator bit that goes high above a
    threshold (-64 dBm at 1/2 Mbps, -83 dBm at 250 kbps). A "level" here is
    a **hit rate over repeated samples**, 0..`svc_nrf_samples()`. The screen
    prints the scale (`hits 6/8`) and never labels the axis dBm. Labelling
    it dBm would be inventing a number the chip never produced - the same
    error class as presenting the sniff rate as absolute (see the RX
    callback note above).
  - **RPD IS LATCHED — this is the trap that decides whether the feature
    works at all.** The status holds until a packet arrives or the host
    pulls CE low. Reading it N times inside one RX period returns the SAME
    bit N times, so the obvious "startListening() then loop N reads" shape
    yields 0 or N and no gradation whatsoever — the reference project has
    exactly this bug (10 "samples" that return 0 or 10). `svc_nrf_step()`
    re-enters RX (CE low→high) before **every** sample and waits
    Tstby2a+Tdelay_AGC = 130+40 us for the AGC. That is what turns one bit
    into N+1 usable levels, and it is why a sweep costs ~190 us/sample.
  - **Not STATUS bit 5.** Widely repeated online and wrong for this use.
    `STATUS` (0x07) bit 5 is `TX_DS`; RPD is register 0x09 bit 0, confirmed
    against `nRF24/RF24` (`nRF24L01.h`: `constexpr uint8_t RPD = 0x09`,
    `testRPD()` → `read_register(RPD) & 1`). Using bit 5 reads TX_DS and
    reports a transmit flag as RF power.
  - **`EN_RXADDR` is a separate register (0x02), not CONFIG bits 6:7.** CONFIG
    bits 6/5/4 are the IRQ masks. For a measurement receiver, `CONFIG_RX` is
    `PRIM_RX | PWR_UP | MASK_MAX_RT | MASK_TX_DS | MASK_RX_DR` = **0x73**,
    `EN_RXADDR` = `0x01` (RPD does not respond with every pipe disabled),
    `RF_SETUP` = `LNA_HCURR` plus the data rate — LNA_HCURR is the difference
    between a receiver that hears the band and one that does not.
  - **CSN is driven BY HAND, and the pairing is load-bearing.** The device
    config asks the driver for `spics_io_num = -1` because GPIO14 is not
    SPI3's IO_MUX CS (that is GPIO5). Nothing else in the stack will ever
    select the radio, so **every** bus access must be wrapped in
    `cs_low()`/`cs_high()` inside `xfer2()`/`write_addr()`. An unpaired
    access leaves the chip deselected for ever and every read returns the
    bus idle value 0xFF — indistinguishable from an absent chip, so the
    presence probe would fail for the wrong reason.
  - **Two of the six buttons ARE the radio.** CSN=GPIO14 is `PIN_BTN_OK`
    and CE=GPIO26 is `PIN_BTN_RIGHT`. Consequences, all real:
    - `hacku_input_init()` makes GPIO14 an input with a pull-up at boot, so
      `svc_nrf_init()` must **reclaim the pin as an output** — which means
      the analyser cannot be initialised before `hacku_input_init()` runs.
    - Both pins go into `hacku_input_set_button_mask()` while the screen is
      live. Masked buttons are neither sampled as keys nor able to drag CSN
      to ground mid-transaction (which would merge every following SPI
      frame into garbage and spike current through the button).
    - **OK and RIGHT are therefore physically dead on this screen.** They
      still arrive over the caster, which is an independent path. The
      on-screen legend deliberately does **not** mention RIGHT: a hint
      naming a key that cannot physically fire there is a lie about what
      the device can do. `teardown_radios()` is the only place that clears
      the mask, so OK/RIGHT cannot stay dead for the rest of the session.
  - **RF self-interference is not optional housekeeping.** The ESP32's own
    Wi-Fi and BT are in the same band and would light every column of the
    trace. `enter_menu_item(9)` tears Wi-Fi and BT down before the screen
    opens, and `ui_nrf_run()` repeats both so the screen is safe to enter
    from anywhere.
  - **Sweeps are INCREMENTAL, on a 200 ms tick, not one blocking pass.** A
    full 125-channel sweep at 8 samples is ~150 ms of solid SPI and busy
    wait; doing that in one call would stall the key loop for the whole
    sweep. `ui_nrf_tick()` advances `STEP_CHANNELS` (24) per tick instead,
    which keeps input responsive and gives the screen a moving sweep cursor
    for free. `NRF_TICK_MS` is its own cadence, deliberately faster than
    every other live screen's 500 ms.
  - **The waterfall history is `uint64_t`, not `uint32_t`.** A column is
    `PLOT_H` = 36 rows deep and `1 << 35` is not representable in Xtensa's
    32-bit `unsigned long`, so a `uint32_t` column is both an overflow and a
    silent truncation. 125 × 8 B = 1 KB static. History advances once per
    *completed sweep*, so all columns scroll in lockstep.
  - **The plot scale is FIXED at 0..samples, not auto-ranged.** An autoscale
    makes a quiet band look busy and stops two screenshots taken a minute
    apart from being comparable — the same reason the background status bar
    has no jitter. Changing `svc_nrf_samples()` or the rate resets the trace
    rather than rescaling it, because old samples against a new scale are
    the quietest kind of wrong.
  - **Decoupling is mandatory on the module.** Add 10 µF across the nRF's
    VCC/GND at the module. Without it the chip browns out on SPI activity
    and the presence probe fails intermittently — which looks exactly like
    bad wiring.
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

* `tools/hk_probe_victim.py COMx [secs] [--load] [--control]` — the only
  attack test here that measures **effect** rather than transmission. It
  watches the PC's own adapter and reports *time on target*: the fraction of
  samples still associated with the target AP on 2.4 GHz. `--load` holds a
  ~5.6 MB/s download for a real baseline, `--control` runs the whole
  sequence with nothing launched. **Always run `--control` first** -
  disconnects happen on their own, and one observed drop proves nothing
  without it. Three traps it encodes:
  - `netsh` blanks SSID/Band/Channel while associating. Treat that as *no
    information*, never as a state - feeding the blanks in turned three
    ordinary reconnects into phantom "band hop to 5 GHz" events, which is
    how a real effect nearly got published with a false mechanism.
  - `netsh` costs ~1 s per call, so the real sampling interval is ~1 s, not
    the 0.4 s `POLL` suggests. Report the measured rate; a drop shorter than
    the interval can be missed entirely.
  - Event times must be stamped relative to launch, or phase A (0-20 s,
    beacon only) and phase B cannot be told apart. This is what proved the
    PowerKill drops were all phase B.
  Cloudflare's `__down` endpoint silently returns a **1-byte body** above
  ~20 MB instead of erroring, so it looks like an instant download; keep the
  request at 20 MB and repeat it inside ONE curl process.
* `tools/hacku_viewer.py --port COMx` — live display viewer + remote keys
  (`wasd`/`e`/`b`). `tools/hk_test.py --port COMx [--cycles N]` —
  headless pass/fail incl. multi-cycle heap screenshots.
  `tools/hk_test_deauth.py` — deauth flow. `tools/hk_log.py` — boot log
  over UART0 @ 115200 (**early boot only**: once the always-on caster
  starts, logs are silenced so they cannot corrupt the PKC stream). `tools/hk_wire.py`,
  `tools/hk_px.py`, `tools/hk_anchor.py`, `tools/hk_palette.py` —
  caster-wire and pixel probes. All accept `--port`, auto-detect if
  omitted, and run on Windows/Linux/macOS.
* `gcc -O2 -Wall -o /tmp/pace tools/hk_a2dp_pace.c -I main && /tmp/pace` —
  the A2DP producer's real-time schedule. **No hardware and no serial port
  needed, and it is the only place these properties can be observed**: it
  includes `main/a2dp_pace.h`, the same header the firmware's producer task
  uses, so it measures the real arithmetic rather than a copy of it. Checks
  exact tick boundaries, zero drift, stall recovery, the MTU clamp and the
  no-busy-loop property. Needs only a C compiler — unlike `hk_rtttl.py`,
  which wants MSVC. Run it before touching any of the numbers in
  `a2dp_pace.h`.
* Test PNGs (`test_mem*`, `test_out`, `test_deauth`, `tools/probe_*`) are
  throwaway and gitignored — never commit them.

## Git

GitHub `Velmet44/HackUlator`, branch `main`. Commit/push and cut
releases only when explicitly asked. `firmware/*.bin` are tracked release
artifacts — refresh from a verified build before a release commit.
