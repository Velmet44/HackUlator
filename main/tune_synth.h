#pragma once

/* Integer tone generator for the Audio Device Hijacker.
 *
 * WHY INTEGER AND NOT sin():
 * The ESP32 has no FPU. A sin() call pulls libm into the image and runs in
 * software floating point, which is both slow and, worse, pulls in a code
 * footprint this project cannot spare. So: a 256-entry signed-byte sine table
 * in .rodata (which is flash-mapped, so the table costs ZERO RAM) driven by a
 * 32-bit fixed-point phase accumulator. The whole oscillator state is five
 * scalars.
 *
 * WHY A PHASE ACCUMULATOR AND NOT A TABLE INDEX:
 * Advancing an index by a per-note integer step is the obvious approach and it
 * is wrong here: at 44.1 kHz, 440 Hz over 256 entries is a step of 2.55, so
 * the increment would have to alternate between 2 and 3 forever and every note
 * would be audibly out of tune by a fraction of a percent. Accumulating the
 * phase in 32 bits and taking the top 8 bits as the index keeps the error
 * below 1 Hz.
 *
 * WHY AN ENVELOPE:
 * A raw square/sine gate clicks at every note boundary. On a speaker in a room
 * that is not subtle, and on a Bluetooth link it is the first thing anyone
 * hears. 5 ms attack and 10 ms release removes it for about ten lines and no
 * memory at all - it is arithmetic on two integers already in the struct.
 *
 * MONO IN, STEREO OUT:
 * btc_a2dp_source_pcm2sbc_init() promotes a Mono negotiation to Joint Stereo
 * anyway ("Some AV Headsets do not support Mono"), so generating mono to save
 * half the work would save nothing at all. One channel is synthesised and
 * copied to both.
 *
 * Pure C, no ESP dependency: tools/hk_rtttl.py compiles and checks this too.
 */

#include <stdint.h>

#define SYNTH_PEAK 11000   /* of 32767; headroom so nothing clips downstream */

typedef struct {
    uint32_t phase;    /* 0..2^32; top 8 bits index the table */
    uint32_t inc;      /* phase increment per sample */
    uint32_t pos;      /* samples into the current note */
    uint32_t len;      /* total samples in the current note */
} synth_t;

void synth_init(synth_t *s);

/* Begin a note. freq_hz 0 is a rest: the phase stops, the envelope runs, and
 * the output is silence - a rest still occupies time, it is not a gap. */
void synth_note(synth_t *s, uint32_t freq_hz, uint16_t dur_ms);

/* Fill interleaved stereo 16-bit samples. Returns 1 on the call in which the
 * note finishes, so the caller knows to fetch the next one. */
int synth_fill(synth_t *s, int16_t *out, int frames);

/* 1 while a note is still sounding. */
int synth_busy(const synth_t *s);

/* A pure tone, for the streaming smoke test with no tune attached. */
void synth_tone(synth_t *s, uint32_t freq_hz);