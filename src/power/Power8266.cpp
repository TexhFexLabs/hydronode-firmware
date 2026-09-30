// ESP8266 power modes. Deep sleep needs GPIO16 wired to RST, the web flasher asks for it.

#if defined(ESP8266)

#include <Arduino.h>
#include <ESP8266WiFi.h>

#include "Power.h"
#include "status/Status.h"

extern "C" {
#include <user_interface.h>
}

namespace hn::power {

namespace {

constexpr uint32_t kSleepMagic = 0x484E534C;  // "HNSL"
constexpr uint32_t kRtcSlot = 0;              // slots 0..3, the WiFi module uses 8+

struct LongSleep {
    uint32_t magic;
    uint32_t remainingSeconds;
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

uint32_t maxChunkSeconds() {
    // deepSleepMax() depends on the RTC clock calibration; keep a safety margin.
    return static_cast<uint32_t>(ESP.deepSleepMax() / 1000000ULL * 95 / 100);
}

[[noreturn]] void sleepChunk(uint32_t total) {
    uint32_t chunk = total > maxChunkSeconds() ? maxChunkSeconds() : total;
    uint32_t rest = total - chunk;
    writeState(rest);
    status::flush();
    // Only the wake-up that measures needs the radio calibrated.
    ESP.deepSleep(uint64_t(chunk) * 1000000ULL, rest ? WAKE_RF_DISABLED : WAKE_RF_DEFAULT);
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

uint32_t sleepSeconds(uint32_t intervalSeconds, uint32_t awakeMs) {
    uint32_t awake = awakeMs / 1000;
    return intervalSeconds > awake + 1 ? intervalSeconds - awake : 1;
}

void resumeLongSleep(const Config& cfg) {
    if (cfg.mode != SleepMode::DeepSleep || strcmp(wakeReason(), "TIMER") != 0) {
        writeState(0);
        return;
    }
    LongSleep state = readState();
    if (state.remainingSeconds > 0) sleepChunk(state.remainingSeconds);
}

void lightSleep(const Config& cfg, uint32_t seconds) {
    status::line("SLEEP LIGHT %lu", (unsigned long)seconds);
    status::flush();
    // Forced light sleep: radio off, CPU halted until the timer fires. The SDK timer is limited
    // to about 268 s per call, so longer waits are split.
    wifi_set_opmode_current(NULL_MODE);
    wifi_fpm_set_sleep_type(LIGHT_SLEEP_T);
    wifi_fpm_open();
    uint32_t left = seconds;
    while (left > 0) {
        uint32_t chunk = left > 260 ? 260 : left;
        wifi_fpm_do_sleep(chunk * 1000000UL);
        delay(chunk * 1000UL + 1);  // the CPU sleeps inside this delay
        left -= chunk;
    }
    wifi_fpm_close();
    (void)cfg;
}

void deepSleep(const Config& cfg, uint32_t seconds) {
    status::line("SLEEP DEEP %lu", (unsigned long)seconds);
    (void)cfg;
    sleepChunk(seconds);
}

}  // namespace hn::power

#endif  // ESP8266
