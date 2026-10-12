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
#include "esp_log.h"
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
 * the same shape the working ESP32-A2DP library uses, and it is the only
 * reason this feature can be trusted at all.
 *
 * The synth writes into s_pcm first, which then gets copied into the stack's
 * buffer. s_pcm is int16_t purely so the compiler aligns it: Xtensa faults on
 * an unaligned halfword load. */
#define SEND_BYTES  672           /* the A2DP audio MTU in practice */
#define SEND_FRAMES (SEND_BYTES / 4)

#define FEED_STACK 3584
#define FEED_PRIO  4

#define NAME_PROP_SHORT  0x08   /* Shortened Local Name  */
#define NAME_PROP_FULL   0x09   /* Complete Local Name   */

/* Declared int16_t rather than uint8_t so it is 2-byte aligned by the compiler.
 * The synth writes through an int16_t* and the stack reads through a uint8_t*;
 * a uint8_t array would only be 1-byte aligned and Xtensa faults on an
 * unaligned halfword load. */
static int16_t s_pcm[SEND_BYTES / 2];

/* ---- stack state ---- */
static int s_up;
static int s_connected;
static int s_playing;
static int s_media_ready;      /* ESP_A2D_AUDIO_STATE_STARTED seen */
static int s_conn_hdl;
static int s_tx_error;
static uint8_t s_peer_bda[6];
static char s_peer_name[A2DP_NAME_MAX + 1];
static int s_peer_mtu = 672;

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
        }
    } else if (ev == ESP_BT_GAP_PIN_REQ_EVT) {
        /* A headset that only accepts bonded hosts will refuse the very first
         * A2DP connection, and pairing cannot begin without a PIN request
         * being answered. Legacy PIN "0000" is what these devices expect from
         * a source that has no display; Just Works needs nothing at all. Any
         * key length is accepted here because the alternative is a
         * connection that silently never completes. */
        ESP_LOGI(TAG, "PIN request, answering");
        esp_bt_gap_pin_reply(param->pin_req.bda, true, 4,
                             (esp_bt_pin_code_t){0, 0, 0, 0});
    } else if (ev == ESP_BT_GAP_KEY_REQ_EVT) {
        esp_bt_gap_ssp_passkey_reply(param->key_req.bda, true, 0);
    } else if (ev == ESP_BT_GAP_AUTH_CMPL_EVT) {
        if (param->auth_cmpl.stat == ESP_BT_STATUS_SUCCESS) {
            ESP_LOGI(TAG, "bonded with peer");
        } else {
            ESP_LOGW(TAG, "bond failed %d", (int)param->auth_cmpl.stat);
        }
    }
}

/* ---- A2DP ---- */

