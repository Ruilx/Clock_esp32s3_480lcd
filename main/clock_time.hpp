#pragma once

#include <ctime>

#include "driver/i2c_master.h"

namespace clock_time {

enum class Source { Invalid, Rtc, Manual, Sntp };

// Call once at startup, before the network starts. The BSP owns the bus;
// it must outlive the service. nullptr or an unreadable RTC is not fatal.
// Returns whether the service worker was initialized.
bool init(i2c_master_bus_handle_t bus);

// Submits a trusted provisioner's Unix UTC seconds (2020 through 2099).
// True means queued, not yet applied/persisted. SNTP takes precedence.
bool submit_manual_utc(std::time_t utc);

// Task context only. True after the station obtains IP, false on loss.
void notify_network(bool online);

// No I2C or locking in the UI's read path. Use time(nullptr) for the time.
Source source();
bool valid();

}  // namespace clock_time
