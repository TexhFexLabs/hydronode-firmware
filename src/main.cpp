// HydroNode universal firmware.
//
// Boot → read the "hncfg" partition → start the configured sensors → measure,
// send, sleep according to the configured power mode. Nothing here is board
// specific; the web flasher decides everything through the config block.

#include <Arduino.h>
#include <HydroNode.h>
#include <Wire.h>

#if !defined(ESP8266)
#include <soc/soc_caps.h>
#endif

#include "actuators/Actuators.h"
#include "config/Config.h"
#include "config/ConfigStore.h"
#include "drivers/Driver.h"
#include "net/Net.h"
#include "ota/Ota.h"
#include "power/Power.h"
#include "status/Status.h"

using namespace hn;

namespace {

constexpr uint32_t kWifiTimeoutMs = 20000;
constexpr uint32_t kNoConfigRepeatMs = 10000;

Config cfg;
Driver* drivers[kMaxDevices] = {};
TwoWire* buses[kMaxI2cBuses] = {};
HydroNode* hydro = nullptr;
bool configOk = false;
uint32_t cycleStart = 0;
uint32_t roundIndex = 0;  // measuring rounds since reset; a value with every = n goes out when roundIndex % n == 0
// Internet time (ms) the first round started at. Sleeping rounds are aligned to it, so the drift of
// the sleep timer (the ESP8266 light sleep runs up to 5 % short) never adds up. 0 = not known yet.
uint64_t anchorMs = 0;
// ESP8266 light sleep stops the clock: the next round fetches the time again before signing.
bool clockStopped = false;
ParseResult configError = makeResult(ConfigError::Ok, "");

ParseResult loadConfig() {
    // Heap, not .bss: the ESP32-S2 and the ESP8266 have no RAM to spare for a static 8 KB buffer.
    size_t len = kHeaderSize + kMaxPayload;
    uint8_t* block = static_cast<uint8_t*>(malloc(len));
    if (!block) return makeResult(ConfigError::NoPartition, "memory");
    ParseResult result = makeResult(ConfigError::NoPartition, "hncfg");
    if (store::readConfigBlock(block, len)) result = parseBlock(block, len, cfg);
    memset(block, 0, len);  // the block holds the WiFi password
    free(block);
    return result;
}

uint32_t maxWarmupMs() {
    uint32_t ms = 0;
    for (uint8_t i = 0; i < cfg.deviceCount; i++) {
        if (drivers[i] && drivers[i]->warmupMs() > ms) ms = drivers[i]->warmupMs();
    }
    return ms;
}

bool restartsEachCycle() { return cfg.mode == SleepMode::DeepSleep || cfg.mode == SleepMode::Hibernate; }

bool anyContinuous() {
    for (uint8_t i = 0; i < cfg.deviceCount; i++) {
        if (drivers[i] && drivers[i]->continuous()) return true;
    }
    return false;
}

bool connectedForSend() { return hydro && net::connected(); }

// Button presses go out right away, between rounds.
void sendPresses() {
    for (const char* type = act::takePress(); type; type = act::takePress()) {
        if (!connectedForSend()) continue;  // no connection: the press still toggled locally
        int code = hydro->sendValue(type, 1);
        ota::afterSend(code);
        status::line("SEND %s %d", type, code);
        hydro->closeConnection();
    }
}

// Waits `ms` awake. Meanwhile: one sample per second for continuous gas sensors, timed outputs
// switch off on time, buttons are read every 10 ms.
void idle(uint32_t ms) {
    bool ticking = anyContinuous();
    if (!ticking && !act::busy()) {
        delay(ms);
        return;
    }
    uint32_t start = millis();
    uint32_t nextTick = start;
    while (millis() - start < ms) {
        uint32_t now = millis();
        if (ticking && int32_t(now - nextTick) >= 0) {
            for (uint8_t i = 0; i < cfg.deviceCount; i++) {
                if (drivers[i] && drivers[i]->continuous()) drivers[i]->tick();
            }
            nextTick += 1000;
            if (int32_t(millis() - nextTick) >= 0) nextTick = millis() + 1000;  // fell behind
        }
        act::service();
        sendPresses();
        uint32_t left = ms - (millis() - start);
        if (int32_t(left) <= 0) break;
        uint32_t step = left;
        if (act::busy()) step = min<uint32_t>(step, 10);
        if (ticking) step = min<uint32_t>(step, max<int32_t>(int32_t(nextTick - millis()), 1));
        delay(step);
    }
}

// `warm`: the sensors kept their supply since the last cycle (timer wake-up without a sensor
// power pin), so the power-up warm-up can be skipped. Saves up to 5 s awake per wake-up.
void startSensors(bool warm = false) {
    power::sensorsOn(cfg);
    for (uint8_t i = 0; i < cfg.i2cCount; i++) {
        if (!buses[i]) {
#if defined(ESP8266)
            buses[i] = i == 0 ? &Wire : nullptr;  // one software I2C bus
            if (!buses[i]) {
                status::line("ERR CONFIG BAD_VALUE i2c.second_bus");
                continue;
            }
#elif SOC_HP_I2C_NUM > 1
            buses[i] = i == 0 ? &Wire : &Wire1;
#else
            buses[i] = i == 0 ? &Wire : nullptr;  // C3/C6 have one I2C controller
            if (!buses[i]) {
                status::line("ERR CONFIG BAD_VALUE i2c.second_bus");
                continue;
            }
#endif
        }
#if defined(ESP8266)
        buses[i]->begin(cfg.i2c[i].sda, cfg.i2c[i].scl);
        buses[i]->setClock(cfg.i2c[i].hz);
#else
        buses[i]->begin(cfg.i2c[i].sda, cfg.i2c[i].scl, cfg.i2c[i].hz);
#endif
    }
    // Sensors that were just powered up need their warm-up before begin().
    if (cfg.sensorPowerPin >= 0) delay(50);
    for (uint8_t i = 0; i < cfg.deviceCount; i++) {
        if (act::isActuator(cfg.devices[i].driver)) continue;  // set up once in setup()
        if (!drivers[i]) {
            drivers[i] = createDriver(cfg.devices[i], buses, cfg.adcRangeMv, restartsEachCycle());
            if (!drivers[i]) {
                status::line("ERR SENSOR %s unknown", cfg.devices[i].driver);
                continue;
            }
        }
        bool ok = drivers[i]->begin();
        status::line("DEV %s %s", cfg.devices[i].driver, ok ? "ok" : "missing");
    }
    if (!warm) delay(maxWarmupMs());
}

// Gas sensors compensate with the temperature and humidity another sensor measured.
void updateAmbient(const Reading readings[][kMaxChannels]) {
    for (uint8_t i = 0; i < cfg.deviceCount; i++) {
        if (!drivers[i] || drivers[i]->continuous()) continue;
        for (uint8_t c = 0; c < cfg.devices[i].channelCount; c++) {
            const Reading& r = readings[i][c];
            if (!r.ok) continue;
            if (strcmp(cfg.devices[i].channels[c].q, "t") == 0) ambient().t = r.value;
            if (strcmp(cfg.devices[i].channels[c].q, "rh") == 0) ambient().rh = r.value;
        }
    }
}

bool due(const ChannelConfig& ch) { return roundIndex % (ch.every ? ch.every : 1) == 0; }

bool deviceDue(uint8_t i) {
    for (uint8_t c = 0; c < cfg.devices[i].channelCount; c++) {
        if (due(cfg.devices[i].channels[c])) return true;
    }
    return false;
}

void readDevice(uint8_t i, Reading readings[][kMaxChannels]) {
    for (uint8_t c = 0; c < kMaxChannels; c++) readings[i][c] = {nullptr, 0, false};
    if (drivers[i] && deviceDue(i)) drivers[i]->read(readings[i]);
}

// Reads every sensor that has a value due this round, then sends. Reading first keeps the radio
// off while the sensors settle, which matters for self-heating DHT/SHT sensors too. A round with
// nothing due does not connect at all. The report tells an update in verification how it went.
ota::RoundReport measureAndSend() {
    ota::RoundReport report{false, 0, nullptr};
    Reading readings[kMaxDevices][kMaxChannels] = {};
    bool anyDue = false;
    for (uint8_t i = 0; i < cfg.deviceCount; i++) {
        anyDue = anyDue || (drivers[i] && deviceDue(i));
        if (!drivers[i] || !drivers[i]->afterConnect()) readDevice(i, readings);
    }
    updateAmbient(readings);
    status::line("ROUND %lu", (unsigned long)roundIndex);
    if (!anyDue) return report;

    if (!net::connected() && !net::connect(cfg, kWifiTimeoutMs)) return report;
    report.wifiOk = true;
    if (!hydro) {
        hydro = new HydroNode(cfg.sensorId, cfg.secret, cfg.host);
        hydro->begin();
        act::attach(*hydro);
        ota::attach(*hydro, cfg);
    } else if (clockStopped) {
        clockStopped = !hydro->syncTime();
    }
    // Values that only exist with a connection (WiFi signal).
    for (uint8_t i = 0; i < cfg.deviceCount; i++) {
        if (drivers[i] && drivers[i]->afterConnect()) readDevice(i, readings);
    }

    // Drivers that failed this round travel with every value (X-Device-Status readErr=…).
    hydro->clearReadErrors();
    for (uint8_t i = 0; i < cfg.deviceCount; i++) {
        for (uint8_t c = 0; c < cfg.devices[i].channelCount; c++) {
            const Reading& r = readings[i][c];
            if (!due(cfg.devices[i].channels[c]) || act::isActuator(cfg.devices[i].driver)) continue;
            // Only confirmed communication with a warming algorithm is exempt. Missing drivers
            // and failed gas sensor initialization must block Strict verification too.
            if (!r.ok && !r.warming) {
                hydro->reportReadError(cfg.devices[i].driver);
                if (!report.failedDriver) report.failedDriver = cfg.devices[i].driver;
            }
        }
    }

    for (uint8_t i = 0; i < cfg.deviceCount; i++) {
        for (uint8_t c = 0; c < cfg.devices[i].channelCount; c++) {
            const Reading& r = readings[i][c];
            if (!r.type || !due(cfg.devices[i].channels[c])) continue;
            if (!r.ok) {
                bool settling = r.warming;
                status::line("%s SENSOR %s %s", settling ? "WAIT" : "ERR", cfg.devices[i].driver,
                             cfg.devices[i].channels[c].type);
                continue;
            }
            int code = hydro->sendValue(r.type, r.value);
            ota::afterSend(code);
            status::line("SEND %s %d", r.type, code);
            if (code == 401 || code == 403) status::line("ERR AUTH %d", code);
            if (code >= 200 && code < 300) report.bestStatus = code;
            else if (report.bestStatus < 200 || report.bestStatus >= 300) report.bestStatus = code;
            act::service();  // a command in the answer may have started a short pulse
        }
    }
    // All values of a round share one TLS connection; do not hold its buffers until the next.
    hydro->closeConnection();
    return report;
}

// One round, and again while an update in verification asks for another try. Afterwards a
// waiting update offer is carried out (it may restart the board).
void runRound() {
    ota::RoundReport report = measureAndSend();
    while (ota::afterRound(hydro, cfg, report)) {
        idle(ota::kRetryPauseMs);
        report = measureAndSend();
    }
}

// Milliseconds until the next round should start. With internet time from the last send, rounds
// land on anchor + n × interval however much the sleep timer drifts (±1 s, the resolution of
// NTP here). Without it, the interval minus the time awake.
uint32_t untilNextRound() {
    uint32_t fallback = power::sleepMs(cfg.intervalSeconds, millis() - cycleStart);
    // A round that sent nothing did not fetch the time; after a stopped clock it would be off.
    uint64_t now = hydro && !clockStopped ? hydro->epochMs() : 0;
    if (now == 0) return fallback;
    uint64_t interval = uint64_t(cfg.intervalSeconds) * 1000;
    if (anchorMs == 0 || anchorMs > now || now - anchorMs > 30ULL * 24 * 3600 * 1000) {
        anchorMs = now - (millis() - cycleStart);  // this round's start
    }
    uint64_t next = anchorMs + ((now - anchorMs) / interval + 1) * interval;
    uint64_t wait = next - now;
    return wait < 1000 ? 1000 : uint32_t(wait);
}

void printConfigError() {
    status::line("ERR CONFIG %s %s", errorName(configError.error), configError.detail);
}

}  // namespace

