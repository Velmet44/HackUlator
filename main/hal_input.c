#include "hal_input.h"
#include "app_config.h"
#include "caster.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "input";

static QueueHandle_t s_key_q;
static int64_t s_chord_since = 0; /* OK+BACK hold start (us), 0 = not held */

static const int s_btn_pins[6] = {
    PIN_BTN_UP, PIN_BTN_DOWN, PIN_BTN_LEFT, PIN_BTN_RIGHT,
    PIN_BTN_OK, PIN_BTN_BACK,
};
static const hacku_key_t s_btn_keys[6] = {
    KEY_UP, KEY_DOWN, KEY_LEFT, KEY_RIGHT, KEY_OK, KEY_BACK,
};

/* Pins currently borrowed by a peripheral (see hal_input.h). Owned here,
 * written by whoever borrows the pin - currently only svc_nrf - and read
 * every poll, never latched, so it can be changed while keys are held. */
static uint32_t s_btn_mask;

#define POLL_MS           15
#define DEBOUNCE_SAMPLES  3
/* Remote (caster) keys have no release edge: treat as held this long. */
#define REMOTE_HOLD_MS    400
/* 3x BACK inside this window also triggers relock (remote-friendly). */
#define BACK_MULTI_MS     800

static void push(hacku_key_t k) {
    if (s_key_q) {
        hacku_key_t v = k;
        xQueueSend(s_key_q, &v, 0);
    }
}

static hacku_key_t remote_to_key(uint8_t c) {
    switch (c) {
        case 'w': case 'W': return KEY_UP;
        case 's': case 'S': return KEY_DOWN;
        case 'a': case 'A': return KEY_LEFT;
        case 'd': case 'D': return KEY_RIGHT;
        case 'e': case 'E': case '\n': case '\r': return KEY_OK;
        case 'b': case 'B': case 0x1B: return KEY_BACK;
        /* Space is a TAP on the one button; 'x' is the 3 s HOLD.
         *
         * They MUST stay distinct. Routing both through KEY_ONEBTN made a
         * remote hold increment a tap counter in one-button mode, so it could
         * never reach the toggle - and the check for it still went green,
         * because the screen it should have changed was not the one being
         * measured.
         *
         * 0x20 is safe to send: caster_task() queues EVERY byte it reads from
         * UART0 unfiltered, so this mapping is the only place a space is
         * discarded. Checked before trusting it to stay that way. */
        case ' ': return KEY_ONEBTN;
        case 'x': case 'X': return KEY_ONEBTN_HOLD;
        default: return KEY_NONE;
    }
}

static int btn_index(hacku_key_t k) {
    for (int i = 0; i < 6; i++)
        if (s_btn_keys[i] == k)
            return i;
    return -1;
}

/* ---- one-button mode ----
 * s_onebtn lives here but is owned by main.c, which is where the mode itself
 * lives; main.c calls hacku_input_set_onebutton() whenever it changes. It is
 * re-read every poll rather than latched into a local, so the switch takes
 * effect on the next 15 ms tick with no task hand-off. */
static volatile int s_onebtn = 0;

/* Start of the current one-button hold, 0 when not held. Written by the
 * input task, read by the main loop; a torn read is harmless because the
 * value only ever moves monotonically between 0 and a timestamp. */
static volatile int64_t s_onebtn_held_since = 0;

static void hold_begin(int64_t now) {
    if (s_onebtn_held_since == 0) {
        s_onebtn_held_since = now;
    }
}

static void hold_end(void) {
    s_onebtn_held_since = 0;
}

void hacku_input_set_onebutton(int on) {
    s_onebtn = on ? 1 : 0;
}

int hacku_input_onebutton(void) {
    return s_onebtn;
}

/* How long the one button has been held, 0..ONEBTN_HOLD_MS, 0 if not held.
 *
 * The physical pin and the caster burst are both sources: a held BOOT button
 * and a held spacebar are the same gesture and must give the same feedback.
 *
 * This exists so the UI can show a filling bar over a 3 second hold. Without
 * it the hold is a 3 second silence that looks identical to a dead button,
 * which is exactly how a real wiring fault presents. */
int hacku_input_onebtn_held_ms(void) {
    int64_t since = s_onebtn_held_since;
    if (since == 0) {
        return 0;
    }
    int64_t held = (esp_timer_get_time() - since) / 1000;
    if (held < 0) {
        return 0;
    }
    int cap = hacku_input_onebtn_hold_target_ms();
    if (held > cap) {
        held = cap;
    }
    return (int)held;
}

int hacku_input_onebtn_hold_target_ms(void) {
    return s_onebtn ? ONEBTN_UP_HOLD_MS : ONEBTN_HOLD_MS;
}

