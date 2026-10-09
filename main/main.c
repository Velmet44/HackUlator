#include "app_config.h"
#include "caster.h"
#include "hal_oled.h"
#include "hal_input.h"
#include "ui_calc.h"
#include "ui_menu.h"
#include "ui_wifiscan.h"
#include "ui_blescan.h"
#include "ui_wifiatk.h"
#include "ui_bleatk.h"
#include "svc_wifi.h"
#include "svc_ble.h"
#include "svc_deauth.h"
#include "svc_resume.h"
#include "svc_sniff.h"
#include "ui_sniff.h"
#include "ui_blepassive.h"
#include "ui_bleadv.h"
#include "ui_wifipassive.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "esp_bt.h"

static const char *TAG = "hackulator";

typedef enum {
    SCR_CALC,
    SCR_MENU,
    SCR_WIFISCAN,
    SCR_BLESCAN,
    SCR_WIFIATK,
    SCR_BLEATK,
    SCR_WIFIPASSIVE,
    SCR_SNIFF,
    SCR_BLEPASSIVE,
    SCR_BLEADV,
} screen_t;

static const char *MENU_ITEMS[] = {
    "WiFi scan", "BLE scan", "WiFi attacks", "BLE attacks",
    "WiFi passive", "BLE passive",
};
#define MENU_N 6

static int64_t s_last_atk_tick = 0; /* live-attack status repaint (ms) */
static int64_t s_last_sniff_tick = 0; /* passive counter repaint (ms) */

static void show_menu(void) {
    ui_menu_enter("HACKULATOR", MENU_ITEMS, MENU_N);
}

/* Guard menu selection: drop the deauth TX timer AND the promiscuous RX
 * callback before ANY radio teardown or re-verify, then guard the whole
 * selection+transition. A live promiscuous callback pointing into a
 * deinitialised driver faults, exactly like an orphan TX timer. */
static int enter_menu_item(int sel, screen_t *screen_out) {
    if (sel == 0) {                       /* WiFi scan */
        svc_deauth_stop();
        svc_sniff_stop();
        svc_ble_stop();
        *screen_out = SCR_WIFISCAN;
        ui_wifiscan_run();
        return 1;
    }
    if (sel == 1) {                       /* BLE scan (stops WiFi itself) */
        svc_deauth_stop();
        svc_sniff_stop();
        *screen_out = SCR_BLESCAN;
        ui_blescan_run();
        return 1;
    }
    if (sel == 2) {                       /* WiFi attacks */
        svc_deauth_stop();
        svc_sniff_stop();
        svc_ble_stop();
        svc_wifi_teardown();              /* attacks start radio-cold */
        /* Enter the screen unconditionally: target verification happens
         * when an attack that needs one is launched (beacon spam does
         * not), so no scan is forced just by opening the list. */
        *screen_out = SCR_WIFIATK;
        ui_wifiatk_run();
        return 1;
    }
    if (sel == 3) {                       /* BLE attacks */
        svc_deauth_stop();
        svc_sniff_stop();
        svc_ble_stop();
        svc_wifi_teardown();
        *screen_out = SCR_BLEATK;
        ui_bleatk_run();
        return 1;
    }
    if (sel == 4) {                       /* WiFi passive submenu */
        svc_deauth_stop();
        svc_ble_passive_stop();
        svc_sniff_stop();
        svc_ble_stop();
        /* Radio stays up: promiscuous RX shares the STA path, so no
         * teardown here - that is the whole point of a passive monitor. */
        *screen_out = SCR_WIFIPASSIVE;
        ui_wifipassive_run();
        return 1;
    }
    if (sel == 5) {                       /* BLE passive submenu */
        svc_deauth_stop();
        svc_sniff_stop();
        svc_ble_passive_stop();
        svc_ble_stop();
        *screen_out = SCR_BLEPASSIVE;
        ui_blepassive_run();
        return 1;
    }
    return 0;
}