void setup() {
    cycleStart = millis();
    status::begin();
    delay(50);
    status::line("BOOT fw=%s family=%s wake=%s heap=%u", HN_FW_VERSION, HN_FAMILY, power::wakeReason(),
                 (unsigned)ESP.getFreeHeap());

    configError = loadConfig();
    configOk = configError.error == ConfigError::Ok;
    // An update waiting for its verdict; a new config that does not parse goes back to the old one.
    ota::begin(cfg, configError);
    if (!configOk) {
        printConfigError();
        return;
    }
    // ESP8266 intervals longer than one deep sleep: intermediate wake-ups end here.
    power::resumeLongSleep(cfg);
    bool woken = strcmp(power::wakeReason(), "TIMER") == 0 || strcmp(power::wakeReason(), "PIN") == 0;
    power::Rounds rounds = power::loadRounds(!woken);
    roundIndex = rounds.index;
    anchorMs = rounds.anchorMs;
    status::line("CFG ok board=%s devices=%u mode=%s interval=%lu rev=%lu", cfg.board, cfg.deviceCount,
                 sleepModeName(cfg.mode), (unsigned long)cfg.intervalSeconds, (unsigned long)cfg.rev);

    act::begin(cfg);
    // After a timer wake-up the sensors stayed powered unless a power pin switched them off.
    startSensors(cfg.sensorPowerPin < 0 && strcmp(power::wakeReason(), "TIMER") == 0);
    if (cfg.mode == SleepMode::AlwaysOn || cfg.mode == SleepMode::ModemSleep) {
        if (net::connect(cfg, kWifiTimeoutMs)) net::setPowerSave(cfg.mode == SleepMode::ModemSleep);
    }
}

