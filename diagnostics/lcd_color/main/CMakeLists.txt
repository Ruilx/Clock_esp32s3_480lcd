set(board_dir "${CMAKE_CURRENT_LIST_DIR}/../../../main")

idf_component_register(
    SRCS "color_test.cpp" "${board_dir}/board.cpp"
    PRIV_INCLUDE_DIRS "${board_dir}"
    PRIV_REQUIRES esp_lcd esp_driver_gpio esp_driver_i2c esp_psram
)
