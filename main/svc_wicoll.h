#pragma once

/* Collateral: continuous wideband 802.11 TX used to disrupt in-range
 * Bluetooth LE, Bluetooth Classic and WiFi links.
 *
 * WHY THE WIFI RADIO AND NOT BLE:
 * The BLE controller's only raw-TX mode (DTM) is 2 MHz wide, capped at
 * +9 dBm, and permanently wedges BLE advertising. The WiFi radio emits
 * 22 MHz at +19.5 dBm - eleven times the bandwidth and ten dB more power -
 * so one WiFi channel covers eleven 2 MHz BLE channels at once. That is the
 * entire basis of this service; the BLE controller is never touched.
 *
 * MEASURED on ESP32-WROOM / IDF 5.5.5, 2026-10-11. Every figure below was
 * observed on the device, not taken from a datasheet.
 * Full provenance in TEMP/collatoralspec.md.
 *
 * THE THREE THINGS THAT ACTUALLY DECIDE WHETHER THIS WORKS, all measured:
 *
 * 1. RATE AND FRAME LENGTH. Real duty went 4% -> 97%. The previous build
 *    reported "61%" from a hardcoded 1492 us airtime that assumed 1 Mbps
 *    while nothing forced the radio there, so the driver ran a fast OFDM
 *    rate and each frame occupied ~60 us instead of 18884. Forcing
 *    WIFI_PHY_RATE_1M_L and raising the frame to 2346 B (aMaxFrameSize)
 *    fixes both ends: ~18.9 ms per frame, three times a Bluetooth DH5
 *    packet (6.25 ms), so no BT packet starting inside one can complete.
 *    The on-screen figure is only trustworthy because the rate is forced to
 *    match the model. Do not "optimise" the rate away.
 *
 * 2. DO NOT CALL esp_wifi_set_max_tx_power(78). Writing the maximum makes
 *    this driver report 58-59 (14.7 dBm); left alone it reports 78 (19.5
 *    dBm) at every PHY rate. The write is the fault, not the hardware -
 *    4.8 dB, about 3x in power. Read first, write only if genuinely low.
 *    Chasing this as a "module ceiling" and then a "regulatory country
 *    clamp" cost two hours; both were imaginary.
 *
 * 3. YIELD ON A CLOCK, NOT A FRAME COUNT. The old rule was "every 64
 *    frames", tuned when the loop ran at ~565 fps. After fix 1 it runs at
 *    ~54 fps, so the same 64 frames became 1.19 s of uninterrupted
 *    hammering and the screen froze and jumped. Same class of bug as the
 *    hardcoded airtime constant: a timing constant tuned against a rate
 *    that then changed underneath it.
 *
 * STILL UNVERIFIED: whether HT40 actually widens a raw non-HT frame on this
 * driver. An earlier header asserted it does not, backed only by a duty
 * measurement - and duty is airtime over elapsed, which cannot distinguish
 * 20 MHz from 40 MHz at all. That was not evidence for anything. bw40 is
 *    switchable from the UI and the default is HT20 until an on-device A/B
 *    says otherwise.
 *
 * MEASURED ON A VICTIM'S OWN LINK, 2026-10-11, 60 cm, AP on channel 1
 * (JioFiber 2.4 GHz, reported signal 100%, victim rx rate 72.2 Mbps).
 * This is the first measurement in this file that is about EFFECT rather
 * than about the jammer's own counters, and it is worth reading the two
 * caveats before quoting the numbers.
 *
 *   Lock: control p50 36-48 ms -> jammed p50 122-146 ms, i.e. 3.4x on one
 *   build and 3.54x on another, n=51/52 pooled over 3 paired 15 s windows.
 *   p90 68 -> 383 ms (5.6x). Packet loss 0.0% -> 3.8%.
 *
 * CAVEAT 1 - the control is noisy, which is why only the distribution is
 * quoted. The same control produced a 694 ms outlier with a 35 ms median.
 * A single mean over a handful of samples on this link means nothing; the
 * p50 shift reproduces across every trial and the p90 shift is 5x, which is
 * not. Earlier readings of "no effect at 45 cm" came from the same rig and
 * should be read as n=16, one trial, not as a range measurement.
 *
 * CAVEAT 2 - there is NO calibrated power here. The victim's reported RSSI
 * is a percentage, not dBm, so the actual C/I at the laptop was never
 * measured, only inferred from a "100%" reading. Anything claiming a
 * specific effective range from this data is extrapolating.
 *
 * Earlier at 10 cm this same build showed 3.54x, and 60 cm also shows 3.54x.
 * Six times the distance for no change is NOT what a power-limited jammer
 * should do, and it is the single biggest unexplained result here. Either
 * the mechanism is threshold-based rather than power-based (a decoded frame
 * imposing NAV, which works across a wide range of C/I), or the radiated
 * power is well below the 19.5 dBm the driver reports. Nothing measured so
 * far distinguishes those two, and they imply completely different fixes.
 *
 * WHAT IT DOES AND DOES NOT DO:
 * - Discovery / pairing / reconnection is killable on BOTH Bluetooth
 *   protocols, with no defence to evade, because AFH does not exist before
 *   a link. WiFi beacons are the same story in reverse: a client cannot
 *   hear an AP it never receives a beacon from.
 * - BLE established links will NOT drop. LE has no ARQ; supervision timeout
 *   needs 267 consecutive missed events and a sweep cannot produce them.
 *   Disconnecting an active BLE link is not achievable by any jammer.
 * - Classic established links CAN drop. Verified on hardware: audio CUTS
 *   outright rather than stuttering. Classic ACL retries indefinitely, so
 *   there is no middle state - that is the protocol, not a shortfall.
 * - WiFi is the easiest target of the three. There is no AFH, no channel
 *   map, and nothing to adapt: a client on a jammed channel cannot leave,
 *   because a single-channel 2.4 GHz AP gives it nowhere to go. Throughput
 *   collapses first, then the client gives up on its own retry limits.
 *
 * DEPLOYMENT GEOMETRY (what actually decides the range):
 * A victim hears its peer faintly while a nearby jammer does not. Put the
 * device AT the victim's receiver and it breaks any link whose peer is
 * further away. Measured: works at ~0.3 m from the receiver, nothing at
 * 2 m, and placement - ear cup versus beside the peer - matters as much as
 * power because of antenna nulls and body shadowing.
 */

