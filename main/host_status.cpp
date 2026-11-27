#include "host_status.hpp"

#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstring>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"

namespace host_status {
namespace {

constexpr char kTag[] = "host_status";
constexpr uint16_t kPort = 9461;
constexpr int64_t kUsPerSecond = 1000000;
constexpr int64_t kValidityUs = 10 * kUsPerSecond;
constexpr uint32_t kPacketSize = 80;
constexpr uint32_t kMagic = 0x48535431u;  // "HST1"
constexpr uint32_t kVersion = 1;

std::atomic<bool> s_initialized{false};
std::atomic<uint32_t> s_sequence{0};
std::atomic<int64_t> s_last_received_us{-1};
Snapshot s_snapshot;
portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;

uint32_t read_u32(const uint8_t* data)
{
    return (static_cast<uint32_t>(data[0]) << 24) |
           (static_cast<uint32_t>(data[1]) << 16) |
           (static_cast<uint32_t>(data[2]) << 8) |
           static_cast<uint32_t>(data[3]);
}

void receiver_task(void*)
{
    const int fd = lwip_socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (fd < 0) {
        ESP_LOGE(kTag, "UDP socket could not be created");
        s_initialized.store(false);
        return;
    }

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = lwip_htons(kPort);
    address.sin_addr.s_addr = lwip_htonl(LWIP_MAKEU32(0, 0, 0, 0));
    if (lwip_bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) < 0) {
        ESP_LOGE(kTag, "UDP port %u could not be bound", kPort);
        lwip_close(fd);
        s_initialized.store(false);
        return;
    }

    uint8_t packet[kPacketSize];
    for (;;) {
        const int length = lwip_recv(fd, packet, sizeof(packet), 0);
        if (length < 0) continue;
        if (length != static_cast<int>(kPacketSize)) continue;
        if (read_u32(packet) != kMagic) continue;
        if (read_u32(packet + 4) != kVersion) continue;

        Snapshot next{};
        next.sequence = read_u32(packet + 12);
        next.cpu_percent = read_u32(packet + 20);
        next.memory_used_mb = read_u32(packet + 24);
        next.memory_total_mb = read_u32(packet + 28);
        next.gpu_percent = read_u32(packet + 32);
        next.gpu_memory_used_mb = read_u32(packet + 36);
        next.gpu_memory_total_mb = read_u32(packet + 40);
        next.network_rx_kbps = read_u32(packet + 44);
        next.network_tx_kbps = read_u32(packet + 48);
        next.available_flags = read_u32(packet + 8);

        if (next.cpu_percent > 100 || next.gpu_percent > 100) continue;
        if (next.memory_total_mb > 0 && next.memory_used_mb > next.memory_total_mb) continue;
        if (next.gpu_memory_total_mb > 0 &&
            next.gpu_memory_used_mb > next.gpu_memory_total_mb) continue;

        const int64_t now = esp_timer_get_time();
        portENTER_CRITICAL(&s_lock);
        s_snapshot = next;
        portEXIT_CRITICAL(&s_lock);
        s_sequence.store(next.sequence);
        s_last_received_us.store(now);
        ESP_LOGD(kTag, "CPU=%lu%% MEM=%lu/%lu MB GPU=%lu%% VRAM=%lu/%lu MB",
                 static_cast<unsigned long>(next.cpu_percent),
                 static_cast<unsigned long>(next.memory_used_mb),
                 static_cast<unsigned long>(next.memory_total_mb),
                 static_cast<unsigned long>(next.gpu_percent),
                 static_cast<unsigned long>(next.gpu_memory_used_mb),
                 static_cast<unsigned long>(next.gpu_memory_total_mb));
    }
}

}  // namespace

bool init()
{
    if (s_initialized.load()) return true;
    const bool created = xTaskCreate(receiver_task, "host_status", 8192, nullptr, 4, nullptr) == pdPASS;
    s_initialized.store(created);
    return created;
}

bool valid()
{
    const int64_t last = s_last_received_us.load();
    if (last < 0) return false;
    return esp_timer_get_time() - last < kValidityUs;
}

Snapshot snapshot()
{
    Snapshot result{};
    portENTER_CRITICAL(&s_lock);
    result = s_snapshot;
    portEXIT_CRITICAL(&s_lock);
    return result;
}

int64_t last_received_us()
{
    return s_last_received_us.load();
}

}  // namespace host_status
