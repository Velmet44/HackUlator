#pragma once

/* Audio Device Hijacker: connect to a Bluetooth audio sink as an A2DP source
 * and stream coded tunes to it. Own lab / own gear.
 *
 * WHY CLASSIC BT AND NOT BLE - THE ONE HARD LIMIT HERE:
 * A2DP over classic BR/EDR is what this implements and all it will ever do.
 * The ESP32's radio is a BLE 4.2 controller with no LE Audio, no LC3 and no
 * BAP, so a true-wireless earbud on the LE Audio path is unreachable. That is
 * silicon, not configuration: no amount of software work reaches it. What
 * DOES work is car stereos, older and budget headphones, older TVs and
 * soundbars, and any speaker that still offers a classic A2DP profile.
 *
 * WHY DISCOVERY IS NOT THE BLE SCANNER:
 * A2DP sinks are found with a CLASSIC INQUIRY, filtered on the Class-of-Device
 * "Rendering" service bit. The BLE scanner will not see these devices at all,
 * so svc_ble is not reused and the sink list is its own. The practical
 * consequence the UI must state plainly: the target has to be in pairing or
 * discoverable mode, because a device that is not advertising itself cannot be
 * found by anyone. That is not a malfunction, and without the prompt it looks
 * like one.
 *
 * WHY THE WHOLE STACK IS BROUGHT UP AND TORN DOWN PER SCREEN:
 * Measured on this board, BTDM + Bluedroid + the A2DP source cost about 41 KB
 * of RAM, on top of about 25 KB of static RAM that the link pulls in whatever
 * happens. The project gates on the largest contiguous free block and that
 * budget is the binding constraint on every other screen, so this service gets
 * the radio only while it is on screen and gives it back on the way out. The
 * other services tear down on the same pattern; nothing new here.
 *
 * WHY THERE IS NO REFERENCE IMPLEMENTATION TO COPY:
 * The ESP-IDF a2dp_source example negotiates and starts media but never calls
 * esp_a2d_source_audio_data_send(); a third-party IDF port ships a 5.3 MB PCM
 * header and never plays it either. The streaming path below is written from
 * the API rather than adapted from an example.
 */

#include <stdint.h>
#include <stdbool.h>

/* Optional pre-filled speaker, so the feature works without a first discovery.
 * EMPTY BY DEFAULT and that is deliberate: a baked-in address is specific to
 * one speaker and would be wrong for everyone else.
 *
 * Set it to a real address to skip the pairing-once step - useful when the
 * address is already known from a phone or PC. Otherwise leave it empty and
 * the address is learned once through discovery, then stored in NVS.
 *
 * There is no way around discovery for the FIRST speaker: classic inquiry
 * only reaches a discoverable device, and a speaker that is merely switched
 * on advertises nothing. That is a Bluetooth protocol property, not an ESP32
 * limitation. Every later run uses the stored address and needs no pairing
 * mode at all. */
#ifndef HACKU_A2DP_DEFAULT_PEER
#define HACKU_A2DP_DEFAULT_PEER  ""
#endif
#ifndef HACKU_A2DP_DEFAULT_NAME
#define HACKU_A2DP_DEFAULT_NAME  "speaker"
#endif

#define A2DP_MAX_SINKS 8
#define A2DP_NAME_MAX   20

typedef struct {
    uint8_t bda[6];
    char    name[A2DP_NAME_MAX + 1];
    int8_t  rssi;
} a2dp_sink_t;

/* Bring up controller (BTDM), Bluedroid and the A2DP source. Call only after
 * svc_ble_stop(), which owns the same controller. 0 ok, <0 stage error:
 * -10 controller_init, -11 controller_enable, -12 bluedroid_init,
 * -13 bluedroid_enable, -14 a2d_source_init. */
int svc_a2dp_up(void);

/* Full teardown. MUST run before any other radio teardown: the producer task
 * and the A2DP callbacks point into this stack, and unwinding it from under a
 * live callback is the same fault as svc_deauth's orphan TX timer. Safe when
 * already down. */
void svc_a2dp_stop(void);

int svc_a2dp_up_ok(void);

