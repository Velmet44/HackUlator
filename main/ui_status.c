#include "ui_status.h"
#include "svc_target.h"
#include "esp_heap_caps.h"
#include <stdio.h>

unsigned ui_status_memk(void) {
    unsigned kb =
        (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT) / 1024);
    return kb > 9999 ? 9999 : kb;
}

void ui_status_targets(char *out, size_t n) {
    const char *w = tgt_wifi_has() && tgt_wifi_ssid()[0] ? tgt_wifi_ssid()
                                                         : "-";
    const char *b = tgt_ble_has() && tgt_ble_name()[0] ? tgt_ble_name() : "-";
    snprintf(out, n, "W:%.3s B:%.3s", w, b);
}

void ui_status_line(char *out, size_t n) {
    char t[16];
    ui_status_targets(t, sizeof(t));
    snprintf(out, n, "%uK %s", ui_status_memk(), t);
}
