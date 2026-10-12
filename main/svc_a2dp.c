#include "svc_a2dp.h"
#include "app_config.h"
#include "tune_rtttl.h"
#include "tune_synth.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_gap_bt_api.h"
#include "esp_a2dp_api.h"
/* Not esp_ble_gap.h: IDF 5.x split the BLE GAP API out, and the old name is
 * gone. svc_ble.c already carries this one. */
#include "esp_gap_ble_api.h"
/* AVRCP controller. See svc_a2dp_up() for why it is not optional. */
#include "esp_avrc_api.h"
/* The producer's real-time schedule. Split out so tools/hk_a2dp_pace.c can
 * compile the SAME functions and check the cadence - see the header. */
#include "a2dp_pace.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "string.h"
#include "nvs.h"
#include "nvs_flash.h"
#include <stdio.h>

static const char *TAG = "a2dp";

/* NO STATIC SEND BUFFER, and that is not a style preference.
 *
 * esp_a2d_source_audio_data_send() does NOT copy the payload:
 * btc_av_audio_buff_alloc() hands out a BT_HDR with the audio living INSIDE
 * that same allocation, and the esp_a2d_audio_buff_t the caller passes
 * overlays that BT_HDR. On ESP_OK the buffer is queued and the stack OWNS it;
 * it may only be freed if the send is refused. Handing it a static array
 * instead means the stack queues a pointer that is not a real BT_HDR
 * allocation - which corrupts the BT queue and presents as audio corruption
 * rather than an obvious crash.
 *
 * So: allocate per send, hand it over on success, free on refusal. This is
 * the same shape the working ESP32-A2DP library uses (A2DPEncoder.cpp,
 * send_pending()), and it is the only reason this feature can be trusted at
 * all.
 *
 * The synth writes into s_pcm first, which then gets copied into the stack's
 * buffer. s_pcm is int16_t purely so the compiler aligns it: Xtensa faults on
 * an unaligned halfword load. */

/* The chunk size, the MTU clamp and the whole real-time schedule live in
 * a2dp_pace.h, not here: they are arithmetic, they are the part of this
 * feature that cannot be observed from outside, and tools/hk_a2dp_pace.c
 * compiles the same header so the cadence is actually tested rather than
 * asserted in a comment. Everything below uses SEND_FRAMES only as the size
 * of the staging buffer. */
#define SEND_FRAMES (A2DP_SEND_BYTES / 4)

#define FEED_STACK 4096
#define FEED_PRIO  4

/* Housekeeping cadences, in milliseconds. Note that every one of these is a
 * RETRY: see svc_a2dp_pump(). A one-shot handshake with no retry is the whole
 * difference between "works on my speaker" and "works".
 *
 * The grace window is deliberately finite. Re-asking forever would also fight
 * the user: a speaker that suspends the stream because somebody pressed pause
 * on the speaker would be restarted by the pump about a second later. So the
 * pump is only allowed to be insistent while it is useful - during the window
 * right after connecting, or while the user has actually asked for audio and
 * it is not arriving. */
#define CONNECT_TIMEOUT_MS 12000
#define MEDIA_GRACE_MS     20000
#define MEDIA_RETRY_MS     2000
#define MEDIA_START_MS     6000

#define NAME_PROP_SHORT  0x08   /* Shortened Local Name  */
#define NAME_PROP_FULL   0x09   /* Complete Local Name   */

/* PIN offered to a sink that asks for legacy pairing. "0000" is what most
 * consumer audio gear expects from a source with no display; plenty of car
 * stereos and older headsets want "1234", which is what the working reference
 * library ships. Change it here, not in the reply handler.
 *
 * Not const: the GAP takes a non-const pointer and immediately writes a
 * terminator into the code it was handed, so a const array is a lie the
 * compiler is right to complain about. */
#ifndef HACKU_A2DP_PIN
#define HACKU_A2DP_PIN "1234"
#endif
static esp_bt_pin_code_t s_pin = {
    HACKU_A2DP_PIN[0], HACKU_A2DP_PIN[1],
    HACKU_A2DP_PIN[2], HACKU_A2DP_PIN[3],
};

/* Declared int16_t rather than uint8_t so it is 2-byte aligned by the compiler.
 * The synth writes through an int16_t* and the stack reads through a uint8_t*;
 * a uint8_t array would only be 1-byte aligned and Xtensa faults on an
 * unaligned halfword load. */
static int16_t s_pcm[A2DP_SEND_BYTES / 2];

/* ---- stack state ---- */
static int s_up;
static int s_connected;
static int s_playing;
static int s_media_ready;      /* ESP_A2D_AUDIO_STATE_STARTED seen */
static int s_conn_hdl;
static int s_tx_error;
static uint8_t s_peer_bda[6];
static char s_peer_name[A2DP_NAME_MAX + 1];
static int s_peer_mtu = 0;

/* Media state machine.
 *
 * The old code had two booleans and exactly ONE CHECK_SRC_RDY, sent once from
 * the connect callback. A sink that answered BUSY or FAILURE that first time -
 * entirely normal for a sink still opening its own end of the connection -
 * left the link up, the screen saying "linked", and no audio for ever, with no
 * way for the user to tell that apart from success. Every step now has a name
 * and a retry. */
typedef enum {
    MEDIA_IDLE = 0,   /* connected, not started */
    MEDIA_STARTING,   /* START issued, waiting for STARTED */
    MEDIA_STREAMING,  /* ESP_A2D_AUDIO_STATE_STARTED seen */
    MEDIA_DEAD        /* gave up; s_note says why */
} media_t;

static media_t s_media;
static int64_t s_media_ask_us;     /* last CHECK_SRC_RDY */
static int64_t s_media_start_us;   /* START issued */
static int64_t s_conn_at_us;       /* connect requested */
static int64_t s_conn_ok_us;       /* link came up */
static int     s_conn_asked;       /* a connect request is outstanding */
static int     s_avrc_connected;
static int     s_avrc_inited;
static char    s_note[24];

static void note_set(const char *n) {
    if (n) {
        snprintf(s_note, sizeof(s_note), "%s", n);
        s_note[sizeof(s_note) - 1] = 0;
    } else {
        s_note[0] = 0;
    }
}

/* ---- discovery ---- */
static a2dp_sink_t *s_out;
static int s_max;
static int s_n;
static a2dp_sink_t *s_ble_out;
static int s_ble_max;
static int s_ble_n;
static const char *s_ble_want;

/* Active scan: a passive scan would miss devices that keep their name in the
 * scan response rather than the advert. */
static esp_ble_scan_params_t g_ble_params = {
    .scan_type          = BLE_SCAN_TYPE_ACTIVE,
    .own_addr_type      = BLE_ADDR_TYPE_PUBLIC,
    .scan_filter_policy = BLE_SCAN_FILTER_ALLOW_ALL,
    .scan_interval      = 0x50,
    .scan_window        = 0x30,
    .scan_duplicate     = BLE_SCAN_DUPLICATE_DISABLE,
};


/* ---- stored peer ----
 *
 * The whole point of this block is that the speaker must NOT have to be in
 * pairing mode on a normal run. Once bonded, Bluedroid holds the link key in
 * NVS and the ESP32 can connect straight to a stored address. Discovery is
 * only for the first bond, and that is stated on screen rather than assumed.
 *
 * NVS is shared with everything else on the device, so the namespace is our
 * own and the key is short. A failure here is not fatal: the feature falls
 * back to discovery, which is exactly what the user did the first time.
 */
