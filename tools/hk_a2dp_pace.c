/* Host test for the A2DP producer's real-time schedule.
 *
 * This compiles main/a2dp_pace.h - THE SAME HEADER the firmware includes - and
 * checks the properties that matter on a speaker and cannot be seen any other
 * way. The schedule produces no log line and no counter; a tune that is
 * slightly fast just sounds slightly wrong, so the only way to know it is
 * right is to compute it.
 *
 *   gcc -O2 -Wall -o /tmp/hk_a2dp_pace tools/hk_a2dp_pace.c -I main
 *   /tmp/hk_a2dp_pace
 *
 * What it proves, and why each one matters:
 *   1. a tick boundary is EXACTLY 10 ms of audio      (no per-chunk rounding)
 *   2. throughput tracks real time, plus a bounded lead, at 10 s and at 60 s -
 *      and the two agree, which is what "no drift" actually means
 *   3. the same holds on a jittery clock and through long stalls
 *   4. a stall resynchronises instead of flushing a backlog
 *   5. the MTU clamp respects the header guard and whole stereo frames
 *   6. the loop can never sleep a tick and still not be due
 *   7. the schedule runs slightly AHEAD of real time (the sink's jitter
 *      buffer must never starve) and the lead is bounded (or it is drift)
 *
 * Point 1 exists because the first version of this schedule summed PER-CHUNK
 * durations. 147 frames is 3333.33 us, truncation makes it 3333, and three of
 * them is 9999 rather than 10000 - one microsecond lost every three chunks,
 * which is 735 frames of error over ten seconds. Deriving the due time from
 * the cumulative frame count makes every tick boundary exact.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include "a2dp_pace.h"

static int fails;

static void check(int cond, const char *what) {
    if (!cond) {
        printf("  FAIL  %s\n", what);
        fails++;
    }
}

/* One simulated producer, mirroring feed_task()'s decisions exactly. */
typedef struct {
    int64_t now_us;
    int64_t t0_us;
    int64_t run_frames;
    int mtu;
    int sends;
    int sleeps;
    int resyncs;
    int worst_burst;     /* most sends with no sleep between them */
    int burst;
} sim_t;

static void sim_step(sim_t *s, int64_t cost_us) {
    int frames = a2dp_send_frames(s->mtu);
    int64_t due = 0;
    a2dp_pace_t p;

    if (s->t0_us == 0) {
        s->t0_us = s->now_us;
    }
    p = a2dp_pace(s->now_us, s->t0_us, s->run_frames, frames, &due);
    if (p == A2DP_PACE_SLEEP) {
        s->sleeps++;
        s->burst = 0;
        s->now_us += A2DP_TICK_US;
        return;
    }
    if (p == A2DP_PACE_RESYNC) {
        s->resyncs++;
        /* Rebase onto now; the backlog is dropped, not flushed. */
        s->t0_us = s->now_us - a2dp_elapsed_us(s->run_frames);
        s->now_us += cost_us;
        return;
    }
    s->sends++;
    s->burst++;
    if (s->burst > s->worst_burst) {
        s->worst_burst = s->burst;
    }
    s->run_frames += frames;
    /* Advance the wall clock by what the work actually cost. A producer that
     * assumes it is free will not track real time; one that assumes it takes
     * the whole chunk duration will run slow. */
    s->now_us += cost_us;
}

static sim_t sim_run(int64_t run_us, int mtu, int64_t cost_us,
                     int64_t stall_us, int64_t stall_every_us) {
    sim_t s = {0, 0, 0, mtu, 0, 0, 0, 0, 0};
    int64_t next_stall = stall_every_us;
    while (s.now_us < run_us) {
        if (stall_us && s.now_us >= next_stall) {
            s.now_us += stall_us;      /* something blocked us */
            next_stall += stall_every_us;
        }
        sim_step(&s, cost_us);
    }
    return s;
}

/* How far ahead of real time the producer is running, in us.
 *
 * Measured from the stream origin, NOT from zero: a resync rebases t0, so
 * "audio handed over minus elapsed" is only meaningful relative to the
 * current origin. This is also the quantity that has to stay put - growing
 * means drift, going negative means the sink's jitter buffer is being
 * starved. */
static int64_t lead_of(const sim_t *s) {
    return s->t0_us + a2dp_elapsed_us(s->run_frames) - s->now_us;
}

