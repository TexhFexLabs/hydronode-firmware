#pragma once

#include "config/Config.h"

namespace hn::power {

// Wake cause of this boot, as printed in HN:BOOT (TIMER, PIN, RESET, ...).
const char* wakeReason();

// Switches the sensor supply pin on (releasing a deep sleep hold first).
void sensorsOn(const Config& cfg);

// Switches it off and holds the level through deep sleep.
void sensorsOff(const Config& cfg);

// Milliseconds to sleep so that rounds start every `interval` seconds, given the time already
// spent awake. Never less than one second.
uint32_t sleepMs(uint32_t intervalSeconds, uint32_t awakeMs);

// Light sleep for `ms` (or until the wake pin fires). Returns afterwards.
void lightSleep(const Config& cfg, uint32_t ms);

// Deep sleep or hibernate. Does not return: the chip restarts on wake.
[[noreturn]] void deepSleep(const Config& cfg, uint32_t ms);

// Measuring rounds since the last reset (for values sent only every n-th round) and the internet
// time of the first round in ms (rounds are aligned to it). Kept in RTC memory, so both survive
// deep sleep (not hibernate, which powers that memory down).
struct Rounds {
    uint32_t index;
    uint64_t anchorMs;  // 0 = not known yet
};
Rounds loadRounds(bool fresh);
void saveRounds(const Rounds& rounds);

// ESP8266 only: its deep sleep lasts at most ~3.5 h. Longer intervals are chained, and the
// intermediate wake-ups go straight back to sleep with the radio off. Call first thing in setup();
// returns only when this wake-up is meant to measure. No-op on the ESP32 family.
void resumeLongSleep(const Config& cfg);

}  // namespace hn::power
