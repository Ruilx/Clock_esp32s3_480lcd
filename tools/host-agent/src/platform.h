#pragma once

#include <cstdint>
#include <string>
#include <vector>

struct HostSnapshot {
    uint32_t cpu_percent = 0;
    uint64_t memory_used_bytes = 0;
    uint64_t memory_total_bytes = 0;
    uint32_t gpu_percent = 0;
    uint64_t gpu_memory_used_bytes = 0;
    uint64_t gpu_memory_total_bytes = 0;
    uint64_t network_rx_bytes_per_second = 0;
    uint64_t network_tx_bytes_per_second = 0;
    uint32_t available_flags = 0;
};

struct GPUDevice {
    uint32_t id = 0;
    std::string name;
};

constexpr uint32_t kAvailableCpu = 1u << 0;
constexpr uint32_t kAvailableMemory = 1u << 1;
constexpr uint32_t kAvailableGpu = 1u << 2;
constexpr uint32_t kAvailableGpuMemory = 1u << 3;
constexpr uint32_t kAvailableNetwork = 1u << 4;

bool platform_init();
bool platform_sample(HostSnapshot& snapshot);
std::vector<GPUDevice> enumerate_gpus();
bool select_gpu(uint32_t id);
std::string platform_hostname();
