#include "svc_resume.h"
#include "esp_attr.h"
#include "esp_system.h"

/* NOTE: RTC_NOINIT_ATTR, not RTC_DATA_ATTR: the startup code re-applies
 * .rtc.data initializers on every boot (wiping a pending request before
 * main() ever reads it). NOINIT memory is left alone across esp_restart;
 * the magic tells garbage (cold boot) apart from a real request. */
#define RESUME_MAGIC 0xACED1234u

RTC_NOINIT_ATTR static uint32_t s_magic;
RTC_NOINIT_ATTR static int s_pending;
RTC_NOINIT_ATTR static int s_strikes;

static void ensure_init(void) {
    if (s_magic != RESUME_MAGIC) {
        s_magic = RESUME_MAGIC;
        s_pending = RESUME_NONE;
        s_strikes = 0;
    }
}

void resume_request(int which) {
    ensure_init();
    s_pending = which;
    esp_restart();
}

int resume_take(void) {
    ensure_init();
    int w = s_pending;
    s_pending = RESUME_NONE;
    return w;
}

int resume_note_lowmem(void) {
    ensure_init();
    return ++s_strikes;
}

void resume_mark_ok(void) {
    ensure_init();
    s_strikes = 0;
}

static const char *cause_str(esp_reset_reason_t r) {
    switch (r) {
        case ESP_RST_POWERON:   return "PWRON";
        case ESP_RST_EXT:       return "EXT";
        case ESP_RST_SW:        return "SW";
        case ESP_RST_PANIC:     return "PANIC";
        case ESP_RST_INT_WDT:   return "IWDT";
        case ESP_RST_TASK_WDT:  return "TWDT";
        case ESP_RST_WDT:       return "WDT";
        case ESP_RST_DEEPSLEEP: return "SLEEP";
        case ESP_RST_BROWNOUT:  return "BROWN";
        case ESP_RST_SDIO:      return "SDIO";
        default:                return "?";
    }
}

/* Short boot-cause tag for the menu footer (PWRON/EXT/SW/PANIC/BROWN/...).
 * Tells brownouts and EN-pin glitches apart from clean boots. */
const char *resume_boot_cause(void) {
    return cause_str(esp_reset_reason());
}
