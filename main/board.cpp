#include "board.hpp"
#include "st7701_4b.hpp"

#include <cstdint>

#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_rgb.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace board {
namespace {

constexpr char kTag[] = "board";
constexpr auto kBacklight = GPIO_NUM_4;
// BL 经 R37/R29 注入 AP3032 的 FB，而不是驱动 CTRL。拉高会压低 LED 电流。
constexpr int kBacklightOff = 1;
constexpr int kBacklightOn = 0;
constexpr int kPclkHz = 12000000;
constexpr int kBounceLines = 20;
constexpr int kI2cTimeoutMs = 50;

i2c_master_bus_handle_t s_bus = nullptr;

// TCA9554：P0=LCD CS，P1=LCD SDA，P2=LCD SCL，P7=LCD RST。
// 不更改 P3～P6 的输出锁存值或方向（包括触摸的 RST/INT）。
constexpr uint8_t kCs = 1u << 0;
constexpr uint8_t kData = 1u << 1;
constexpr uint8_t kClock = 1u << 2;
constexpr uint8_t kReset = 1u << 7;
constexpr uint8_t kLcdMask = kCs | kData | kClock | kReset;
constexpr uint8_t kOutputPort = 0x01;
constexpr uint8_t kDirectionPort = 0x03;

void delay_ms(uint32_t ms)
{
    vTaskDelay(pdMS_TO_TICKS(ms) + 1);
}

class PanelIo {
public:
    explicit PanelIo(i2c_master_dev_handle_t device) : device_(device) {}

    esp_err_t init_panel()
    {
        uint8_t direction;
        ESP_RETURN_ON_ERROR(read(kOutputPort, out_), kTag, "read expander output latch");
        ESP_RETURN_ON_ERROR(read(kDirectionPort, direction), kTag, "read expander direction");
        // 先写输出锁存，再使能 LCD 引脚输出：CS/CLK/SDA 高，RST 拉低。
        out_ = static_cast<uint8_t>((out_ & ~kLcdMask) | kCs | kClock | kData);
        ESP_RETURN_ON_ERROR(write(kOutputPort, out_), kTag, "preload LCD outputs");
        ESP_RETURN_ON_ERROR(write(kDirectionPort, static_cast<uint8_t>(direction & ~kLcdMask)),
                            kTag, "enable LCD outputs");
        delay_ms(10);
        ESP_RETURN_ON_ERROR(set(kReset, true), kTag, "release LCD reset");
        delay_ms(100);

        // 同官方示例：硬复位后软复位，随后整组发送寄存器和 Sleep Out。
        ESP_RETURN_ON_ERROR(send_group(&kSoftwareReset, 1), kTag, "ST7701 software reset");
        delay_ms(120);
        const auto& table = st7701_4b::before_sleep_out;
        ESP_RETURN_ON_ERROR(send_group(table, sizeof(table) / sizeof(table[0])),
                            kTag, "ST7701 setup / sleep out");
        delay_ms(120);
        return send_group(&kDisplayOn, 1);
    }

private:
    static constexpr st7701_4b::Command kSoftwareReset{0x01};
    static constexpr st7701_4b::Command kDisplayOn{0x29};

    esp_err_t read(uint8_t reg, uint8_t& value)
    {
        return i2c_master_transmit_receive(device_, &reg, 1, &value, 1, kI2cTimeoutMs);
    }

    esp_err_t write(uint8_t reg, uint8_t value)
    {
        const uint8_t msg[] = {reg, value};
        return i2c_master_transmit(device_, msg, sizeof(msg), kI2cTimeoutMs);
    }

    esp_err_t set(uint8_t mask, bool high)
    {
        const auto next = static_cast<uint8_t>(high ? out_ | mask : out_ & ~mask);
        if (next == out_) return ESP_OK;
        const auto err = write(kOutputPort, next);
        if (err == ESP_OK) out_ = next;
        return err;
    }

    esp_err_t send_word(bool data, uint8_t value)
    {
        // 3 线串行控制：D/C + 8 位数据，MSB 先出，SCL 上升沿送入面板。
        const unsigned word = (data ? 0x100u : 0u) | value;
        for (unsigned bit = 0x100u; bit != 0; bit >>= 1) {
            if (const auto err = set(kData, (word & bit) != 0); err != ESP_OK) return err;
            if (const auto err = set(kClock, false); err != ESP_OK) return err;
            if (const auto err = set(kClock, true); err != ESP_OK) return err;
        }
        return ESP_OK;
    }

    esp_err_t send_group(const st7701_4b::Command* commands, size_t count)
    {
        auto err = set(kCs, false);
        for (size_t i = 0; i < count && err == ESP_OK; ++i) {
            const auto& cmd = commands[i];
            err = send_word(false, cmd.reg);
            for (size_t p = 0; p < cmd.len && err == ESP_OK; ++p) {
                err = send_word(true, cmd.data[p]);
            }
        }
        const auto deselect = set(kCs, true);
        return err == ESP_OK ? deselect : err;
    }

