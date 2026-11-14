idf_component_register(SRCS "main.c" "wifi_app.c" "weather.c" "clock_ui.c"
                             "radio.c" "radio_ui.c" "spectrum.c"
                             "media_list.c" "ts_demux.c" "tv_ui.c"
                             "settings.c" "backlight.c" "web_config.c"
                             "settings_ui.c" "ui_sync.c"
                             "font_cn_14.c" "font_cn_16.c"
                             "font_radio_14.c" "font_radio_20.c" "font_zh_14.c"
                       INCLUDE_DIRS "."
                       REQUIRES esp_http_client json nvs_flash esp_wifi esp_netif
                                esp_event esp_timer mbedtls esp_adc esp_ringbuf
                                driver esp_lcd_touch esp_codec_dev led_strip esp_http_server)