#define NVS_NS   "a2dp"
#define NVS_KEY  "peer"
#define NVS_LEN  (6 + A2DP_NAME_MAX + 1)

static uint8_t s_stored_bda[6];
static char s_stored_name[A2DP_NAME_MAX + 1];
static int s_have_stored;

static void stored_load(void) {
    nvs_handle_t h;
    size_t len = 0;
    uint8_t buf[NVS_LEN];
    s_have_stored = 0;
    s_stored_name[0] = 0;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        if (nvs_get_blob(h, NVS_KEY, buf, &len) == ESP_OK &&
            len >= 6 && len <= NVS_LEN) {
            memcpy(s_stored_bda, buf, 6);
            memcpy(s_stored_name, &buf[6], len - 6);
            s_stored_name[len - 6] = 0;
            s_have_stored = 1;
        }
        nvs_close(h);
    }
    /* Fall back to the built-in target when NVS has nothing yet.
     *
     * This is what removes the pairing-mode requirement entirely. Discovery is
     * the fragile part of the whole feature: classic inquiry only reaches a
     * device in discoverable mode, and a speaker that is merely switched on
     * is invisible to it. Connecting to a KNOWN address needs neither
     * discoverability nor pairing - it is a direct connection request to a
     * powered-on device, which is exactly "turn it on and nothing else".
     * NVS overrides this the first time a speaker is picked from the list. */
    if (!s_have_stored && HACKU_A2DP_DEFAULT_PEER[0]) {
        uint32_t a, b, c, d, e, g;
        if (sscanf(HACKU_A2DP_DEFAULT_PEER, "%2x:%2x:%2x:%2x:%2x:%2x",
                   (unsigned *)&a, (unsigned *)&b, (unsigned *)&c,
                   (unsigned *)&d, (unsigned *)&e, (unsigned *)&g) == 6) {
            s_stored_bda[0] = (uint8_t)a;
            s_stored_bda[1] = (uint8_t)b;
            s_stored_bda[2] = (uint8_t)c;
            s_stored_bda[3] = (uint8_t)d;
            s_stored_bda[4] = (uint8_t)e;
            s_stored_bda[5] = (uint8_t)g;
            snprintf(s_stored_name, sizeof(s_stored_name), "%s",
                     HACKU_A2DP_DEFAULT_NAME);
            s_have_stored = 1;
            ESP_LOGI(TAG, "built-in peer %s %s", HACKU_A2DP_DEFAULT_NAME,
                     HACKU_A2DP_DEFAULT_PEER);
        }
    }
    if (s_have_stored) {
        ESP_LOGI(TAG, "stored peer %s", s_stored_name);
    }
}

static void stored_save(const uint8_t *bda, const char *name) {
    uint8_t buf[NVS_LEN];
    nvs_handle_t h;
    size_t nl = strlen(name);
    if (nl > A2DP_NAME_MAX) {
        nl = A2DP_NAME_MAX;
    }
    memcpy(s_stored_bda, bda, 6);
    memcpy(s_stored_name, name, nl);
    s_stored_name[nl] = 0;
    memcpy(buf, bda, 6);
    memcpy(&buf[6], name, nl);
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    if (nvs_set_blob(h, NVS_KEY, buf, 6 + nl) != ESP_OK) {
        nvs_close(h);
        return;
    }
    nvs_commit(h);
    nvs_close(h);
    s_have_stored = 1;
    ESP_LOGI(TAG, "saved peer %s", s_stored_name);
}

/* ---- BLE-by-name discovery ----
 *
 * See the header for why this exists: it is what removes the pairing-mode
 * requirement. A BLE advert's MAC field IS the device's Bluetooth address, so
 * a scan hands back exactly the thing esp_a2d_source_connect() wants.
 *
 * The radio is the controller svc_ble shares, so this cannot run while the
 * A2DP source is live - the caller has already torn the other side down, and
 * this brings BLE up only for the duration of the scan.
 */
/* Defined further down, next to the other EIR use; needed here first because
 * the BLE path reuses the same parser. */
static void name_from_eir(const uint8_t *eir, int len, char *out, int outsz);

/* BLE scan result arrives as ONE event per device in IDF 5.x. */
static void ble_find_cb(esp_gap_ble_cb_event_t ev, esp_ble_gap_cb_param_t *param) {
    a2dp_sink_t *out;
    const char *want;
    char nm[A2DP_NAME_MAX + 1];
    int i;
    int len;

    if (ev != ESP_GAP_BLE_SCAN_RESULT_EVT) {
        return;
    }
    out = s_ble_out;
    want = s_ble_want;
    /* want is OPTIONAL: NULL means collect every named advertiser. The guard
     * must not require it, or the collect-all mode silently finds nothing -
     * which is exactly what it did until this line was fixed. */
    if (!out || s_ble_max <= 0 || s_ble_n >= s_ble_max) {
        return;
    }
    /* IDF 5.x delivers ONE device per event, with the raw EIR blob rather
     * than a pre-parsed name, so the name has to come out of the advert.
     * The bda here IS the Bluetooth address esp_a2d_source_connect() wants -
     * that is the whole reason this path exists. */
    len = (int)param->scan_rst.adv_data_len + (int)param->scan_rst.scan_rsp_len;
    if (len <= 0 || len > (int)sizeof(param->scan_rst.ble_adv)) {
        len = (int)sizeof(param->scan_rst.ble_adv);
    }
    name_from_eir(param->scan_rst.ble_adv, len, nm, sizeof(nm));
    /* want == NULL collects EVERY named advertiser. That is the normal mode:
     * asking the user to type an exact device name on a six-button keypad is
     * a terrible UI, and most speakers broadcast BLE continuously while merely
     * powered on - which is exactly the case pairing mode was supposed to
     * avoid needing. */
    if (!nm[0]) {
        return;
    }
    if (want && strcmp(nm, want) != 0) {
        return;
    }
    for (i = 0; i < s_ble_n; i++) {
        if (memcmp(out[i].bda, param->scan_rst.bda, 6) == 0) {
            return;
        }
    }
    memset(&out[s_ble_n], 0, sizeof(out[s_ble_n]));
    memcpy(out[s_ble_n].bda, param->scan_rst.bda, 6);
    snprintf(out[s_ble_n].name, sizeof(out[s_ble_n].name), "%s", nm);
    out[s_ble_n].rssi = (int8_t)param->scan_rst.rssi;
    s_ble_n++;
}

