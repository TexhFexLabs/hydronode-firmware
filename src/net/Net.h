#pragma once

#include "config/Config.h"

namespace hn::net {

// Connects in station mode. With fast reconnect, BSSID and channel of the last
// successful connection (kept in RTC memory across deep sleep) skip the scan.
// Prints HN:WIFI ok|… and returns true on success.
bool connect(const Config& cfg, uint32_t timeoutMs);

// Radio off, for light/deep sleep.
void off();

bool connected();

// Modem sleep (power save between beacons) or full power.
void setPowerSave(bool on);

}  // namespace hn::net
