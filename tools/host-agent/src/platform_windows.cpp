#ifdef _WIN32

#include "platform.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <iphlpapi.h>
#include <pdh.h>

#include <memory>
#include <vector>
#include <cwchar>

namespace {

struct FileTimePair {
    ULONGLONG idle = 0;
    ULONGLONG kernel = 0;
    ULONGLONG user = 0;
};

FileTimePair s_cpu_before;
ULONGLONG s_network_rx_before = 0;
ULONGLONG s_network_tx_before = 0;
bool s_first_sample = true;
PDH_HQUERY s_gpu_query = nullptr;
PDH_HCOUNTER s_gpu_counter = nullptr;

using NvmlDevice = void*;

struct NvmlUtilization {
    unsigned int gpu;
    unsigned int memory;
};

struct NvmlMemory {
    unsigned long long total;
    unsigned long long free;
    unsigned long long used;
};

struct NVML {
    HMODULE dll = nullptr;
    int (*init_v2)(void) = nullptr;
    int (*shutdown)(void) = nullptr;
    int (*device_get_count)(unsigned int*) = nullptr;
    int (*device_get_handle)(unsigned int, NvmlDevice*) = nullptr;
    int (*device_get_name)(NvmlDevice, char*, unsigned int) = nullptr;
    int (*device_get_utilization)(NvmlDevice, NvmlUtilization*) = nullptr;
    int (*device_get_memory)(NvmlDevice, NvmlMemory*) = nullptr;
};

NVML s_nvml;
std::vector<NvmlDevice> s_gpu_devices;
uint32_t s_selected_gpu = 0;

template<typename T>
T get_proc(HMODULE dll, const char* name)
{
    return reinterpret_cast<T>(GetProcAddress(dll, name));
}

ULONGLONG filetime(const FILETIME& value)
{
    ULARGE_INTEGER time{};
    time.LowPart = value.dwLowDateTime;
    time.HighPart = value.dwHighDateTime;
    return time.QuadPart;
}

bool load_nvml()
{
    if (s_nvml.dll) return true;
    s_nvml.dll = LoadLibraryA("nvml.dll");
    if (!s_nvml.dll) {
        wchar_t system_dir[MAX_PATH]{};
        UINT length = GetSystemDirectoryW(system_dir, MAX_PATH);
        if (length > 0) {
            std::wstring path(system_dir);
            path += L"\\nvml.dll";
            s_nvml.dll = LoadLibraryW(path.c_str());
        }
    }
    if (!s_nvml.dll) return false;

    s_nvml.init_v2 = get_proc<int (*)(void)>(s_nvml.dll, "nvmlInit_v2");
    s_nvml.shutdown = get_proc<int (*)(void)>(s_nvml.dll, "nvmlShutdown");
    s_nvml.device_get_count = get_proc<int (*)(unsigned int*)>(s_nvml.dll, "nvmlDeviceGetCount_v2");
    s_nvml.device_get_handle = get_proc<int (*)(unsigned int, NvmlDevice*)>(s_nvml.dll, "nvmlDeviceGetHandleByIndex_v2");
    s_nvml.device_get_name = get_proc<int (*)(NvmlDevice, char*, unsigned int)>(s_nvml.dll, "nvmlDeviceGetName");
    s_nvml.device_get_utilization = get_proc<int (*)(NvmlDevice, NvmlUtilization*)>(s_nvml.dll, "nvmlDeviceGetUtilizationRates");
    s_nvml.device_get_memory = get_proc<int (*)(NvmlDevice, NvmlMemory*)>(s_nvml.dll, "nvmlDeviceGetMemoryInfo");

    if (!s_nvml.init_v2 || s_nvml.init_v2() != 0 ||
        !s_nvml.device_get_count || !s_nvml.device_get_handle ||
        !s_nvml.device_get_name) {
        FreeLibrary(s_nvml.dll);
        s_nvml = {};
        return false;
    }

    unsigned int count = 0;
    if (s_nvml.device_get_count(&count) != 0) return false;
    s_gpu_devices.clear();
    s_gpu_devices.reserve(count);
    for (unsigned int i = 0; i < count; ++i) {
        NvmlDevice device = nullptr;
        if (s_nvml.device_get_handle(i, &device) == 0) s_gpu_devices.push_back(device);
    }
    return true;
}

void network_totals(ULONGLONG& rx, ULONGLONG& tx)
{
    rx = 0;
    tx = 0;
    ULONG size = 0;
    if (GetIfTable(nullptr, &size, false) != ERROR_INSUFFICIENT_BUFFER || size == 0) return;
    std::vector<unsigned char> buffer(size);
    MIB_IFTABLE* table = reinterpret_cast<MIB_IFTABLE*>(buffer.data());
    if (GetIfTable(table, &size, false) != NO_ERROR) return;
    for (DWORD i = 0; i < table->dwNumEntries; ++i) {
        const MIB_IFROW& row = table->table[i];
        if (row.dwOperStatus != IF_OPER_STATUS_OPERATIONAL) continue;
        rx += row.dwInOctets;
        tx += row.dwOutOctets;
    }
}

bool gpu_engine_utilization(uint32_t& percent)
{
    if (!s_gpu_query) {
        if (PdhOpenQueryW(nullptr, 0, &s_gpu_query) != ERROR_SUCCESS) {
            s_gpu_query = nullptr;
            return false;
        }
        if (PdhAddEnglishCounterW(s_gpu_query,
                                  L"\\GPU Engine(*)\\Utilization Percentage",
                                  0, &s_gpu_counter) != ERROR_SUCCESS) {
            PdhCloseQuery(s_gpu_query);
            s_gpu_query = nullptr;
            s_gpu_counter = nullptr;
            return false;
        }
    }

    if (PdhCollectQueryData(s_gpu_query) != ERROR_SUCCESS) return false;
    DWORD buffer_size = 0;
    DWORD item_count = 0;
    PdhGetFormattedCounterArrayW(s_gpu_counter, PDH_FMT_DOUBLE,
                                 &buffer_size, &item_count, nullptr);
    if (buffer_size == 0 || item_count == 0) return false;
    std::vector<unsigned char> buffer(buffer_size);
    auto* items = reinterpret_cast<PDH_FMT_COUNTERVALUE_ITEM_W*>(buffer.data());
    if (PdhGetFormattedCounterArrayW(s_gpu_counter, PDH_FMT_DOUBLE,
                                     &buffer_size, &item_count, items) != ERROR_SUCCESS) {
        return false;
    }

    double total = 0.0;
    bool found_3d = false;
    for (DWORD i = 0; i < item_count; ++i) {
        const wchar_t* name = items[i].szName;
        if (!wcsstr(name, L"engtype_3d")) continue;
        found_3d = true;
        total += items[i].FmtValue.doubleValue;
    }
    if (!found_3d) return false;
    percent = static_cast<uint32_t>(total > 100.0 ? 100.0 : total);
    return true;
}

}  // namespace

