#pragma once

/* RTTTL streaming parser: Nokia's "Ringing Tones Text Transfer Language".
 *
 * WHY THIS FORMAT AND NOT MIDI OR WAV:
 * RTTTL is plain text, roughly 200 bytes for a whole tune, and its notes are
 * already monophonic note+duration+octave+tempo - a direct 1:1 with an
 * oscillator. MIDI for the same melody is 5-50 KB and needs an SMF reader and
 * a polyphony-to-mono decision the hardware cannot help with. Baked PCM is
 * worse still: esp-idf-a2dp-source carries a 5.3 MB music.h, which is about
 * thirteen seconds of 44.1 kHz stereo, and never plays it. This app has a
 * 1.875 MB partition.
 *
 * It is a STREAMING parser on purpose. Materialising a tune into a note array
 * would be about 2 bytes per note, which is affordable, but it would also mean
 * two representations of the same thing that can disagree. Parsing at the
 * sample boundary means the text in flash is the only copy.
 *
 * DELIBERATE NON-CONFORMANCE. Real RTTTL violates its own specification
 * constantly, so the parser accepts things the grammar does not:
 *   - the dotted-length '.' appears BEFORE the octave in most strings in the
 *     wild ("c.6") and AFTER it in the spec ("c6."). Both are accepted,
 *     because the majority of real files use the former and a parser that
 *     only follows the spec fails on the first tune anyone hands it.
 *   - letters are usually lowercase, and names exceed the spec's 10 chars
 *   - 'h' and 'H' both mean B (German/British notation)
 *   - 'p' is a rest and takes no octave
 *   - octaves drift outside the documented 4..7 range
 *   - embedded spaces are everywhere and must be ignored
 * Unknown control names are ignored rather than rejected, as the spec
 * requires. Pure C, no ESP dependency: tools/hk_rtttl.py compiles this file on
 * a PC and checks it against a corpus of real tunes.
 */

#include <stdint.h>

/* Sample rate the synth runs at. Fixed because btc_a2dp_source_setup_codec()
 * hardcodes 44.1 kHz / 16-bit / stereo as the PCM feeding format, and any
 * other rate is silently coerced to 48 kHz by the stack. */
#define TUNE_RATE 44100

typedef struct {
    const char *cur;   /* cursor into the caller's string */
    int def_dur;       /* default note length divisor: 4 = quarter note */
    int def_oct;       /* default octave */
    int bpm;           /* quarter-note beats per minute */
    int note_no;       /* 1-based index of the next note, for the UI */
} tune_rtttl_t;

typedef struct {
    uint32_t freq_hz;  /* 0 = rest */
    uint16_t dur_ms;   /* sounding length, envelope included */
} tune_note_t;

/* Parse the header (name, d=, o=, b=) and position the cursor on the first
 * note. 0 ok, <0 on a malformed string. The string must stay valid for as
 * long as the parser is used - it is not copied. */
int tune_rtttl_init(tune_rtttl_t *p, const char *rtttl);

/* Fetch the next note. 1 = got one, 0 = end of tune or a parse fault after
 * init. */
int tune_rtttl_next(tune_rtttl_t *p, tune_note_t *out);

/* Notes emitted so far. Drives "n/N" on screen without a second pass. */
int tune_rtttl_count(const tune_rtttl_t *p);