/* Count taps on the one button. The key is emitted by onebtn_flush() once
 * the tap window closes, NOT here: emitting on the first release and then
 * correcting it when a second tap turns out to mean OK would deliver two
 * keys to the UI for one gesture. */
static void onebtn_tap(int64_t now, int *taps, int64_t *first_us, int *done) {
    if (*taps == 0 || now - *first_us > ONEBTN_TAP_WINDOW_MS * 1000) {
        *taps = 1;
        *first_us = now;
        *done = 0;
        return;
    }
    if (*taps < ONEBTN_TAP_MAX) {
        (*taps)++;
    }
}

static void onebtn_flush(int64_t now, int *taps, int64_t *first_us, int *done) {
    if (*taps == 0 || *done)
        return;
    if (now - *first_us <= ONEBTN_TAP_WINDOW_MS * 1000)
        return;
    switch (*taps) {
    case 1:  push(KEY_DOWN); break;
    case 2:  push(KEY_OK);   break;
    default: push(KEY_BACK); break;
    }
    *taps = 0;
    *done = 1;
}

/* One remote press, as opposed to a burst of auto-repeat bytes.
 *
 * The wire has no release edge, so "held" can only be inferred from spacing,
 * and there are TWO questions to answer separately:
 *
 *   gap <= ONEBTN_REPEAT_GAP_MS  same press repeating -> cancel taps
 *   gap <= ONEBTN_HOLD_GAP_MS    still held           -> do not re-arm
 *
 * Using one threshold for both meant a single slow repeat byte restarted the
 * 3 s timer, so an 8 s hold toggled again near the end. The hold LATCH is
 * what stops a re-toggle; it is only cleared once the key has genuinely been
 * let go. */
static void onebtn_remote_press(int64_t now, int64_t *last_us, int *burst,
                                int64_t *hold_us, int *hold_fired,
                                int *taps, int64_t *first_us, int *done,
                                int hold_ms) {
    int64_t gap = (*last_us != 0) ? (now - *last_us) : (int64_t)9999999;
    *last_us = now;

    int hold_alive = gap <= (int64_t)ONEBTN_HOLD_GAP_MS * 1000;
    if (!hold_alive) {
        *burst = 0;
        *hold_fired = 0;
        hold_begin(now);
    }

    if (gap <= (int64_t)ONEBTN_REPEAT_GAP_MS * 1000) {
        *burst = 1;
        *taps = 0;
        *done = 1;
    } else {
        *burst = 0;
        *hold_us = now;
        onebtn_tap(now, taps, first_us, done);
    }

    /* The gesture is emitted by a COMPLETED hold. Emitting it on the first
     * byte is what made a held key re-toggle at every repeat gap.
     *
     * Inside one-button mode a completed hold is UP; outside it, a completed
     * 3 s hold enters the mode. */
    if (hold_alive && !*hold_fired &&
        now - *hold_us > (int64_t)hold_ms * 1000) {
        push(s_onebtn ? KEY_UP : KEY_ONEBTN_HOLD);
        *hold_fired = 1;
    }
}

