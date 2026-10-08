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
        default: return KEY_NONE;
    }
}

static int btn_index(hacku_key_t k) {
    for (int i = 0; i < 6; i++)
        if (s_btn_keys[i] == k)
            return i;
    return -1;
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

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(POLL_MS));
        int64_t now = esp_timer_get_time();

        /* Physical buttons, debounced. */
        for (int i = 0; i < 6; i++) {
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

        /* OK+BACK chord held -> relock (physical and/or remote). */
        int ok_held = (prev[4] == 0) || (now < remote_held_until[4]);
        int back_held = (prev[5] == 0) || (now < remote_held_until[5]);
        if (ok_held && back_held && !relock_sent) {
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
    xTaskCreatePinnedToCore(input_task, "hk_input", 4096, NULL, 10, NULL,
                            tskNO_AFFINITY);
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
