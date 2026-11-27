#include <algorithm>
#include <cstdint>

#include "board.hpp"
#include "clock_time.hpp"
#include "clock_ui.hpp"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "host_status.hpp"
#include "network.hpp"
#include "nvs_flash.h"

extern "C" void app_main()
{
    if (const auto err = board::init_display(); err != ESP_OK) {
        ESP_LOGE("clock", "Display initialization failed: %s", esp_err_to_name(err));
        return;
    }
    if (!clock_time::init(board::i2c_bus())) {
        ESP_LOGE("clock", "Time service initialization failed");
        return;
    }
    if (const auto err = nvs_flash_init(); err == ESP_OK) {
        network_init();
    } else {
        ESP_LOGE("clock", "NVS initialization failed: %s", esp_err_to_name(err));
    }
    clock_ui::init();
    lv_timer_create([](lv_timer_t *) { clock_ui::refresh(); }, 1000, nullptr);
    while (true) {
        const uint32_t delay_ms = std::clamp<uint32_t>(lv_timer_handler(), 5, 50);
        vTaskDelay(std::max<TickType_t>(1, pdMS_TO_TICKS(delay_ms)));
    }
}
