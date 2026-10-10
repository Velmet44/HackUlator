/* Host-side verification of PowerKill's TPC/CSA beacon.
 *
 * Compiles main/pk_frame.c DIRECTLY - the same translation unit the firmware
 * links - so a pass here says something about the bytes that actually go
 * out. The BLE flood builder instead has a hand-kept twin in
 * tools/hk_advtest.c, and those two copies drift; this file exists so
 * PowerKill does not inherit that trap.
 *
 * Checks, and why each matters:
 *   - every IE's length byte matches what was written. A wrong length makes a
 *     client stop parsing at that point, silently killing every element after.
 *   - the fixed header is correct (FC, three MACs, TSF, interval, caps)
 *   - the CSA names a channel OTHER than the victim's (else it is a no-op)
 *   - the frame fits PK_FRAME_MAX. It does NOT fit the old 80-byte TX buffer,
 *     which truncated it while the driver still reported success.
 *   - TSF advances monotonically by exactly one beacon interval
 *   - the Power Constraint actually sweeps 30/35/20
 *   - a survey-supplied CSA channel is honoured, and a bad one is refused
 *   - degenerate inputs are rejected rather than building a broken frame
 *
 * ASCII only, deliberately: MSVC's default codepage mangles the em-dashes
 * the rest of the project uses, and /utf-8 has to be passed to every build.
 *
 * Build:
 *   cl /nologo /W4 /O1 /Fe:pkframe.exe tools\hk_pkframe.c main\pk_frame.c
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "../main/pk_frame.h"

static int g_fail;

static void check(int cond, const char *what) {
    printf("%s  %s\n", cond ? "PASS" : "FAIL", what);
    if (!cond) {
        g_fail++;
    }
}

static const uint8_t BSSID[6] = {0x28, 0xa9, 0x15, 0x6b, 0x93, 0x19};

/* Fixed beacon header is 36 bytes; IEs start there. */
#define IE_OFF 36

/* Walk the IE stream and verify each element's length byte accounts for
 * exactly the bytes it claims. Returns the element count, or a negative
 * error with *bad set to the offending id (-1 = trailing garbage). */
static int walk_ies(const uint8_t *f, int off, int end, int *bad) {
    int n = 0;
    while (off + 2 <= end) {
        uint8_t id = f[off];
        uint8_t len = f[off + 1];
        if (off + 2 + len > end) {
            *bad = id;
            return -1;
        }
        if (++n > 64) {
            *bad = id;          /* a bogus length that happens to fit */
            return -2;
        }
        off += 2 + len;
    }
    if (off != end) {
        *bad = -1;
        return -3;
    }
    return n;
}

static int find_ie(const uint8_t *f, int len, uint8_t want, const uint8_t **body,
                   int *body_len) {
    int off = IE_OFF;
    while (off + 2 <= len) {
        uint8_t id = f[off];
        uint8_t bl = f[off + 1];
        if (off + 2 + bl > len) {
            return 0;
        }
        if (id == want) {
            *body = f + off + 2;
            *body_len = bl;
            return 1;
        }
        off += 2 + bl;
    }
    return 0;
}

static uint32_t tsf_low(const uint8_t *f) {
    return (uint32_t)f[24] | ((uint32_t)f[25] << 8) | ((uint32_t)f[26] << 16) |
           ((uint32_t)f[27] << 24);
}