#include <stdint.h>

/* Target protocol. Selects the mode list, the channel plan and the dwell
 * policy; the TX engine is identical for all three. */
typedef enum {
    WICOL_PROFILE_BLE = 0,
    WICOL_PROFILE_CLASSIC,
    WICOL_PROFILE_WIFI,
    WICOL_PROFILE_COUNT
} wicol_profile_t;

/* Modes are a superset; each profile exposes a subset of them. WiFi has no
 * Discover because it has no undefended advertising channels - a parked
 * beacon channel is discovery denial, which is what Lock already does. The
 * Bluetooth profiles have no All because tiling the band is strictly better
 * than sweeping every channel (see CFG20_ALL).
 *
 * BURST exists because measurement contradicted the design. Lock parks at
 * 100% on the victim's channel and measured the WEAKEST of the strong modes:
 * constant interference lets the client's adaptive rate control settle on
 * 1 Mbps DSSS and survive. Bursty interference does not - the link keeps
 * attempting a high MCS, meets a hard burst, and backs off hard. All is
 * bursty too, but its bursts are 25 ms; these are long, with a deliberate
 * quiet gap after each so the client re-optimises between hits. */
typedef enum {
    WICOL_MODE_FULL = 0,   /* even coverage of the whole band          */
    WICOL_MODE_HOT,        /* survey-weighted dwell, floor on every ch */
    WICOL_MODE_BURST,      /* long hard bursts with quiet gaps between */
    WICOL_MODE_LOCK,       /* park one slab, UP/DOWN picks the channel */
    WICOL_MODE_DISCOVER,   /* advertising / paging only, nothing else  */
    WICOL_MODE_ALL,        /* every channel 1..13, edge cases          */
    WICOL_MODE_SWEEP,      /* cycle dwell values to find the optimum   */
    WICOL_MODE_MAX
} wicol_mode_t;