bool platform_init()
{
    WSADATA data{};
    WSAStartup(MAKEWORD(2, 2), &data);
    FILETIME idle{}, kernel{}, user{};
    if (GetSystemTimes(&idle, &kernel, &user)) {
        s_cpu_before = {filetime(idle), filetime(kernel), filetime(user)};
    }
    network_totals(s_network_rx_before, s_network_tx_before);
    load_nvml();
    s_first_sample = true;
    return true;
}

bool platform_sample(HostSnapshot& snapshot)
{
    FILETIME idle{}, kernel{}, user{};
    if (GetSystemTimes(&idle, &kernel, &user)) {
        FileTimePair now{filetime(idle), filetime(kernel), filetime(user)};
        const ULONGLONG idle_delta = now.idle - s_cpu_before.idle;
        const ULONGLONG kernel_delta = now.kernel - s_cpu_before.kernel;
        const ULONGLONG user_delta = now.user - s_cpu_before.user;
        const ULONGLONG total_delta = kernel_delta + user_delta;
        snapshot.cpu_percent = total_delta > 0
            ? static_cast<uint32_t>((total_delta - idle_delta) * 100 / total_delta)
            : 0;
        if (snapshot.cpu_percent > 100) snapshot.cpu_percent = 100;
        s_cpu_before = now;
        snapshot.available_flags |= kAvailableCpu;
    }

    MEMORYSTATUSEX memory{};
    memory.dwLength = sizeof(memory);
    if (GlobalMemoryStatusEx(&memory)) {
        const uint64_t used = memory.ullTotalPhys - memory.ullAvailPhys;
        snapshot.memory_total_bytes = memory.ullTotalPhys;
        snapshot.memory_used_bytes = used;
        snapshot.available_flags |= kAvailableMemory;
    }

    ULONGLONG network_rx = 0;
    ULONGLONG network_tx = 0;
    network_totals(network_rx, network_tx);
    if (!s_first_sample) {
        snapshot.network_rx_bytes_per_second = network_rx >= s_network_rx_before
            ? network_rx - s_network_rx_before : 0;
        snapshot.network_tx_bytes_per_second = network_tx >= s_network_tx_before
            ? network_tx - s_network_tx_before : 0;
        snapshot.available_flags |= kAvailableNetwork;
    }
    s_network_rx_before = network_rx;
    s_network_tx_before = network_tx;

    if (s_nvml.dll && s_selected_gpu < s_gpu_devices.size()) {
        NvmlDevice device = s_gpu_devices[s_selected_gpu];
        uint32_t engine_percent = 0;
        if (gpu_engine_utilization(engine_percent)) {
            snapshot.gpu_percent = engine_percent;
            snapshot.available_flags |= kAvailableGpu;
        } else if (s_nvml.device_get_utilization) {
            NvmlUtilization utilization{};
            if (s_nvml.device_get_utilization(device, &utilization) == 0) {
                snapshot.gpu_percent = utilization.gpu <= 100 ? utilization.gpu : 100;
                snapshot.available_flags |= kAvailableGpu;
            }
        }
        if (s_nvml.device_get_memory) {
            NvmlMemory memory{};
            if (s_nvml.device_get_memory(device, &memory) == 0) {
                snapshot.gpu_memory_used_bytes = memory.used;
                snapshot.gpu_memory_total_bytes = memory.total;
                snapshot.available_flags |= kAvailableGpuMemory;
            }
        }
    }

    s_first_sample = false;
    return true;
}

std::vector<GPUDevice> enumerate_gpus()
{
    std::vector<GPUDevice> devices;
    if (!load_nvml()) return devices;
    devices.reserve(s_gpu_devices.size());
    for (uint32_t i = 0; i < s_gpu_devices.size(); ++i) {
        char name[128]{};
        if (s_nvml.device_get_name &&
            s_nvml.device_get_name(s_gpu_devices[i], name, sizeof(name)) == 0) {
            devices.push_back({i, name});
        } else {
            devices.push_back({i, "NVIDIA GPU " + std::to_string(i)});
        }
    }
    return devices;
}

bool select_gpu(uint32_t id)
{
    if (!load_nvml()) return false;
    if (id >= s_gpu_devices.size()) return false;
    s_selected_gpu = id;
    return true;
}

std::string platform_hostname()
{
    wchar_t name[256]{};
    DWORD size = sizeof(name) / sizeof(name[0]);
    if (!GetComputerNameW(name, &size)) return "Windows-host";
    char result[256]{};
    if (WideCharToMultiByte(CP_UTF8, 0, name, -1, result, sizeof(result), nullptr, nullptr) > 0) {
        return result;
    }
    return "Windows-host";
}

#endif
