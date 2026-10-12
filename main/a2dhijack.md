# Audio Device Hijacker — implementation notes

**Status: the silence bugs are fixed; PCM streaming is still UNVERIFIED on
hardware.** Everything below records what was measured, what was fixed, and what
is still open. It is written so the next person does not repeat the expensive
mistakes.

## What it does

Menu item 9, "Audio hijack". Connect to a Bluetooth audio sink as an **A2DP
source** and play coded tunes on it. RTTTL melodies synthesised on the ESP32 —
no audio files, no decoder, no SD card.

```
menu -> 9 -> AUDIO HIJACK -> SPEAKERS -> LINKING -> TUNES -> PLAYING
```

## The report that started this pass

> it connects but does not play sound. it flashes into another screen and comes
> back into the audio selection

Both halves share one root cause class: **a state machine that was given exactly
one chance to run, and no way to report that it had failed.** Every fix below is
grouped by that theme because every one of them is the same bug.

### A. "It flashes into another screen and comes back"

`s_tune_done` is sticky — only `start_feed()` ever cleared it — and
`ui_a2dhijack_tick()` acted on it from *every* screen by forcing
`s_screen = SCR_TUNES`. So once any tune had ended, pressing BACK from the tune
list was undone by the very next 500 ms tick, and the user could not leave the
screen at all. The only escape was the one-button hold, i.e. go home.

Two independent fixes, both required:

- the tick only acts on the flag while it is **on the play screen**
- `svc_a2dp_clear_tune_done()` exists, and the UI calls it as soon as it has
  acted, so the flag cannot fire a second time

Same shape as the `ui_bleatk.c` return-code trap and the collateral profile
desync in `AGENTS.md`: one piece of state, two owners, one of which assumed it
was alone.

### B. "Connects but does not play sound"

Six separate reasons, all with the identical symptom — link up, screen
cheerful, no audio, and **no diagnostic**, because the always-on caster
suppresses the log bus by design:

| # | what happened | fix |
|---|---|---|
| 1 | `CHECK_SRC_RDY` was sent **once**, from the connect callback. A sink answering `BUSY`/`FAILURE` that first time — normal for one still opening its own end — left the link up with the media machine stopped for good. | `svc_a2dp_pump()` re-asks every 2 s while useful. The single most important change here. |
| 2 | **`ESP_BT_GAP_CFM_REQ_EVT` was never handled.** SSP raises a confirmation request the application *must* answer with `esp_bt_gap_ssp_confirm_reply()`; there is no default. Every headset from the last decade uses SSP, so pairing sat until it timed out. | Answered unconditionally (own lab kit). Plus `ESP_BT_SP_IOCAP_MODE` = `ESP_BT_IO_CAP_NONE`, `esp_bt_gap_set_pin()`, and `min_16_digit` honoured in the PIN reply. |
| 3 | `START` acknowledged but `AUDIO_STATE_STARTED` never arrived — nothing timed it out, nothing retried. | `MEDIA_START_MS` watchdog → `MEDIA_IDLE` → re-ask. |
| 4 | Four silent `break`s in the producer killed the task on an alloc failure or a refused send; the screen said "notes 0" and nothing else. | Each now retries against a bounded stall counter, and if genuinely stuck sets a **named** reason that is displayed: `tx stalled`, `mtu`, `bad handle`, `out of mem`. |
| 5 | Sends used exactly the reported MTU. The stack adds its own AVDU media header on top, so that is one byte more than the channel was negotiated for → `ESP_ERR_INVALID_SIZE` → and the old code quit on the *first* one. | `A2DP_SEND_GUARD` = 8 bytes of head-room; an overshoot **halves the MTU and continues** instead of killing the tune. |
| 6 | No AVRC controller. TVs, soundbars and Android devices in A2DP-source mode routinely sit "connected, streaming, silent" until sent a PLAY. | `esp_avrc_ct_init()` + `esp_avrc_ct_send_passthrough_cmd(PLAY)`, initialised **before** A2DP and deinitialised after — the order the IDF documents. Optional: failure is logged and stepped over. |

**Bounded insistence, deliberately.** The pump does *not* re-ask forever. A
speaker that suspended the stream because somebody pressed pause **on the
speaker** would be restarted a second later, which is worse than the bug. It is
insistent only inside `MEDIA_GRACE_MS` of the link coming up, or while the user
has actually asked for audio and it has not arrived.