static void a2d_cb(esp_a2d_cb_event_t ev, esp_a2d_cb_param_t *param) {
    switch (ev) {
    case ESP_A2D_CONNECTION_STATE_EVT: {
        if (param->conn_stat.state == ESP_A2D_CONNECTION_STATE_CONNECTED) {
            s_connected = 1;
            s_conn_hdl = (int)param->conn_stat.conn_hdl;
            ESP_LOGI(TAG, "connected hdl=%d mtu=%d", s_conn_hdl,
                     (int)param->conn_stat.audio_mtu);
            if (param->conn_stat.audio_mtu > 0) {
                s_peer_mtu = param->conn_stat.audio_mtu;
            }
            /* Ask whether the source is ready, then start. The stack will ACK
             * and the audio state will flip to STARTED, which is what actually
             * gates the producer task. */
            esp_a2d_media_ctrl(ESP_A2D_MEDIA_CTRL_CHECK_SRC_RDY);
        } else {
            s_connected = 0;
            s_playing = 0;
            s_media_ready = 0;
            ESP_LOGI(TAG, "disconnected");
        }
        break;
    }
    case ESP_A2D_MEDIA_CTRL_ACK_EVT: {
        esp_a2d_media_ctrl_t cmd = param->media_ctrl_stat.cmd;
        esp_a2d_media_ctrl_ack_t st = param->media_ctrl_stat.status;
        if (cmd == ESP_A2D_MEDIA_CTRL_CHECK_SRC_RDY &&
            st == ESP_A2D_MEDIA_CTRL_ACK_SUCCESS) {
            esp_a2d_media_ctrl(ESP_A2D_MEDIA_CTRL_START);
        } else if (cmd == ESP_A2D_MEDIA_CTRL_START) {
            if (st != ESP_A2D_MEDIA_CTRL_ACK_SUCCESS) {
                ESP_LOGE(TAG, "media start failed %d", (int)st);
            }
        } else if (cmd == ESP_A2D_MEDIA_CTRL_SUSPEND) {
            s_playing = 0;
            s_media_ready = 0;
        }
        break;
    }
    case ESP_A2D_AUDIO_STATE_EVT: {
        if (param->audio_stat.state == ESP_A2D_AUDIO_STATE_STARTED) {
            s_media_ready = 1;
            s_playing = 1;
            ESP_LOGI(TAG, "media started");
        } else {
            s_media_ready = 0;
            s_playing = 0;
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

    /* Real-time pacing.
     *
     * The first version just sent as fast as the stack would take it. That
     * tripped the task watchdog: the loop never yielded, so the idle task
     * stopped running and the WDT fired inside feed_task. An A2DP source is a
     * REAL-TIME producer - it must emit 44100 samples per second, no more and
     * no less - so the fix is to pace against the sample clock instead of
     * against the CPU.
     *
     * Frames are counted against a 10 ms budget (441 samples), which is also
     * the FreeRTOS tick, so one vTaskDelay(1) per window keeps send rate and
     * wall clock together. Nothing here is allowed to busy-wait. */
    {
        int32_t budget = 0;
        while (s_task_run) {
        if (!s_media_ready) {
            vTaskDelay(pdMS_TO_TICKS(10));
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
        {
            int frames = SEND_FRAMES;
            esp_a2d_audio_buff_t *buf;
            esp_err_t rc;
            int bytes;
            synth_fill(&syn, s_pcm, frames);
            bytes = frames * 4;
            if (bytes > s_peer_mtu) {
                bytes = s_peer_mtu & ~3;   /* whole stereo frames only */
            }
            buf = esp_a2d_audio_buff_alloc((uint16_t)bytes);
            if (!buf) {
                ESP_LOGE(TAG, "a2dp buff alloc failed");
                break;
            }
            memcpy(buf->data, s_pcm, (size_t)bytes);
            buf->buff_size = (uint16_t)bytes;
            buf->number_frame = (uint16_t)(bytes / 4);
            buf->timestamp = 0;
            buf->data_len = (uint16_t)bytes;
            rc = esp_a2d_source_audio_data_send((esp_a2d_conn_hdl_t)s_conn_hdl, buf);
            if (rc == ESP_OK) {
                /* The stack owns it now. Do not touch it again. */
                s_frames += (uint32_t)frames;
                budget += frames;
                if (budget >= 441) {   /* 10 ms of audio at 44.1 kHz */
                    budget = 0;
                    vTaskDelay(1);
                }
            } else {
                if (s_tx_error == 0) {
                    s_tx_error = (int)rc;
                }
                esp_a2d_audio_buff_free(buf);
                if (rc == ESP_FAIL) {
                    s_under++;
                    /* Queue full: the stack is behind. Back off and let it
                     * drain rather than hammering the same call. */
                    vTaskDelay(pdMS_TO_TICKS(2));
                } else {
                    /* ESP_ERR_INVALID_SIZE means the MTU moved under us;
                     * anything else is a state fault. Either way, spinning on
                     * a permanently refused call would just wedge the task. */
                    ESP_LOGE(TAG, "send refused %d (len=%d mtu=%d)", (int)rc,
                             (int)bytes, s_peer_mtu);
                    break;
                }
            }
        }
        }
    }

    s_playing = 0;
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
     * the same controller. */
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
    /* Missed on the first pass: without this the whole media-control
     * state machine below never runs and the screen connects and then sits
     * there. The linker flagged it as an unused function. */
    esp_a2d_register_callback(a2d_cb);
    r = esp_a2d_source_init();
    if (r != ESP_OK) {
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
    stored_load();
    ESP_LOGI(TAG, "a2dp source up");
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
    esp_bluedroid_disable();
    esp_bluedroid_deinit();
    esp_bt_controller_disable();
    esp_bt_controller_deinit();
    s_up = 0;
    s_connected = 0;
    s_playing = 0;
    s_media_ready = 0;
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

    /* BLE first: it finds a speaker that is merely powered on. */
    svc_a2dp_find_by_ble(out, max, seconds / 2, NULL);

    /* Classic inquiry second, for anything in pairing mode or with no BLE. */
    s_out = out;
    s_max = max;
    if (esp_bt_gap_start_discovery(ESP_BT_INQ_MODE_GENERAL_INQUIRY, 30, 0)
        == ESP_OK) {
        vTaskDelay(pdMS_TO_TICKS(seconds * 1000 / 2));
        esp_bt_gap_cancel_discovery();
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
    s_frames = 0;
    s_notes = 0;
    s_under = 0;
    s_tx_error = 0;
    if (esp_a2d_source_connect(s_peer_bda) != ESP_OK) {
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
    }
    s_connected = 0;
    s_playing = 0;
    s_media_ready = 0;
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

const char *svc_a2dp_state_name(void) {
    if (!s_up) {
        return "down";
    }
    if (!s_connected) {
        return "idle";
    }
    if (s_task || s_task_run) {
        return "playing";
    }
    if (s_playing) {
        return "stream";
    }
    return "linked";
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