    i2c_master_dev_handle_t device_;
    uint8_t out_ = 0;
};

esp_err_t init_panel_commands(i2c_master_bus_handle_t bus)
{
    i2c_device_config_t config{};
    config.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    config.device_address = 0x20;
    config.scl_speed_hz = 400000;
    i2c_master_dev_handle_t device = nullptr;
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(bus, &config, &device), kTag, "attach TCA9554");
    const auto err = PanelIo(device).init_panel();
    const auto cleanup = i2c_master_bus_rm_device(device);
    return err == ESP_OK ? cleanup : err;
}

esp_err_t new_rgb_panel(esp_lcd_panel_handle_t* panel)
{
    esp_lcd_rgb_panel_config_t config{};
    config.clk_src = LCD_CLK_SRC_DEFAULT;
    config.timings.pclk_hz = kPclkHz;
    config.timings.h_res = kWidth;
    config.timings.v_res = kHeight;
    config.timings.hsync_front_porch = 10;
    config.timings.hsync_pulse_width = 8;
    config.timings.hsync_back_porch = 50;
    config.timings.vsync_front_porch = 10;
    config.timings.vsync_pulse_width = 8;
    config.timings.vsync_back_porch = 20;
    // 沿用随包 Arduino RGB 驱动：HS/VS 空闲高，DE/PCLK 空闲低，pclk_active_neg=0。
    config.data_width = 16;
    config.in_color_format = LCD_COLOR_FMT_RGB565;
    config.out_color_format = LCD_COLOR_FMT_RGB565;
    config.num_fbs = 1;
    config.bounce_buffer_size_px = kWidth * kBounceLines;
    config.hsync_gpio_num = GPIO_NUM_46;
    config.vsync_gpio_num = GPIO_NUM_3;
    config.de_gpio_num = GPIO_NUM_17;
    config.pclk_gpio_num = GPIO_NUM_9;
    config.disp_gpio_num = GPIO_NUM_NC;
    // ESP-IDF 并行数据位 D[0..15]，不是 J2 的 D0..D17 引脚编号。
    // 官方示例顺序恰好是 J2 的 R1..R5、G0..G5、B1..B5；R0/B0 均不接 ESP32。
    constexpr gpio_num_t data_pins[] = {
        GPIO_NUM_10, GPIO_NUM_11, GPIO_NUM_12, GPIO_NUM_13, GPIO_NUM_14,
        GPIO_NUM_21, GPIO_NUM_8, GPIO_NUM_18, GPIO_NUM_45, GPIO_NUM_38, GPIO_NUM_39,
        GPIO_NUM_40, GPIO_NUM_41, GPIO_NUM_42, GPIO_NUM_2, GPIO_NUM_1,
    };
    static_assert(sizeof(data_pins) / sizeof(data_pins[0]) == 16);
    for (size_t i = 0; i < 16; ++i) config.data_gpio_nums[i] = data_pins[i];
    config.flags.fb_in_psram = true;
    return esp_lcd_new_rgb_panel(&config, panel);
}

Lcd s_lcd;

}  // namespace

esp_err_t init_lcd(Lcd& lcd)
{
    if (s_bus) {
        lcd = s_lcd;
        return ESP_OK;
    }

    gpio_config_t bl{};
    bl.pin_bit_mask = 1ULL << kBacklight;
    bl.mode = GPIO_MODE_OUTPUT;
    ESP_RETURN_ON_ERROR(gpio_set_level(kBacklight, kBacklightOff), kTag, "set backlight off");
    ESP_RETURN_ON_ERROR(gpio_config(&bl), kTag, "configure backlight GPIO");

    i2c_master_bus_config_t i2c{};
    i2c.i2c_port = I2C_NUM_1;
    i2c.sda_io_num = GPIO_NUM_47;
    i2c.scl_io_num = GPIO_NUM_48;
    i2c.clk_source = I2C_CLK_SRC_DEFAULT;
    i2c.glitch_ignore_cnt = 7;
    // 原理图有外部上拉；LCD 扩展器和 RTC 复用同一个总线句柄。
    i2c_master_bus_handle_t bus = nullptr;
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&i2c, &bus), kTag, "start shared I2C");

    esp_lcd_panel_handle_t panel = nullptr;
    void* framebuffer = nullptr;
    esp_err_t err = init_panel_commands(bus);
    if (err == ESP_OK) err = new_rgb_panel(&panel);
    if (err == ESP_OK) err = esp_lcd_panel_reset(panel);
    if (err == ESP_OK) err = esp_lcd_panel_init(panel);
    if (err == ESP_OK) err = esp_lcd_rgb_panel_get_frame_buffer(panel, 1, &framebuffer);
    if (err == ESP_OK) err = gpio_set_level(kBacklight, kBacklightOn);
    if (err != ESP_OK) {
        gpio_set_level(kBacklight, kBacklightOff);
        if (panel) esp_lcd_panel_del(panel);
        i2c_del_master_bus(bus);
        return err;
    }

    s_lcd = {panel, static_cast<uint16_t*>(framebuffer)};
    lcd = s_lcd;
    s_bus = bus;
    ESP_LOGI(kTag, "4B LCD initialized: %dx%d, 16 data lines, PCLK target %d Hz", kWidth, kHeight, kPclkHz);
    return ESP_OK;
}

i2c_master_bus_handle_t i2c_bus()
{
    return s_bus;
}

}  // namespace board