int svc_a2dp_find_by_ble(a2dp_sink_t *out, int max, int seconds,
                         const char *name_want) {
    esp_err_t r;
    if (!out || max <= 0 || max > A2DP_MAX_SINKS || seconds <= 0) {
        return -1;
    }
    /* The stack is brought up by the CALLER and must already be running.
     *
     * This used to bring the controller up and tear it down itself, which
     * silently broke the classic inquiry that ran next: svc_a2dp_discover()
     * calls this first, and this function ended with esp_bt_controller_deinit(),
     * so esp_bt_gap_start_discovery() afterwards was posting requests at a
     * stack that no longer existed. Result: the BLE half worked, the inquiry
     * half found nothing, forever, and it looked exactly like a device that
     * was not discoverable. */
    if (!s_up) {
        return -2;
    }
    s_ble_out = out;
    s_ble_max = max;
    s_ble_want = name_want;
    s_ble_n = 0;
    if (esp_ble_gap_register_callback(ble_find_cb) != ESP_OK) {
        s_ble_out = NULL;
        return -3;
    }
    esp_ble_gap_set_scan_params(&g_ble_params);
    r = esp_ble_gap_start_scanning((uint32_t)seconds);
    if (r != ESP_OK) {
        esp_ble_gap_register_callback(NULL);
        s_ble_out = NULL;
        return -4;
    }
    vTaskDelay(pdMS_TO_TICKS(seconds * 1000));
    esp_ble_gap_stop_scanning();
    esp_ble_gap_register_callback(NULL);
    s_ble_out = NULL;
    s_ble_want = NULL;
    ESP_LOGI(TAG, "ble-by-name found %d", s_ble_n);
    return s_ble_n;
}
/* ---- producer ---- */
static TaskHandle_t s_task;
static volatile int s_task_run;
static volatile int s_task_quit;
static volatile int s_tune_done;
static const char *s_tune;
static volatile uint32_t s_frames;
static volatile uint32_t s_notes;
static volatile uint32_t s_under;

/* ---- helpers ---- */

static void bda_str(const uint8_t *b, char *out) {
    snprintf(out, 18, "%02x:%02x:%02x:%02x:%02x:%02x",
             b[0], b[1], b[2], b[3], b[4], b[5]);
}

/* Pull a device name out of the EIR blob from an inquiry result. Inquiry is
 * the only way to find an A2DP sink and the name is optional in the reply, so
 * this can legitimately come back empty - the address still works. */
static void name_from_eir(const uint8_t *eir, int len, char *out, int outsz) {
    int i = 0;
    out[0] = 0;
    if (!eir) {
        return;
    }
    while (i + 1 < len) {
        int l = eir[i];
        if (l == 0) {
            break;
        }
        if (i + 1 + l > len) {
            break;
        }
        if (eir[i + 1] == NAME_PROP_FULL || eir[i + 1] == NAME_PROP_SHORT) {
            int n = l - 1;
            if (n > outsz - 1) {
                n = outsz - 1;
            }
            memcpy(out, &eir[i + 2], (size_t)n);
            out[n] = 0;
            return;
        }
        i += 1 + l;
    }
}

static void add_sink(const uint8_t *bda, const uint8_t *eir, int eirlen,
                     int8_t rssi) {
    int i;
    char tmp[A2DP_NAME_MAX + 1];
    if (s_n >= s_max) {
        return;
    }
    /* Dedup: a speaker answers inquiry from several channels and would
     * otherwise fill the whole list with the same device. */
    for (i = 0; i < s_n; i++) {
        if (memcmp(s_out[i].bda, bda, 6) == 0) {
            return;
        }
    }
    memset(&s_out[s_n], 0, sizeof(s_out[s_n]));
    memcpy(s_out[s_n].bda, bda, 6);
    name_from_eir(eir, eirlen, tmp, sizeof(tmp));
    if (tmp[0]) {
        snprintf(s_out[s_n].name, sizeof(s_out[s_n].name), "%s", tmp);
    } else {
        snprintf(s_out[s_n].name, sizeof(s_out[s_n].name), "%02x:%02x:%02x",
                 bda[0], bda[1], bda[2]);
    }
    s_out[s_n].rssi = rssi;
    s_n++;
}

/* ---- GAP: classic inquiry ---- */

static void gap_cb(esp_bt_gap_cb_event_t ev, esp_bt_gap_cb_param_t *param) {
    if (ev == ESP_BT_GAP_DISC_RES_EVT) {
        uint32_t cod = 0;
        int8_t rssi = 0;
        const uint8_t *eir = NULL;
        int eirlen = 0;
        int i;
        for (i = 0; i < param->disc_res.num_prop; i++) {
            esp_bt_gap_dev_prop_t *pr = param->disc_res.prop + i;
            switch (pr->type) {
            case ESP_BT_GAP_DEV_PROP_COD:
                cod = *(uint32_t *)pr->val;
                break;
            case ESP_BT_GAP_DEV_PROP_RSSI:
                rssi = *(int8_t *)pr->val;
                break;
            case ESP_BT_GAP_DEV_PROP_EIR:
                eir = (const uint8_t *)pr->val;
                eirlen = pr->len;
                break;
            default:
                break;
            }
        }
        /* The whole filter: an audio SINK advertises the "Rendering" service
         * class in its Class of Device. Headphones and speakers set it,
         * phones and laptops generally do not, which is what keeps the list
         * to things this can actually play to. */
        if (!esp_bt_gap_is_valid_cod(cod)) {
            return;
        }
        if (!(esp_bt_gap_get_cod_srvc(cod) & ESP_BT_COD_SRVC_RENDERING)) {
            return;
        }
        add_sink(param->disc_res.bda, eir, eirlen, rssi);
    } else if (ev == ESP_BT_GAP_DISC_STATE_CHANGED_EVT) {
        if (param->disc_st_chg.state == ESP_BT_GAP_DISCOVERY_STOPPED) {
            ESP_LOGI(TAG, "inquiry done: %d sinks", s_n);
            /* Inquiry REQUIRES the local device to be discoverable, and
             * esp_bt_gap_start_discovery() does not make it so by itself -
             * it only issues the HCI_Inquiry. So the window opened for the
             * search is closed again here, or we stay answerable to every
             * other classic device in the room for the rest of the session.
             * Connectable is kept: the peer may want to call us back. */
            esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE,
                                     ESP_BT_NON_DISCOVERABLE);
        }
    } else if (ev == ESP_BT_GAP_PIN_REQ_EVT) {
        /* A headset that only accepts bonded hosts refuses the very first
         * A2DP connection, and pairing cannot begin without a PIN request
         * being answered. Nothing here can type into the sink, so a fixed PIN
         * is the only option and the answer must be to every request.
         *
         * LENGTH MATTERS and the old code got it wrong: the request says
         * whether it wants at least 16 digits, and replying with 4 to a
         * 16-digit request is a pairing failure that looks exactly like an
         * unresponsive device. */
        ESP_LOGI(TAG, "PIN request (16-digit=%d), answering",
                 (int)param->pin_req.min_16_digit);
        if (param->pin_req.min_16_digit) {
            esp_bt_pin_code_t sixteen = {0};
            /* Sixteen NULs, not '0' characters: the IDF example replies with
             * all-zero for this case and it is what gear expects. */
            esp_bt_gap_pin_reply(param->pin_req.bda, true, 16, sixteen);
        } else {
            esp_bt_gap_pin_reply(param->pin_req.bda, true, 4, s_pin);
        }
    } else if (ev == ESP_BT_GAP_CFM_REQ_EVT) {
        /* THE EVENT THAT WAS MISSING, and it is the whole reason pairing
         * stalls on most modern audio gear.
         *
         * Every headset from the last decade negotiates Secure Simple Pairing
         * rather than legacy PIN. SSP means the stack raises a user-
         * confirmation request that the application MUST answer with
         * esp_bt_gap_ssp_confirm_reply() - there is no default. Handling
         * PIN_REQ and KEY_REQ but not CFM_REQ means pairing sits until it
         * times out (tens of seconds) and the A2DP connection never completes,
         * which presents as "the device is not discoverable" or "it will not
         * connect" with no log anywhere near the cause.
         *
         * Accepting unconditionally is correct HERE and only here: this is a
         * source with no display and no user, own lab kit, and the peer is a
         * speaker being driven with a synthesised tone. The reference
         * implementation does exactly this. */
        ESP_LOGI(TAG, "SSP confirm request, accepting");
        esp_bt_gap_ssp_confirm_reply(param->cfm_req.bda, true);
    } else if (ev == ESP_BT_GAP_KEY_REQ_EVT) {
        /* Passkey entry. 0 means "Just Works": correct for a device with no
         * keypad, and the only value that pairs with anything that does not
         * insist on a six-digit code. */
        esp_bt_gap_ssp_passkey_reply(param->key_req.bda, true, 0);
    } else if (ev == ESP_BT_GAP_KEY_NOTIF_EVT) {
        /* Just informational - the peer showing us a passkey it generated. */
        ESP_LOGI(TAG, "SSP passkey notification %d",
                 (int)param->key_notif.passkey);
    } else if (ev == ESP_BT_GAP_AUTH_CMPL_EVT) {
        if (param->auth_cmpl.stat == ESP_BT_STATUS_SUCCESS) {
            ESP_LOGI(TAG, "bonded with peer");
        } else {
            ESP_LOGW(TAG, "bond failed %d", (int)param->auth_cmpl.stat);
            note_set("no pair");
        }
    } else if (ev == ESP_BT_GAP_MODE_CHG_EVT) {
        ESP_LOGD(TAG, "scan mode %d", (int)param->mode_chg.mode);
    }
}