### C. The screen used to lie

`svc_a2dp_state_name()` returned `"linked"` for anything merely CONNECTED. Six
distinguishable states now, on the tune list, the linking screen and the playing
screen: `down / not linked / linking... / no stream / streaming / <reason>`.
**A failure that cannot be read on the screen cannot be debugged**, and the log
bus is off by default on this feature.

A `LINKING` screen was added between picking a speaker and the tune list: the
connect is asynchronous and pairing can take tens of seconds, so the old code
dropped the user onto a tune list for a speaker that was not connected.

## Pacing — a measurable bug, found by measuring

The old producer counted frames into a 441-frame (10 ms) budget and
`vTaskDelay(1)` per window. The chunk was 672/4 = **168** frames, so a window
closed on 504 frames — **11.4 ms of audio handed over in 10 ms of wall clock.**
Every tune ran ~4 % fast and the error compounded, because the delay was a tick
(and jitters) while the budget was never re-derived from real time.

Fixed in two layers.

**`main/a2dp_pace.h`** holds the whole schedule as pure arithmetic, header-only,
so `tools/hk_a2dp_pace.c` compiles **the same functions the firmware runs**
instead of a copy. `AGENTS.md` records twice that a hand-synced copy is exactly
the failure mode this project has already been bitten by.

* Chunk is now **588 bytes / 147 frames = 3.33 ms**. Three of them is 441
  frames — exactly one 10 ms tick at 44.1 kHz.
* The due time comes from the **cumulative** frame count, never a per-chunk
  duration. Measured, not reasoned about: `147e6/44100` is 3333.33 µs and
  truncation makes it 3333, so summing truncated chunks loses a microsecond every
  three of them — a simulated 10 s run handed over **441735** frames where
  441000 is real time. One division at the end makes every tick boundary exact
  and the error cancels.
