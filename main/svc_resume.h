#pragma once

/* Resume-a-scan-across-reboot. Radio switches leak heap per cycle (IDF
 * driver deinit residue: ~15K per WiFi cycle, ~9K per BLE cycle against a
 * ~34K budget), so no static diet survives many switches. When the
 * bring-up guard trips, persist the requested scan, reboot to pristine
 * heap, and auto-run it on boot.
 *
 * RTC memory: survives esp_restart, cleared on power loss (correct: a cold
 * boot must land on the locked calculator, never inside a scan). */

#define RESUME_NONE 0
#define RESUME_WIFI 1
#define RESUME_BLE  2

void resume_request(int which); /* store + reboot (does not return) */
int  resume_take(void);         /* read + clear pending request */
void resume_mark_ok(void);      /* a scan completed: heap proven fine */
int  resume_note_lowmem(void);  /* record a guard trip; returns strikes */
const char *resume_boot_cause(void); /* short reset-cause tag for UI */
