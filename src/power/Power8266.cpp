// ESP8266 power modes. Deep sleep needs GPIO16 wired to RST, the web flasher asks for it.

#if defined(ESP8266)

#include <Arduino.h>
#include <ESP8266WiFi.h>

#include <coredecls.h>

#include "Power.h"
#include "status/Status.h"

extern "C" {
#include <user_interface.h>
}

namespace hn::power {

namespace {

constexpr uint32_t kSleepMagic = 0x484E5332;  // "HNS2": remaining time in ms
constexpr uint32_t kRtcSlot = 0;              // slots 0..3, the WiFi module uses 8+
constexpr uint32_t kRoundMagic = 0x484E5244;  // "HNRD"
constexpr uint32_t kRoundSlot = 4;            // slots 4..7

struct LongSleep {
    uint32_t magic;
    uint32_t remainingMs;
    uint32_t reserved[2];
};

LongSleep readState() {
    LongSleep state{};
    ESP.rtcUserMemoryRead(kRtcSlot, reinterpret_cast<uint32_t*>(&state), sizeof(state));
    if (state.magic != kSleepMagic) state = {};
    return state;
}

void writeState(uint32_t remaining) {
    LongSleep state{kSleepMagic, remaining, {0, 0}};
    ESP.rtcUserMemoryWrite(kRtcSlot, reinterpret_cast<uint32_t*>(&state), sizeof(state));
}

uint32_t maxChunkMs() {
    // deepSleepMax() depends on the RTC clock calibration; keep a safety margin.
    return static_cast<uint32_t>(ESP.deepSleepMax() / 1000ULL * 95 / 100);
}

[[noreturn]] void sleepChunk(uint32_t total) {
    uint32_t chunk = total > maxChunkMs() ? maxChunkMs() : total;
    uint32_t rest = total - chunk;
    writeState(rest);
    status::flush();
    // Only the wake-up that measures needs the radio calibrated.
    ESP.deepSleep(uint64_t(chunk) * 1000ULL, rest ? WAKE_RF_DISABLED : WAKE_RF_DEFAULT);
    for (;;) delay(1000);
}

}  // namespace

const char* wakeReason() {
    const rst_info* info = ESP.getResetInfoPtr();
    return info && info->reason == REASON_DEEP_SLEEP_AWAKE ? "TIMER" : "RESET";
}

void sensorsOn(const Config& cfg) {
    if (cfg.sensorPowerPin < 0) return;
    pinMode(cfg.sensorPowerPin, OUTPUT);
    digitalWrite(cfg.sensorPowerPin, HIGH);
}

void sensorsOff(const Config& cfg) {
    if (cfg.sensorPowerPin < 0) return;
    digitalWrite(cfg.sensorPowerPin, LOW);
}

uint32_t sleepMs(uint32_t intervalSeconds, uint32_t awakeMs) {
    uint32_t interval = intervalSeconds * 1000;
    return interval > awakeMs + 1000 ? interval - awakeMs : 1000;
}

Rounds loadRounds(bool fresh) {
    uint32_t state[4] = {0, 0, 0, 0};
    ESP.rtcUserMemoryRead(kRoundSlot, state, sizeof(state));
    if (fresh || state[0] != kRoundMagic) return {0, 0};
    return {state[1], (uint64_t(state[3]) << 32) | state[2]};
}

void saveRounds(const Rounds& rounds) {
    uint32_t state[4] = {kRoundMagic, rounds.index, uint32_t(rounds.anchorMs), uint32_t(rounds.anchorMs >> 32)};
    ESP.rtcUserMemoryWrite(kRoundSlot, state, sizeof(state));
}

void resumeLongSleep(const Config& cfg) {
    if (cfg.mode != SleepMode::DeepSleep || strcmp(wakeReason(), "TIMER") != 0) {
        writeState(0);
        return;
    }
    LongSleep state = readState();
    if (state.remainingMs > 0) sleepChunk(state.remainingMs);
}

namespace {
volatile bool woke = false;
void onWake() {
    woke = true;
    esp_schedule();  // ends the esp_delay() below
}
}  // namespace

void lightSleep(const Config& cfg, uint32_t ms) {
    status::line("SLEEP LIGHT %lu.%02lu", (unsigned long)(ms / 1000), (unsigned long)(ms % 1000 / 10));
    status::flush();
    // Forced light sleep: radio off, CPU halted until the timer fires. The SDK timer is limited
    // to about 268 s per call, so longer waits are split. millis() stands still while the CPU
    // sleeps, so a plain delay() would wait the whole time a second time after waking up (a 30 s
    // interval became 51 s). The wake-up callback ends the wait instead.
    wifi_set_opmode_current(NULL_MODE);
    wifi_fpm_set_sleep_type(LIGHT_SLEEP_T);
    wifi_fpm_open();
    wifi_fpm_set_wakeup_cb(onWake);
    uint32_t left = ms;
    while (left > 0) {
        uint32_t chunk = left > 260000 ? 260000 : left;
        woke = false;
        wifi_fpm_do_sleep(chunk * 1000UL);
        esp_delay(chunk + 1, []() { return !woke; });
        left -= chunk;
    }
    wifi_fpm_close();
    (void)cfg;
}

void deepSleep(const Config& cfg, uint32_t ms) {
    status::line("SLEEP DEEP %lu.%02lu", (unsigned long)(ms / 1000), (unsigned long)(ms % 1000 / 10));
    (void)cfg;
    sleepChunk(ms);
}

}  // namespace hn::power

#endif  // ESP8266