* A **lead** runs the producer ahead of real time (a source exactly on time
  starves the sink's jitter buffer the moment one send is a tick late), and a
  **lag limit** rebases rather than flushing a backlog after a long stall.

Host model, `gcc -O2 tools/hk_a2dp_pace.c -I main`:

```
  147 x 3 = 10000 us, 147 x 300 = 1000000 us (one tick = 10000)
  realistic stack: lead 10633 us at 10 s, 10633 us at 60 s
  free stack: lead 10000 us after 10 s (1000 sleeps)
  jittery stack (0.9 ms per send): lead 10633 us after 60 s
  12 x 90 ms stalls: 11 resyncs, worst burst 9, lead 9934 us
```

The identical lead at 10 s and at 60 s **is** the no-drift proof. It is a
statement about arithmetic, not about sound.

## Other fixes found in the same pass

- **Inquiry could not work at all.** `esp_bt_gap_start_discovery()` only issues
  the HCI_Inquiry; it does *not* make the local device discoverable. The window
  is now opened with `esp_bt_gap_set_scan_mode(CONNECTABLE,
  GENERAL_DISCOVERABLE)` before the search and closed on `DISCOVERY_STOPPED`.
- **`cfg.mode` now matches `esp_bt_controller_enable()`.** The IDF example relies
  on sdkconfig agreeing between the two calls; the reference library sets the
  field explicitly because ESP32 rejects the enable when they disagree
  (`btdm_controller_get_mode()`).
- **Scan mode goes non-discoverable once connected**, which is what the
  reference library does and why: leaving the device discoverable during a source
  connection is how the peer ends up racing us for the link.
- **`svc_a2dp_disconnect()` waits for the link to actually come down** (bounded),
  like `svc_a2dp_stop()`. Clearing the flags on request rather than in the
  callback meant the next `esp_a2d_source_connect()` raced a live ACL.
- Dead `s_msg[40]` removed from `ui_a2dhijack.c` (written, never read).

## RAM

Measured delta of this whole pass against the same tree with only these files
reverted (`idf.py size`):

```
DRAM   99448 -> 99408    (-40: the dead s_msg[40])
IRAM  114863 -> 114863   (unchanged)
flash  +3988 B           (the AVRCP controller was not linked before)
```

AVRCP costs a few KB of **runtime** heap when `esp_avrc_ct_init()` runs, on top
of the ~41 KB the A2DP profile already needs. On this screen WiFi is torn down
entirely (`enter_menu_item(8)`), so the largest-block figure is far above the
measured 100 KB with both radios up. The 16 KB floor is untouched.

## The hardware limit, stated once

**Classic BT only. There is no BLE audio and there never will be.** The ESP32
radio is a BLE 4.2 controller with no LE Audio, no LC3, no BAP. True-wireless
earbuds on the LE Audio path are unreachable and no amount of software changes
that — it is silicon.

What does work: car stereos, older/budget headphones, older TVs and soundbars,
anything exposing a **classic A2DP profile with SBC**.

SBC is the only codec this stack encodes; the A2DP MPEG-1/2 CIE structs in
`esp_a2dp_api.h` are marked *"Not supported yet"*. SBC is mandatory for every
A2DP sink, so nothing is lost.

## RAM — measured, not estimated

The project gates on the **largest contiguous free block**, not total free, and
its floor is 16 KB.

Measured on this board, on-device, boot → controller → A2DP source → WiFi:

| stage | largest | total free | Δfree |
|---|---:|---:|---:|
| A boot | 110592 | 196184 | — |
| B `esp_bt_controller_init` | 110592 | 177032 | −19152 |
| C `+enable(BTDM)` | 110592 | 176944 | −88 |
| D `bluedroid_init/enable` | 110592 | 155924 | −21020 |
| E `+a2d_source_init` | 110592 | 154956 | **−968** |
| F `+esp_wifi_init` (coex) | **102400** | 126412 | −28544 |

**The binding metric barely moves: 108 KB → 100 KB with both radios up, against
a 16 KB floor.** The reason is visible in the boot log's own heap layout: the
108 KB block at `3FFE4350` sits in D/IRAM, and BT allocates out of the smaller
DRAM blocks (43/16/7 KB) instead. Classic BT eats ~40 KB of *total* free without
touching the block the firmware gates on.

Component costs:

| | bytes |
|---|---:|
| Classic BT + A2DP + AVRCP, **static** (always linked) | +25996 |
| this feature's own code, **static** | +1408 |
| BT controller + Bluedroid + A2DP, **runtime** | ~41000 |

The whole feature's own RAM cost is **1,408 bytes** — a 1 KB PCM buffer plus
state. The parser, the synth and the sine table are all `const` and therefore
flash-resident. The 41 KB runtime cost is only paid while a speaker is playing.

Per-symbol static growth, for anyone wondering where the 25 KB goes:

```
 9680  a2dp_sink_local_param   <- the A2DP SINK role; this device never uses it
 1948  rfc_cb        1696  avdt_cb      1244  l2cb
 1304  btm_cb        944  bta_dm_cb     840  sdp_cb
 1024  g_disc_raw_data_buf (inquiry)   1024  up_sampled_buffer (SBC)
 1752  a2dp_source_local_param          720  smp_cb
```

## Bugs found and fixed, in the order they bit

### 1. `sdkconfig.defaults` is only read on a fresh configure
Deleting `sdkconfig` (gitignored) is mandatory after changing it. Otherwise an
existing `sdkconfig` silently overrides the defaults and **every Classic-BT
symbol fails to link**, with no error pointing anywhere near the cause. This
cost a full build cycle.

### 2. `esp_bt_controller_mem_release()` had to be removed
`main/app_main()` called `esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT)`.
It is a **one-way door**: once it runs the controller can never return that RAM,
which makes A2DP impossible. It is gone; do not put it back.

### 3. Discovery's two halves were fighting each other
`svc_a2dp_discover()` ran the BLE scan first, and `svc_a2dp_find_by_ble()` ended
with `esp_bt_controller_deinit()`. The classic inquiry that ran immediately
afterwards was **posting requests at a stack that no longer existed**. Inquiry
could never have worked, and it presented exactly like a device that was not
discoverable. Both halves now share one `svc_a2dp_up()`.

Same bug shape as the collateral profile desync in this repo: two owners of one
piece of state, each assuming it is alone.

### 4. `esp_a2d_source_audio_data_send()` does not copy the buffer
`btc_av_audio_buff_alloc()` hands out a `BT_HDR` with the payload **inside**
that allocation, and the caller's `esp_a2d_audio_buff_t` overlays it. On
`ESP_OK` the **stack owns it** and it must never be freed. A static buffer
corrupts the BT queue and presents as *audio corruption*, not a crash.

Caught by studying `pschatzmann/ESP32-A2DP` — the IDF's own `a2dp_source`
example never calls this function at all, so there was nothing in-tree to copy.
Correct pattern: allocate per send, hand over on success,
`esp_a2d_audio_buff_free()` on refusal.

### 5. The producer must be paced against the sample clock
Sending as fast as the stack accepts starved the idle task and tripped the task
WDT inside `feed_task` (decoded backtrace confirmed
`task_wdt_timeout_handling → feed_task`). Fixed by counting frames against a
10 ms budget (441 samples) and `vTaskDelay(1)` per window. **An A2DP source is a
real-time producer**, not a throughput benchmark.

### 6. `start_feed()` must NOT be gated on `s_media_ready`
Media starts asynchronously: connection → `CHECK_SRC_RDY` → `START` →
`AUDIO_STATE_STARTED`. It is normally not ready at the instant the user presses
play. The task already waits internally, so gating the start on it meant the
tune **never began and never retried**, with no error shown anywhere.

### 7. Disconnect is asynchronous
`esp_a2d_source_disconnect()` only posts a request; the reply arrives in the
profile being deinit'd. `svc_a2dp_stop()` waits (bounded, 50 × 20 ms) for
`s_connected` to clear before unwinding. Same fault class as `svc_deauth`'s
orphan TX timer, one layer up.

### 8. `stored_load()` must not be hidden inside `svc_a2dp_up()`
The pair screen asks *"is there a stored speaker?"* to decide between reconnect
and discovery — and that check happens **before** the stack is brought up. So
loading NVS inside the bring-up meant the question always answered "no" and the
screen always ran discovery. `svc_a2dp_load_stored()` is public and called when
the screen opens.

### 9. `MAX_TUNE = 400` in `gen_tunes.py` silently truncated a full melody
RickRoll is 1,297 bytes of RTTTL. The cap cut it to a third, producing a tune
that simply stopped halfway with no diagnostic anywhere. Raised to 2048.
**A cap that silently corrupts data is worse than no cap.**

### 10. Xtensa alignment
The PCM staging buffer is `int16_t[]`, not `uint8_t[]`. A `uint8_t` array is
only 1-byte aligned and Xtensa **faults on an unaligned halfword load**.

## Return-code trap

`A2DH_EXIT_MENU` is `1` and **`0` means STAY**. A sub-handler returning 1 makes
`main.c` eject the user to the top menu — the screen appears to do nothing and
the next keypress lands somewhere unrelated. This is the exact trap `ui_bleatk.c`
fell into. Every path in `ui_a2dhijack_key()` returns 0 unless it means to leave.

## RTTTL parser — deliberately non-conformant

Real RTTTL violates its own specification constantly, so the parser accepts:

- the dotted `.` **before or after** the octave — most files in the wild use
  *before*, the spec says *after*, and a spec-strict parser fails on the first
  real tune anyone hands it
- lowercase letters, embedded spaces, names over the spec's 10-char limit
- `h`/`H` for B, `p` for rest
- octaves outside the documented 4–7
- unknown control keys, which the spec requires be **ignored**, not rejected

Frequency model: equal temperament, `A4 = 440 Hz`, RTTTL octave convention
(A in octave 4 = 440 Hz). Computed by table walk, not `ldexp`/`pow`, because the
ESP32 has no FPU and libm is software floating point.

## Synthesis — integer only, 256-byte table in flash

A `sin()` call would pull in libm and run in software float. Instead: a
256-entry **signed-byte** sine table in `.rodata`, which is flash-mapped on the
ESP32, so it costs **zero RAM**. Driven by a 32-bit fixed-point phase
accumulator with the top 8 bits as the table index.

The accumulator is not decoration: an integer *index step* at 44.1 kHz for
440 Hz over 256 entries is 2.55, so the step would have to alternate between 2
and 3 forever and every note would be audibly out of tune. Accumulating in 32
bits keeps the error below 1 Hz.

5 ms attack / 10 ms release. A raw gate clicks at every note boundary, which on a
speaker in a room is the first thing anyone hears.

**Mono in, stereo out.** `btc_a2dp_source_pcm2sbc_init()` promotes a Mono
negotiation to Joint Stereo regardless — *"Some AV Headsets do not support
Mono"* — so a mono synth would save nothing. One channel is synthesised and
copied to both.

## Discovery, and the pairing-mode reality

Two techniques, one stack bring-up:

1. **BLE advert scan** — a speaker that is merely powered on is invisible to
   classic inquiry, but most broadcast BLE, and the advert's MAC field *is* the
   address `esp_a2d_source_connect()` needs.
2. **Classic inquiry**, filtered on the Class-of-Device **"Rendering"** service
   bit (`esp_bt_gap_get_cod_srvc(cod) & ESP_BT_COD_SRVC_RENDERING`). This is
   also what keeps the list to things audio can actually be sent to — phones and
   laptops generally don't set it.

### What was actually measured, on a real headset

`boAt Rockerz 512 ANC`, BT 5.4, paired to a Windows PC:

| state | discovery result |
|---|---|
| powered **off** | nothing (obviously) |
| powered on, **not connected to anything** | **nothing** |
| off → hold MFB 3 s → pairing mode (blue/red alternate) | **nothing** |

The third row was never obtained cleanly because the headset **auto-shuts-off
on pairing timeout** (boAt docs: *"Power Off: Red LED flashes for 1 second"*),
and there is no setting to lock pairing mode on consumer hardware.

**Conclusion, and it is a protocol property rather than an ESP32 limitation:**
a Bluetooth device that is neither discoverable nor advertising is **unfindable
by any scan**. There is no third mechanism. Connecting to a **known address**
needs neither, which is why `HACKU_A2DP_DEFAULT_PEER` exists — it is the
no-pairing-mode path, and it is **empty by default** because a baked-in address
is specific to one speaker (and would leak that speaker's MAC into a public
repo).

Once a bond exists, `esp_a2d_source_connect()` on the stored address works with
the speaker simply switched on. The address is persisted in NVS.

## Verified vs unverified

**Verified on hardware:**

- firmware builds, links, flashes, boots clean, no panic
- all screens render (`AUDIO HIJACK`, `SPEAKERS`, `LINKING`, `TUNES`, `PLAYING`)
- BT stack + Bluedroid + A2DP source bring up, all init calls `ESP_OK`
- `esp_a2d_source_connect()` is issued without error
- feature RAM cost 1,408 bytes static; 108 KB largest free block with BT live
- 16/18 of the pre-existing regression checks still pass (WiFi scan, detail,
  calculator, unlock, relock all unaffected)
- two watchdog/panic bugs found and fixed by on-device backtrace decoding

**Verified on the host, this pass (`tools/hk_a2dp_pace.c`):**

- the producer's schedule is exactly real time, with zero drift (identical lead
  at 10 s and at 60 s), bounded bursts, and no busy-loop
