#pragma once

#include <cstdint>

#include "driver/i2c_master.h"
#include "esp_err.h"
#include "esp_lcd_types.h"

namespace board {
inline constexpr int kWidth = 480;
inline constexpr int kHeight = 480;

struct Lcd {
    esp_lcd_panel_handle_t panel = nullptr;
    uint16_t* pixels = nullptr;  // 驱动持有的 RGB565 帧缓冲，按行紧密排列。
};

// 底层点屏，不依赖 LVGL；仅由启动任务调用。
esp_err_t init_lcd(Lcd& lcd);
// 时钟应用的 LVGL 接入；只有 app_main 调用 lv_timer_handler()。
esp_err_t init_display();
i2c_master_bus_handle_t i2c_bus();
}