static void input_task(void *arg) {
    (void)arg;
    uint8_t raw[6]  = {1, 1, 1, 1, 1, 1};
    uint8_t cnt[6]  = {0};
    uint8_t prev[6] = {1, 1, 1, 1, 1, 1}; /* 1 = released (pull-up) */
    int64_t remote_held_until[6] = {0};   /* synthetic hold for caster keys */
    int back_taps = 0;
    int64_t back_first_us = 0;
    int relock_sent = 0;
    int64_t one_since = 0;   /* GPIO0 hold start, 0 = not held */
    int one_fired = 0;       /* KEY_ONEBTN already sent for this press */
    int64_t hold_target_us = 0; /* what THIS press is counting towards */
    int one_taps = 0;        /* taps in the current one-button gesture */
    int64_t one_first_us = 0;
    int one_done = 1;        /* tap run already emitted */
    int64_t one_last_us = 0; /* last caster one-button byte */
    int one_burst = 0;       /* byte arrived inside the repeat gap */
    int64_t one_hold_us = 0; /* hold timer start */
    int one_hold_fired = 0;  /* hold already emitted for this burst */

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(POLL_MS));
        int64_t now = esp_timer_get_time();

        /* A held key is over once no byte has arrived inside the HOLD gap -
         * wider than the tap threshold on purpose, see ONEBTN_HOLD_GAP_MS.
         * Reset the hold latch here, every poll, REGARDLESS of one-button
         * mode.
         *
         * It used to be reset in the mode-change branch, which meant the hold
         * fired, toggled the mode, cleared the latch, and the REST of the
         * still-held key started a fresh 3 s timer and toggled again.
         * Measured: one 8 s hold toggled three times and finished exactly
         * where it started. Ending a hold is a property of the wire, not of
         * which mode we happen to be in. */
        if (one_last_us != 0 &&
            now - one_last_us > (int64_t)ONEBTN_HOLD_GAP_MS * 1000) {
            one_burst = 0;
            one_hold_fired = 0;
        }

        /* Physical buttons, debounced. */
        int onebtn = s_onebtn;
        for (int i = 0; i < 6; i++) {
            /* A masked pin belongs to a peripheral. Report it permanently
             * released AND keep raw[]/prev[] in step, so unmasking later
             * cannot replay an edge that happened while it was masked. */
            if (s_btn_mask & (1ULL << s_btn_pins[i])) {
                raw[i] = 1;
                cnt[i] = 0;
                prev[i] = 1;
                continue;
            }
            uint8_t lvl = (uint8_t)gpio_get_level(s_btn_pins[i]);
            if (lvl == raw[i]) {
                if (cnt[i] < DEBOUNCE_SAMPLES)
                    cnt[i]++;
            } else {
                raw[i] = lvl;
                cnt[i] = 0;
            }
            if (cnt[i] == DEBOUNCE_SAMPLES && raw[i] != prev[i]) {
                prev[i] = raw[i];
                /* In one-button mode these are dead - including the edges,
                 * so prev[] still tracks the real level and leaving the mode
                 * does not replay a button that was held across it. */
                if (onebtn)
                    continue;
                if (raw[i] == 0) { /* pressed */
                    relock_sent = 0;
                    push(s_btn_keys[i]);
                    if (s_btn_keys[i] == KEY_BACK) {
                        if (back_taps == 0 || now - back_first_us > BACK_MULTI_MS * 1000) {
                            back_taps = 1;
                            back_first_us = now;
                        } else if (++back_taps >= 3) {
                            back_taps = 0;
                            push(KEY_RELOCK);
                            relock_sent = 1;
                        }
                    }
                } else { /* released */
                    relock_sent = 0;
                }
            }
        }

        /* Remote keys from the caster RX queue. */
        QueueHandle_t rx = caster_get_rx_queue();
        if (rx) {
            uint8_t c;
            while (xQueueReceive(rx, &c, 0) == pdTRUE) {
                hacku_key_t k = remote_to_key(c);
                if (k == KEY_NONE)
                    continue;
                /* Space and 'x' are the one button in BOTH modes, and behave
                 * identically in both. In particular a held spacebar is a
                 * 3 s hold that toggles the disguise - it is NOT "toggle on
                 * the first byte".
                 *
                 * The first-byte toggle was a real bug: auto-repeat has gaps
                 * longer than ONEBTN_REPEAT_GAP_MS all the time, and every
                 * such gap cleared the burst, so the next byte looked like a
                 * brand new press and toggled again. Measured - one 8 s hold
                 * toggled at 3.6 s, 5.5 s and again after the test ended,
                 * finishing exactly where it started. A toggle has to be
                 * driven by a COMPLETED hold, not by the start of one. */
                if (k == KEY_ONEBTN) {
                    onebtn_remote_press(now, &one_last_us, &one_burst,
                                        &one_hold_us, &one_hold_fired,
                                        &one_taps, &one_first_us, &one_done,
                                        hacku_input_onebtn_hold_target_ms());
                    continue;
                }
                if (k == KEY_ONEBTN_HOLD) {
                    push(KEY_ONEBTN_HOLD);
                    continue;
                }
                if (onebtn) {
                    continue; /* the six keys and the caster arrows are dead */
                }
                int idx = btn_index(k);
                if (idx >= 0)
                    remote_held_until[idx] = now + REMOTE_HOLD_MS * 1000;
                push(k);
                if (k == KEY_BACK) {
                    if (back_taps == 0 || now - back_first_us > BACK_MULTI_MS * 1000) {
                        back_taps = 1;
                        back_first_us = now;
                    } else if (++back_taps >= 3) {
                        back_taps = 0;
                        push(KEY_RELOCK);
                    }
                }
            }
        }

        /* OK+BACK chord -> relock. Suppressed in one-button mode: both keys
         * are dead there, and prev[] only tracks their real level, so a
         * chord held across the switch must not fire afterwards. */
        int ok_held = (!onebtn && prev[4] == 0) || (now < remote_held_until[4]);
        int back_held = (!onebtn && prev[5] == 0) || (now < remote_held_until[5]);
        if (!onebtn && ok_held && back_held && !relock_sent) {
            if (s_chord_since == 0)
                s_chord_since = now;
            if (now - s_chord_since > (int64_t)RELOCK_HOLD_MS * 1000) {
                push(KEY_RELOCK);
                relock_sent = 1;
                s_chord_since = 0;
            }
        } else {
            s_chord_since = 0;
        }

        /* GPIO0 BOOT button held long enough -> KEY_ONEBTN.
         *
         * Fires ONCE per press, while still held, and only re-arms after a
         * release - otherwise a 10 s hold would fire three times and toggle
         * the mode back off, which is the sort of thing that looks like a
         * broken button rather than a gesture. */
        if (gpio_get_level(PIN_BTN_ONE) == 0) {
            if (one_since == 0) {
                one_since = now;
                hold_target_us = (int64_t)hacku_input_onebtn_hold_target_ms()
                                 * 1000;
            } else if (!one_fired &&
                       now - one_since > hold_target_us) {
                /* Inside one-button mode a hold is UP. Outside it, a 3 s hold
                 * enters the mode and nothing else - it cannot leave, so it is
                 * spent on something useful the rest of the time.
                 *
                 * Fires in BOTH modes' senses: the 3 s entry hold is how
                 * one-button mode is reached from the button at all, so
                 * gating it on the mode already being on would make the mode
                 * unreachable. */
                push(s_onebtn ? KEY_UP : KEY_ONEBTN_HOLD);
                one_fired = 1;
                /* A 3 s hold is not a tap run: cancel anything pending so the
                 * release cannot also emit a DOWN/OK/BACK for the same
                 * gesture. */
                one_taps = 0;
                one_done = 1;
            }
        } else {
            if (one_since != 0 && onebtn && !one_fired) {
                onebtn_tap(now, &one_taps, &one_first_us, &one_done);
            }
            one_since = 0;
            one_fired = 0;
        }
        if (onebtn) {
            onebtn_flush(now, &one_taps, &one_first_us, &one_done);
        } else {
            /* Not in one-button mode: no tap can be pending, or it would fire
             * once the window closed and move the cursor on a screen that has
             * no cursor. The hold latch is NOT cleared here - see the
             * burst-end check at the top of the loop. */
            one_taps = 0;
            one_done = 1;
        }

        /* Hold progress is shown only while the button is genuinely DOWN.
         *
         * The physical pin has a real edge, so that is exact. The wire does
         * not: a remote "hold" only becomes visible when a repeat byte
         * arrives, so the bar starts at the second byte and is cleared once
         * the bytes stop. Without that a single tap would leave the bar
         * filling for 3 s and look like a long press that never completed. */
        int64_t since_byte = (one_last_us != 0) ? (now - one_last_us)
                                                : (int64_t)9999999;
        int remote_held = one_burst &&
                          since_byte <= (int64_t)ONEBTN_REPEAT_GAP_MS * 2 * 1000;
        if (one_since != 0 || remote_held) {
            hold_begin(now);
        } else {
            hold_end();
        }
    }
}