- every tick boundary of the send cadence is exact — 441 frames is 10000 µs
- the MTU clamp respects the AVDU header guard and whole stereo frames

**NOT verified — the open items:**

- **A2DP has never completed against real audio gear.** No speaker was available
  during this pass, so the handshake is still untested on silicon; what changed
  is that it now retries, times out, and says which step failed.
- **PCM streaming has never actually played audio.** Every code path up to the
  send call is exercised; audio coming out of a speaker is not. The screen now
  distinguishes `no stream` from `streaming` and names any tx fault, so the next
  session on real gear produces an answer instead of silence.
- **`CONFIG_BT_SMP_ENABLE=y`** appears in the regenerated `sdkconfig` where
  `sdkconfig.defaults` says `n`. A Kconfig defaulting quirk (the symbol is
  invisible and tracks `BT_BLE_SMP_ENABLE`, which the defaults set to `n`).
  Costs RAM; believed harmless; **unexamined**.

**The fastest way to settle the first two:** run it and read the PLAY screen.
`stream notes N` means the media state machine completed and bytes are moving.
Anything else now names the step that failed — `no link`, `no stream`, `tx
stalled`, `mtu`, `bad handle`, `sink busy`. That word is the whole diagnostic,
and it did not exist before this pass.

## Known dead ends, so they are not re-tried

