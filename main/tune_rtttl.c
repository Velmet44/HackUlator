#include "tune_rtttl.h"
#include <string.h>

/* MIDI note number for a semitone letter. -1 for anything unrecognised. */
static int semitone_of(char c) {
    switch (c) {
    case 'c': case 'C': return 0;
    case 'd': case 'D': return 2;
    case 'e': case 'E': return 4;
    case 'f': case 'F': return 5;
    case 'g': case 'G': return 7;
    case 'a': case 'A': return 9;
    /* 'h' is B in the notation that appears in real files. Documented as
     * 'H' in some references and 'h' in others, and both occur, so both are
     * accepted rather than one being treated as a parse fault. */
    case 'b': case 'B': case 'h': case 'H': return 11;
    default: return -1;
    }
}

static int is_digit(char c) {
    return (c >= '0' && c <= '9');
}

static char lower(char c) {
    if (c >= 'A' && c <= 'Z') {
        return (char)(c - 'A' + 'a');
    }
    return c;
}

/* Frequency of a MIDI note, integer Hz, rounded down. Equal temperament with
 * A4 = 440 Hz. The table is walked rather than exponentiated because the ESP32
 * has no FPU: ldexp/pow pull in libm and run in software. An int shift is
 * exact and costs nothing. 12 semitones is a factor of 2 exactly, so the table
 * only ever needs the fractional part of the pitch. */
static uint32_t midi_hz(int n) {
    static const uint16_t frac[12] = {
        /* C-1 .. B-1, as thousandths of the power of two below it */
        0,    29,   59,   90,   122,  155,  186,  223,  263,  301,  343,  384
    };
    int oct;
    int idx;
    uint32_t hz;
    if (n < 0) {
        return 0;
    }
    /* MIDI 69 = A4 = 440. Work out the octave below 69 so the result is
     * always >= 35 Hz for any legal input. */
    oct = (n - 9) / 12;      /* A in the octave that makes n a note number */
    idx = (n - 9) - oct * 12;
    if (idx < 0) {
        idx += 12;
        oct -= 1;
    }
    if (oct < 0) {
        return 0;
    }
    /* 35 Hz * 2^oct is C1. Every note is that plus a semitone ratio. The
     * semitone is a 2^(1/12) step; 12 of them is exactly an octave, so the
     * mantissa table above holds the twelfth roots and accumulating oct
     * doubles is exact. */
    hz = 35u << oct;
    /* frac[idx] is thousandths of a doubling; interpolate in fixed point.
     * hz * (1000 + frac) / 1000 risks overflowing 32 bits at the top of the
     * range, so the ratio is applied as two fixed multiplies. */
    hz = (hz * 1000u + (hz * frac[idx]) / 1000u) / 1000u;
    return hz;
}

/* Skip spaces. Real strings are full of them and the spec says ignore them. */
static void skip_ws(tune_rtttl_t *p) {
    while (*p->cur == ' ') {
        p->cur++;
    }
}