/* Start. Brings the WiFi driver up if it is down and verifies one frame
 * actually leaves the radio before reporting success - an attack that
 * silently transmits nothing is worse than one that refuses to start.
 * 0 ok; <0 stage error (-1 wifi init, -2 frame verify, -3 task, -4 mode,
 * -5 bad profile, -6 frame length rejected, -8 mode not offered by this
 * profile). */
int wicoll_start(wicol_profile_t profile, wicol_mode_t mode);

/* Select the target profile without starting an attack. The UI MUST call
 * this when the user changes profile: wicoll_mode_count() and
 * wicoll_mode_at() read the profile stored HERE, so if the UI keeps its own
 * copy the row a user presses resolves against a different profile's mode
 * list. That desync produced a measured NULL-deref panic; see
 * wicoll_start(). Ignored while an attack is running. */
void wicoll_set_profile(wicol_profile_t p);

void wicoll_stop(void);
int wicoll_running(void);
wicol_mode_t wicoll_mode(void);
wicol_profile_t wicoll_profile(void);

/* Modes offered by the current profile, and their names. Not a 1:1 enum
 * map: each profile has its own subset, so the UI must ask rather than
 * assume WICOL_MODE_MAX is reachable. */
int wicoll_mode_count(void);
wicol_mode_t wicoll_mode_at(int idx); /* row -> mode, for THIS profile */
const char *wicoll_mode_name(wicol_mode_t m);
const char *wicoll_profile_name(wicol_profile_t p);

/* Frames the driver ACCEPTED. A real count from return codes, not an
 * estimate. */
uint32_t wicoll_frames(void);
uint32_t wicoll_fps(void);

/* Channel occupancy in permille, DERIVED from frames x airtime. The airtime
 * is computed from the CONFIGURED bandwidth, rate and frame length, not from
 * a hardcoded constant. The UI must label it with a leading '~'. */
uint32_t wicoll_duty_permille(void);
uint32_t wicoll_airtime_us(void);

uint32_t wicoll_elapsed_s(void);
uint32_t wicoll_remaining_s(void);

/* Auto-stop: 1 once the timeout elapsed. The TX task only raises this flag
 * and exits (a task must not wait on its own completion); the owner polls it
 * and calls wicoll_stop(). Same two-phase rule as svc_deauth. */
int wicoll_expired(void);

int wicoll_tx_error(void); /* first esp_err from a REFUSED tx, 0 = none */
uint8_t wicoll_channel(void);

/* Configured max TX power in the driver's 0.25 dBm units (78 = 19.5 dBm),
 * or -1 when the radio is down. Exposed so the UI can show the claim
 * instead of asserting it. */
int wicoll_tx_power_raw(void);

/* Bandwidth override. The engine supports both because HT40 on a raw non-HT
 * frame is NOT established on this driver, and the only prior "measurement"
 * of it compared duty, which cannot distinguish 20 and 40 MHz. */
void wicoll_set_bw40(int on);
int wicoll_bw40(void);

/* LOCK target channel, clamped to 1..14, persists across runs. In HOT mode
 * the survey overwrites this with the loudest channel it found, so the
 * strongest single-channel attack aims itself. */
void wicoll_set_pinned(uint8_t ch);
uint8_t wicoll_pinned(void);

/* What the HOT survey decided, for the UI to show. 0 before a run. */
uint8_t wicoll_hot_lock_channel(void);
uint32_t wicoll_hot_dwell_ms(void);

/* What the LOCK survey decided. LOCK surveys at arm time (it needs the
 * promiscuous receiver, which the TX loop then owns) and parks on
 * wicoll_lock_channel() for the whole run.
 *   wicoll_lock_channel()      0 if the survey failed - LOCK then falls back
 *                              to the pinned channel rather than not starting
 *   wicoll_lock_ap_channels()  how many of the 13 channels carried a beacon,
 *                              i.e. how many were real APs worth aiming at */
uint8_t wicoll_lock_channel(void);
uint8_t wicoll_lock_ap_channels(void);