int main(void) {
    uint8_t f[PK_FRAME_MAX];
    const char *ssid = "JioFiber";
    int len;
    const uint8_t *b;
    int bl;

    printf("== header ==\n");
    pk_frame_seed(0x11111111u);
    len = pk_build_tpc_beacon(f, BSSID, ssid, 1, 0, 0);
    if (len <= 0) {
        check(0, "builds a frame");
        printf("\nRESULT: FAIL (builder returned %d)\n", len);
        return 1;
    }
    check(1, "builds a frame");
    check(len <= PK_FRAME_MAX, "fits PK_FRAME_MAX");
    printf("      length=%d  PK_FRAME_MAX=%d  (old TX buffer was 80)\n", len,
           PK_FRAME_MAX);
    check(len > 80, "would NOT have fit the old 80-byte TX buffer");

    check(f[0] == 0x80 && f[1] == 0x00, "FC = beacon, duration 0");
    check(memcmp(f + 4, "\xff\xff\xff\xff\xff\xff", 6) == 0,
          "addr1 is broadcast");
    check(memcmp(f + 10, BSSID, 6) == 0, "addr2 = target BSSID (SA)");
    check(memcmp(f + 16, BSSID, 6) == 0, "addr3 = target BSSID (BSSID)");
    check(tsf_low(f) != 0, "TSF initialised");
    check(f[32] == 0x64 && f[33] == 0x00, "beacon interval 100 TU");
    check(f[34] == 0x05 && f[35] == 0x01,
          "capability spectrum-mgmt + preamble");

    printf("\n== SSID ==\n");
    check(f[IE_OFF] == 0x00, "first IE is SSID");
    check(f[IE_OFF + 1] == (uint8_t)strlen(ssid), "SSID length byte correct");
    check(memcmp(f + IE_OFF + 2, ssid, strlen(ssid)) == 0,
          "SSID is the target's own");

    printf("\n== IE structure ==\n");
    int bad = 0;
    int n = walk_ies(f, IE_OFF, len, &bad);
    check(n > 0, "IE walk completes, every length byte self-consistent");
    if (n < 0) {
        printf("      malformed at element id %d\n", bad);
    } else {
        printf("      %d elements walk cleanly to the end of the frame\n", n);
    }

    printf("\n== required elements ==\n");
    check(find_ie(f, len, 0x07, &b, &bl), "Country IE present (TPC needs it)");
    check(find_ie(f, len, 0x20, &b, &bl), "Power Constraint present");
    check(find_ie(f, len, 0x25, &b, &bl), "CSA present");
    check(find_ie(f, len, 0x30, &b, &bl), "RSN present");

    printf("\n== CSA ==\n");
    if (find_ie(f, len, 0x25, &b, &bl) && bl == 3) {
        check(b[0] == 0x01, "CSA mode 1 (stop TX while switching)");
        check(b[2] == 0x00, "CSA count 0 (switch immediately)");
        check(b[1] >= 1 && b[1] <= 14, "CSA names a valid channel");
        check(b[1] != 1, "CSA names a channel OTHER than the victim's");
        printf("      victim on ch1, beacon steers to ch%d\n", b[1]);
    } else {
        check(0, "CSA body is 3 bytes");
    }

    printf("\n== TPC ==\n");
    if (find_ie(f, len, 0x07, &b, &bl) && bl == 6) {
        check(memcmp(b, "US ", 3) == 0, "Country code 'US '");
        check(b[5] == 30, "Country max TX power 30 dBm");
    } else {
        check(0, "Country body is 6 bytes");
    }
    if (find_ie(f, len, 0x20, &b, &bl) && bl == 1) {
        check(b[0] == 30, "Power Constraint 30 on tick 0");
        printf("      constraint %d vs ceiling 30 -> client computes %d dBm\n",
               b[0], 30 - b[0]);
    }

    printf("\n== CSA steering from a survey ==\n");
    pk_frame_seed(1);
    len = pk_build_tpc_beacon(f, BSSID, ssid, 1, 0, 11);
    if (find_ie(f, len, 0x25, &b, &bl) && bl == 3) {
        check(b[1] == 11, "surveyed channel honoured when valid");
    } else {
        check(0, "steered CSA parses");
    }
    len = pk_build_tpc_beacon(f, BSSID, ssid, 1, 0, 1);
    if (find_ie(f, len, 0x25, &b, &bl) && bl == 3) {
        check(b[1] != 1, "survey naming the victim's own channel is refused");
    }
    len = pk_build_tpc_beacon(f, BSSID, ssid, 1, 0, 99);
    if (find_ie(f, len, 0x25, &b, &bl) && bl == 3) {
        check(b[1] >= 1 && b[1] <= 14, "out-of-range survey falls back safely");
    }

    printf("\n== TSF advance ==\n");
    pk_frame_seed(0x20000000u);
    pk_build_tpc_beacon(f, BSSID, ssid, 1, 0, 0);
    uint32_t t1 = tsf_low(f);
    pk_build_tpc_beacon(f, BSSID, ssid, 1, 1, 0);
    uint32_t t2 = tsf_low(f);
    check(t2 == t1 + 100 * 1024, "TSF advances exactly one 100 TU interval");
    check(tsf_low(f) > t1, "TSF is monotonic (never stands still)");

    printf("\n== constraint sweep ==\n");
    int seen[3] = {-1, -1, -1};
    pk_frame_seed(7);
    for (int t = 0; t < 6; t++) {
        int l2 = pk_build_tpc_beacon(f, BSSID, ssid, 1, t, 0);
        if (find_ie(f, l2, 0x20, &b, &bl) && bl == 1) {
            seen[t % 3] = b[0];
        }
    }
    check(seen[0] == 30 && seen[1] == 35 && seen[2] == 20,
          "constraint sweeps 30 / 35 / 20");
    printf("      sequence across ticks: %d, %d, %d\n", seen[0], seen[1], seen[2]);

    printf("\n== long SSID ==\n");
    char longssid[33];
    memset(longssid, 'A', 32);
    longssid[32] = 0;
    pk_frame_seed(9);
    int ll = pk_build_tpc_beacon(f, BSSID, longssid, 6, 0, 0);
    check(ll > 0, "builds with a 32-char SSID");
    check(ll <= PK_FRAME_MAX, "long SSID still fits");
    bad = 0;
    check(walk_ies(f, IE_OFF, ll, &bad) > 0,
          "long-SSID IE walk still self-consistent");
    printf("      length with a 32-char SSID = %d (ch6)\n", ll);

    printf("\n== degenerate inputs ==\n");
    check(pk_build_tpc_beacon(f, BSSID, ssid, 0, 0, 0) < 0, "channel 0 rejected");
    check(pk_build_tpc_beacon(f, BSSID, ssid, 15, 0, 0) < 0, "channel 15 rejected");
    check(pk_build_tpc_beacon(f, NULL, ssid, 1, 0, 0) < 0, "NULL bssid rejected");
    check(pk_build_tpc_beacon(f, BSSID, NULL, 1, 0, 0) < 0, "NULL ssid rejected");
    check(pk_build_tpc_beacon(NULL, BSSID, ssid, 1, 0, 0) < 0, "NULL buffer rejected");

    printf("\n%s: %d failure(s)\n",
           g_fail ? "RESULT: FAIL" : "RESULT: ALL PASS", g_fail);
    return g_fail ? 1 : 0;
}