/* Find a speaker by scanning BLE for its advertised Bluetooth address.
 *
 * WHY THIS EXISTS, and it is the reason pairing mode is never needed:
 * classic inquiry can only reach a device that is DISCOVERABLE, which in
 * practice means pairing mode. A speaker that is merely powered on and not
 * connected to anything is invisible to inquiry - yet almost all of them
 * broadcast BLE adverts continuously, because that is how a phone finds them.
 * A BLE advert carries the device's Bluetooth address in its MAC field, so one
 * BLE scan yields the address that esp_a2d_source_connect() needs.
 *
 * The two are complementary and both are kept:
 *   - BLE-by-name   : works with the speaker just switched on. The normal path.
 *   - classic inquiry: works when the speaker IS in pairing mode, and is the
 *     fallback for devices that broadcast no BLE at all.
 *
 * Blocking, like svc_ble_scan(). Returns the number of candidate addresses
 * found, or <0 on error. */
int svc_a2dp_find_by_ble(a2dp_sink_t *out, int max, int seconds,
                        const char *name_want);

/* Classic inquiry for A2DP sinks. Blocking, like svc_ble_scan(). Returns the
 * number found, or <0 on error. Fills at most `max` entries. */
int svc_a2dp_discover(a2dp_sink_t *out, int max, int seconds);

/* Connect to a sink by address. 0 = request sent, <0 refused. */
int svc_a2dp_connect(const uint8_t *bda);

/* RECONNECT WITHOUT PAIRING MODE, which is the normal case after the first
 * ever run. Bluedroid persists the bond in NVS, so once the speaker has been
 * bonded the ESP32 can call esp_a2d_source_connect() on the stored address with
 * no inquiry at all - and the speaker does not have to be in pairing mode,
 * because a bonded host is trusted.
 *
 * Asking the user to put the speaker in pairing mode on every run is a trap:
 * it invites them to believe the feature only works that way, and on a device
 * with no pairing button it makes the feature unusable.
 *
 * Pairing mode IS needed exactly once, for the first bond. There is no way
 * around that and pretending otherwise would just move the failure.
 *
 * The stored address is saved the moment a speaker is chosen, so it survives
 * reboots; the bond itself is stored by Bluedroid. Returns 1 if one is
 * stored and loaded. */
bool svc_a2dp_have_stored(void);

/* Load the stored address NOW rather than as a side effect of svc_a2dp_up().
 * The screen has to ask "is there a stored speaker?" BEFORE it decides whether
 * to bring the BT stack up - that check is what chooses between reconnect and
 * discovery - so the load cannot be hidden inside the bring-up or the question
 * always answers "no" and the screen always runs discovery. */
void svc_a2dp_load_stored(void);
int svc_a2dp_connect_stored(void);
void svc_a2dp_forget_stored(void);
const char *svc_a2dp_stored_name(void);

/* Disconnect and unwind media. */
int svc_a2dp_disconnect(void);

int svc_a2dp_connected(void);
int svc_a2dp_playing(void);
const char *svc_a2dp_state_name(void);

/* Start streaming an RTTTL tune. The string must stay valid until the tune
 * ends or svc_a2dp_stop_tune() returns; it is parsed in place, never copied.
 * 0 ok, <0 refused. */
int svc_a2dp_play_tune(const char *rtttl);

/* A pure tone, for proving the streaming path with no tune attached. */
int svc_a2dp_play_tone(uint32_t freq_hz);

/* Ask the producer task to finish. Returns once it has actually exited, so a
 * caller can safely unwind the stack afterwards. Never signals from inside the
 * task itself: two-phase stop, same rule as svc_deauth and svc_ble. */
void svc_a2dp_stop_tune(void);

uint32_t svc_a2dp_frames(void);   /* PCM frames accepted by the stack */
uint32_t svc_a2dp_notes(void);    /* notes played */
uint32_t svc_a2dp_underruns(void);/* sends refused for lack of buffer */
int      svc_a2dp_busy(void);     /* producer task alive */
int      svc_a2dp_tune_done(void);/* tune reached its end */

/* First esp_err from a refused send, 0 = none. */
int svc_a2dp_tx_error(void);

/* Connected peer, for the screen. */
const char *svc_a2dp_peer_name(void);
void svc_a2dp_set_name(const char *name);
void svc_a2dp_bda_str(char *out);