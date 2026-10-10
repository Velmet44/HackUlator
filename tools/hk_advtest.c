/* Host-side test for the ADV payload builder in svc_ble.c.
 *
 * The builder is pure logic - wordlist selection plus AD-element packing -
 * but it normally lives behind esp_random() and the BT stack. This harness
 * copies the two functions verbatim and links them against a stub RNG and a
 * stub length counter, so the AD structure and the name budget can be checked
 * on a PC without an ESP32 or a radio.
 *
 * If you change flood_pick_name() or flood_build_raw() in svc_ble.c, copy the
 * change here too. Keeping the two in sync by hand is deliberate: the copy is
 * small and the alternative (compiling svc_ble.c for the host) drags in
 * esp_bt.h.
 *
 * Build: gcc -o hk_advtest hk_advtest.c && ./hk_advtest
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* ---- stubs for the ESP-IDF bits the copied code touches ---- */
static uint32_t s_rand_state = 12345;
uint32_t esp_random(void) {
    s_rand_state = s_rand_state * 1103515245u + 12345u;
    return s_rand_state >> 8;
}
#define ESP_BLE_AD_TYPE_FLAG    0x01
#define ESP_BLE_AD_TYPE_NAME_CMPL 0x09
#define snprintf snprintf

/* ---- verbatim from svc_ble.h ---- */
typedef enum {
    BLEADV_NAMES_COMMON = 0,
    BLEADV_NAMES_GARBAGE,
    BLEADV_NAMES_RICKROLL,
    BLEADV_NAMES_SECURITY,
    BLEADV_NAMES_ALL,
    BLEADV_NAMES_COUNT
} bleadv_name_mode_t;

/* ---- verbatim from svc_ble.c ---- */
#define BLEADV_NAME_MAX 26
#define BLEADV_RAW_MAX  31
#define NEL(a) (sizeof(a) / sizeof((a)[0]))

static bleadv_name_mode_t s_name_mode = BLEADV_NAMES_COMMON;
static char s_flood_name[BLEADV_NAME_MAX + 1];

static const char *const s_names_common[] = {
    "HUAWEI Watch GT 3", "Fitbit Charge 5", "Galaxy Buds2", "AirPods Pro",
    "Tile Tracker", "Govee Sensor", "Sony WF-1000XM4", "Beats Studio Buds",
    "Yale Linus", "SwitchBot Meter", "Echo Dot", "Nest Mini",
};
static const char *const s_names_rickroll[] = {
    "Never Gonna Give You Up", "Together Forever", "Rickroll",
    "Never Gonna Let You Down",
};
static const char *const s_names_security[] = {
    "Free Public WiFi", "FBI Surveillance Van", "Update Your iPhone",
    "Microsoft Support", "IT Helpdesk", "Your Account Hacked",
};

static void flood_pick_name(char *out, size_t out_sz) {
    bleadv_name_mode_t m = s_name_mode;
    if (m == BLEADV_NAMES_ALL) {
        m = (bleadv_name_mode_t)(esp_random() % BLEADV_NAMES_COUNT);
    }
    if (m == BLEADV_NAMES_GARBAGE) {
        /* Printable but meaningless. 0x00 is excluded because it would
         * truncate the name the UI logs. */
        static const char alpha[] =
            "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_";
        size_t n = 4u + (size_t)(esp_random() % (BLEADV_NAME_MAX - 4u));
        if (n > out_sz - 1) {
            n = out_sz - 1;
        }
        for (size_t i = 0; i < n; i++) {
            out[i] = alpha[esp_random() % (sizeof(alpha) - 1)];
        }
        out[n] = 0;
        return;
    }
    const char *const *tbl;
    size_t n;
    if (m == BLEADV_NAMES_RICKROLL) {
        tbl = s_names_rickroll;
        n = NEL(s_names_rickroll);
    } else if (m == BLEADV_NAMES_SECURITY) {
        tbl = s_names_security;
        n = NEL(s_names_security);
    } else {
        tbl = s_names_common;
        n = NEL(s_names_common);
    }
    snprintf(out, out_sz, "%s", tbl[esp_random() % n]);
}

static int flood_build_raw(uint8_t *raw, size_t raw_sz) {
    char name[BLEADV_NAME_MAX + 1];
    flood_pick_name(name, sizeof(name));
    size_t nl = strlen(name);
    if (nl == 0 || nl > BLEADV_NAME_MAX) {
        return -1;
    }
    if (raw_sz < nl + 5u) {
        return -1;
    }
    size_t i = 0;
    /* LE General Discoverable (0x02) | BR/EDR not supported (0x04). Without
     * 0x04 some scanners treat the phantom as a classic device. */
    raw[i++] = 2;
    raw[i++] = ESP_BLE_AD_TYPE_FLAG;
    raw[i++] = 0x06;
    raw[i++] = (uint8_t)(nl + 1); /* AD length counts type + payload */
    raw[i++] = ESP_BLE_AD_TYPE_NAME_CMPL;
    memcpy(raw + i, name, nl);
    i += nl;
    snprintf(s_flood_name, sizeof(s_flood_name), "%s", name);
    return (int)i;
}