/* ---- A2DP ---- */

/* Nudge the sink into actually rendering.
 *
 * Not every sink starts rendering because the media stream started. TVs,
 * soundbars and anything running an Android AVRCP target typically wait for a
 * PLAY passthrough before they open their output, and they will sit there
 * "connected, streaming, silent" for ever without it. The reference library
 * drives the same AVRC controller for exactly this reason.
 *
 * Best effort: a sink with no AVRC channel, or one that has not finished
 * connecting AVRCP yet, simply has nothing to send and the call is skipped.
 * It is never allowed to be load-bearing - A2DP alone is enough for a
 * headphone, and this must not be able to break that. */
static void avrc_play(void) {
    static uint8_t tl;
    if (!s_avrc_inited || !s_avrc_connected) {
        return;
    }
    tl = (uint8_t)((tl + 1) & 0x0F);
    if (esp_avrc_ct_send_passthrough_cmd(tl, ESP_AVRC_PT_CMD_PLAY,
                                         ESP_AVRC_PT_CMD_STATE_PRESSED) != ESP_OK) {
        return;
    }
    esp_avrc_ct_send_passthrough_cmd(tl, ESP_AVRC_PT_CMD_PLAY,
                                     ESP_AVRC_PT_CMD_STATE_RELEASED);
    ESP_LOGI(TAG, "avrc play sent");
}

static void avrc_cb(esp_avrc_ct_cb_event_t ev, esp_avrc_ct_cb_param_t *param) {
    if (ev == ESP_AVRC_CT_CONNECTION_STATE_EVT) {
        s_avrc_connected = param->conn_stat.connected ? 1 : 0;
        ESP_LOGI(TAG, "avrc %s", s_avrc_connected ? "up" : "down");
        if (s_avrc_connected && s_media_ready) {
            avrc_play();
        }
    }
}

static void media_ask(void) {
    s_media_ask_us = esp_timer_get_time();
    s_media = MEDIA_STARTING;
    if (esp_a2d_media_ctrl(ESP_A2D_MEDIA_CTRL_CHECK_SRC_RDY) != ESP_OK) {
        /* The stack refused the request itself - it is on the way down, or a
         * previous command has not been acknowledged yet. Either way the pump
         * retries, so this is a note and not an error. */
        s_media = MEDIA_IDLE;
    }
}

static void a2d_cb(esp_a2d_cb_event_t ev, esp_a2d_cb_param_t *param) {
    switch (ev) {
    case ESP_A2D_CONNECTION_STATE_EVT: {
        if (param->conn_stat.state == ESP_A2D_CONNECTION_STATE_CONNECTED) {
            s_connected = 1;
            s_conn_asked = 0;
            s_conn_hdl = (int)param->conn_stat.conn_hdl;
            s_peer_mtu = param->conn_stat.audio_mtu;
            if (s_peer_mtu <= A2DP_SEND_GUARD + 32) {
                /* Never trust a nonsensical MTU: everything downstream sizes
                 * its sends from this and a zero would divide by nothing. */
                s_peer_mtu = 672;
            }
            s_media = MEDIA_IDLE;
            s_media_ready = 0;
            s_conn_ok_us = esp_timer_get_time();
            note_set(NULL);
            ESP_LOGI(TAG, "connected hdl=%d mtu=%d", s_conn_hdl, s_peer_mtu);
            /* Invisible to everyone else from here on. The reference library
             * does this for the same reason: leaving the device connectable
             * and discoverable while a source connection is up is how the
             * peer ends up racing us for the link. */
            esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE,
                                     ESP_BT_NON_DISCOVERABLE);
            /* Ask whether the source is ready, then start. FIRST ask only -
             * svc_a2dp_pump() retries, which is what makes this work on a sink
             * that is not ready the instant the ACL comes up. */
            media_ask();
        } else {
            s_connected = 0;
            s_playing = 0;
            s_media_ready = 0;
            s_media = MEDIA_IDLE;
            s_conn_hdl = 0;
            ESP_LOGI(TAG, "disconnected");
        }
        break;
    }
    case ESP_A2D_MEDIA_CTRL_ACK_EVT: {
        esp_a2d_media_ctrl_t cmd = param->media_ctrl_stat.cmd;
        esp_a2d_media_ctrl_ack_t st = param->media_ctrl_stat.status;
        if (cmd == ESP_A2D_MEDIA_CTRL_CHECK_SRC_RDY) {
            if (st == ESP_A2D_MEDIA_CTRL_ACK_SUCCESS) {
                ESP_LOGI(TAG, "sink ready, starting media");
                s_media = MEDIA_STARTING;
                s_media_start_us = esp_timer_get_time();
                if (esp_a2d_media_ctrl(ESP_A2D_MEDIA_CTRL_START) != ESP_OK) {
                    s_media = MEDIA_IDLE;
                }
            } else {
                /* FAILURE or BUSY. Both mean "not now", and the old code just
                 * dropped them on the floor - which is why a busy sink stayed
                 * silent for the rest of the session. Back to IDLE and let the
                 * pump ask again. */
                ESP_LOGW(TAG, "sink not ready (ack %d)", (int)st);
                s_media = MEDIA_IDLE;
                note_set("sink busy");
            }
        } else if (cmd == ESP_A2D_MEDIA_CTRL_START) {
            if (st == ESP_A2D_MEDIA_CTRL_ACK_SUCCESS) {
                /* Waiting for AUDIO_STATE_STARTED, which is the real gate. */
                s_media = MEDIA_STARTING;
            } else if (st == ESP_A2D_MEDIA_CTRL_ACK_FAILURE) {
                ESP_LOGE(TAG, "media start refused");
                s_media = MEDIA_IDLE;
                note_set("no start");
            }
            /* ACK_BUSY is explicitly "the previous command has not been
             * acknowledged yet" - stay in STARTING and let the pump time it
             * out. Nothing to do here, and treating it as a failure would
             * restart a start that is already in flight. */
        } else if (cmd == ESP_A2D_MEDIA_CTRL_SUSPEND) {
            s_playing = 0;
            s_media_ready = 0;
            s_media = MEDIA_IDLE;
        }
        break;
    }
    case ESP_A2D_AUDIO_CFG_EVT: {
        /* Codec capabilities negotiated. Nothing to configure - the SBC
         * encoder is the stack's - but the event is the proof that the two
         * ends agreed, and logging it is the only way to tell "no sound" from
         * "never negotiated" on a bring-up. */
        ESP_LOGI(TAG, "codec cfg type=0x%08x", (unsigned)param->audio_cfg.mcc.type);
        break;
    }
    case ESP_A2D_AUDIO_STATE_EVT: {
        if (param->audio_stat.state == ESP_A2D_AUDIO_STATE_STARTED) {
            s_media_ready = 1;
            s_playing = 1;
            s_media = MEDIA_STREAMING;
            note_set(NULL);
            ESP_LOGI(TAG, "media started");
            avrc_play();
        } else {
            s_media_ready = 0;
            s_playing = 0;
            if (s_media != MEDIA_DEAD) {
                s_media = MEDIA_IDLE;
            }
            /* The sink suspended us. Worth saying out loud, because the
             * symptom is identical to a dead producer: silence. */
            ESP_LOGI(TAG, "media suspended by sink");
        }
        break;
    }
    default:
        break;
    }
}


