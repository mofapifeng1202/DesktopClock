idf_component_register(SRCS "main.c" "wifi_app.c" "weather.c" "clock_ui.c"
                             "font_cn_14.c" "font_cn_16.c"
                       INCLUDE_DIRS "."
                       REQUIRES esp_http_client json nvs_flash esp_wifi esp_netif
                                esp_event esp_timer mbedtls esp_adc
                                driver esp_lcd_touch esp_codec_dev led_strip)
