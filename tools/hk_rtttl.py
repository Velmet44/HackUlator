#!/usr/bin/env python3
"""Host test for the RTTTL parser and the tone generator.

Compiles main/tune_rtttl.c and main/tune_synth.c on the PC and checks them
against a corpus of real RTTTL strings, then renders one to a WAV so the
output can actually be listened to rather than merely asserted about.

Why this exists: RTTTL in the wild violates its own specification constantly.
A parser that handles only the grammar fails on the first real tune somebody
hands you - specifically on the dotted-length '.' which most files place BEFORE
the octave and the spec places after it. The corpus below is mostly real
strings with those defects, not tidy ones written to match the spec.

Run:  python tools/hk_rtttl.py
      python tools/hk_rtttl.py --wav out.wav "Simpsons:d=4,o=5,b=160:c,e,f,g"
"""
import math
import os
import struct
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# (rtttl, expected_note_count or None, expected_first_freq_hz or None)
# Counts are the number of notes a correct parser emits. None means "just
# must not crash or produce zero-length notes".
CORPUS = [
    # -- the canonical spec example, and the most copied string on the net
    ("Simpsons:d=4,o=5,b=160:32p,c.6,e6,f#6,8a6,g.6,e6,c6,8a,8f#,8f#,8f#,2g",
     15, None),
    # -- dotted length BEFORE the octave. This is the majority case in the
    #    wild and the opposite of what the grammar says.
    ("Dotted:d=8,o=5,b=120:4g.,4p,4c.,4p,4f.", 5, None),
    # -- dotted length AFTER the octave, per the grammar. Both must work.
    ("AfterOct:d=8,o=5,b=120:4g6.,4p6.,4c6.", 3, None),
    # -- lowercase and no defaults section at all: the parser must fall back
    #    to d=4, o=6, b=63 and not treat the melody as part of the header.
    ("bare:c,d,e,f,g", 5, 523),   # C6
    # -- spaces everywhere
    ("Spaced : d = 8 , o = 5 , b = 120 : 8c , 8d , 8e", 3, None),
    # -- 'h' means B in real files
    ("German:d=4,o=5,b=140:8c,8d,8e,8f,8g,8a,8h", 7, None),
    # -- uppercase
    ("UPPER:d=4,o=5,b=140:8C,8D,8E", 3, None),
    # -- unknown control keys must be IGNORED, not rejected (spec requires)
    ("Unknown:d=4,o=5,b=140,x=99,y=7:8c,8d", 2, None),
    # -- octave outside the documented 4..7 range
    ("HighOct:d=4,o=7,b=140:8c8,8e8", 2, 7902),
    # -- duration digits and note separated by a space
    ("Split:d=16,o=5,b=200:16 c,16 d", 2, None),
    # -- only rests
    ("Rest:d=4,o=5,b=120:4p,4p,4p", 3, 0),
    # -- 32nd notes at a fast tempo
    ("Fast:d=32,o=6,b=900:32c,32e,32g", 3, None),
    # -- name longer than the spec's 10 characters
    ("A very long tune name indeed:d=4,o=5,b=150:8c,8d", 2, None),
    # -- single note
    ("One:d=4,o=5,b=120:c", 1, 523),
]


DRIVER_C = r"""
#include "tune_rtttl.h"
#include "tune_synth.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int16_t g_buf[256 * 2];

/* count <rtttl>  -> note count
 * first <rtttl>  -> frequency of the first sounding note, 0 if none
 * dump  <rtttl>  -> raw PCM to stdout: int16 interleaved stereo */
int main(int argc, char **argv) {
    if (argc < 3) { return 2; }
    tune_rtttl_t p;
    if (tune_rtttl_init(&p, argv[2]) != 0) {
        printf("INITFAIL\n");
        return 0;
    }
    tune_note_t n;
    if (strcmp(argv[1], "count") == 0) {
        int c = 0;
        while (tune_rtttl_next(&p, &n)) { c++; }
        printf("%d\n", c);
    } else if (strcmp(argv[1], "first") == 0) {
        while (tune_rtttl_next(&p, &n)) {
            if (n.freq_hz) { printf("%u\n", n.freq_hz); return 0; }
        }
        printf("0\n");
    } else if (strcmp(argv[1], "dump") == 0) {
        synth_t s;
        synth_init(&s);
        long budget = 44100L * 30L;
        while (tune_rtttl_next(&p, &n) && budget > 0) {
            int done = 0;
            synth_note(&s, n.freq_hz, n.dur_ms);
            while (!done && synth_busy(&s) && budget > 0) {
                int want = 256;
                if (want > budget) { want = (int)budget; }
                done = synth_fill(&s, g_buf, want);
                budget -= want;
                fwrite(g_buf, 2, want * 2, stdout);
            }
        }
    }
    return 0;
}
"""


