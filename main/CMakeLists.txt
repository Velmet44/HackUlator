idf_component_register(SRCS "main.c"
                            "caster.c"
                            "hal_oled.c"
                            "hal_input.c"
                            "oled_gfx.c"
                            "ui_calc.c"
                            "ui_menu.c"
                            "ui_wifiscan.c"
                            "ui_blescan.c"
                            "ui_wifiatk.c"
                            "ui_bleatk.c"
                            "ui_sniff.c"
                            "ui_blepassive.c"
                            "ui_wifipassive.c"
                            "ui_status.c"
                            "svc_wifi.c"
                            "svc_ble.c"
                            "svc_target.c"
                            "svc_deauth.c"
                            "svc_resume.c"
                            "svc_sniff.c"
                            "wsl_bypasser.c"
                       INCLUDE_DIRS "."
                       PRIV_REQUIRES esp_wifi esp_event esp_netif nvs_flash
                                     driver esp_timer bt esp_lcd)

# WSL bypass: let our ieee80211_raw_frame_sanity_check override the copy in
# the closed Wi-Fi binary. Without -zmuldefs the duplicate symbol is dropped
# and esp_wifi_80211_tx() keeps rejecting deauth frames with INVALID_ARG.
target_link_libraries(${COMPONENT_LIB} -Wl,-zmuldefs)