int tune_rtttl_init(tune_rtttl_t *p, const char *rtttl) {
    const char *c;
    const char *sec;
    if (!p || !rtttl) {
        return -1;
    }
    memset(p, 0, sizeof(*p));
    /* Spec defaults, used when the control section omits a key. */
    p->def_dur = 4;
    p->def_oct = 6;
    p->bpm = 63;
    p->cur = rtttl;
    p->note_no = 0;

    /* Section 1: name, up to the first colon. */
    c = strchr(rtttl, ':');
    if (!c) {
        return -2;
    }
    p->cur = c + 1;

    /* Section 2: controls, up to the second colon. */
    sec = p->cur;
    c = strchr(sec, ':');
    if (!c) {
        return -3;
    }
    while (sec < c) {
        skip_ws(p);
        if (sec >= c) {
            break;
        }
        char key = lower(*p->cur);
        if (key == 'd' || key == 'o' || key == 'b') {
            /* "<key>=<value>", but '=' is optional in the wild. */
            p->cur++;
            if (p->cur < c && *p->cur == '=') {
                p->cur++;
            }
            int v = 0;
            int any = 0;
            while (p->cur < c && is_digit(*p->cur)) {
                v = v * 10 + (*p->cur - '0');
                p->cur++;
                any = 1;
            }
            if (any) {
                if (key == 'd') {
                    p->def_dur = v;
                } else if (key == 'o') {
                    p->def_oct = v;
                } else {
                    p->bpm = v;
                }
            }
            /* Step over the separator between controls. */
            while (p->cur < c && *p->cur != ',') {
                p->cur++;
            }
            if (p->cur < c) {
                p->cur++;
            }
        } else {
            /* Unknown control: the spec requires ignoring it. Step to the
             * next comma so one unknown key cannot swallow the rest. */
            while (p->cur < c && *p->cur != ',') {
                p->cur++;
            }
            if (p->cur < c) {
                p->cur++;
            }
        }
    }
    /* Guard the tempo: a zero or absurd value would divide by zero or make
     * every note longer than the screen can express. */
    if (p->bpm < 1) {
        p->bpm = 1;
    }
    if (p->bpm > 900) {
        p->bpm = 900;
    }
    if (p->def_dur < 1) {
        p->def_dur = 1;
    }
    p->cur = c + 1;
    return 0;
}

/* A whole note is four beats, and a beat is 60000/bpm ms. */
static uint32_t whole_ms(const tune_rtttl_t *p) {
    return (240000u + (uint32_t)(p->bpm / 2)) / (uint32_t)p->bpm;
}

int tune_rtttl_next(tune_rtttl_t *p, tune_note_t *out) {
    int dur = p->def_dur;
    int oct = p->def_oct;
    int semi;
    int dotted = 0;
    char n;
    uint32_t ms;
    if (!p || !out) {
        return 0;
    }
    skip_ws(p);
    if (*p->cur == 0) {
        return 0;
    }

    /* Optional leading duration divisor: 1, 2, 4, 8, 16, 32. The digits are
     * contiguous in every real file ("16c"), but a space between them is seen
     * in the wild, so skip it between digits too. */
    if (is_digit(*p->cur)) {
        int v = 0;
        while (is_digit(*p->cur)) {
            v = v * 10 + (*p->cur - '0');
            p->cur++;
            if (*p->cur == ' ') {
                p->cur++;
            }
        }
        if (v > 0) {
            dur = v;
        }
    }

    n = lower(*p->cur);
    if (n == 0) {
        return 0;
    }
    if (n == 'p') {
        /* A rest carries no pitch. Consume the letter and let the trailing
         * parser see any octave/dot that follows it. */
        semi = -1;
        p->cur++;
    } else {
        semi = semitone_of(n);
        if (semi < 0) {
            /* Separator or junk: step over one character and try again, so a
             * single malformed token cannot end the tune early. */
            p->cur++;
            return tune_rtttl_next(p, out);
        }
        p->cur++;
    }

    /* Order matters and this is the conformance trap: real files put the dot
     * BEFORE the octave. Accept whichever comes first. */
    if (*p->cur == '.') {
        dotted = 1;
        p->cur++;
        if (is_digit(*p->cur)) {
            oct = *p->cur - '0';
            p->cur++;
        }
    } else if (is_digit(*p->cur)) {
        oct = *p->cur - '0';
        p->cur++;
        if (*p->cur == '.') {
            dotted = 1;
            p->cur++;
        }
    }
    /* Optional doubled dot, seen in the wild. */
    if (*p->cur == '.') {
        dotted = 1;
        p->cur++;
    }

    ms = whole_ms(p) / (uint32_t)(dur > 0 ? dur : 1);
    if (dotted) {
        ms = (ms * 3u) / 2u;
    }
    if (ms == 0) {
        ms = 1;
    }
    if (ms > 60000u) {
        ms = 60000u;
    }

    out->freq_hz = (semi < 0) ? 0u : midi_hz((oct + 1) * 12 + semi);
    out->dur_ms = (uint16_t)ms;
    p->note_no++;
    return 1;
}

int tune_rtttl_count(const tune_rtttl_t *p) {
    return p ? p->note_no : 0;
}