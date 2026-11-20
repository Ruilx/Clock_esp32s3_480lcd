#include "board.hpp"

#include "esp_lcd_panel_ops.h"
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
    // DIRECT 模式已写入同一帧缓冲。返回不表示 DMA 已停止扫描该帧。
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
    lv_display_set_color_format(display, LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers_with_stride(display, lcd.pixels, nullptr, stride * kHeight, stride,
                                       LV_DISPLAY_RENDER_MODE_DIRECT);
    lv_display_set_user_data(display, lcd.panel);
    lv_display_set_flush_cb(display, flush);
    return ESP_OK;
}

}  // namespace board
