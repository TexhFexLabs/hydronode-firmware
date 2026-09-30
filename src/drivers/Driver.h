#pragma once

#include <Wire.h>

#include "config/Config.h"

namespace hn {

// One reading per configured channel. `ok == false` means the sensor did not
// answer; the value is then not sent.
struct Reading {
    const char* type;
    float value;
    bool ok;
};

class Driver {
public:
    explicit Driver(const DeviceConfig& cfg) : cfg_(cfg) {}
    virtual ~Driver() = default;

    // Called after power-up and again after every sensor power cycle.
    virtual bool begin() = 0;

    // Fills one Reading per channel (cfg().channelCount entries).
    virtual void read(Reading* out) = 0;

    // Time the sensor needs after power-up before the first valid reading.
    virtual uint32_t warmupMs() const { return 0; }

    const DeviceConfig& cfg() const { return cfg_; }

protected:
    // Maps a channel quantity key to a value produced by the driver.
    void fill(Reading* out, const char* q, float value, bool ok) const;

    const DeviceConfig& cfg_;
};

// Creates the driver for `cfg`, or nullptr for an unknown driver id.
Driver* createDriver(const DeviceConfig& cfg, TwoWire* buses[kMaxI2cBuses]);

}  // namespace hn
