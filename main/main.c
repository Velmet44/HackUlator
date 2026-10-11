#include "app_config.h"
#include "caster.h"
#include "hal_oled.h"
#include "oled_gfx.h"
#include "font_oled.h"
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
#include "ui_survey.h"
#include "ui_wicoll.h"
#include "svc_wicoll.h"
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
    SCR_SURVEY,
    SCR_WICOLL,
} screen_t;

static const char *MENU_ITEMS[] = {
    "WiFi scan", "BLE scan", "WiFi attacks", "BLE attacks",
    "WiFi passive", "BLE passive", "Channel survey", "Collateral",
};
#define MENU_N 8

static int64_t s_last_atk_tick = 0; /* live-attack status repaint (ms) */
static int64_t s_last_sniff_tick = 0; /* passive counter repaint (ms) */

/* "Direct" mode: the calculator disguise is off and the device goes straight
 * to the menu. Toggled by holding the module's BOOT button (GPIO0) for
 * ONEBTN_HOLD_MS, delivered as KEY_ONEBTN.
 *
 * RAM only, deliberately - a reboot always comes back disguised, so this can
 * never be left on by accident and it is not a stored setting an attacker
 * (or a passer-by) could find and re-arm. */
static int s_direct = 0;

static void show_menu(void); /* used by go_home() below */

/* How long the one-button confirmation stays on screen before the home
 * screen is repainted. Long enough to read, short enough that it never feels
 * like the device has locked up. */
#define DIRECT_NOTICE_MS 1500

/* Confirm the mode actually changed. Removing the disguise is a silent,
 * large change to how the device looks - without this the operator has no
 * way to tell a mode they enabled from a broken screen.
 *
 * The wording is one-sided on purpose: the mode can only be entered, so
 * "OFF" is not a state this device can reach and must not be advertised.
 *
 * Transient: it paints over the home screen, so something has to put the
 * home screen back afterwards or the device sits on a dead message with no
 * visible way out. s_notice_until drives that; a keypress also clears it. */
static int64_t s_notice_until = 0;

static void direct_notice(void) {
    oled_fb_t *fb = hal_oled_fb();
    oled_clear(fb, 0);
    const char *l1 = "DIRECT MODE ON";
    const char *l2 = "reboot to restore";
    int w1 = oled_text_w(&oled_font, l1);
    oled_text(fb, &oled_font, (OLED_W - w1) / 2, 22, l1, 1);
    int w2 = oled_text_w(&oled_font, l2);
    oled_text(fb, &oled_font, (OLED_W - w2) / 2, 36, l2, 1);
    hal_oled_flush_all();
    s_notice_until = esp_timer_get_time() + DIRECT_NOTICE_MS * 1000;
}

/* Repaint whichever home screen the current mode owns. */
static void redraw_home(void) {
    if (s_direct) {
        show_menu();
    } else {
        ui_calc_enter();
    }
}

/* Full teardown back to a stopped, radio-free state. The ORDER matters:
 * every TX timer and RX callback must die before the driver underneath them
 * goes away, or the next bring-up faults. See enter_menu_item(). */
static void teardown_radios(void) {
    svc_deauth_stop();
    wicoll_stop();
    svc_sniff_stop();
    svc_ble_passive_stop();
    svc_wifi_teardown();
    svc_ble_stop();
    ui_wifiscan_drop(); /* free scan lists so the next bring-up gets the block */
    ui_blescan_drop();
}

/* Return to the home screen for the current mode, with every radio stopped.
 *
 * `unlocked` is derived, not chosen: in direct mode the home screen IS the
 * menu, so the device must stay unlocked or the key loop would keep handing
 * presses to ui_calc_key() while the menu is on the display. Getting that
 * pairing wrong is silent - no crash, just a menu that ignores the keypad. */
static void go_home(int *unlocked, screen_t *screen) {
    teardown_radios();
    *screen = s_direct ? SCR_MENU : SCR_CALC;
    *unlocked = s_direct;
    /* Keep the input layer in step. In one-button mode it refuses the six
     * keys and the caster arrows and turns the one button into taps - so this
     * call is what makes the mode actually take effect. */
    hacku_input_set_onebutton(s_direct);
    if (s_direct) {
        show_menu();
    } else {
        ui_calc_enter();
    }
}

static void show_menu(void) {
    ui_menu_enter("HACKULATOR", MENU_ITEMS, MENU_N);
}

/* Fill a bar along the bottom while the one button is held.
 *
 * Purely feedback, and deliberately so: it is the only thing that
 * distinguishes "the hold is counting" from "this pin is not wired to
 * anything", which look identical from the outside. Costs one bar's worth of
 * pixels and no state. Returns 1 while it drew, so the caller can skip its
 * own repaint and the bar does not flash. */
