// HydroNode universal firmware.
//
// Boot → read the "hncfg" partition → start the configured sensors → measure,
// send, sleep according to the configured power mode. Nothing here is board
// specific; the web flasher decides everything through the config block.

#include <Arduino.h>
#include <HydroNode.h>
#include <Wire.h>

#if defined(ESP8266)
// No partition table on the ESP8266: the config lives in a fixed 8 KB area at the end of the
// (unused) file system region of the 4 MB layout. The release manifest carries the same offset.
constexpr uint32_t kConfigOffset8266 = 0x3F8000;
#else
#include <esp_partition.h>
#include <soc/soc_caps.h>
#endif

#include "config/Config.h"
#include "drivers/Driver.h"
#include "net/Net.h"
#include "power/Power.h"
#include "status/Status.h"

using namespace hn;

namespace {

#if !defined(ESP8266)
constexpr uint8_t kConfigSubtype = 0x40;
#endif
constexpr uint32_t kWifiTimeoutMs = 20000;
constexpr uint32_t kNoConfigRepeatMs = 10000;

Config cfg;
Driver* drivers[kMaxDevices] = {};
TwoWire* buses[kMaxI2cBuses] = {};
HydroNode* hydro = nullptr;
bool configOk = false;
uint32_t cycleStart = 0;
ParseResult configError = makeResult(ConfigError::Ok, "");

ParseResult loadConfig() {
    // Heap, not .bss: the ESP32-S2 and the ESP8266 have no RAM to spare for a static 8 KB buffer.
    size_t len = kHeaderSize + kMaxPayload;
#if !defined(ESP8266)
    const esp_partition_t* part =
        esp_partition_find_first(ESP_PARTITION_TYPE_DATA, esp_partition_subtype_t(kConfigSubtype), "hncfg");
    if (!part) return makeResult(ConfigError::NoPartition, "hncfg");
    if (part->size < len) len = part->size;
#endif
    uint8_t* block = static_cast<uint8_t*>(malloc(len));
    if (!block) return makeResult(ConfigError::NoPartition, "memory");
    ParseResult result = makeResult(ConfigError::NoPartition, "read");
#if defined(ESP8266)
    bool read = ESP.flashRead(kConfigOffset8266, reinterpret_cast<uint32_t*>(block), len);
#else
    bool read = esp_partition_read(part, 0, block, len) == ESP_OK;
#endif
    if (read) result = parseBlock(block, len, cfg);
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

void startSensors() {
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
        if (!drivers[i]) {
            drivers[i] = createDriver(cfg.devices[i], buses, cfg.adcRangeMv);
            if (!drivers[i]) {
                status::line("ERR SENSOR %s unknown", cfg.devices[i].driver);
                continue;
            }
        }
        bool ok = drivers[i]->begin();
        status::line("DEV %s %s", cfg.devices[i].driver, ok ? "ok" : "missing");
    }
    delay(maxWarmupMs());
}

// Reads every sensor, then sends. Reading first keeps the radio off while the
// sensors settle, which matters for self-heating DHT/SHT sensors too.
void measureAndSend() {
    Reading readings[kMaxDevices][kMaxChannels];
    for (uint8_t i = 0; i < cfg.deviceCount; i++) {
        for (uint8_t c = 0; c < kMaxChannels; c++) readings[i][c] = {nullptr, 0, false};
        if (drivers[i]) drivers[i]->read(readings[i]);
    }

    if (!net::connected() && !net::connect(cfg, kWifiTimeoutMs)) return;
    if (!hydro) {
        hydro = new HydroNode(cfg.sensorId, cfg.secret, cfg.host);
        hydro->begin();
    }

    for (uint8_t i = 0; i < cfg.deviceCount; i++) {
        for (uint8_t c = 0; c < cfg.devices[i].channelCount; c++) {
            const Reading& r = readings[i][c];
            if (!r.type) continue;
            if (!r.ok) {
                status::line("ERR SENSOR %s %s", cfg.devices[i].driver, cfg.devices[i].channels[c].type);
                continue;
            }
            int code = hydro->sendValue(r.type, r.value);
            status::line("SEND %s %d", r.type, code);
            if (code == 401 || code == 403) status::line("ERR AUTH %d", code);
        }
    }
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
    if (!configOk) {
        printConfigError();
        return;
    }
    // ESP8266 intervals longer than one deep sleep: intermediate wake-ups end here.
    power::resumeLongSleep(cfg);
    status::line("CFG ok board=%s devices=%u mode=%s interval=%lu", cfg.board, cfg.deviceCount,
                 sleepModeName(cfg.mode), (unsigned long)cfg.intervalSeconds);

    startSensors();
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

    measureAndSend();
    uint32_t awakeMs = millis() - cycleStart;
    uint32_t sleepFor = power::sleepSeconds(cfg.intervalSeconds, awakeMs);

    switch (cfg.mode) {
        case SleepMode::AlwaysOn:
        case SleepMode::ModemSleep:
            delay(uint64_t(sleepFor) * 1000);
            cycleStart = millis();
            break;

        case SleepMode::LightSleep:
            net::off();
            power::sensorsOff(cfg);
            power::lightSleep(cfg, sleepFor);
            cycleStart = millis();
            if (cfg.sensorPowerPin >= 0) {
                startSensors();  // sensors lost power, initialise again
            } else {
                delay(maxWarmupMs());
            }
            break;

        case SleepMode::DeepSleep:
        case SleepMode::Hibernate:
            net::off();
            power::sensorsOff(cfg);
            power::deepSleep(cfg, sleepFor);
    }
}