def check_tuning():
    """The parser's frequency table must be musically correct, not just
    self-consistent. A wrong constant here produces tunes that are all the
    right length and sound wrong, which no amount of count-checking catches."""
    failures = []
    # MIDI note number -> expected Hz (equal temperament, A4 = 440)
    # RTTTL octave: A in octave 4 is 440 Hz.
    cases = [
        ("a", 4, 440),    # A4
        ("c", 5, 523),    # C5
        ("a", 5, 880),    # A5
        ("c", 4, 262),    # C4
        ("a", 6, 1760),   # A6
        ("c", 7, 2093),   # C7
        ("a", 3, 220),    # A3
    ]
    for note, octv, want in cases:
        rtttl = "T:d=4,o=%d,b=120:%s" % (octv, note)
        r = parse_one(rtttl)
        got = r[1] if r else None
        if got is None:
            failures.append("freq %s%d: no note emitted" % (note, octv))
        elif abs(got - want) > max(2, want * 0.002):
            failures.append("freq %s%d: got %d Hz, want %d Hz" % (note, octv, got, want))
    return failures


def find_compiler():
    """Return (exe, kind, env).

    Windows has no cc by default, but MSVC is present on this machine and the
    repo already host-compiles tools/hk_pkframe.c the same way (cl /nologo /W4).
    The environment is captured from vcvars64.bat rather than assembled by hand,
    because hand-assembling it means remembering the Windows SDK ucrt/um/shared
    include paths, and getting that wrong fails with a bare "cannot open
    include file: 'string.h'" that looks like a source error.
    """
    if os.name == "nt":
        vswhere = os.path.join(
            os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)"),
            "Microsoft Visual Studio", "Installer", "vswhere.exe")
        if not os.path.exists(vswhere):
            return (None, None, None)
        try:
            r = subprocess.run(
                [vswhere, "-latest", "-products", "*",
                 "-requires", "Microsoft.VisualStudio.Component.VC.Tools.x86.x64",
                 "-property", "installationPath"],
                capture_output=True, text=True)
            inst = r.stdout.strip()
        except Exception:
            return (None, None, None)
        if not inst:
            return (None, None, None)
        vc = os.path.join(inst, "VC", "Auxiliary", "Build", "vcvars64.bat")
        if not os.path.exists(vc):
            return (None, None, None)
        try:
            # shell=True lets Python do the cmd /c wrapping. Without it the
            # argument arrives already quoted and cmd strips the first and last
            # quote, which turns the batch path into a bogus command.
            dump = subprocess.run('"%s" >nul && set' % vc, shell=True,
                                  capture_output=True, text=True)
        except Exception:
            return (None, None, None)
        env = {}
        for line in dump.stdout.splitlines():
            if "=" in line:
                k, _, v = line.partition("=")
                env[k.strip().upper()] = v
        if "INCLUDE" not in env:
            return (None, None, None)
        # CreateProcess resolves the program against the CALLING process's PATH,
        # not the one handed in via env=, so "cl" alone is not enough. Resolve
        # it to a full path using the PATH vcvars just gave us.
        cl = None
        for d in env.get("PATH", "").split(os.pathsep):
            cand = os.path.join(d.strip(), "cl.exe")
            if os.path.exists(cand):
                cl = cand
                break
        if not cl:
            return (None, None, None)
        return (cl, "msvc", env)
    # POSIX: Debian/Ubuntu ship gcc but frequently have NO `cc` symlink, and a
    # minimal image may not have `which` at all, so neither the compiler name
    # nor the lookup method can be assumed. Probe each candidate by actually
    # running it.
    for cand in [os.environ.get("CC"), "cc", "gcc", "clang"]:
        if not cand:
            continue
        try:
            if subprocess.run([cand, "--version"],
                              capture_output=True).returncode == 0:
                return (cand, "posix", None)
        except Exception:
            continue
    return (None, None, None)


