#pragma once

/* PowerKill's TPC/CSA/Quiet beacon, as a standalone frame builder.
 *
 * Deliberately free of ESP-IDF dependencies (no esp_wifi.h, no esp_random)
 * so tools/hk_pkframe.c can compile this exact file on a PC and verify the
 * bytes. The BLE flood builder has a hand-kept twin in tools/hk_advtest.c,
 * and the two copies drift - a test that passes against a stale copy proves
 * nothing about what the firmware transmits. One file, both consumers.
 *
 * The frame carries the TARGET'S OWN BSSID and SSID, so it lies to the
 * victim in its real AP's voice. See svc_deauth.c for the mechanism notes.
 */

#include <stdint.h>

/* Fixed header + IE bytes. The long SSID plus RSN makes this ~107 B, which
 * is why the TX buffer had to grow from 80: an 80-byte buffer truncates
 * silently and the driver still reports success. */
#define PK_FRAME_MAX 160

/* Seed the TSF clock. Call once per launch with a time-ish value; 0 means
 * "no seed" and the builder picks a fixed non-zero base, which keeps the
 * host test deterministic. */
void pk_frame_seed(uint32_t seed);

/* Build the beacon into f (at least PK_FRAME_MAX bytes). Returns the length,
 * or <0 if the arguments cannot produce a valid frame (bad channel, or an SS
 * ID too long to fit).
 *
 * tick drives the Power Constraint sweep (30 -> 35 -> 20) so stacks that
 * clamp the standard value still see a different number.
 *
 * csa_ch is the channel the Channel Switch Announcement names. Pass 0 to let
 * the builder choose (any channel other than this one); pass a surveyed
 * value to steer the victim somewhere measured-empty. Ignored if it equals
 * `channel`, since a switch to the current channel is a no-op. */
int pk_build_tpc_beacon(uint8_t *f, const uint8_t bssid[6], const char *ssid,
                        int channel, int tick, int csa_ch);

/* Channel the CSA element points the victim at, given the frame's channel.
 * Exposed so the survey can steer it at a measured-empty band instead of the
 * old hardcoded 13. */
int pk_csa_target_channel(int channel);