/* ---- checks ---- */
static int fails;

static void check(const char *name, int cond) {
    printf("%s %s\n", cond ? "PASS" : "FAIL", name);
    if (!cond) {
        fails++;
    }
}

/* Walk the AD elements the way a parser does and confirm every length byte
 * agrees with the bytes actually present. */
static int ad_wellformed(const uint8_t *raw, int len) {
    int i = 0;
    while (i < len) {
        int el = raw[i];
        if (el == 0) {
            return 0; /* a zero-length element is not a valid trailing byte */
        }
        if (i + 1 + el > len) {
            return 0; /* element claims more bytes than the buffer holds */
        }
        i += 1 + el;
    }
    return i == len;
}

int main(void) {
    uint8_t raw[BLEADV_RAW_MAX];

    printf("name budget: max %d, raw %d\n", BLEADV_NAME_MAX, BLEADV_RAW_MAX);

    /* 1. every wordlist builds a well-formed payload within budget */
    for (int m = 0; m < BLEADV_NAMES_COUNT; m++) {
        bleadv_name_mode_t mode = (bleadv_name_mode_t)m;
        const char *label[BLEADV_NAMES_COUNT] = {
            "COMMON", "GARBAGE", "RICKROLL", "SECURITY", "ALL"};
        int ok = 1, worst = 0;
        for (int i = 0; i < 4000; i++) {
            s_name_mode = mode;
            int n = flood_build_raw(raw, sizeof(raw));
            if (n < 0 || n > (int)sizeof(raw)) {
                ok = 0;
                break;
            }
            if (!ad_wellformed(raw, n)) {
                ok = 0;
                break;
            }
            if (strlen(s_flood_name) > BLEADV_NAME_MAX) {
                ok = 0;
                break;
            }
            if (n > worst) {
                worst = n;
            }
        }
        printf("  %-9s worst payload %2d bytes\n", label[m], worst);
        check(label[m], ok);
        check("  fits the 31-byte legacy budget", worst <= BLEADV_RAW_MAX);
    }

    /* 2. flags element is exactly what a scanner needs */
    s_name_mode = BLEADV_NAMES_COMMON;
    int n = flood_build_raw(raw, sizeof(raw));
    check("first element is flags", raw[0] == 2 && raw[1] == 0x01);
    check("flags = LE discoverable + no BR/EDR", raw[2] == 0x06);
    check("second element is complete name",
          raw[3] == (uint8_t)(strlen(s_flood_name) + 1) &&
          raw[4] == ESP_BLE_AD_TYPE_NAME_CMPL);

    /* 3. the name in the payload is the name reported to the UI */
    check("payload name matches reported name",
          memcmp(raw + 5, s_flood_name, strlen(s_flood_name)) == 0);
    check("total length matches builder return",
          n == 5 + (int)strlen(s_flood_name));

    /* 4. the longest hardcoded name still fits the budget */
    size_t longest = 0;
    for (size_t i = 0; i < NEL(s_names_common); i++) {
        size_t l = strlen(s_names_common[i]);
        if (l > longest) {
            longest = l;
        }
    }
    for (size_t i = 0; i < NEL(s_names_rickroll); i++) {
        size_t l = strlen(s_names_rickroll[i]);
        if (l > longest) {
            longest = l;
        }
    }
    for (size_t i = 0; i < NEL(s_names_security); i++) {
        size_t l = strlen(s_names_security[i]);
        if (l > longest) {
            longest = l;
        }
    }
    printf("longest wordlist entry: %zu bytes\n", longest);
    check("longest name fits beside the flags", longest + 5 <= BLEADV_RAW_MAX);

    /* 5. GARBAGE never emits a NUL or an out-of-alphabet byte */
    int alpha_ok = 1;
    for (int i = 0; i < 4000; i++) {
        s_name_mode = BLEADV_NAMES_GARBAGE;
        if (flood_build_raw(raw, sizeof(raw)) < 0) {
            alpha_ok = 0;
            break;
        }
        for (const char *c = s_flood_name; *c; c++) {
            if (!((*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z') ||
                  (*c >= '0' && *c <= '9') || *c == '-' || *c == '_')) {
                alpha_ok = 0;
                break;
            }
        }
        if (!alpha_ok) {
            break;
        }
    }
    check("GARBAGE stays inside the alphabet", alpha_ok);

    printf("RESULT: %s\n", fails ? "FAILURES" : "ALL PASS");
    return fails ? 1 : 0;
}