#ifdef __APPLE__

#include "platform.h"

#include <mach/mach.h>
#include <mach/mach_host.h>
#include <sys/sysctl.h>
#include <sys/types.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <unistd.h>
#include <cstdio>
#include <cstring>

namespace {

uint64_t s_rx_before = 0;
uint64_t s_tx_before = 0;
host_cpu_load_info_data_t s_cpu_before{};
bool s_first = true;
bool s_cpu_initialized = false;

bool read_network(uint64_t& rx, uint64_t& tx)
{
    rx = 0;
    tx = 0;
    ifaddrs* list = nullptr;
    if (getifaddrs(&list) != 0) return false;
    for (ifaddrs* item = list; item; item = item->ifa_next) {
        if (!item->ifa_addr || item->ifa_addr->sa_family != AF_LINK) continue;
        if (!item->ifa_data) continue;
        if (std::strcmp(item->ifa_name, "lo0") == 0) continue;
        const auto* data = static_cast<if_data*>(item->ifa_data);
        rx += data->ifi_ibytes;
        tx += data->ifi_obytes;
    }
    freeifaddrs(list);
    return true;
}

}  // namespace

bool platform_init()
{
    mach_msg_type_number_t count = HOST_CPU_LOAD_INFO_COUNT;
    if (host_statistics(mach_host_self(), HOST_CPU_LOAD_INFO,
                        reinterpret_cast<host_info_t>(&s_cpu_before), &count) == KERN_SUCCESS) {
        s_cpu_initialized = true;
    }
    read_network(s_rx_before, s_tx_before);
    s_first = true;
    return true;
}

bool platform_sample(HostSnapshot& snapshot)
{
    if (s_cpu_initialized) {
        host_cpu_load_info_data_t cpu{};
        mach_msg_type_number_t count = HOST_CPU_LOAD_INFO_COUNT;
        if (host_statistics(mach_host_self(), HOST_CPU_LOAD_INFO,
                            reinterpret_cast<host_info_t>(&cpu), &count) == KERN_SUCCESS) {
            natural_t user_delta = cpu.cpu_ticks[CPU_STATE_USER] - s_cpu_before.cpu_ticks[CPU_STATE_USER];
            natural_t system_delta = cpu.cpu_ticks[CPU_STATE_SYSTEM] - s_cpu_before.cpu_ticks[CPU_STATE_SYSTEM];
            natural_t idle_delta = cpu.cpu_ticks[CPU_STATE_IDLE] - s_cpu_before.cpu_ticks[CPU_STATE_IDLE];
            natural_t nice_delta = cpu.cpu_ticks[CPU_STATE_NICE] - s_cpu_before.cpu_ticks[CPU_STATE_NICE];
            const natural_t total = user_delta + system_delta + idle_delta + nice_delta;
            snapshot.cpu_percent = total > 0
                ? static_cast<uint32_t>((total - idle_delta) * 100 / total)
                : 0;
            s_cpu_before = cpu;
            snapshot.available_flags |= kAvailableCpu;
        }
    }

    vm_size_t page_size = 0;
    host_page_size(mach_host_self(), &page_size);
    vm_statistics64_data_t vm{};
    mach_msg_type_number_t count = HOST_VM_INFO64_COUNT;
    if (host_statistics64(mach_host_self(), HOST_VM_INFO64,
                          reinterpret_cast<host_info64_t>(&vm), &count) == KERN_SUCCESS) {
        uint64_t total = 0;
        int mib[2] = {CTL_HW, HW_MEMSIZE};
        size_t size = sizeof(total);
        if (sysctl(mib, 2, &total, &size, nullptr, 0) == 0) {
            const uint64_t used = static_cast<uint64_t>(vm.active_count + vm.wire_count +
                                                        vm.compressor_page_count) * page_size;
            snapshot.memory_used_bytes = used < total ? used : total;
            snapshot.memory_total_bytes = total;
            snapshot.available_flags |= kAvailableMemory;
        }
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
    return "Mac-host";
}

#endif
