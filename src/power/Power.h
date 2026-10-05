#pragma once

#include "config/Config.h"

namespace hn::power {

// Wake cause of this boot, as printed in HN:BOOT (TIMER, PIN, RESET, ...).
const char* wakeReason();

// Restarts the board when the loop stops coming back (a library spinning on a sensor that left
// the bus would otherwise drain the battery). ESP32 family, 5 minutes; the ESP8266 has its own.
// feedWatchdog() from every place that may take long: the loop, waits, an image download.
void startWatchdog();
void feedWatchdog();

// Switches the sensor supply pin on (releasing a deep sleep hold first).
void sensorsOn(const Config& cfg);

// Switches it off and holds the level through deep sleep.
void sensorsOff(const Config& cfg);

// Pins whose level has to stay while the board sleeps: the sensor supply, a fan's SET pin, the
// outputs. keepLevel() registers a pin; holdLevels(true) latches all of them before a sleep,
// holdLevels(false) releases them afterwards. A deep sleep restarts the board with the pins
// still latched: set the level first, then releaseLevel(), so nothing glitches.
void keepLevel(int8_t pin);
void releaseLevel(int8_t pin);
void holdLevels(bool hold);

// Milliseconds to sleep so that rounds start every `interval` seconds, given the time already
// spent awake. Never less than one second.
uint32_t sleepMs(uint32_t intervalSeconds, uint32_t awakeMs);

// A pin that wakes the board: the "wake up early" pin, a button, a rain gauge. `level` 1 = wake
// while high, 0 = while low.
struct WakeInput {
    int8_t pin;
    uint8_t level;
};
constexpr uint8_t kMaxWakeInputs = 8;

// Light sleep for `ms` or until an input fires. Returns afterwards; outputs and held pins keep
// their level. `quiet`: no HN:SLEEP line (naps while sensors measure).
void lightSleep(const Config& cfg, uint32_t ms, const WakeInput* inputs, uint8_t count, bool quiet = false);

// Deep sleep or hibernate. Does not return: the chip restarts on wake.
[[noreturn]] void deepSleep(const Config& cfg, uint32_t ms, const WakeInput* inputs, uint8_t count);

// After a wake-up by a pin: a bit per entry of `inputs` that woke the board. After a deep sleep
// from the wake-up status registers only (the pins themselves may float until set up); after a
// light sleep from the levels, and only when a pin and not the timer ended the sleep.
uint32_t activeInputs(const WakeInput* inputs, uint8_t count, bool afterDeepSleep);

// When the next round is due, kept through deep sleep (the RTC timer keeps the system time).
void setNextRound(uint32_t inMs);
int64_t untilNextRoundMs();

// Measuring rounds since the last reset (for values sent only every n-th round) and the internet
// time of the first round in ms (rounds are aligned to it). Kept in RTC memory, so both survive
// deep sleep (not hibernate, which powers that memory down).
struct Rounds {
    uint32_t index;
    uint64_t anchorMs;  // 0 = not known yet
};
Rounds loadRounds(bool fresh);
void saveRounds(const Rounds& rounds);

// What the last round cost and what went wrong since, for the X-Device-Report header. Kept in
// RTC memory like the rounds; a reset starts it over.
struct ReportState {
    uint32_t awakeMs;
    uint32_t napMs;
    uint32_t wifiMs;
    uint16_t pinWakes;
    uint16_t wifiFailures;
    char wifiError[12];
};
ReportState& report();
void loadReport(bool fresh);
void saveReport();

// ESP8266 only: its deep sleep lasts at most ~3.5 h. Longer intervals are chained, and the
// intermediate wake-ups go straight back to sleep with the radio off. Call first thing in setup();
// returns only when this wake-up is meant to measure. No-op on the ESP32 family.
void resumeLongSleep(const Config& cfg);

}  // namespace hn::power
