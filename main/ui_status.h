#pragma once

/* Shared bottom status bar: free heap + attack targets.
 * Canonical line (fits 21 cells): "34K W:abc B:def".
 * mem = largest free 8-bit block in KB (capped at 9999, like the
 * scan screens). w = first 3 letters of the WiFi target SSID ("-"
 * when unset), b = first 3 of the BLE target name ("-" when unset).
 * The calculator screen deliberately has no bar (it is a disguise). */

#include <stddef.h>

unsigned ui_status_memk(void); /* largest free block, KB */
void ui_status_targets(char *out, size_t n); /* "W:abc B:def" */
void ui_status_line(char *out, size_t n);     /* "34K W:abc B:def" */