static int draw_hold_progress(void) {
    int held = hacku_input_onebtn_held_ms();
    if (held <= 0) {
        return 0;
    }
    oled_fb_t *fb = hal_oled_fb();
    int total = hacku_input_onebtn_hold_target_ms();
    int w = (int)((int64_t)OLED_W * held / (total > 0 ? total : 1));
    if (w < 1) {
        w = 1;
    }
    if (w > OLED_W) {
        w = OLED_W;
    }
    int y = OLED_H - 2;
    oled_fill_rect(fb, 0, y - 1, w, 2, 1);
    hal_oled_flush_all();
    return 1;
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
    if (sel == 6) {                       /* channel survey */
        svc_deauth_stop();
        svc_sniff_stop();
        svc_ble_passive_stop();
        svc_ble_stop();
        /* Radio stays up: the survey listens in short bursts across the band
         * rather than scanning, so it needs the STA path, not a cold radio.
         * svc_wifi_teardown() here would make it re-init the driver 13 times. */
        *screen_out = SCR_SURVEY;
        ui_survey_run();
        return 1;
    }
    if (sel == 7) {                       /* collateral */
        /* Guard every radio owner before ANY teardown or transition. The TX
         * task points into the WiFi driver, so it must be gone first - same
         * reason the deauth timer and the promiscuous callback are stopped
         * above. */
        svc_deauth_stop();
        wicoll_stop();
        svc_sniff_stop();
        svc_ble_passive_stop();
        svc_ble_stop();
        *screen_out = SCR_WICOLL;
        ui_wicoll_run();
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
    } else if (s_direct) {
        /* Disguise off: straight to the menu, no calculator. s_direct is
         * RAM-only and defaults to 0, so this is unreachable at first boot -
         * a fresh boot is always disguised. */
        unlocked = 1;
        screen = SCR_MENU;
        show_menu();
        last_activity = esp_timer_get_time();
    } else {
        ui_calc_enter();
    }
    ESP_LOGI(TAG, "boot: %s%s", s_direct ? "direct menu" : "calculator",
             has_oled ? " (OLED)" : " (caster)");

    hacku_key_t k;
    for (;;) {
        while (hacku_input_get(&k)) {
            last_activity = esp_timer_get_time();
            if (k == KEY_RELOCK) {
                if (unlocked) {
                    go_home(&unlocked, &screen);
                }
                continue;
            }
            if (k == KEY_ONEBTN_HOLD) {
                /* ONE WAY. This can enter the mode and never leave it; a
                 * reboot is the only way back to the calculator.
                 *
                 * That is deliberate, and it is also the safety property. The
                 * disguise is what the device is for; a single mis-fired
                 * long press used to put it back on screen, which is the one
                 * outcome the operator cannot afford on a device that is
                 * supposed to look like a calculator. With the mode one-way,
                 * every press either advances or does nothing - there is no
                 * path that re-reveals it by accident.
                 *
                 * Reachable from BOTH home screens. This used to require
                 * SCR_MENU, which made the gesture unreachable from a cold
                 * boot: a disguised device sits on SCR_CALC, so the hold fell
                 * into the "go home" branch and just re-drew the calculator.
                 * Nothing appeared to happen - and the suite passed anyway,
                 * because every check ran from the menu, the one screen where
                 * the bug is invisible. */
                if (!s_direct) {
                    s_direct = 1;
                    ESP_LOGI(TAG, "one-button: direct ON (one-way, "
                                  "reboot to restore)");
                    go_home(&unlocked, &screen);
                    direct_notice();
                } else {
                    /* Already on. The hold must NOT disarm the disguise. It
                     * is still the escape hatch out of whatever screen we
                     * are in - which tears down any running attack - and on
                     * the menu it is simply a harmless redraw. */
                    go_home(&unlocked, &screen);
                }
                continue;
            }
            /* Any real key dismisses the confirmation immediately, so an
             * impatient operator is never stuck looking at a dead message. */
            if (s_notice_until) {
                s_notice_until = 0;
                redraw_home();
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
            } else if (screen == SCR_SURVEY) {
                if (ui_survey_key(k) == SURVEY_EXIT_MENU) {
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
            } else if (screen == SCR_WICOLL) {
                if (ui_wicoll_key(k)) {
                    screen = SCR_MENU;
                    show_menu();
                }
            }
        }
        hal_oled_caster_poll(); /* skipped-frame catch-up + self-heal */
        /* One-button hold progress. Only from the home screens, where a hold
         * actually does something - drawing it inside a submenu would
         * promise feedback for a gesture that means "go back" instead. */
        if (screen == SCR_CALC || screen == SCR_MENU) {
            draw_hold_progress();
        }
        /* One-button confirmation expires back to the home screen. */
        if (s_notice_until && esp_timer_get_time() >= s_notice_until) {
            s_notice_until = 0;
            redraw_home();
        }
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
        /* Live counters for a running collateral attack. Its own tick also
         * honours the auto-stop flag, so it must be on the same 500 ms
         * cadence as the other live screens. */
        if (screen == SCR_WICOLL) {
            int64_t now_ms = esp_timer_get_time() / 1000;
            if (now_ms - s_last_atk_tick >= 500) {
                s_last_atk_tick = now_ms;
                ui_wicoll_tick();
            }
        }
        if (unlocked &&
            esp_timer_get_time() - last_activity > (int64_t)IDLE_RELOCK_MS * 1000) {
            go_home(&unlocked, &screen);
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}
