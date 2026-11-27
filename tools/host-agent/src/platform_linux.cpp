#ifdef __linux__

#include "platform.h"

#include <ifstream>
#include <filesystem>
#include <chrono>
#include <sstream>
#include <unordered_map>
#include <unistd.h>

namespace {

uint64_t s_cpu_total_before = 0;
uint64_t s_cpu_idle_before = 0;
uint64_t s_rx_before = 0;
uint64_t s_tx_before = 0;
bool s_first = true;

bool read_cpu(uint64_t& total, uint64_t& idle)
{
    std::ifstream file("/proc/stat");
    std::string name;
    uint64_t user = 0, nice = 0, system = 0, idle_value = 0;
    uint64_t iowait = 0, irq = 0, softirq = 0, steal = 0;
    if (!(file >> name >> user >> nice >> system >> idle_value >> iowait >> irq >> softirq >> steal)) {
        return false;
    }
    idle = idle_value + iowait;
    total = user + nice + system + idle_value + iowait + irq + softirq + steal;
    return true;
}

bool read_memory(uint64_t& used, uint64_t& total)
{
    std::ifstream file("/proc/meminfo");
    std::string key;
    uint64_t value = 0;
    uint64_t available = 0;
    total = 0;
    while (file >> key >> value) {
        if (key == "MemTotal:") total = value * 1024;
        else if (key == "MemAvailable:") available = value * 1024;
    }
    if (total == 0) return false;
    used = total > available ? total - available : 0;
    return true;
}

bool read_network(uint64_t& rx, uint64_t& tx)
{
    rx = 0;
    tx = 0;
    std::ifstream file("/proc/net/dev");
    std::string line;
    std::getline(file, line);
    std::getline(file, line);
    while (std::getline(file, line)) {
        const auto colon = line.find(':');
        if (colon == std::string::npos) continue;
        std::string interface = line.substr(0, colon);
        interface.erase(0, interface.find_first_not_of(" "));
        interface.erase(interface.find_last_not_of(" ") + 1);
        if (interface == "lo") continue;
        std::istringstream values(line.substr(colon + 1));
        uint64_t receive = 0, transmit = 0;
        uint64_t unused = 0;
        values >> receive;
        for (int i = 0; i < 7; ++i) values >> unused;
        values >> transmit;
        rx += receive;
        tx += transmit;
    }
    return true;
}

}  // namespace

bool platform_init()
{
    read_cpu(s_cpu_total_before, s_cpu_idle_before);
    read_network(s_rx_before, s_tx_before);
    s_first = true;
    return true;
}

bool platform_sample(HostSnapshot& snapshot)
{
    uint64_t cpu_total = 0;
    uint64_t cpu_idle = 0;
    if (read_cpu(cpu_total, cpu_idle)) {
        const uint64_t total_delta = cpu_total - s_cpu_total_before;
        const uint64_t idle_delta = cpu_idle - s_cpu_idle_before;
        snapshot.cpu_percent = total_delta > 0
            ? static_cast<uint32_t>((total_delta - idle_delta) * 100 / total_delta)
            : 0;
        s_cpu_total_before = cpu_total;
        s_cpu_idle_before = cpu_idle;
        snapshot.available_flags |= kAvailableCpu;
    }

    uint64_t memory_used = 0;
    uint64_t memory_total = 0;
    if (read_memory(memory_used, memory_total)) {
        snapshot.memory_used_bytes = memory_used;
        snapshot.memory_total_bytes = memory_total;
        snapshot.available_flags |= kAvailableMemory;
    }

    uint64_t rx = 0;
    uint64_t tx = 0;
    if (read_network(rx, tx)) {
        if (!s_first) {
            snapshot.network_rx_bytes_per_second = rx >= s_rx_before ? rx - s_rx_before : 0;
            snapshot.network_tx_bytes_per_second = tx >= s_tx_before ? tx - s_tx_before : 0;
            snapshot.available_flags |= kAvailableNetwork;
        }
        s_rx_before = rx;
        s_tx_before = tx;
    }

    s_first = false;
    return true;
}

std::vector<GPUDevice> enumerate_gpus()
{
    return {};
}

bool select_gpu(uint32_t)
{
    return false;
}

std::string platform_hostname()
{
    char name[256]{};
    if (gethostname(name, sizeof(name)) == 0) return name;
    return "Linux-host";
}

#endif
