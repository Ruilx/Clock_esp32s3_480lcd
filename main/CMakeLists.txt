idf_component_register(
    SRCS "app_main.cpp" "board.cpp" "board_lvgl.cpp" "clock_ui.cpp" "clock_time.cpp" "network.cpp"
    INCLUDE_DIRS "."
    PRIV_REQUIRES esp_driver_gpio esp_driver_i2c esp_timer esp_hw_support esp_wifi esp_netif esp_event
                  esp_http_server lwip nvs_flash esp_lcd
)
