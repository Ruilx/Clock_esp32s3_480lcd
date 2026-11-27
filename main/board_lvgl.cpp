#include "board.hpp"

#include "esp_lcd_panel_ops.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "lvgl.h"

namespace board {
namespace {

constexpr char kTag[] = "board_lvgl";
constexpr lv_display_rotation_t kScreenRotation = LV_DISPLAY_ROTATION_90;
uint16_t* s_framebuffer = nullptr;

uint32_t tick_ms()
{
    return static_cast<uint32_t>(esp_timer_get_time() / 1000);
}

void flush(lv_display_t* display, const lv_area_t* area, uint8_t* pixels)
{
    auto panel = static_cast<esp_lcd_panel_handle_t>(lv_display_get_user_data(display));
    const lv_display_rotation_t rotation = lv_display_get_rotation(display);
    if (rotation == LV_DISPLAY_ROTATION_0) {
        const auto err = esp_lcd_panel_draw_bitmap(panel, area->x1, area->y1,
                                                   area->x2 + 1, area->y2 + 1, pixels);
        if (err != ESP_OK) ESP_LOGE(kTag, "RGB flush: %s", esp_err_to_name(err));
        lv_display_flush_ready(display);
        return;
    }

    lv_area_t rotated_area = *area;
    lv_display_rotate_area(display, &rotated_area);

    const int32_t width = lv_area_get_width(area);
    const int32_t height = lv_area_get_height(area);
    const uint32_t source_stride = lv_draw_buf_width_to_stride(width, LV_COLOR_FORMAT_RGB565);
    constexpr uint32_t framebuffer_stride = kWidth * sizeof(uint16_t);
    uint16_t* framebuffer = s_framebuffer + rotated_area.y1 * kWidth + rotated_area.x1;
    lv_draw_sw_rotate(pixels, framebuffer, width, height, source_stride, framebuffer_stride,
                      rotation, LV_COLOR_FORMAT_RGB565);

    const auto err = esp_lcd_panel_draw_bitmap(panel, rotated_area.x1, rotated_area.y1,
                                               rotated_area.x2 + 1, rotated_area.y2 + 1, framebuffer);
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
    s_framebuffer = lcd.pixels;
    lv_display_set_user_data(display, lcd.panel);
    lv_display_set_flush_cb(display, flush);
    lv_display_set_rotation(display, kScreenRotation);
    return ESP_OK;
}

}  // namespace board
