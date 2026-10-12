#pragma once

#include <stdint.h>

/* The A2DP producer's real-time schedule, as pure arithmetic.
 *
 * WHY THIS IS A HEADER AND NOT A BLOCK OF CODE IN svc_a2dp.c:
 * the schedule is the one part of the audio path that cannot be observed from
 * outside - it produces no log line and no counter, it only shows up as a
 * tune that is slightly fast and then drifts. A test that re-implemented it
 * would therefore be testing a copy, and a copy is exactly the failure mode
 * this project has already been bitten by twice (see AGENTS.md on the two
 * hand-synced copies of flood_build_raw()). So the firmware and the host test
 * compile the SAME functions from the SAME file: if the cadence changes, the
 * test changes with it or fails.
 *
 * THE MODEL, and the one mistake in it worth naming:
 *   - one send carries `frames` stereo frames of 44.1 kHz PCM
 *   - `t0` is when THIS stream started and `done` is how many frames have been
 *     handed to the stack since
 *   - the due time of the next send is therefore a function of the CUMULATIVE
 *     frame count, never of a per-chunk duration
 *
 * That distinction is not cosmetic and it was measured, not reasoned about.
 * The chunk duration 147 * 1e6 / 44100 is 3333.33 us, and integer truncation
 * makes it 3333. Summing truncated chunks loses a microsecond every three of
 * them: a 10 s run hands over 441735 frames where 441000 is real time, which is
 * 735 frames of drift - 0.17 % fast, and climbing. Deriving the due time from
 * the cumulative count instead makes every tick boundary exact (441 frames is
 * exactly 10000 us) and the error cancels exactly, because it is one division
 * at the end rather than one per chunk.
 *
 * The lead runs the producer slightly AHEAD of real time on purpose: a source
 * that is exactly on time starves the sink's jitter buffer the instant one
 * send is a tick late. The lag limit is the opposite failure - after a long
 * stall the backlog is resynchronised rather than dumped, which would come out
 * of the speaker as a burst of noise.
 */

/* 588 bytes = 147 stereo frames = 3.33 ms of 44.1 kHz audio.
 *
 * THE SIZE IS NOT ARBITRARY. Three of them is 441 frames, which is exactly one
 * FreeRTOS tick of audio at 44.1 kHz, and this build runs at 100 Hz. The size
 * this replaced was 672 / 4 = 168 frames, so three sends covered 504 frames -
 * 11.4 ms of audio handed over in a 10 ms window. Every tune ran ~4 % fast and
 * the error compounded, because the delay was a tick (and jitters) while the
 * budget was never re-derived from real time at all.
 *
 * It also leaves head-room under the negotiated A2DP MTU: see A2DP_SEND_GUARD. */
#define A2DP_SEND_BYTES 588

/* Bytes of head-room left below the MTU the sink reported. The stack adds its
 * own AVDU media header on top of the PCM handed to it - A2DPEncoder.h in the
 * reference library puts payload_header_size() at 1 for SBC - so sending
 * exactly `mtu` bytes asks for one byte more than the L2CAP channel was
 * negotiated for. Eight leaves the header room and still lands on a whole
 * stereo frame. */
#define A2DP_SEND_GUARD 8

/* One FreeRTOS tick at CONFIG_FREERTOS_HZ=100. The only sleep available to
 * the producer; anything finer would mean busy-waiting, which starves the
 * idle task and trips the WDT. */
#define A2DP_TICK_US 10000

#define A2DP_FEED_LEAD_US    8000
#define A2DP_FEED_LAG_US    40000
/* Send when we are within this much of (due - lead). Bigger than a tick, so
 * the wake-up is never "sleep a tick and still not due". */
#define A2DP_SEND_AHEAD_US 15000

#define A2DP_RATE_HZ 44100

typedef enum {
    A2DP_PACE_SLEEP = 0,  /* nothing to do yet: yield for a tick */
    A2DP_PACE_SEND,       /* due: hand one chunk to the stack */
    A2DP_PACE_RESYNC      /* far behind: drop the backlog, restart from now */
} a2dp_pace_t;

/* Wall time `frames` stereo frames of 44.1 kHz audio takes, integer us.
 * Truncated, and deliberately: call it with a CUMULATIVE count, never with a
 * per-chunk size. See the note at the top of this file. */
static inline int64_t a2dp_elapsed_us(int64_t frames) {
    if (frames <= 0) {
        return 0;
    }
    return (frames * 1000000) / A2DP_RATE_HZ;
}

/* Absolute time at which a cumulative frame count must have been handed over.
 * This is the whole schedule. */
static inline int64_t a2dp_due_us(int64_t t0_us, int64_t frames) {
    return t0_us + a2dp_elapsed_us(frames);
}

/* How many stereo frames one send may carry for a given negotiated MTU.
 *
 * Whole stereo frames only - a half frame is not something that can be handed
 * to the SBC encoder - and a floor of 16 frames, because an MTU below that
 * cannot carry audio at all and dividing straight through would produce a
 * zero-length send that the stack rejects for ever. */
static inline int a2dp_send_frames(int mtu) {
    int b = mtu - A2DP_SEND_GUARD;
    if (b > A2DP_SEND_BYTES) {
        b = A2DP_SEND_BYTES;
    }
    b &= ~3;
    if (b < 64) {
        b = 64;
    }
    return b / 4;
}

/* What the producer should do this iteration, given the stream origin `t0_us`
 * and how many frames have already been handed over. `*next_out` receives the
 * due time that was evaluated, which the caller needs after a RESYNC to rebase
 * `t0_us`.
 *
 * After a RESYNC the caller must rebase to `now_us`, discarding the backlog:
 * that is the difference between a stall costing the sink a brief gap and a
 * stall flushing a quarter second of queued audio at it. */
static inline a2dp_pace_t a2dp_pace(int64_t now_us, int64_t t0_us,
                                    int64_t frames_done, int frames,
                                    int64_t *due_out) {
    int64_t due = a2dp_due_us(t0_us, frames_done + frames);
    *due_out = due;
    if (now_us - due > A2DP_FEED_LAG_US) {
        return A2DP_PACE_RESYNC;
    }
    if (due - A2DP_FEED_LEAD_US - now_us > A2DP_SEND_AHEAD_US) {
        return A2DP_PACE_SLEEP;
    }
    return A2DP_PACE_SEND;
}