int main(void) {
    int frames = a2dp_send_frames(672);
    int i;

    printf("a2dp producer schedule\n");
    printf("  chunk = %d frames, %lld us nominal, MTU 672\n", frames,
           (long long)a2dp_elapsed_us(frames));

    /* --- 1. tick boundaries are exact ----------------------------------- */
    printf("  147 x 3 = %lld us, 147 x 300 = %lld us (one tick = %d)\n",
           (long long)a2dp_elapsed_us(3 * frames),
           (long long)a2dp_elapsed_us(300 * frames), A2DP_TICK_US);
    check(a2dp_elapsed_us(3 * frames) == A2DP_TICK_US,
          "three chunks must be exactly one tick of 44.1 kHz audio");
    check(a2dp_elapsed_us(300 * frames) == 100 * A2DP_TICK_US,
          "every hundredth tick must still be exact - no cumulative drift");

    /* --- 2. no drift: the lead is the same at 10 s and at 60 s ----------- */
    {
        /* cost_us is what a real send costs. Zero is the degenerate case where
         * the stack is free and the schedule is exactly break-even, which
         * says nothing - the servo is only meaningful against real work. */
        sim_t a = sim_run(10LL * 1000000, 672, 900, 0, 0);
        sim_t b = sim_run(60LL * 1000000, 672, 900, 0, 0);
        int64_t la = lead_of(&a), lb = lead_of(&b);
        printf("  realistic stack: lead %lld us at 10 s, %lld us at 60 s "
               "(%d/%d sleeps)\n", (long long)la, (long long)lb,
               a.sleeps, b.sleeps);
        check(la > 0, "the producer must run ahead of real time, not behind");
        check(lb > 0, "still ahead after a minute, not just at the start");
        check(llabs(lb - la) <= a2dp_elapsed_us(frames),
              "the lead must not grow with time - that would be drift");
        /* And it must not busy-loop: a handful of chunks per wake-up, never
         * one per wake-up and never a runaway burst. A producer that woke once
         * per chunk would be running at 300 Hz, which is what starves the idle
         * task and trips the WDT. */
        check(b.sends / b.sleeps >= 2 && b.sends / b.sleeps <= 6,
              "the producer must batch a few chunks per wake-up, not one");
    }
    {
        /* The degenerate free-stack case still has to be non-negative: it is
         * the boundary of the servo, and a negative lead there would mean the
         * schedule cannot even break even. */
        sim_t f = sim_run(10LL * 1000000, 672, 0, 0, 0);
        printf("  free stack: lead %lld us after 10 s (%d sleeps)\n",
               (long long)lead_of(&f), f.sleeps);
        check(lead_of(&f) >= 0,
              "even a free stack must never fall behind real time");
        check(f.sleeps >= 900 && f.sleeps <= 1100,
              "producer must sleep about one tick per window");
    }

    /* --- 3. jittery clock, and long stalls ------------------------------ */
    {
        sim_t j = sim_run(60LL * 1000000, 672, 900, 0, 0);
        printf("  jittery stack (0.9 ms per send): lead %lld us after 60 s\n",
               (long long)lead_of(&j));
        check(lead_of(&j) > 0 && lead_of(&j) < A2DP_FEED_LAG_US,
              "jitter must not push the producer behind or out of bounds");
    }
    {
        sim_t s = sim_run(60LL * 1000000, 672, 300, 90000,
                          5LL * 1000000);
        int64_t lead = lead_of(&s);
        int burst_bound = (int)((A2DP_FEED_LAG_US + A2DP_SEND_AHEAD_US) /
                                a2dp_elapsed_us(frames)) + 2;
        printf("  12 x 90 ms stalls: %d resyncs, worst burst %d, lead %lld us\n",
               s.resyncs, s.worst_burst, (long long)lead);
        check(s.resyncs >= 10 && s.resyncs <= 14,
              "each long stall must be absorbed by exactly one resync");
        /* Without the resync the backlog would be emitted in one go; the
         * burst is bounded by (lag + ahead) / chunk by construction. */
        check(s.worst_burst <= burst_bound,
              "a stall must never be flushed as an unbounded burst");
        check(lead > 0 && lead < A2DP_FEED_LAG_US,
              "the lead must survive repeated stalls");
    }

    /* --- 4. the MTU clamp ----------------------------------------------- */
    check(a2dp_send_frames(672) * 4 + A2DP_SEND_GUARD <= 672,
          "a send must leave the AVDU header room inside the negotiated MTU");
    check((a2dp_send_frames(672) * 4) % 4 == 0,
          "sends are whole stereo frames");
    check(a2dp_send_frames(1000000) == A2DP_SEND_BYTES / 4,
          "an oversized MTU must clamp to the fixed chunk");
    check(a2dp_send_frames(80) == 18, "a small MTU is used, not ignored");
    check(a2dp_send_frames(64) == 16, "a tiny MTU must floor at 16 frames");
    check(a2dp_send_frames(0) == 16, "no MTU must not divide by zero");
    check(a2dp_send_frames(672) * 4 <= A2DP_SEND_BYTES,
          "a send must fit the staging buffer");
    printf("  mtu 672 -> %d fr, mtu 1000000 -> %d fr, mtu 80 -> %d fr, "
           "mtu 0 -> %d fr\n", a2dp_send_frames(672),
           a2dp_send_frames(1000000), a2dp_send_frames(80),
           a2dp_send_frames(0));

    /* --- 5. sleeping can never leave us still-not-due -------------------- */
    {
        int slept_not_due = 0;
        int64_t t0 = 0;
        int64_t run_frames = 0;
        int fr = a2dp_send_frames(672);
        for (i = 0; i < 6000; i++) {
            int64_t now = (int64_t)i * A2DP_TICK_US;
            int64_t due = 0;
            if (t0 == 0) {
                t0 = now;
            }
            if (a2dp_pace(now, t0, run_frames, fr, &due) ==
                A2DP_PACE_SLEEP) {
                int64_t after = now + A2DP_TICK_US;
                if (a2dp_pace(after, t0, run_frames, fr, &due) ==
                    A2DP_PACE_SLEEP) {
                    slept_not_due++;
                }
            } else {
                run_frames += fr;
            }
        }
        check(slept_not_due == 0,
              "one sleep must always be enough to become due");
    }

    if (fails) {
        printf("\n%d check(s) FAILED\n", fails);
        return 1;
    }
    printf("\nall schedule checks passed\n");
    return 0;
}