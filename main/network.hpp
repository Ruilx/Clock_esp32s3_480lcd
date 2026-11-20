#pragma once

#include <cstdint>

struct NetworkStatus {
    bool ap_active;
    bool sta_online;
    char ap_ssid[33];
    char setup_key[17]; // 仅交给本机 LCD，不输出到串口或网页。
};

void network_init();
void network_sntp_result();
NetworkStatus network_status();
int32_t network_utc_offset_minutes();
