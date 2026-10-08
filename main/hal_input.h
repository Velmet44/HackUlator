#pragma once

/* Unified key input: 6 physical buttons (internal pull-ups, active low)
 * merged with remote single-byte keys from the caster RX queue. */

typedef enum {
    KEY_NONE = 0,
    KEY_UP,
    KEY_DOWN,
    KEY_LEFT,
    KEY_RIGHT,
    KEY_OK,
    KEY_BACK,
    KEY_RELOCK,   /* OK+BACK held, or 3x BACK: return to locked calculator */
} hacku_key_t;

void hacku_input_init(void);

/* Non-blocking drain: returns 1 and fills *out when a key event is pending. */
int hacku_input_get(hacku_key_t *out);

/* Drop all pending key events. */
void hacku_input_drain(void);
