#!/usr/bin/env python3
"""Convert an Arduino `melody[]` tune to RTTTL.

The Arduino songs format (robsoncouto/arduino-songs and friends) encodes a
tune as flat pairs of (frequency, duration-divisor), where a NEGATIVE divisor
means a dotted note. RTTTL encodes the same information as text, and the tempo
maths is identical:

    Arduino:  wholenote = (60000 * 4) / tempo      note = wholenote / divider
    RTTTL:    whole_ms  = 240000 / bpm              note = whole_ms / d

so the conversion is exact rather than approximate. Sharp names map
one-for-one too: NOTE_DS5 -> d#5, where 'S' stands for sharp in the Arduino
table and '#' in RTTTL.

Usage:
    python tools/arduino2rtttl.py in.ino out.rtttl [tempo]
    python tools/arduino2rtttl.py in.ino --print [tempo]

tempo defaults to whatever the sketch's `int tempo = N;` says. The NOTE_*
frequency table is read from the sketch itself rather than duplicated here, so
the two cannot drift apart.
"""
import os
import re
import sys

DEFINE_RE = re.compile(r"#define\s+NOTE_([A-G]S?)(\d)\s+(\d+)")
TOKEN_RE = re.compile(r"NOTE_REST|NOTE_[A-Z]+\d+|-?\d+")


def read_note_table(text):
    """NOTE name (e.g. 'CS5') -> frequency in Hz."""
    table = {}
    for m in DEFINE_RE.finditer(text):
        table[m.group(1) + m.group(2)] = int(m.group(3))
    return table


def split_note(name):
    """'CS5' -> ('CS', 5). Scan FORWARD to the first digit: scanning backwards
    walks off the front of a two-character name like 'D5'."""
    for i, ch in enumerate(name):
        if ch.isdigit():
            return name[:i], int(name[i:])
    return name, 0


def sharp_to_hash(letter):
    """'CS' -> 'C#'. RTTTL spells a sharp with '#', not 'S'."""
    return letter[:-1] + "#" if letter.endswith("S") else letter


def parse_melody(text, table):
    """Return a list of (freq_hz, duration) with the sign still on the
    duration, because negative means dotted."""
    body = re.search(r"int\s+melody\s*\[\s*\]\s*=\s*\{(.*?)\}\s*;", text, re.S)
    if not body:
        raise SystemExit("no `int melody[] = { ... }` found")

    pairs = []
    pending = None
    for tok in TOKEN_RE.findall(body.group(1)):
        if tok == "NOTE_REST":
            pending = 0
        elif tok.startswith("NOTE_"):
            name = tok[5:]
            if name not in table:
                raise SystemExit("unknown note " + tok)
            pending = table[name]
        else:
            if pending is None:
                continue          # stray duration with no note before it
            pairs.append((pending, int(tok)))
            pending = None
    return pairs


def most_common_octave(pairs, freq_to_name):
    counts = {}
    for f, _d in pairs:
        if f == 0:
            continue
        counts[split_note(freq_to_name[f])[1]] = \
            counts.get(split_note(freq_to_name[f])[1], 0) + 1
    return max(counts, key=counts.get) if counts else 5


def convert(text, tempo_override=None):
    table = read_note_table(text)
    if not table:
        raise SystemExit("no NOTE_* defines found")
    freq_to_name = {v: k for k, v in table.items()}

    pairs = parse_melody(text, table)
    if not pairs:
        raise SystemExit("no notes parsed")

    tempo = tempo_override
    if tempo is None:
        m = re.search(r"int\s+tempo\s*=\s*(\d+)", text)
        tempo = int(m.group(1)) if m else 63

    default_oct = most_common_octave(pairs, freq_to_name)

    toks = []
    for f, d in pairs:
        dur = "%d" % abs(d)
        if d < 0:
            dur += "."                 # dotted: the Arduino sign, the RTTTL mark
        if f == 0:
            toks.append(dur + "p")
            continue
        letter, octv = split_note(freq_to_name[f])
        letter = sharp_to_hash(letter)
        # Leave the default octave and default duration implicit: the strings
        # stay readable and a whole tune still fits in a couple of hundred
        # bytes, which is the entire reason for choosing RTTTL.
        toks.append(dur + letter + ("" if octv == default_oct else str(octv)))

    return "d=4,o=%d,b=%d:%s" % (default_oct, tempo, ",".join(toks))


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return 2
    src = sys.argv[1]
    tempo = int(sys.argv[3]) if len(sys.argv) > 3 else None
    with open(src, "r", encoding="utf-8", errors="replace") as f:
        text = f.read()
    rtttl = convert(text, tempo)

    if sys.argv[2] == "--print":
        print(rtttl)
        return 0
    name = os.path.splitext(os.path.basename(src))[0]
    with open(sys.argv[2], "w", encoding="utf-8", newline="\n") as f:
        f.write("# %s, converted from %s by tools/arduino2rtttl.py\n"
                % (name, os.path.basename(src)))
        f.write("%s:%s\n" % (name, rtttl))
    print("wrote %s (%d bytes of RTTTL)" % (sys.argv[2], len(rtttl)))
    return 0


if __name__ == "__main__":
    sys.exit(main())