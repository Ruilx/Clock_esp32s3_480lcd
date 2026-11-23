#include "board.hpp"

#include "esp_lcd_panel_ops.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "lvgl.h"

namespace board {
namespace {

constexpr char kTag[] = "board_lvgl";

uint32_t tick_ms()
{
    return static_cast<uint32_t>(esp_timer_get_time() / 1000);
}

void flush(lv_display_t* display, const lv_area_t* area, uint8_t* pixels)
{
    auto panel = static_cast<esp_lcd_panel_handle_t>(lv_display_get_user_data(display));
    // PARTIAL 缓冲只复制 LVGL 的脏区到 RGB 面板帧缓冲。
    const auto err = esp_lcd_panel_draw_bitmap(panel, area->x1, area->y1,
                                               area->x2 + 1, area->y2 + 1, pixels);
    if (err != ESP_OK) ESP_LOGE(kTag, "RGB flush: %s", esp_err_to_name(err));
    lv_display_flush_ready(display);
}

}  // namespace

esp_err_t init_display()
{
    lv_init();
    lv_tick_set_cb(tick_ms);
    auto* display = lv_display_create(kWidth, kHeight);
    if (!display) return ESP_ERR_NO_MEM;

    Lcd lcd;
    const auto err = init_lcd(lcd);
    if (err != ESP_OK) {
        lv_display_delete(display);
        return err;
    }

    constexpr uint32_t stride = kWidth * sizeof(uint16_t);
    constexpr uint32_t buffer_lines = 40;
    const uint32_t buffer_size = stride * buffer_lines;
    auto* buffer = static_cast<uint8_t*>(
        heap_caps_aligned_alloc(64, buffer_size, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA));
    if (!buffer) return ESP_ERR_NO_MEM;

    lv_display_set_color_format(display, LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers_with_stride(display, buffer, nullptr, buffer_size, stride,
                                       LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_user_data(display, lcd.panel);
    lv_display_set_flush_cb(display, flush);
    return ESP_OK;
}

}  // namespace board