void app_main(void) {
    /* BLE-only device: release Classic-BT controller RAM to the heap FIRST,
     * before anything else fragments it. One-shot and irreversible; BLE
     * bring-up later is unaffected. */
    esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT);
    ESP_LOGI(TAG, "largest free block: %u",
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    int has_oled = hal_oled_init();
    if (has_oled < 0) {
        ESP_LOGE(TAG, "display init failed, halting");
        return;
    }

    esp_err_t r = nvs_flash_init();
    if (r == ESP_ERR_NVS_NO_FREE_PAGES || r == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }

    /* Caster mirror is always on: streams the mono UI over USB for the PC
     * viewer and takes remote keys. Silences the log bus by design. */
    caster_start();

    hal_oled_set_contrast(100);
    hacku_input_init();

    int unlocked = 0;
    screen_t screen = SCR_CALC;
    int64_t last_activity = esp_timer_get_time();

    int resume = resume_take();
    if (resume == RESUME_WIFI || resume == RESUME_BLE) {
        /* Reboot-resumed radio switch: heap is pristine, run it now. */
        unlocked = 1;
        ESP_LOGI(TAG, "resume scan %d", resume);
        if (resume == RESUME_WIFI) {
            screen = SCR_WIFISCAN;
            svc_ble_stop();
            ui_wifiscan_run();
        } else {
            screen = SCR_BLESCAN;
            ui_blescan_run();
        }
        last_activity = esp_timer_get_time();
    } else {
        ui_calc_enter();
    }
    ESP_LOGI(TAG, "boot: calculator%s", has_oled ? " (OLED)" : " (caster)");

    hacku_key_t k;
    for (;;) {
        while (hacku_input_get(&k)) {
            last_activity = esp_timer_get_time();
            if (k == KEY_RELOCK) {
                if (unlocked) {
                    unlocked = 0;
                    screen = SCR_CALC;
                    svc_deauth_stop(); /* TX timer must die before radio */
                    svc_sniff_stop();  /* RX callback must die before radio */
                    svc_ble_passive_stop(); /* BT scan must die before radio */
                    svc_wifi_teardown(); /* radios off + heap freed */
                    svc_ble_stop();
                    ui_wifiscan_drop(); /* free scan lists so the next
                                         * bring-up gets the block back */
                    ui_blescan_drop();
                    ui_calc_enter();
                }
                continue;
            }
            if (!unlocked) {
                if (ui_calc_key(k) == ACT_UNLOCK) {
                    unlocked = 1;
                    screen = SCR_MENU;
                    show_menu();
                }
                continue;
            }
            if (screen == SCR_MENU) {
                /* ui_menu_key returns MENU_BACK/-1 or an index; anything
                 * that transitions must pass the deauth/radio guard. */
                int sel = ui_menu_key(k);
                if (sel >= 0) {
                    enter_menu_item(sel, &screen);
                }
                /* BACK on the top menu does nothing (relock via gesture) */
            } else if (screen == SCR_WIFISCAN) {
                if (ui_wifiscan_key(k)) {
                    screen = SCR_MENU;
                    show_menu();
                }
            } else if (screen == SCR_BLESCAN) {
                if (ui_blescan_key(k)) {
                    screen = SCR_MENU;
                    show_menu();
                }
            } else if (screen == SCR_WIFIATK) {
                int r = ui_wifiatk_key(k);
                if (r == WIFATK_EXIT_SCAN) {
                    /* no/vanished target: bounce to the scan page */
                    screen = SCR_WIFISCAN;
                    ui_wifiscan_run();
                } else if (r == WIFATK_EXIT_MENU) {
                    screen = SCR_MENU;
                    show_menu();
                }
            } else if (screen == SCR_BLEATK) {
                int r = ui_bleatk_key(k);
                if (r == BLEATK_EXIT_SCAN) {
                    screen = SCR_BLESCAN;
                    ui_blescan_run();
                } else if (r == BLEATK_EXIT_MENU) {
                    screen = SCR_MENU;
                    show_menu();
                }
            } else if (screen == SCR_WIFIPASSIVE) {
                int r = ui_wifipassive_key(k);
                if (r == WIFIPASS_EXIT_MENU) {
                    screen = SCR_MENU;
                    show_menu();
                } else if (r == WIFIPASS_OPEN_RX) {
                    screen = SCR_SNIFF;
                    ui_sniff_run();
                }
            } else if (screen == SCR_SNIFF) {
                /* BACK from a monitor goes to its submenu when there is
                 * one, so the user can reach the sibling passive tools
                 * without walking the whole top menu again. */
                if (ui_sniff_key(k) == SNIFF_EXIT_MENU) {
                    screen = SCR_WIFIPASSIVE;
                    ui_wifipassive_run();
                }
            } else if (screen == SCR_BLEPASSIVE) {
                int r = ui_blepassive_key(k);
                if (r == BLEPASS_EXIT_MENU) {
                    screen = SCR_MENU;
                    show_menu();
                } else if (r == BLEPASS_OPEN_ADV) {
                    /* The only screen that actually brings Bluedroid up;
                     * it tears WiFi down itself since BT needs the heap. */
                    screen = SCR_BLEADV;
                    ui_bleadv_run();
                }
            } else if (screen == SCR_BLEADV) {
                /* BACK returns to the BLE passive submenu, mirroring the
                 * WiFi side. */
                if (ui_bleadv_key(k) == BLEADV_EXIT_MENU) {
                    screen = SCR_BLEPASSIVE;
                    ui_blepassive_run();
                }
            }
        }
        hal_oled_caster_poll(); /* skipped-frame catch-up + self-heal */
        /* Live status for a running attack (paced, cheap, OLED+caster). */
        if (screen == SCR_WIFIATK || screen == SCR_BLEATK) {
            int64_t now_ms = esp_timer_get_time() / 1000;
            if (now_ms - s_last_atk_tick >= 500) {
                s_last_atk_tick = now_ms;
                ui_wifiatk_tick();
                ui_bleatk_tick();
            }
        }
        /* Live counters for the passive RX monitor (paced, cheap). */
        if (screen == SCR_SNIFF || screen == SCR_BLEADV) {
            int64_t now_ms = esp_timer_get_time() / 1000;
            if (now_ms - s_last_sniff_tick >= 500) {
                s_last_sniff_tick = now_ms;
                ui_sniff_tick();
                ui_bleadv_tick();
            }
        }
        if (unlocked &&
            esp_timer_get_time() - last_activity > (int64_t)IDLE_RELOCK_MS * 1000) {
            unlocked = 0;
            screen = SCR_CALC;
            svc_deauth_stop();
            svc_sniff_stop();
            svc_ble_passive_stop();
            svc_wifi_teardown();
            svc_ble_stop();
            ui_wifiscan_drop();
            ui_blescan_drop();
            ui_calc_enter();
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}
