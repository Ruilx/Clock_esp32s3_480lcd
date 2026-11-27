#pragma once

#include <cstdint>

namespace host_status {

struct Snapshot {
    uint32_t sequence = 0;
    uint32_t cpu_percent = 0;
    uint32_t memory_used_mb = 0;
    uint32_t memory_total_mb = 0;
    uint32_t gpu_percent = 0;
    uint32_t gpu_memory_used_mb = 0;
    uint32_t gpu_memory_total_mb = 0;
    uint32_t network_rx_kbps = 0;
    uint32_t network_tx_kbps = 0;
    uint32_t available_flags = 0;
};

constexpr uint32_t kAvailableCpu = 1u << 0;
constexpr uint32_t kAvailableMemory = 1u << 1;
constexpr uint32_t kAvailableGpu = 1u << 2;
constexpr uint32_t kAvailableGpuMemory = 1u << 3;
constexpr uint32_t kAvailableNetwork = 1u << 4;

bool init();
bool valid();
Snapshot snapshot();
int64_t last_received_us();

}  // namespace host_status
