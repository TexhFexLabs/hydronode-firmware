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
    bool warming = false;  // valid sensor communication, algorithm not ready yet
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

    // Gas sensors (SGP30/40/41) run their algorithm on one sample per second. The firmware
    // calls tick() every second while it waits between cycles; the catalog only offers them
    // with Always on and Modem sleep, where the CPU keeps running.
    virtual bool continuous() const { return false; }
    virtual void tick() {}

    // Read after WiFi is up instead of before (the WiFi signal strength).
    virtual bool afterConnect() const { return false; }

    const DeviceConfig& cfg() const { return cfg_; }

    // Maps a channel quantity key to a value produced by the driver. Channels the user removed
    // are simply not filled.
    void fill(Reading* out, const char* q, float value, bool ok, bool warming = false) const;

protected:
    const DeviceConfig& cfg_;
};

// Latest temperature and humidity measured by any sensor on the device, NAN when there is none.
// Gas sensors use it for humidity compensation instead of a fixed 25 °C / 50 %.
struct Ambient {
    float t;
    float rh;
};
Ambient& ambient();

// Creates the driver for `cfg`, or nullptr for an unknown driver id. `restartsEachCycle` is true
// in deep sleep and hibernate, where a sensor cannot keep measuring between readings.
Driver* createDriver(const DeviceConfig& cfg, TwoWire* buses[kMaxI2cBuses], uint16_t adcRangeMv,
                     bool restartsEachCycle);

}  // namespace hn
