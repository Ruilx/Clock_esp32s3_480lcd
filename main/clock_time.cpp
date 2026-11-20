#include "clock_time.hpp"

#include <atomic>
#include <cstdint>
#include <sys/time.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_sntp.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

void network_sntp_result();

namespace {

using clock_time::Source;

constexpr char kTag[] = "clock_time";
constexpr char kNtpServer[] = "pool.ntp.org";
constexpr int64_t kUsPerSecond = 1000000;
constexpr int64_t kRtcIntervalUs = 3600 * kUsPerSecond;
constexpr int64_t kRecentSntpUs = 3600 * kUsPerSecond;
constexpr int kI2cTimeoutMs = 50;

// NXP PCF85063A datasheet: A2h/A3h encode 0x51 in the 7-bit IDF API.
// The seven calendar registers at 04h..0Ah auto-increment in one transfer.
constexpr uint8_t kRtcAddress = 0x51;
constexpr uint8_t kControl1 = 0x00;
constexpr uint8_t kSeconds = 0x04;
constexpr uint8_t kOscillatorStopped = 0x80;  // Seconds.OS, latched
constexpr uint8_t kExternalTest = 0x80;       // Control_1.EXT_TEST
constexpr uint8_t kStop = 0x20;               // Control_1.STOP, divider hold
constexpr uint8_t kTwelveHour = 0x02;         // Control_1.12_24
constexpr uint8_t kPreservedControl = 0x05;   // Control_1.CIE and CAP_SEL

// No century bit: 00..99 denotes 2000..2099. Reject pre-2020 values as
// implausible (notably plausible-looking power-on/default calendar dates).
constexpr int kCentury = 2000;
constexpr int kMinYear = 2020;
constexpr int kEndYear = 2100;

constexpr bool leap(int year)
{
    return year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
}

constexpr int days_in_month(int year, int month)
{
    constexpr int days[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    return days[month - 1] + (month == 2 && leap(year));
}

// Convert in UTC regardless of the UI's TZ setting.
constexpr int64_t unix_seconds(int year, int month, int day, int hour, int minute, int second)
{
    int64_t days = day - 1;
    for (int y = 1970; y < year; ++y) {
        days += 365 + leap(y);
    }
    for (int m = 1; m < month; ++m) {
        days += days_in_month(year, m);
    }
    return ((days * 24 + hour) * 60 + minute) * 60 + second;
}

constexpr int64_t kMinUtc = unix_seconds(kMinYear, 1, 1, 0, 0, 0);
constexpr int64_t kEndUtc = unix_seconds(kEndYear, 1, 1, 0, 0, 0);
static_assert(sizeof(std::time_t) >= 8, "ESP-IDF 64-bit time_t required");
static_assert(kMinUtc == 1577836800LL && kEndUtc == 4102444800LL, "UTC bounds");

bool sane_utc(std::time_t utc)
{
    return utc >= kMinUtc && utc < kEndUtc;
}

int decode_bcd(uint8_t raw, uint8_t mask, int min, int max)
{
    const int bcd = raw & mask;
    const int ones = bcd & 0x0f;
    const int tens = bcd >> 4;
    const int value = 10 * tens + ones;
    return ones <= 9 && tens <= 9 && value >= min && value <= max ? value : -1;
}

uint8_t bcd(int value)
{
    return static_cast<uint8_t>(((value / 10) << 4) | (value % 10));
}

struct TimeUpdate {
    timeval time;
    int64_t received_us;
    uint32_t network_state;  // 0 for manual; an online generation for SNTP
};

std::atomic<Source> s_source{Source::Invalid};
std::atomic<bool> s_has_sntp{false};
std::atomic<TaskHandle_t> s_worker{nullptr};
std::atomic<uint32_t> s_network{0};  // Low bit: online. Upper bits: transition generation.
QueueHandle_t s_updates = nullptr;
i2c_master_dev_handle_t s_rtc = nullptr;

bool online(uint32_t state)
{
    return (state & 1u) != 0;
}

bool read_registers(uint8_t reg, uint8_t* data, size_t size)
{
    const esp_err_t err = i2c_master_transmit_receive(s_rtc, &reg, 1, data, size, kI2cTimeoutMs);
    if (err != ESP_OK) {
        ESP_LOGW(kTag, "RTC read at 0x%02x failed: %s", reg, esp_err_to_name(err));
    }
    return err == ESP_OK;
}

bool write_registers(const uint8_t* data, size_t size)
{
    const esp_err_t err = i2c_master_transmit(s_rtc, data, size, kI2cTimeoutMs);
    if (err != ESP_OK) {
        ESP_LOGW(kTag, "RTC write at 0x%02x failed: %s", data[0], esp_err_to_name(err));
    }
    return err == ESP_OK;
}

bool read_rtc(std::time_t& utc)
{
    uint8_t control;
    uint8_t data[7];
    if (!s_rtc || !read_registers(kControl1, &control, 1) ||
        !read_registers(kSeconds, data, sizeof(data))) {
        return false;
    }
    if ((data[0] & kOscillatorStopped) || (control & (kStop | kExternalTest))) {
        ESP_LOGW(kTag, "RTC oscillator/counter is not valid");
        return false;
    }

    const int second = decode_bcd(data[0], 0x7f, 0, 59);
    const int minute = decode_bcd(data[1], 0x7f, 0, 59);
    int hour;
    if (control & kTwelveHour) {
        hour = decode_bcd(data[2], 0x1f, 1, 12);
        if (hour >= 0) {
            hour = hour % 12 + ((data[2] & 0x20) ? 12 : 0);
        }
    } else {
        hour = decode_bcd(data[2], 0x3f, 0, 23);
    }
    const int day = decode_bcd(data[3], 0x3f, 1, 31);
    const int weekday = data[4] & 0x07;
    const int month = decode_bcd(data[5], 0x1f, 1, 12);
    const int year = kCentury + decode_bcd(data[6], 0xff, 0, 99);
    if (second < 0 || minute < 0 || hour < 0 || day < 0 || weekday > 6 ||
        month < 0 || year < kMinYear || day > days_in_month(year, month)) {
        ESP_LOGW(kTag, "RTC BCD/calendar is not valid");
        return false;
    }
    utc = static_cast<std::time_t>(unix_seconds(year, month, day, hour, minute, second));
    return true;
}

void persist_rtc()
{
    uint8_t control;
    if (!s_rtc || !read_registers(kControl1, &control, 1)) {
        return;
    }
    const std::time_t utc = std::time(nullptr);
    std::tm tm{};
    if (!sane_utc(utc) || !gmtime_r(&utc, &tm)) {
        ESP_LOGW(kTag, "Current UTC cannot be written to RTC");
        return;
    }
    // Preserve board-specific crystal load and interrupt config. Select
    // 24-hour mode and hold the divider while writing all seven registers.
    const uint8_t stopped[] = {kControl1, static_cast<uint8_t>((control & kPreservedControl) | kStop)};
    const uint8_t calendar[] = {kSeconds, bcd(tm.tm_sec), bcd(tm.tm_min), bcd(tm.tm_hour),
                                bcd(tm.tm_mday), static_cast<uint8_t>(tm.tm_wday),
                                bcd(tm.tm_mon + 1), bcd(tm.tm_year + 1900 - kCentury)};
    if (!write_registers(stopped, sizeof(stopped)) || !write_registers(calendar, sizeof(calendar))) {
        // A partial calendar write may have cleared OS. Leave STOP set so
        // future reads cannot trust the incomplete data.
        return;
    }
    const uint8_t running[] = {kControl1, static_cast<uint8_t>(control & kPreservedControl)};
    write_registers(running, sizeof(running));
}

bool set_clock(const timeval& tv, Source source)
{
    if (settimeofday(&tv, nullptr) != 0) {
        ESP_LOGW(kTag, "settimeofday failed");
        return false;
    }
    s_source.store(source);
    return true;
}

void compare_offline_rtc(uint32_t expected_state)
{
    std::time_t rtc;
    if (!read_rtc(rtc) || s_network.load() != expected_state) {
        return;
    }
    if (s_source.load() == Source::Invalid) {
        if (set_clock(timeval{rtc, 0}, Source::Rtc)) {
            ESP_LOGI(kTag, "System clock restored from RTC");
        }
        return;
    }
    const auto delta = rtc - std::time(nullptr);
    // RTC has one-second resolution. Ignore small offsets; for other plausible
    // offsets adjust the rate, not the displayed time, even if the ESP is fast.
    if (delta >= -2 && delta <= 2) return;
    if (delta < -300 || delta > 300) {
        ESP_LOGW(kTag, "RTC differs by %lld s; waiting for an authoritative time source",
                 static_cast<long long>(delta));
        return;
    }
    const timeval correction{delta, 0};
    if (adjtime(&correction, nullptr) == 0) {
        s_source.store(Source::Rtc);
        ESP_LOGI(kTag, "Slewing system clock toward RTC (%lld s)", static_cast<long long>(delta));
    } else {
        ESP_LOGW(kTag, "Could not slew system clock toward RTC");
    }
}

bool enqueue(const TimeUpdate& update)
{
    const auto worker = s_worker.load();
    if (!worker || xQueueSend(s_updates, &update, 0) != pdTRUE) {
        return false;
    }
    xTaskNotifyGive(worker);
    return true;
}

void handle_update(const TimeUpdate& update, int64_t& last_sntp_us)
{
    const bool from_sntp = online(update.network_state);
    if (from_sntp ? s_network.load() != update.network_state : s_has_sntp.load()) {
        return;
    }
    timeval tv = update.time;
    // Account for worker dispatch latency using a clock we never step.
    const int64_t elapsed = esp_timer_get_time() - update.received_us + tv.tv_usec;
    tv.tv_sec += elapsed / kUsPerSecond;
    tv.tv_usec = elapsed % kUsPerSecond;
    if (!sane_utc(tv.tv_sec) || !set_clock(tv, from_sntp ? Source::Sntp : Source::Manual)) {
        return;
    }
    if (from_sntp) {
        last_sntp_us = update.received_us;
        s_has_sntp.store(true);
        esp_sntp_set_sync_status(SNTP_SYNC_STATUS_COMPLETED);
        ESP_LOGI(kTag, "SNTP synchronized UTC (%lld)", static_cast<long long>(tv.tv_sec));
    } else {
        ESP_LOGI(kTag, "Provisioning UTC applied (%lld)", static_cast<long long>(tv.tv_sec));
    }
    persist_rtc();
    if (from_sntp) {
        network_sntp_result();
    }
}

void worker_main(void*)
{
    int64_t next_rtc_check = esp_timer_get_time() + kRtcIntervalUs;
    int64_t last_sntp_us = -1;
    uint32_t handled_state = 0;
    bool sntp_initialized = false;
    for (;;) {
        const uint32_t state = s_network.load();
        if (state != handled_state) {
            handled_state = state;
            if (online(state)) {
                esp_err_t err;
                if (sntp_initialized) {
                    // Restart immediately after reconnect rather than wait
                    // for the next poll. On disconnect keep the poller
                    // allocated: lwIP may have a pending hostname lookup.
                    err = esp_netif_sntp_start();
                } else {
                    esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG(kNtpServer);
                    cfg.wait_for_sync = false;
                    esp_sntp_set_sync_mode(SNTP_SYNC_MODE_IMMED);
                    err = esp_netif_sntp_init(&cfg);
                    sntp_initialized = err == ESP_OK;
                }
                if (err != ESP_OK) {
                    ESP_LOGW(kTag, "SNTP start failed: %s", esp_err_to_name(err));
                }
            }
        }

        TimeUpdate update;
        if (xQueueReceive(s_updates, &update, 0) == pdTRUE) {
            handle_update(update, last_sntp_us);
            continue;
        }

        TickType_t wait_ticks = portMAX_DELAY;
        // A STA address does not imply that SNTP is reachable.
        if (s_rtc) {
            const int64_t now = esp_timer_get_time();
            const int64_t fresh_until = last_sntp_us < 0 ? 0 : last_sntp_us + kRecentSntpUs;
            const int64_t due = next_rtc_check > fresh_until ? next_rtc_check : fresh_until;
            if (due <= now) {
                next_rtc_check = now + kRtcIntervalUs;
                compare_offline_rtc(state);
                continue;
            }
            wait_ticks = pdMS_TO_TICKS((due - now + 999) / 1000);
            if (wait_ticks == 0) {
                wait_ticks = 1;
            }
        }
        ulTaskNotifyTake(pdTRUE, wait_ticks);
    }
}

}  // namespace

// ESP-IDF documents this weak hook as the system-clock update point. Use it
// to serialize immediate SNTP steps with the manual and RTC steps and reject
// offline or implausible samples before they can affect the clock. The lwIP
// callback never waits on the worker or touches I2C.
extern "C" void sntp_sync_time(timeval* tv)
{
    const auto state = s_network.load();
    if (online(state) && tv && sane_utc(tv->tv_sec) && tv->tv_usec >= 0 && tv->tv_usec < kUsPerSecond) {
        enqueue(TimeUpdate{*tv, esp_timer_get_time(), state});
    }
}

namespace clock_time {

bool init(i2c_master_bus_handle_t bus)
{
    if (s_worker.load()) {
        return true;
    }
    if (bus) {
        i2c_device_config_t cfg{};
        cfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
        cfg.device_address = kRtcAddress;
        cfg.scl_speed_hz = 100000;
        const auto err = i2c_master_bus_add_device(bus, &cfg, &s_rtc);
        if (err != ESP_OK) {
            s_rtc = nullptr;
            ESP_LOGW(kTag, "Unable to attach RTC to BSP I2C: %s", esp_err_to_name(err));
        }
    }
    std::time_t rtc;
    if (read_rtc(rtc) && set_clock(timeval{rtc, 0}, Source::Rtc)) {
        ESP_LOGI(kTag, "System clock restored from boot RTC");
    }

    s_updates = xQueueCreate(8, sizeof(TimeUpdate));
    TaskHandle_t worker;
    if (!s_updates || xTaskCreate(worker_main, "clock_time", 4096, nullptr, 4, &worker) != pdPASS) {
        ESP_LOGE(kTag, "Unable to create clock service worker");
        if (s_updates) {
            vQueueDelete(s_updates);
            s_updates = nullptr;
        }
        if (s_rtc) {
            i2c_master_bus_rm_device(s_rtc);
            s_rtc = nullptr;
        }
        return false;
    }
    s_worker.store(worker);
    return true;
}

bool submit_manual_utc(std::time_t utc)
{
    return sane_utc(utc) && !s_has_sntp.load() &&
           enqueue(TimeUpdate{timeval{utc, 0}, esp_timer_get_time(), 0});
}

void notify_network(bool is_online)
{
    auto old = s_network.load();
    uint32_t state;
    do {
        state = ((old & ~1u) + 2u) | static_cast<uint32_t>(is_online);
    } while (!s_network.compare_exchange_weak(old, state));
    if (const auto worker = s_worker.load()) {
        xTaskNotifyGive(worker);
    }
}

Source source()
{
    return s_source.load();
}

bool valid()
{
    return source() != Source::Invalid;
}

}  // namespace clock_time