- **`ESP_A2D_REPORT_SNK_CODEC_CAPS_EVT` does not exist in IDF 5.5.** The
  5.3-era example references it; only `ESP_A2D_REPORT_SNK_DELAY_VALUE_EVT` is
  present. The case was removed rather than left as dead code.
- **`ESP_A2D_MCC_1_SOURCE` is not in the public API.** With the default
  (non-external-codec) configuration Bluedroid pre-registers the mandatory SBC
  endpoint, so `esp_a2d_source_register_stream_endpoint()` should **not** be
  called at all.
- **`esp_ble_gap.h` no longer exists.** IDF 5.x split it; use
  `esp_gap_ble_api.h`. Types are `esp_gap_ble_cb_event_t`, and scan results
  arrive **one device per event** as `ESP_GAP_BLE_SCAN_RESULT_EVT` with a raw
  `ble_adv` EIR blob — there is no `num_res`/`res` batch array.
- **minimp3 is not usable on ESP32.** Floating point, no SIMD. pschatzmann
  measured it directly: *"too slow to decode any audio ≥ 32 kHz with poor
  quality."* Fixed-point libhelix is the correct decoder if MP3 is ever added.

## Adding tunes

```
tunes/*.rtttl              one tune per file, plain text
tools/gen_tunes.py         -> main/tune_data.h (generated, never hand-edit)
```