/* ---- producer ----
 *
 * This task is the ONLY thing that calls esp_a2d_source_audio_data_send().
 * Two rules, both learned from projects that crashed before they worked:
 *   - never synthesise inside a Bluetooth callback. The A2DP callbacks run on
 *     the stack's own task and anything slow there stalls the link.
 *   - ESP_FAIL from the send means the buffer queue is full, not that
 *     something broke. Back off and retry; spinning would starve the stack of
 *     the CPU it needs to drain it.
 */

/* How many stereo frames one send may carry, for the current MTU. */
static int send_frames(void) {
    return a2dp_send_frames(s_peer_mtu);
}

static void feed_task(void *arg) {
    synth_t syn;
    tune_rtttl_t rp;
    tune_note_t note;
    int have_tune = 0;
    (void)arg;

    synth_init(&syn);
    memset(&rp, 0, sizeof(rp));
    memset(&note, 0, sizeof(note));

    if (s_tune) {
        if (tune_rtttl_init(&rp, s_tune) == 0) {
            have_tune = 1;
        } else {
            ESP_LOGE(TAG, "tune header rejected");
            have_tune = 0;
        }
    }

    /* Real-time pacing against the SAMPLE CLOCK, not the CPU.
     *
     * Three attempts are visible in this loop's history and the third is the
     * one that is right:
     *
     *  1. Send as fast as the stack takes it. Trips the task WDT - the loop
     *     never yields, the idle task starves. A2DP is a real-time producer.
     *  2. Count frames into a 441-frame (10 ms) budget and vTaskDelay(1) per
     *     window. That is roughly the right idea but the arithmetic is wrong:
     *     the chunk is 168 frames, so a window closes on 504 frames - 11.4 ms
     *     of audio handed over in 10 ms of wall clock. Every tune ran ~4 %
     *     fast and the error compounded, because the delay is a tick and the
     *     tick jitters, while the budget is not re-derived from real time at
     *     all.
     *  3. Keep an ABSOLUTE due time in esp_timer microseconds and send
     *     whenever it is within one tick. Drift then cancels instead of
     *     accumulating, and a hiccup is caught up over the next few
     *     iterations instead of being paid off as a permanent error. This is
     *     the shape the reference library's managed-encoder task uses, and
     *     the reason its pacing constants are a LEAD and a MAX LAG.
     *
     * The lead keeps the producer slightly AHEAD of real time, because a
     * source that is exactly on time starves the sink's jitter buffer the
     * moment a send is one tick late. The lag limit stops the opposite
     * failure: after a long stall (a debugger pause, a Wi-Fi beacon storm)
     * the backlog is resynchronised rather than dumped, which would come out
     * of the speaker as a burst of noise.
     *
     * Nothing here may busy-wait: the tick is the only sleep available and
     * it is 10 ms. */
    {
        /* t0 is when THIS stream started; run_frames is how many frames have
         * been handed to the stack since. The due time comes from that pair
         * and from nothing else - see a2dp_pace.h for why a per-chunk
         * duration is not the same thing and loses 1 us every three chunks. */
        int64_t t0_us = 0;
        int64_t run_frames = 0;
        while (s_task_run) {
            if (!s_connected) {
                /* The peer went away. Sit out the reconnect rather than
                 * spinning: the pump will put the link back and unblock this
                 * the same way. */
                t0_us = 0;
                run_frames = 0;
                vTaskDelay(pdMS_TO_TICKS(20));
                continue;
            }
            if (!s_media_ready) {
                /* Drop the schedule when the stream stops. Carrying a due time
                 * across a suspend would make the producer instantly "late" on
                 * the other side of it and resynchronise on every pause. */
                t0_us = 0;
                run_frames = 0;
                vTaskDelay(pdMS_TO_TICKS(10));
                continue;
            }
            {
                int frames = send_frames();
                esp_a2d_audio_buff_t *buf;
                esp_err_t rc;
                int bytes = frames * 4;
                int stall = 0;
                int64_t now = esp_timer_get_time();
                int64_t due = 0;
                a2dp_pace_t pace;

                if (t0_us == 0) {
                    /* First send of this stream. Starting the clock now is
                     * what makes the lead a real lead rather than a claim. */
                    t0_us = now;
                }
                pace = a2dp_pace(now, t0_us, run_frames, frames, &due);
                if (pace == A2DP_PACE_SLEEP) {
                    /* A whole tick: the only sleep that does not starve the
                     * idle task, and safe here because a tick is always enough
                     * to become due (proved in tools/hk_a2dp_pace.c). */
                    vTaskDelay(1);
                    continue;
                }
                if (pace == A2DP_PACE_RESYNC) {
                    /* Too far behind to catch up. Rebase onto now and drop
                     * the backlog: flushing it would put a quarter second of
                     * audio at the speaker in one go. */
                    ESP_LOGW(TAG, "producer %lldus late, resync",
                             (long long)(now - due));
                    t0_us = now - a2dp_elapsed_us(run_frames);
                    continue;
                }
            if (!synth_busy(&syn)) {
                if (have_tune) {
                    if (tune_rtttl_next(&rp, &note)) {
                        synth_note(&syn, note.freq_hz, note.dur_ms);
                        s_notes++;
                    } else {
                        /* End of tune. Finish the task; the UI stops the link. */
                        s_tune_done = 1;
                        break;
                    }
                } else {
                    /* Tone mode: no parser, just a steady note. This is the
                     * streaming smoke test, and it exercises exactly the same
                     * send path a real tune does. */
                    synth_tone(&syn, 1000);
                }
            }
            synth_fill(&syn, s_pcm, frames);
            buf = esp_a2d_audio_buff_alloc((uint16_t)bytes);
            if (!buf) {
                /* The stack allocates through its own queue, so a transient
                 * failure here is possible and is NOT fatal - the old code
                 * broke out of the task, which ended the tune silently with
                 * "notes 0" on screen and nothing else. */
                stall++;
                if (stall > 50) {
                    note_set("out of mem");
                    break;
                }
                vTaskDelay(1);
                continue;
            }
            memcpy(buf->data, s_pcm, (size_t)bytes);
            buf->buff_size = (uint16_t)bytes;
            buf->number_frame = (uint16_t)(bytes / 4);
            buf->timestamp = 0;
            buf->data_len = (uint16_t)bytes;
            rc = esp_a2d_source_audio_data_send(
                    (esp_a2d_conn_hdl_t)s_conn_hdl, buf);
            if (rc == ESP_OK) {
                /* The stack owns it now. Do not touch it again. */
                s_frames += (uint32_t)frames;
                run_frames += frames;
                continue;
            }
            /* Refused: the buffer is ours again, so hand it straight back
             * rather than leaking it into the BT queue. */
            if (s_tx_error == 0) {
                s_tx_error = (int)rc;
            }
            esp_a2d_audio_buff_free(buf);
            if (rc == ESP_FAIL) {
                /* Queue full: the stack is behind. Back off, and give up
                 * loudly if it stays behind - a permanently full queue means
                 * the link is wedged, and spinning on it for ever is how
                 * "connected, playing, silent" happens. */
                s_under++;
                if (++stall > 150) {
                    ESP_LOGE(TAG, "send queue wedged after %d", stall);
                    note_set("tx stalled");
                    break;
                }
                vTaskDelay(1);
            } else if (rc == ESP_ERR_INVALID_SIZE) {
                /* The MTU moved under us. Shrink for good rather than
                 * repeating a call the stack will keep refusing: a send that
                 * is one byte too large is refused EVERY time, and the old code
                 * gave up on the first one and killed the tune. */
                ESP_LOGE(TAG, "mtu overshoot at %d (mtu %d)", bytes,
                         s_peer_mtu);
                if (s_peer_mtu > 128) {
                    s_peer_mtu = (s_peer_mtu / 2) & ~3;
                    continue;
                }
                note_set("mtu");
                break;
            } else {
                /* INVALID_STATE means the stack went away underneath us
                 * (disconnect racing the teardown); INVALID_ARG means the
                 * handle is stale. Neither is worth retrying. */
                ESP_LOGE(TAG, "send refused %d (len=%d mtu=%d)", (int)rc,
                         bytes, s_peer_mtu);
                note_set((rc == ESP_ERR_INVALID_STATE) ? "link down"
                                                        : "bad handle");
                break;
            }
        }
        }
    }

    s_playing = 0;
    s_media_ready = 0;
    s_task_run = 0;
    s_task_quit = 1;
    s_task = NULL;
    vTaskDelete(NULL);
}

