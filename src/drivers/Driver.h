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

// What a driver needs to know about how the board runs, to pick the sensor's own low-power
// behaviour. The catalog's sleep rules (catalog/drivers.json) describe the same choices.
struct DriverContext {
    SleepMode mode;
    uint32_t intervalSeconds;
    uint16_t every;      // rounds between two due rounds of this device (its smallest "every")
    bool powerPin;       // the sensor supply is switched off between rounds
    uint16_t adcRangeMv;

    bool sleeps() const { return mode == SleepMode::LightSleep || restarts(); }
    bool restarts() const { return mode == SleepMode::DeepSleep || mode == SleepMode::Hibernate; }
    // Seconds between two rounds that read this device.
    uint32_t dueSeconds() const { return intervalSeconds * (every ? every : 1); }
};

// Why a device delivered no value this round, sent to the server (X-Device-Report sens=…) and
// shown in Fleet in plain words.
namespace problem {
constexpr const char* kMissing = "missing";  // did not answer at start-up (wiring, address, power)
constexpr const char* kTimeout = "timeout";  // answered, but no measurement in time
constexpr const char* kRange = "range";      // a value outside what the sensor can measure
constexpr const char* kWarming = "warming";  // still settling, not an error
}  // namespace problem

// A sensor in rounds. Per due round the firmware calls start(), waits (napping) until poll()
// returns 0, then read() and sleep(). Devices that are not due this round are not touched, so a
// value sent only every 5th round wakes its sensor only every 5th round.
class Driver {
public:
    Driver(const DeviceConfig& cfg, const DriverContext& ctx) : cfg_(cfg), ctx_(ctx) {}
    virtual ~Driver() = default;

    // First contact this boot. `cold`: the sensor has just been powered (power-on, reset or the
    // sensor power pin), so it needs its full set-up; otherwise it kept its state through the
    // sleep and only the driver object is new. Returns false when the sensor does not answer.
    virtual bool begin(bool cold) = 0;

    // Starts this round's measurement. Returns how long the board can nap before poll().
    virtual uint32_t start() { return 0; }
    // Called when that time is over: 0 = ready to read, else milliseconds to wait more.
    virtual uint32_t poll() { return 0; }

    // Fills one Reading per channel (cfg().channelCount entries).
    virtual void read(Reading* out) = 0;

    // After read(): the sensor's lowest power state until its next due round.
    virtual void sleep() {}

    // After power-on: until the sensor accepts commands, and until its first valid value.
    virtual uint32_t bootMs() const { return 0; }
    virtual uint32_t powerUpMs() const { return 0; }

    // Background sampling between rounds (gas index algorithms, pulse counters): milliseconds
    // between two tick() calls, 0 for none. The board stays awake or wakes from light sleep for it.
    virtual uint32_t tickMs() const { return 0; }
    virtual void tick() {}

    // Around a light sleep of the board (not deep sleep, which restarts it).
    virtual void beforeSleep() {}
    virtual void afterSleep() {}

    // Read after WiFi is up instead of before (the WiFi signal strength).
    virtual bool afterConnect() const { return false; }

    // Compensates with ambient() in read(): read after the sensors that measure it.
    virtual bool usesAmbient() const { return false; }

    // Why this round's begin()/start()/read() gave no value, nullptr when it did. The firmware
    // clears it at the start of every round the device is due in.
    const char* problem() const { return problem_; }
    void resetProblem() { problem_ = nullptr; }

    const DeviceConfig& cfg() const { return cfg_; }

    // Maps a channel quantity key to a value produced by the driver. Channels the user removed
    // are simply not filled.
    void fill(Reading* out, const char* q, float value, bool ok, bool warming = false);

protected:
    void setProblem(const char* p) { problem_ = p; }
    // Before read() of an I²C sensor whose library cannot tell a lost sensor from a value: it
    // still answers on its address. Sets kMissing when not.
    bool stillThere(TwoWire* bus);
    // Whether a value with quantity `q` is sent at all.
    bool sends(const char* q) const;

    const DeviceConfig& cfg_;
    const DriverContext& ctx_;
    const char* problem_ = nullptr;
};

// Latest temperature and humidity measured by any sensor on the device, NAN when there is none.
// Gas sensors use it for humidity compensation instead of a fixed 25 °C / 50 %.
struct Ambient {
    float t;
    float rh;
};
Ambient& ambient();

// Creates the driver for `cfg`, or nullptr for an unknown driver id.
Driver* createDriver(const DeviceConfig& cfg, TwoWire* buses[kMaxI2cBuses], const DriverContext& ctx);

// Rain gauge tips counted while the board slept (each tip woke it for a moment). Kept in RTC
// memory; the rain driver adds them to its next reading.
void addSleepTips(int8_t pin, uint16_t tips);

}  // namespace hn