Built in: Simpsons, StarWars, NokiaTune, Tetris, FurElise, BachPrelude,
SmokeAlarm, ScaleTest, DroidBeep, RickRoll. **3,038 bytes of flash total** for
all ten.

### Converting from an Arduino sketch

```
python tools/arduino2rtttl.py tunes/sources/RickRoll.ino tunes/RickRoll.rtttl
```

Reads the sketch's own `NOTE_*` table (so the two cannot drift), maps
`NOTE_DS5` → `D#5` and negative durations → dotted notes, and takes `tempo`
from the sketch unless overridden. The tempo arithmetic is *identical* between
the two formats, so the conversion is exact:

```
Arduino:  wholenote = (60000 * 4) / tempo      note = wholenote / divider
RTTTL:    whole_ms  = 240000 / bpm              note = whole_ms / d
```

RickRoll: 349 notes → 1,297 bytes of RTTTL. Original sketches are kept in
`tunes/sources/` so any conversion is reproducible. This makes the
robsoncouto/arduino-songs library a usable source wholesale rather than hunting
for RTTTL versions of each tune.

### Where to get more

| source | what |
|---|---|
| picaxe.com/rtttl-ringtones-for-tune-command | zips: 450, 375, **10,000 tunes**, 50 TV themes, 70 Christmas |
| rtttl-hub.io | browsable library, MIDI→RTTTL converter, FPV-ESC startup tunes |
| rtttl.com/tools/midi-to-rtttl | type-0 and type-1 MIDI, up to 2 MB |
| ringtone.vulc.in | Merwin's search over old Nokia ringtones |

**MP3, if it is ever wanted:** libhelix fixed-point only, ~25–30 KB RAM, and
files must be stripped with
`ffmpeg -vn -map_metadata -1 -id3v2_version 0 -ar 44100 -ac 2`. `-vn` is the one
that matters — cover art is stored as a *hidden video stream* in the MP3
container and desyncs on-device decoding.

## Testing

```
python tools/hk_rtttl.py                      # parser + synth, on the PC
python tools/hk_rtttl.py --wav out.wav "..."  # render a tune to listen to
gcc -O2 -Wall -o /tmp/pace tools/hk_a2dp_pace.c -I main && /tmp/pace
python tools/hk_a2dhijack.py --port COM5      # screen flow on hardware
```

`hk_rtttl.py` host-compiles `tune_rtttl.c` and `tune_synth.c` with MSVC (found
via `vswhere`, env captured from `vcvars64.bat`) and checks them against 15 real
RTTTL strings, 7 reference pitches, and malformed input. It is a real test, not
a smoke test — the corpus is deliberately full of spec violations.

`hk_a2dp_pace.c` needs no toolchain beyond a C compiler: it includes
`main/a2dp_pace.h`, the header the firmware's producer task uses, so it tests
the real schedule rather than a copy of it. See "Pacing" above for what it
proves and why a host test is the only place these properties can be observed.