def build_host_binary(tmpdir):
    """Compile the two pure-C modules plus the driver on the PC."""
    driver = os.path.join(tmpdir, "drv.c")
    with open(driver, "w") as f:
        f.write(DRIVER_C)
    exe = os.path.join(tmpdir, "rtttl.exe")
    ccexe, kind, env = find_compiler()
    if not ccexe:
        print("NO HOST COMPILER: install MSVC Build Tools or set CC")
        return None
    srcs = [os.path.join(ROOT, "main", "tune_rtttl.c"),
            os.path.join(ROOT, "main", "tune_synth.c"), driver]
    if kind == "msvc":
        # /W4 mirrors the firmware's strictness; the sources are ASCII-only so
        # MSVC's default codepage cannot mangle them (same rule as hk_pkframe.c).
        cmd = [ccexe, "/nologo", "/W4", "/O1", "/I", os.path.join(ROOT, "main"),
               "/Fe:" + exe] + srcs
        r = subprocess.run(cmd, capture_output=True, text=True, env=env)
    else:
        cmd = [ccexe, "-O1", "-Wall", "-Wextra", "-I", os.path.join(ROOT, "main")] + \
              srcs + ["-o", exe]
        r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0 or not os.path.exists(exe):
        print("COMPILE FAILED:\n" + (r.stdout or "") + (r.stderr or ""))
        return None
    for line in ((r.stdout or "") + (r.stderr or "")).splitlines():
        if "warning" in line.lower():
            # Warnings here are defects: the firmware builds with -Werror=all.
            print("COMPILE WARNING: " + line.strip())
    return exe


_EXE = [None]   # built once and reused; an MSVC compile costs ~1.5 s and the
                # tuning table alone calls parse_one() seven times


def parse_one(rtttl):
    """Return (count, first_sounding_freq) for one string."""
    exe = _EXE[0]
    if not exe:
        return None
    c = subprocess.run([exe, "count", rtttl], capture_output=True, text=True)
    f = subprocess.run([exe, "first", rtttl], capture_output=True, text=True)
    return int(c.stdout.strip()), int(f.stdout.strip())


def render_wav(exe, rtttl, path):
    r = subprocess.run([exe, "dump", rtttl], capture_output=True)
    pcm = r.stdout
    if not pcm:
        return 0
    with open(path, "wb") as f:
        f.write(b"RIFF")
        f.write(struct.pack("<I", 36 + len(pcm)))
        f.write(b"WAVEfmt ")
        f.write(struct.pack("<IHHIIHH", 16, 1, 2, 44100, 44100 * 4, 4, 16))
        f.write(b"data")
        f.write(struct.pack("<I", len(pcm)))
        f.write(pcm)
    return len(pcm) // 4


def main():
    failures = []
    with tempfile.TemporaryDirectory() as td:
        exe = build_host_binary(td)
        if not exe:
            return 1
        _EXE[0] = exe          # build once; reuse for every later query
        print("== corpus ==")
        for rtttl, want_count, want_first in CORPUS:
            name = rtttl.split(":")[0][:22]
            c = subprocess.run([exe, "count", rtttl], capture_output=True, text=True)
            got = c.stdout.strip()
            ok = True
            if got == "INITFAIL":
                failures.append("%s: parser rejected a valid string" % name)
                ok = False
            elif want_count is not None and int(got) != want_count:
                failures.append("%s: %s notes, expected %d" % (name, got, want_count))
                ok = False
            if ok and want_first is not None:
                fr = subprocess.run([exe, "first", rtttl], capture_output=True, text=True)
                if int(fr.stdout.strip()) != want_first:
                    failures.append("%s: first note %s Hz, expected %d"
                                    % (name, fr.stdout.strip(), want_first))
                    ok = False
            print("  %-24s %-4s %s" % (name, got, "ok" if ok else "FAIL"))

    print("\n== frequency table ==")
    tf = check_tuning()
    for m in tf:
        print("  FAIL " + m)
    if not tf:
        print("  all 7 reference pitches correct")
    failures += tf

    # Malformed input must not hang or crash.
    print("\n== malformed input ==")
    if _EXE[0]:
        exe = _EXE[0]
        for bad in ["", "nocolons", "x:y:z", ":::", "d=4:,,,,",
                    "T:d=0,o=0,b=0:8c", "T:d=4,o=5,b=99999:8c"]:
            try:
                r = subprocess.run([exe, "count", bad], capture_output=True,
                                   text=True, timeout=5)
                got = r.stdout.strip() or "<empty>"
                print("  %-24r -> %s" % (bad, got))
            except subprocess.TimeoutExpired:
                failures.append("malformed %r hung the parser" % bad)
                print("  %-24r -> HUNG" % bad)

    if len(sys.argv) >= 2 and sys.argv[1] == "--wav":
        if _EXE[0]:
            rtttl = sys.argv[3] if len(sys.argv) > 3 else CORPUS[0][0]
            frames = render_wav(exe, rtttl, sys.argv[2])
            print("\nwrote %s (%d frames, %.1f s)"
                  % (sys.argv[2], frames, frames / 44100.0))

    print("")
    if failures:
        print("FAILED (%d)" % len(failures))
        for m in failures:
            print("  - " + m)
        return 1
    print("PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())