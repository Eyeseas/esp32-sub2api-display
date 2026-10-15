idf_component_register(SRCS "app_main.c" "board.c" "device_config.c" "model.c" "network.c" "ui.c" "display_font.c" "display_font_14.c"
                       INCLUDE_DIRS "."
                       REQUIRES esp_lcd esp_driver_gpio esp_driver_ledc esp_driver_spi esp_driver_i2c esp_wifi esp_netif esp_event
                                nvs_flash esp_http_client esp-tls lwip esp_timer esp_app_format)
