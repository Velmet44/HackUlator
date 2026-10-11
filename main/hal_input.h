#pragma once

/* Unified key input: 6 physical buttons (internal pull-ups, active low)
 * merged with remote single-byte keys from the caster RX queue, plus the
 * module's own BOOT button on GPIO0 as a long-press gesture.
 *
 * KEY_ONEBTN has no remote release edge on the wire, so the caster byte 'x'
 * stands for "already held 3 s" and fires at once. Without it the feature
 * could only ever be tested by walking to the board. */

typedef enum {
    KEY_NONE = 0,
    KEY_UP,
    KEY_DOWN,
    KEY_LEFT,
    KEY_RIGHT,
    KEY_OK,
    KEY_BACK,
    KEY_RELOCK,   /* OK+BACK held, or 3x BACK: return to locked calculator */
    /* One button. Two events, because the caster cannot express a hold:
     * KEY_ONEBTN is a TAP (space, or a physical press shorter than the
     * 3 s hold) and KEY_ONEBTN_HOLD is the long press. Merging them made
     * 'x' count as a tap, so a remote long press could never reach the
     * toggle at all - and it still "passed" a test, because the screen it
     * was supposed to change was not the one being looked at. */
    KEY_ONEBTN,       /* tap on the one button */
    KEY_ONEBTN_HOLD,  /* 3 s hold: ENTERS direct mode, once, one way */
} hacku_key_t;

/* Hold time for the GPIO0 BOOT button that fires KEY_ONEBTN. Long on
 * purpose: this shares a pin with nothing else, but a 3 s press is hard to
 * trigger by accident while poking at the board, which is the whole point of
 * a feature that removes the disguise. */
#define ONEBTN_HOLD_MS 3000

/* One-button mode: the six keypad keys AND the caster's arrows/wasd are
 * dead, and GPIO0 (or the caster's spacebar) is the only input.
 *
 * One button has to express three actions, so it speaks in taps, counted
 * inside TAP_WINDOW_MS of each other:
 *
 *   1 tap  -> KEY_DOWN   move to the next row / cycle a value
 *   2 taps -> KEY_OK     enter the highlighted row / run it
 *   3 taps -> KEY_BACK   leave the screen
 *   hold   -> KEY_UP     move back up (ONEBTN_UP_HOLD_MS = 700)
 *
 * The hold is UP, not the old "toggle". The mode is one-way, so the toggle
 * was only ever needed once, to get IN; spending the hold on UP fixes the
 * real navigation hole. Before this the HAL emitted DOWN/OK/BACK and never
 * UP, so every list was one-way and reaching an earlier row meant cycling
 * the whole list the long way round.
 *
 * OUTSIDE one-button mode the hold is the 3 s entry gesture instead, and
 * there is nothing to navigate, so a tap does nothing there.
 *
 * In NORMAL (disguised) mode a tap means nothing - there is no cursor to move
 * - so space there only acts as the hold. That keeps the caster and the
 * physical BOOT button identical.
 *
 * A tap emits only once the window closes, so every action costs up to
 * TAP_WINDOW_MS of latency. That is the price of not having four more
 * buttons, and it is paid deliberately rather than by double-firing DOWN and
 * correcting it a moment later.
 *
 * KEY_ONEBTN is a long press and cannot be expressed remotely, so the caster
 * also maps 'x' to it.
 */
#define ONEBTN_TAP_WINDOW_MS 500
#define ONEBTN_TAP_MAX 3

/* Bytes of the caster key arriving closer together than this are one
 * CONTINUOUS PRESS, not a fast series of taps.
 *
 * Without this, holding spacebar is auto-repeat at ~30 Hz: the tap counter
 * reaches 3 instantly, emits KEY_BACK, resets, and does it again, so a held
 * key spams "back" dozens of times a second and never toggles anything.
 *
 * 90 ms is deliberately far below the tap window above. It must clear
 * keyboard auto-repeat (~33 ms) while leaving room for two human taps inside
 * ONEBTN_TAP_WINDOW_MS. An earlier value of 120 ms sat ON that boundary: taps
 * 120 ms apart - which is a normal double-tap - were classified as a hold,
 * their tap runs were cancelled, and 2-tap/3-tap gestures silently did
 * nothing. Any threshold near the tap window's step size has this failure. */
#define ONEBTN_REPEAT_GAP_MS 90

/* Hold length that means UP, inside one-button mode.
 *
 * The 3 s hold was spent on toggling the disguise, which is now one way and
 * therefore only needed once to get IN. Reusing it as "up" costs nothing and
 * fixes the real navigation hole: the HAL emits DOWN/OK/BACK but never UP, so
 * every list was downward-only and reaching an earlier row meant cycling the
 * whole list the long way round.
 *
 * Shorter than ONEBTN_HOLD_MS on purpose - that one is a deliberate, hard to
 * trigger-by-accident gesture that cannot be undone; "up" is used constantly
 * and must not cost 3 seconds. */
#define ONEBTN_UP_HOLD_MS 700

/* A hold survives gaps up to this long. Deliberately much wider than
 * ONEBTN_REPEAT_GAP_MS: the two thresholds answer different questions.
 *
 *   REPEAT_GAP  "is this byte a tap, or the same press repeating?"
 *               must stay under a human double-tap (~200 ms)
 *   HOLD_GAP    "has the user let go of the key yet?"
 *
 * Sharing one threshold for both meant a single slow repeat byte ended the
 * hold, which started a fresh 3 s timer and toggled again - an 8 s hold was
 * observed toggling at 3.5 s and again near 8 s. */
#define ONEBTN_HOLD_GAP_MS 300

/* Switch one-button mode on/off. While it is on the HAL ignores the six
 * physical keys and every caster key except the one-button ones, and
 * synthesises KEY_DOWN / KEY_OK / KEY_BACK from taps instead. main.c owns
 * the flag (that is where the mode lives) and keeps this in step with it. */
void hacku_input_set_onebutton(int on);
int hacku_input_onebutton(void);

/* Milliseconds the one button has been held, 0..ONEBTN_HOLD_MS, 0 if not
 * currently held. Lets the UI draw a filling bar so a 3 second hold is not
 * a 3 second silence - a dead button and a working one look identical
 * otherwise, which is precisely how a wiring fault presents. */
int hacku_input_onebtn_held_ms(void);

/* Total the current hold is counting towards: ONEBTN_UP_HOLD_MS inside
 * one-button mode, ONEBTN_HOLD_MS otherwise. The progress bar divides by this,
 * so it always fills to exactly the moment the gesture actually fires. */
int hacku_input_onebtn_hold_target_ms(void);

void hacku_input_init(void);

/* Non-blocking drain: returns 1 and fills *out when a key event is pending. */
int hacku_input_get(hacku_key_t *out);

/* Drop all pending key events. */
void hacku_input_drain(void);
