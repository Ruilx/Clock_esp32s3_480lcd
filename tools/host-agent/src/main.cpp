#include "platform.h"

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>
#include <thread>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace {

constexpr uint16_t kUdpPort = 9461;
constexpr uint32_t kMagic = 0x48535431u;
constexpr uint32_t kVersion = 1;
constexpr uint32_t kPacketSize = 80;

std::atomic<bool> s_running{true};

void stop_handler(int)
{
    s_running.store(false);
}

#ifdef _WIN32
BOOL WINAPI console_control_handler(DWORD control)
{
    if(control == CTRL_C_EVENT || control == CTRL_BREAK_EVENT ||
       control == CTRL_CLOSE_EVENT || control == CTRL_SHUTDOWN_EVENT) {
        s_running.store(false);
        return TRUE;
    }
    return FALSE;
}
#endif

void write_u32(std::uint8_t* data, uint32_t value)
{
    data[0] = static_cast<uint8_t>(value >> 24);
    data[1] = static_cast<uint8_t>(value >> 16);
    data[2] = static_cast<uint8_t>(value >> 8);
    data[3] = static_cast<uint8_t>(value);
}

bool parse_interval(const std::string& text, std::chrono::milliseconds& interval)
{
    char* end = nullptr;
    const unsigned long milliseconds = std::strtoul(text.c_str(), &end, 10);
    if (end == text.c_str() || *end != '\0' || milliseconds < 100 || milliseconds > 60000) {
        return false;
    }
    interval = std::chrono::milliseconds(milliseconds);
    return true;
}

bool parse_gpu(const std::string& text, uint32_t& gpu_id)
{
    if (text == "auto") {
        gpu_id = 0;
        return true;
    }
    char* end = nullptr;
    const unsigned long id = std::strtoul(text.c_str(), &end, 10);
    if (end == text.c_str() || *end != '\0') return false;
    gpu_id = static_cast<uint32_t>(id);
    return true;
}

}  // namespace

int main(int argc, char** argv)
{
    std::string esp_ip = "10.4.212.47";
    std::string gpu_text = "auto";
    std::chrono::milliseconds interval(1000);
    bool headless = false;

    for (int i = 1; i < argc; ++i) {
        const std::string argument = argv[i];
        if (argument == "--esp-ip" && i + 1 < argc) esp_ip = argv[++i];
        else if (argument == "--gpu" && i + 1 < argc) gpu_text = argv[++i];
        else if (argument == "--interval" && i + 1 < argc) {
            if (!parse_interval(argv[++i], interval)) {
                std::cerr << "Invalid --interval value\n";
                return 2;
            }
        } else if (argument == "--headless") headless = true;
        else if (argument == "--show") headless = false;
        else if (argument == "--help") {
            std::cout << "Usage: host-status-agent [--esp-ip IP] [--gpu auto|id] "
                         "[--interval milliseconds] [--headless]\n";
            return 0;
        }
    }

#ifdef _WIN32
    WSADATA data{};
    if (WSAStartup(MAKEWORD(2, 2), &data) != 0) {
        std::cerr << "Could not initialize Winsock\n";
        return 1;
    }
#endif

    uint32_t gpu_id = 0;
    if (!parse_gpu(gpu_text, gpu_id)) {
        std::cerr << "Invalid --gpu value\n";
        return 2;
    }

    if (!platform_init()) {
        std::cerr << "Could not initialize platform sampler\n";
        return 1;
    }

    const auto gpus = enumerate_gpus();
    if (gpu_text != "auto" && !select_gpu(gpu_id)) {
        std::cerr << "Could not select GPU " << gpu_id << "\n";
        return 2;
    }
    if (gpu_text == "auto" && !gpus.empty()) select_gpu(0);

    int socket_fd =
#ifdef _WIN32
        ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
#else
        ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
#endif
    if (socket_fd < 0) {
        std::cerr << "Could not create UDP socket\n";
        return 1;
    }

    sockaddr_in destination{};
    destination.sin_family = AF_INET;
    destination.sin_port = htons(kUdpPort);
    if (inet_pton(AF_INET, esp_ip.c_str(), &destination.sin_addr) != 1) {
        std::cerr << "Invalid ESP32 IP: " << esp_ip << "\n";
        return 2;
    }

    std::signal(SIGINT, stop_handler);
    std::signal(SIGTERM, stop_handler);

#ifdef _WIN32
    SetConsoleCtrlHandler(console_control_handler, TRUE);
#endif

    if (!headless) {
        std::cout << "Host: " << platform_hostname() << "\n";
        std::cout << "Target: " << esp_ip << ":" << kUdpPort << "\n";
        std::cout << "Interval: " << interval.count() << " ms\n";
        if (!gpus.empty()) {
            std::cout << "GPU:";
            for (const auto& gpu : gpus) {
                std::cout << " [" << gpu.id << "] " << gpu.name;
            }
            std::cout << "\n";
        } else {
            std::cout << "GPU: unavailable\n";
        }
    }

    uint32_t sequence = 0;
    if (!headless) {
        std::cout << "Pushing UDP packets..." << std::endl;
    }
    while (s_running.load()) {
        HostSnapshot snapshot{};
        if (platform_sample(snapshot)) {
            ++sequence;
            uint8_t packet[kPacketSize]{};
            write_u32(packet, kMagic);
            write_u32(packet + 4, kVersion);
            write_u32(packet + 8, snapshot.available_flags);
            write_u32(packet + 12, sequence);
            write_u32(packet + 16, 0);
            write_u32(packet + 20, snapshot.cpu_percent);
            write_u32(packet + 24, static_cast<uint32_t>(snapshot.memory_used_bytes / (1024 * 1024)));
            write_u32(packet + 28, static_cast<uint32_t>(snapshot.memory_total_bytes / (1024 * 1024)));
            write_u32(packet + 32, snapshot.gpu_percent);
            write_u32(packet + 36, static_cast<uint32_t>(snapshot.gpu_memory_used_bytes / (1024 * 1024)));
            write_u32(packet + 40, static_cast<uint32_t>(snapshot.gpu_memory_total_bytes / (1024 * 1024)));
            write_u32(packet + 44, static_cast<uint32_t>(snapshot.network_rx_bytes_per_second / 1024));
            write_u32(packet + 48, static_cast<uint32_t>(snapshot.network_tx_bytes_per_second / 1024));

            const int sent = sendto(socket_fd, reinterpret_cast<const char*>(packet), kPacketSize, 0,
                                    reinterpret_cast<const sockaddr*>(&destination), sizeof(destination));
            if (sent != kPacketSize && !headless) {
                std::cerr << "\nUDP send failed: " << WSAGetLastError() << std::endl;
            }

            if (!headless) {
                std::cout << "\rCPU " << snapshot.cpu_percent << "% "
                          << "MEM " << snapshot.memory_used_bytes / (1024 * 1024)
                          << "/" << snapshot.memory_total_bytes / (1024 * 1024) << " MB "
                          << "GPU " << snapshot.gpu_percent << "% "
                          << "DN " << snapshot.network_rx_bytes_per_second / 1024
                          << " KB/s UP " << snapshot.network_tx_bytes_per_second / 1024
                          << " KB/s   " << std::flush;
            }
        }
        std::this_thread::sleep_for(interval);
    }

#ifdef _WIN32
    closesocket(socket_fd);
    WSACleanup();
#else
    close(socket_fd);
#endif
    return 0;
}
