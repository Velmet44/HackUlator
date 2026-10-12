#include "tune_synth.h"
#include "tune_tables.h"

/* Envelope edges. Long enough that a step discontinuity at the note boundary
 * is gone, short enough that a sixteenth note is still recognisably short. */
#define ATTACK_MS  5
#define RELEASE_MS 10

/* Samples per millisecond, kept integer so the edges below stay exact. */
#define TUNE_RATE_MS   (44100u / 1000u)
#define ATTACK_FRAMES  (TUNE_RATE_MS * ATTACK_MS)
#define RELEASE_FRAMES (TUNE_RATE_MS * RELEASE_MS)

void synth_init(synth_t *s) {
    s->phase = 0;
    s->inc = 0;
    s->pos = 0;
    s->len = 0;
}

void synth_note(synth_t *s, uint32_t freq_hz, uint16_t dur_ms) {
    s->pos = 0;
    /* A 32-bit phase holds exactly 256 table entries in its top byte, so the
     * increment is frequency scaled into a fixed 2^24 field. 64-bit only in
     * this one place: the multiply is done once per note, not per sample. */
    if (freq_hz == 0) {
        s->inc = 0;
    } else {
        s->inc = (uint32_t)(((uint64_t)freq_hz << 24) / 44100u);
    }
    {
        uint32_t frames = ((uint32_t)dur_ms * 44100u) / 1000u;
        if (frames == 0) {
            frames = 1;
        }
        s->len = frames;
    }
}

void synth_tone(synth_t *s, uint32_t freq_hz) {
    /* Ten seconds: long enough to hear, finite so it cannot run away. */
    synth_note(s, freq_hz, 10000);
}

/* Envelope gain as 0..256. Attack, then release, else full. */
static uint32_t envelope(const synth_t *s) {
    uint32_t a = ATTACK_FRAMES;
    uint32_t r = RELEASE_FRAMES;
    uint32_t left = s->len - s->pos;
    if (a > s->len / 2) {
        a = s->len / 2;
    }
    if (r > s->len / 2) {
        r = s->len / 2;
    }
    if (a == 0) {
        a = 1;
    }
    if (r == 0) {
        r = 1;
    }
    if (s->pos < a) {
        return (s->pos * 256u) / a;
    }
    if (left < r) {
        return (left * 256u) / r;
    }
    return 256u;
}

int synth_busy(const synth_t *s) {
    return s->pos < s->len;
}

int synth_fill(synth_t *s, int16_t *out, int frames) {
    int i;
    int finished = 0;
    if (!s || !out || frames <= 0) {
        return 0;
    }
    for (i = 0; i < frames; i++) {
        int16_t v = 0;
        if (s->pos < s->len) {
            uint32_t g = envelope(s);
            if (s->inc != 0) {
                /* Top byte indexes the table; the low 24 bits carry the
                 * sub-entry precision that an integer step index loses. */
                int8_t t = k_sine[(s->phase >> 24) & (SINE_TABLE_N - 1)];
                v = (int16_t)((int32_t)t * (int32_t)g * SYNTH_PEAK / (127 * 256));
            }
            s->phase += s->inc;
            s->pos++;
            if (s->pos >= s->len) {
                finished = 1;
            }
        }
        out[i * 2] = v;
        out[i * 2 + 1] = v;
    }
    return finished;
}