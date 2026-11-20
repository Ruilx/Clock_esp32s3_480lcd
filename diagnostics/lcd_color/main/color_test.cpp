#include "board.hpp"

#include "esp_lcd_panel_ops.h"
#include "esp_log.h"

namespace {
constexpr char kTag[] = "lcd_color";
constexpr uint16_t kBars[] = {0xF800, 0x07E0, 0x001F, 0xFFFF};
constexpr int kBarsHeight = board::kHeight * 2 / 3;
constexpr int kBarWidth = board::kWidth / 4;
constexpr int kSteps = 8;
static_assert(board::kWidth % 4 == 0 && board::kWidth % kSteps == 0);

constexpr uint16_t gray565(unsigned gray)
{
    const auto rb = gray >> 3;
    return static_cast<uint16_t>((rb << 11) | ((gray >> 2) << 5) | rb);
}
static_assert(gray565(0) == 0x0000 && gray565(255) == 0xFFFF);
}  // namespace

extern "C" void app_main()
{
    board::Lcd lcd;
    if (const auto err = board::init_lcd(lcd); err != ESP_OK) {
        ESP_LOGE(kTag, "LCD init failed: %s", esp_err_to_name(err));
        return;
    }

    // RGB565 原始像素，独立验证面板及上游位序，不经过 LVGL。
    for (int y = 0; y < board::kHeight; ++y) {
        for (int x = 0; x < board::kWidth; ++x) {
            const auto step = x / (board::kWidth / kSteps);
            lcd.pixels[y * board::kWidth + x] =
                y < kBarsHeight ? kBars[x / kBarWidth] : gray565(step * 255 / (kSteps - 1));
        }
    }
    const auto err = esp_lcd_panel_draw_bitmap(lcd.panel, 0, 0, board::kWidth, board::kHeight, lcd.pixels);
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "LCD draw failed: %s", esp_err_to_name(err));
        return;
    }
    ESP_LOGI(kTag, "Expected left to right: RED GREEN BLUE WHITE; bottom: BLACK to WHITE (8 steps)");
}
