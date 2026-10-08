#include "app_config.h"
#include "caster.h"
#include "hal_display.h"
#include "hal_input.h"
#include "ui_calc.h"
#include "ui_menu.h"
#include "ui_wifiscan.h"
#include "ui_blescan.h"
#include "svc_wifi.h"
#include "svc_ble.h"
#include "svc_resume.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "esp_bt.h"

static const char *TAG = "hackulator";

typedef enum { SCR_CALC, SCR_MENU, SCR_WIFISCAN, SCR_BLESCAN } screen_t;

static const char *MENU_ITEMS[] = { "WiFi scan", "BLE scan" };
#define MENU_N 2

static void show_menu(void) {
    ui_menu_enter("HACKULATOR", MENU_ITEMS, MENU_N);
}

void app_main(void) {
    /* BLE-only device: release Classic-BT controller RAM to the heap FIRST,
     * before the framebuffer carves DRAM into immovable blocks. One-shot
     * and irreversible; BLE bring-up later is unaffected. */
    esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT);
    ESP_LOGI(TAG, "largest free block: %u",
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    /* Framebuffer FIRST: it needs the largest contiguous DRAM block, before
     * NVS/tasks/WiFi fragment the heap. */
    int has_tft = hacku_display_init();
    if (has_tft < 0) {
        ESP_LOGE(TAG, "display init failed, halting");
        return;
    }

    esp_err_t r = nvs_flash_init();
    if (r == ESP_ERR_NVS_NO_FREE_PAGES || r == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }

    if (!has_tft)
        caster_start(); /* headless: stream UI over USB, keys from viewer */

    hacku_display_set_brightness(100);
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
    ESP_LOGI(TAG, "boot: calculator%s", has_tft ? " (TFT)" : " (caster)");

    hacku_key_t k;
    for (;;) {
        while (hacku_input_get(&k)) {
            last_activity = esp_timer_get_time();
            if (k == KEY_RELOCK) {
                if (unlocked) {
                    unlocked = 0;
                    screen = SCR_CALC;
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
                int sel = ui_menu_key(k);
                if (sel == 0) {
                    screen = SCR_WIFISCAN;
                    svc_ble_stop();
                    ui_wifiscan_run();
                } else if (sel == 1) {
                    screen = SCR_BLESCAN;
                    ui_blescan_run(); /* stops WiFi internally */
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
            }
        }
        hacku_display_caster_poll(); /* skipped-frame catch-up + self-heal */
        if (unlocked &&
            esp_timer_get_time() - last_activity > (int64_t)IDLE_RELOCK_MS * 1000) {
            unlocked = 0;
            screen = SCR_CALC;
            svc_wifi_teardown();
            svc_ble_stop();
            ui_wifiscan_drop();
            ui_blescan_drop();
            ui_calc_enter();
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}