void hacku_input_init(void) {
    if (s_key_q)
        return;
    s_key_q = xQueueCreate(32, sizeof(hacku_key_t));
    for (int i = 0; i < 6; i++) {
        gpio_config_t io = {
            .pin_bit_mask = (1ULL << s_btn_pins[i]),
            .mode = GPIO_MODE_INPUT,
            .pull_up_en = GPIO_PULLUP_ENABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        gpio_config(&io);
    }
    ESP_LOGI(TAG, "6 buttons on GPIO %d/%d/%d/%d/%d/%d (internal pull-ups)",
             PIN_BTN_UP, PIN_BTN_DOWN, PIN_BTN_LEFT, PIN_BTN_RIGHT,
             PIN_BTN_OK, PIN_BTN_BACK);
    /* GPIO0 last, and as a plain input: it is a strap pin, and pulling it up
     * here after boot is harmless while holding it low at reset would drop
     * the chip into the download bootloader instead of the app. */
    gpio_config_t one = {
        .pin_bit_mask = (1ULL << PIN_BTN_ONE),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&one);
    ESP_LOGI(TAG, "one-button hold on GPIO%d (%d ms)", PIN_BTN_ONE,
             ONEBTN_HOLD_MS);
    xTaskCreatePinnedToCore(input_task, "hk_input", 4096, NULL, 10, NULL,
                            tskNO_AFFINITY);
}

void hacku_input_set_button_mask(uint32_t pin_mask) {
    s_btn_mask = pin_mask;
    ESP_LOGI(TAG, "button mask = 0x%08lx", (unsigned long)pin_mask);
}

uint32_t hacku_input_button_mask(void) {
    return s_btn_mask;
}

int hacku_input_get(hacku_key_t *out) {
    if (!s_key_q)
        return 0;
    return xQueueReceive(s_key_q, out, 0) == pdTRUE;
}

void hacku_input_drain(void) {
    hacku_key_t k;
    while (hacku_input_get(&k))
        ;
}