void loop() {
    if (!configOk) {
        // Keep repeating so a monitor that attaches late still sees the reason.
        delay(kNoConfigRepeatMs);
        printConfigError();
        return;
    }

    runRound();
    roundIndex++;

    switch (cfg.mode) {
        case SleepMode::AlwaysOn:
        case SleepMode::ModemSleep:
            idle(power::sleepMs(cfg.intervalSeconds, millis() - cycleStart));
            cycleStart += cfg.intervalSeconds * 1000;
            // Fell behind (a slow round): start the next one from now instead of catching up.
            if (int32_t(millis() - cycleStart) > int32_t(cfg.intervalSeconds * 1000)) cycleStart = millis();
            break;

        case SleepMode::LightSleep: {
            // A timed output has to switch off on time: stay awake until it did.
            for (uint32_t left = act::pendingMs(); left; left = act::pendingMs()) {
                delay(min<uint32_t>(left, 10));
                act::service();
            }
            uint32_t sleepFor = untilNextRound();
            net::off();
            power::sensorsOff(cfg);
            act::holdForSleep(true);
            power::lightSleep(cfg, sleepFor);
            act::holdForSleep(false);
            cycleStart = millis();
#if defined(ESP8266)
            clockStopped = true;
#endif
            // Sensors without a power pin stayed powered and settled: no warm-up needed.
            if (cfg.sensorPowerPin >= 0) startSensors();  // sensors lost power, initialise again
            break;
        }

        case SleepMode::DeepSleep:
        case SleepMode::Hibernate: {
            uint32_t sleepFor = untilNextRound();
            net::off();
            power::sensorsOff(cfg);
            power::saveRounds({roundIndex, anchorMs});
            power::deepSleep(cfg, sleepFor);
        }
    }
}