/* ---- public ---- */

int svc_a2dp_up(void) {
    esp_bt_controller_config_t cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    esp_err_t r;
    if (s_up) {
        return 0;
    }
    /* BTDM, not BLE: the A2DP source lives in the BR/EDR half of the
     * controller, and this is the only place the project pays for it. The
     * caller is expected to have run svc_ble_stop() first, since both drive
     * the same controller.
     *
     * cfg.mode MUST equal the mode passed to esp_bt_controller_enable(). The
     * IDF example hard-codes ESP_BT_MODE_CLASSIC_BT in one call and BTDM in
     * the other and relies on sdkconfig agreeing; the reference library sets
     * cfg.mode from the same variable it enables with, for the reason that
     * esp_bt_controller_enable() on ESP32 returns ESP_ERR_INVALID_ARG when
     * they disagree (btdm_controller_get_mode() in components/bt/controller).
     * Matching them explicitly is free and removes a whole class of
     * "BT init failed / BT mode" on screen with no cause. */
    cfg.mode = ESP_BT_MODE_BTDM;
    r = esp_bt_controller_init(&cfg);
    if (r != ESP_OK) {
        return -10;
    }
    r = esp_bt_controller_enable(ESP_BT_MODE_BTDM);
    if (r != ESP_OK) {
        esp_bt_controller_deinit();
        return -11;
    }
    r = esp_bluedroid_init();
    if (r != ESP_OK) {
        esp_bt_controller_disable();
        esp_bt_controller_deinit();
        return -12;
    }
    r = esp_bluedroid_enable();
    if (r != ESP_OK) {
        esp_bluedroid_deinit();
        esp_bt_controller_disable();
        esp_bt_controller_deinit();
        return -13;
    }
    esp_bt_gap_set_device_name("Hackulator");
    esp_bt_gap_register_callback(gap_cb);

    /* Pairing configuration, BEFORE anything can trigger pairing.
     *
     * IO capability NO_INPUT_NO_OUTPUT is the important half: this device has
     * no display and no keypad, so it must claim to have neither, and that is
     * what lets a modern sink pick "Just Works" instead of demanding a
     * six-digit passkey from a device that cannot produce one. Combined with
     * the CFM_REQ handler in gap_cb(), that is the whole of Secure Simple
     * Pairing - and it was simply missing before, which is why pairing hung.
     */
    {
        uint8_t iocap = ESP_BT_IO_CAP_NONE;
        esp_bt_gap_set_security_param(ESP_BT_SP_IOCAP_MODE, &iocap,
                                      sizeof(iocap));
    }
    /* Default PIN for legacy pairing. Set even though the reply handler passes
     * its own code: the stack refuses to start legacy pairing at all without
     * one being configured, so leaving this out makes the PIN_REQ path
     * unreachable on some firmware builds. */
    esp_bt_gap_set_pin(ESP_BT_PIN_TYPE_VARIABLE, 4, s_pin);

    /* Missed on the first pass: without this the whole media-control
     * state machine below never runs and the screen connects and then sits
     * there. The linker flagged it as an unused function. */
    esp_a2d_register_callback(a2d_cb);

    /* AVRCP controller, BEFORE the A2DP source.
     *
     * The IDF is explicit that "AVRC should be initialized before A2DP" and
     * deinitialized after it, and this ordering is not cosmetic.
     *
     * It is worth the RAM because a sink is allowed to render nothing until it
     * is told to play: TVs, soundbars and Android devices in A2DP-source mode
     * routinely show "connected, streaming, silent" and only open their
     * output on an AVRC PLAY. Without a controller there is no way to send
     * that, so "works on a headphone" was the best that could be claimed.
     *
     * Strictly optional in the other direction: if it fails, A2DP alone still
     * plays to anything that does not gate on it, so a failure here is logged
     * and stepped over rather than aborting the bring-up. */
    if (esp_avrc_ct_init() == ESP_OK) {
        esp_avrc_ct_register_callback(avrc_cb);
        s_avrc_inited = 1;
    } else {
        ESP_LOGW(TAG, "avrc init failed, continuing without it");
        s_avrc_inited = 0;
    }

    r = esp_a2d_source_init();
    if (r != ESP_OK) {
        if (s_avrc_inited) {
            esp_avrc_ct_deinit();
            s_avrc_inited = 0;
        }
        esp_bluedroid_disable();
        esp_bluedroid_deinit();
        esp_bt_controller_disable();
        esp_bt_controller_deinit();
        return -14;
    }
    /* No SEP registration call: with the default (non-external-codec)
     * configuration Bluedroid pre-registers the mandatory SBC endpoint, and
     * ESP_A2D_MCC_1_SOURCE does not exist in the public API. */
    s_up = 1;
    s_n = 0;
    s_frames = 0;
    s_notes = 0;
    s_under = 0;
    s_tx_error = 0;
    s_tune = NULL;
    s_connected = 0;
    s_playing = 0;
    s_media_ready = 0;
    s_media = MEDIA_IDLE;
    s_conn_asked = 0;
    s_avrc_connected = 0;
    s_peer_mtu = 0;
    note_set(NULL);
    stored_load();
    ESP_LOGI(TAG, "a2dp source up (avrc %d)", s_avrc_inited);
    return 0;
}

void svc_a2dp_stop(void) {
    if (!s_up) {
        return;
    }
    /* Two-phase stop, and it is not optional: unwinding Bluedroid while the
     * producer task is alive leaves the task calling into a dead stack. The
     * task is asked to leave and waited for; it never tears itself down. */
    svc_a2dp_stop_tune();
    if (s_connected) {
        esp_a2d_media_ctrl(ESP_A2D_MEDIA_CTRL_SUSPEND);
        esp_a2d_source_disconnect(s_peer_bda);
        /* WAIT for the link to actually come down. esp_a2d_source_disconnect()
         * only posts a request; the stack answers asynchronously through
         * a2d_cb(), and that answer arrives inside the profile we are about to
         * deinit. Tearing down in the same tick is the same fault as
         * svc_deauth's orphan TX timer one layer up - a callback firing into a
         * deinitialised profile. Bounded, because a peer that has gone out of
         * range must not wedge the teardown forever. */
        {
            int spins;
            for (spins = 0; spins < 50 && s_connected; spins++) {
                vTaskDelay(pdMS_TO_TICKS(20));
            }
            if (s_connected) {
                ESP_LOGW(TAG, "peer still up after 1s, unwinding anyway");
            }
        }
    }
    esp_a2d_source_deinit();
    /* AVRCP after A2DP on the way DOWN, which is the reverse of the order on
     * the way up and for the same documented reason. Skipped if the bring-up
     * never got that far. */
    if (s_avrc_inited) {
        esp_avrc_ct_deinit();
        s_avrc_inited = 0;
    }
    s_avrc_connected = 0;
    esp_bluedroid_disable();
    esp_bluedroid_deinit();
    esp_bt_controller_disable();
    esp_bt_controller_deinit();
    s_up = 0;
    s_connected = 0;
    s_playing = 0;
    s_media_ready = 0;
    s_media = MEDIA_IDLE;
    s_conn_asked = 0;
    s_peer_mtu = 0;
    s_out = NULL;
    ESP_LOGI(TAG, "a2dp down");
}

int svc_a2dp_up_ok(void) {
    return s_up;
}

int svc_a2dp_discover(a2dp_sink_t *out, int max, int seconds) {
    int up;
    if (!out || max <= 0 || max > A2DP_MAX_SINKS || seconds <= 0) {
        return -1;
    }
    memset(out, 0, sizeof(*out) * (size_t)max);
    s_n = 0;
    /* ONE stack-up for BOTH halves of discovery. Bringing it up and down per
     * half is what broke inquiry before: the second half was posting at a
     * deinitialised stack. It is also cheaper than it looks - this is one
     * bring-up for the whole search, not one per technique. */
    up = svc_a2dp_up();
    if (up < 0) {
        return -100 + up;
    }
    esp_bt_gap_register_callback(gap_cb);

    /* Inquiry needs the LOCAL device to be discoverable, and starting
     * discovery does not make it so - it only issues the HCI_Inquiry. Without
     * this window the classic half of the search finds nothing at all, which
     * is exactly the "no speakers found" that was reported as a protocol
     * limitation when it was a missing line. Close again on STOPPED. */
    esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE, ESP_BT_GENERAL_DISCOVERABLE);

    /* BLE first: it finds a speaker that is merely powered on. */
    svc_a2dp_find_by_ble(out, max, seconds / 2, NULL);

    /* Classic inquiry second, for anything in pairing mode or with no BLE. */
    s_out = out;
    s_max = max;
    if (esp_bt_gap_start_discovery(ESP_BT_INQ_MODE_GENERAL_INQUIRY, 30, 0)
        == ESP_OK) {
        vTaskDelay(pdMS_TO_TICKS(seconds * 1000 / 2));
        esp_bt_gap_cancel_discovery();
    } else {
        ESP_LOGE(TAG, "inquiry refused");
    }
    s_out = NULL;
    ESP_LOGI(TAG, "discovered %d candidates", s_n);
    return s_n;
}

int svc_a2dp_connect(const uint8_t *bda) {
    if (!s_up || !bda) {
        return -1;
    }
    if (s_connected) {
        return -2;
    }
    memcpy(s_peer_bda, bda, 6);
    snprintf(s_peer_name, sizeof(s_peer_name), "%02x:%02x:%02x",
             bda[0], bda[1], bda[2]);
    s_connected = 0;
    s_playing = 0;
    s_media_ready = 0;
    s_media = MEDIA_IDLE;
    s_frames = 0;
    s_notes = 0;
    s_under = 0;
    s_tx_error = 0;
    s_peer_mtu = 0;
    note_set(NULL);
    /* Stamp the attempt. A connect request that never produces a
     * CONNECTION_STATE_EVT is the single most common way this feature looks
     * broken - the speaker is off, already paired to a phone, or simply out
     * of range - and without a clock on it there is nothing to time out
     * against, so the screen sat on "linked" (a lie: it was "requested")
     * for ever. */
    s_conn_at_us = esp_timer_get_time();
    s_conn_asked = 1;
    if (esp_a2d_source_connect(s_peer_bda) != ESP_OK) {
        s_conn_asked = 0;
        note_set("no slot");
        return -3;
    }
    /* Saved on request rather than on success: a connect that is about to
     * fail pairing still has the right address, and the next run should
     * offer it again instead of making the user re-discover it. */
    stored_save(bda, s_peer_name);
    return 0;
}


bool svc_a2dp_have_stored(void) {
    return s_have_stored;
}

void svc_a2dp_load_stored(void) {
    stored_load();
}

const char *svc_a2dp_stored_name(void) {
    return s_stored_name[0] ? s_stored_name : "speaker";
}

void svc_a2dp_forget_stored(void) {
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_erase_key(h, NVS_KEY);
        nvs_commit(h);
        nvs_close(h);
    }
    s_have_stored = 0;
    s_stored_name[0] = 0;
}

/* No inquiry, no pairing mode: straight to the stored address. */
int svc_a2dp_connect_stored(void) {
    if (!s_have_stored) {
        return -1;
    }
    return svc_a2dp_connect(s_stored_bda);
}
int svc_a2dp_disconnect(void) {
    svc_a2dp_stop_tune();
    if (s_connected) {
        esp_a2d_media_ctrl(ESP_A2D_MEDIA_CTRL_SUSPEND);
        esp_a2d_source_disconnect(s_peer_bda);
        /* Wait for the link to actually come down. esp_a2d_source_disconnect()
         * only posts a request, and the screen goes straight on to let the
         * user pick another speaker - so clearing the flags here rather than
         * in the callback would report "not connected" while the old ACL is
         * still up, and the next esp_a2d_source_connect() would race it. */
        {
            int spins;
            for (spins = 0; spins < 40 && s_connected; spins++) {
                vTaskDelay(pdMS_TO_TICKS(20));
            }
        }
    }
    s_connected = 0;
    s_playing = 0;
    s_media_ready = 0;
    s_media = MEDIA_IDLE;
    s_conn_asked = 0;
    s_conn_hdl = 0;
    s_peer_mtu = 0;
    s_avrc_connected = 0;
    return 0;
}

static int start_feed(const char *tune, int is_tone) {
    if (!s_up) {
        return -1;
    }
    if (s_task) {
        return -2;
    }
    /* Deliberately NOT gated on s_media_ready. The media stream is started
     * asynchronously by the A2DP state machine - connection, then
     * CHECK_SRC_RDY, then START, then AUDIO_STATE_STARTED - so it is usually
     * not ready at the instant the user presses play. The task already waits
     * for it in its own loop, so gating the START on it here meant the tune
     * never began and never retried: the screen sat on the tune list doing
     * nothing, with no error anywhere. */
    s_task_run = 1;
    s_task_quit = 0;
    s_tune_done = 0;
    s_tune = is_tone ? NULL : tune;
    /* Priority below the stack's tasks on purpose: the producer must never
     * preempt the Bluetooth task that is draining what it sends. */
    if (xTaskCreate(feed_task, "a2dpfeed", FEED_STACK, NULL, FEED_PRIO, &s_task)
        != pdPASS) {
        s_task_run = 0;
        s_task = NULL;
        return -3;
    }
    return 0;
}

int svc_a2dp_play_tune(const char *rtttl) {
    if (!rtttl) {
        return -1;
    }
    return start_feed(rtttl, 0);
}

int svc_a2dp_play_tone(uint32_t freq_hz) {
    (void)freq_hz;
    return start_feed(NULL, 1);
}

void svc_a2dp_stop_tune(void) {
    int spins;
    if (!s_task && !s_task_run) {
        return;
    }
    s_task_run = 0;
    /* Wait for the task to actually leave. A bounded spin rather than a bare
     * return: returning while it is still inside the stack would make the
     * caller's teardown pull the stack out from under it. */
    for (spins = 0; spins < 250; spins++) {
        if (s_task_quit || !s_task) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    s_playing = 0;
}

int svc_a2dp_connected(void) {
    return s_connected;
}

int svc_a2dp_playing(void) {
    return s_playing;
}

int svc_a2dp_busy(void) {
    return s_task != NULL || s_task_run;
}

int svc_a2dp_tune_done(void) {
    return s_tune_done;
}

void svc_a2dp_clear_tune_done(void) {
    s_tune_done = 0;
}

const char *svc_a2dp_note(void) {
    return s_note;
}

int svc_a2dp_failed(void) {
    return s_note[0] != 0;
}

int svc_a2dp_mtu(void) {
    return s_peer_mtu;
}

a2dp_link_t svc_a2dp_link(void) {
    if (!s_up) {
        return A2DP_LINK_DOWN;
    }
    if (s_connected) {
        return (s_media == MEDIA_STREAMING) ? A2DP_LINK_STREAMING
                                            : A2DP_LINK_MEDIA;
    }
    if (s_conn_asked) {
        return A2DP_LINK_WAIT;
    }
    if (s_note[0]) {
        return A2DP_LINK_FAILED;
    }
    return A2DP_LINK_IDLE;
}

const char *svc_a2dp_state_name(void) {
    switch (svc_a2dp_link()) {
    case A2DP_LINK_DOWN:     return "down";
    case A2DP_LINK_IDLE:     return "idle";
    case A2DP_LINK_WAIT:     return "wait";
    case A2DP_LINK_MEDIA:    return "media";
    case A2DP_LINK_STREAMING: return "stream";
    default:                 return "fail";
    }
}

/* The retry engine. Call from the UI tick.
 *
 * EVERY silent-failure mode in this feature is a state machine that was given
 * exactly one chance to run:
 *
 *   connect requested -> nothing ever answers      -> timed out here
 *   CHECK_SRC_RDY      -> sink says BUSY or FAILURE -> re-asked here
 *   START              -> acknowledged, STARTED never arrives -> re-asked here
 *
 * Each of those used to leave the link "connected", the screen saying
 * "linked", and no sound at all - which is the reported symptom, and which no
 * amount of logging could explain because nothing was wrong: the handshake had
 * simply given up. The reference implementation runs a 10 s heartbeat doing
 * precisely this; here it rides the UI's existing 500 ms cadence so there is
 * no timer to leak and nothing to stop when the screen closes.
 *
 * The insistence is bounded. Inside the grace window after the link comes up,
 * or while the user has actually asked for audio and it has not arrived, the
 * pump keeps asking. Outside that it stops, because a speaker that suspended
 * the stream because somebody pressed pause on the SPEAKER must be allowed to
 * stay paused.
 *
 * Returns 1 if something moved and the screen should be repainted. */
int svc_a2dp_pump(void) {
    int64_t now;
    if (!s_up) {
        return 0;
    }
    now = esp_timer_get_time();

    /* A connect that nobody answered. Not an error state - the speaker may be
     * off - but it MUST stop claiming to be pending, or the screen sits on
     * "connecting" for ever with no way forward. */
    if (s_conn_asked && !s_connected &&
        now - s_conn_at_us > (int64_t)CONNECT_TIMEOUT_MS * 1000) {
        s_conn_asked = 0;
        note_set("no answer");
        ESP_LOGW(TAG, "connect gave up after %dms", CONNECT_TIMEOUT_MS);
        return 1;
    }
    if (!s_connected || s_media == MEDIA_STREAMING) {
        return 0;
    }
    if (s_media == MEDIA_STARTING &&
        now - s_media_start_us < (int64_t)MEDIA_START_MS * 1000) {
        return 0;
    }
    if (!svc_a2dp_busy() &&
        now - s_conn_ok_us > (int64_t)MEDIA_GRACE_MS * 1000) {
        return 0;
    }
    if (now - s_media_ask_us < (int64_t)MEDIA_RETRY_MS * 1000) {
        return 0;
    }
    ESP_LOGI(TAG, "re-asking the sink for readiness");
    media_ask();
    return 1;
}

uint32_t svc_a2dp_frames(void) {
    return s_frames;
}

uint32_t svc_a2dp_notes(void) {
    return s_notes;
}

uint32_t svc_a2dp_underruns(void) {
    return s_under;
}

int svc_a2dp_tx_error(void) {
    return s_tx_error;
}

const char *svc_a2dp_peer_name(void) {
    return s_peer_name;
}

void svc_a2dp_set_name(const char *name) {
    if (name) {
        snprintf(s_peer_name, sizeof(s_peer_name), "%s", name);
    }
}

void svc_a2dp_bda_str(char *out) {
    bda_str((const uint8_t *)s_peer_bda, out);
}