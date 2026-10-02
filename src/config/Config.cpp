#include "Config.h"

#include <ArduinoJson.h>
#include <stdio.h>
#include <string.h>

namespace hn {

namespace {

ParseResult ok() { return makeResult(ConfigError::Ok, ""); }

ParseResult fail(ConfigError error, const char* detail) { return makeResult(error, detail); }

uint16_t readU16(const uint8_t* p) { return uint16_t(p[0] | (p[1] << 8)); }
uint32_t readU32(const uint8_t* p) {
    return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}

// Copies a required string. Fails on missing, empty or too long values.
bool copyString(JsonVariantConst v, char* dst, size_t cap, bool allowEmpty = false) {
    if (!v.is<const char*>()) return false;
    const char* s = v.as<const char*>();
    size_t len = strlen(s);
    if ((!allowEmpty && len == 0) || len >= cap) return false;
    memcpy(dst, s, len + 1);
    return true;
}

bool parseIp(JsonVariantConst v, uint8_t out[4]) {
    if (!v.is<const char*>()) return false;
    unsigned a, b, c, d;
    char tail;
    if (sscanf(v.as<const char*>(), "%u.%u.%u.%u%c", &a, &b, &c, &d, &tail) != 4) return false;
    if (a > 255 || b > 255 || c > 255 || d > 255) return false;
    out[0] = a; out[1] = b; out[2] = c; out[3] = d;
    return true;
}

bool parseMode(const char* s, SleepMode& out) {
    struct { const char* name; SleepMode mode; } modes[] = {
        {"ALWAYS_ON", SleepMode::AlwaysOn},     {"MODEM_SLEEP", SleepMode::ModemSleep},
        {"LIGHT_SLEEP", SleepMode::LightSleep}, {"DEEP_SLEEP", SleepMode::DeepSleep},
        {"HIBERNATE", SleepMode::Hibernate},
    };
    for (auto& m : modes) {
        if (strcmp(s, m.name) == 0) { out = m.mode; return true; }
    }
    return false;
}

bool isHex16(const char* s) {
    if (strlen(s) != 16) return false;
    for (const char* c = s; *c; c++) {
        bool hex = (*c >= '0' && *c <= '9') || (*c >= 'A' && *c <= 'F') || (*c >= 'a' && *c <= 'f');
        if (!hex) return false;
    }
    return true;
}

bool validPin(JsonVariantConst v, int8_t& out) {
    if (!v.is<int>()) return false;
    int pin = v.as<int>();
    if (pin < 0 || pin > 48) return false;
    out = int8_t(pin);
    return true;
}

}  // namespace

ParseResult makeResult(ConfigError error, const char* detail) {
    ParseResult r;
    r.error = error;
    memset(r.detail, 0, sizeof(r.detail));
    strncpy(r.detail, detail, sizeof(r.detail) - 1);
    return r;
}

const OptionValue* DeviceConfig::option(const char* key) const {
    for (uint8_t i = 0; i < optionCount; i++) {
        if (strcmp(options[i].key, key) == 0) return &options[i];
    }
    return nullptr;
}

float DeviceConfig::number(const char* key, float fallback) const {
    const OptionValue* o = option(key);
    return o ? o->number : fallback;
}

const char* DeviceConfig::text(const char* key, const char* fallback) const {
    const OptionValue* o = option(key);
    return o && o->text[0] ? o->text : fallback;
}

uint32_t crc32(const uint8_t* data, size_t len) {
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int k = 0; k < 8; k++) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

bool isValidType(const char* type) {
    // Mirrors the backend rule [A-Z][A-Z0-9_]{0,63}.
    if (!type || type[0] < 'A' || type[0] > 'Z') return false;
    size_t len = 1;
    for (const char* c = type + 1; *c; c++, len++) {
        bool ok = (*c >= 'A' && *c <= 'Z') || (*c >= '0' && *c <= '9') || *c == '_';
        if (!ok || len >= 64) return false;
    }
    return true;
}

ParseResult parseHeader(const uint8_t* block, size_t blockLen, const uint8_t** payload, size_t* payloadLen) {
    if (blockLen < kHeaderSize) return fail(ConfigError::BadLength, "block");
    if (memcmp(block, "HNC1", 4) != 0) return fail(ConfigError::BadMagic, "magic");
    if (readU16(block + 4) != kConfigSchema) return fail(ConfigError::BadSchema, "schema");
    uint32_t len = readU32(block + 8);
    if (len == 0 || len > kMaxPayload || len > blockLen - kHeaderSize) return fail(ConfigError::BadLength, "length");
    if (crc32(block + kHeaderSize, len) != readU32(block + 12)) return fail(ConfigError::BadCrc, "crc");
    *payload = block + kHeaderSize;
    *payloadLen = len;
    return ok();
}

ParseResult parsePayload(const char* json, size_t len, Config& out) {
    memset(&out, 0, sizeof(out));
    JsonDocument doc;
    if (deserializeJson(doc, json, len)) return fail(ConfigError::BadJson, "json");

    if (doc["v"].as<int>() != kConfigSchema) return fail(ConfigError::BadSchema, "v");
    if (!copyString(doc["board"], out.board, sizeof(out.board))) return fail(ConfigError::MissingField, "board");

    JsonObjectConst sensor = doc["sensor"];
    if (!copyString(sensor["id"], out.sensorId, sizeof(out.sensorId))) return fail(ConfigError::MissingField, "sensor.id");
    if (!copyString(sensor["secret"], out.secret, sizeof(out.secret))) return fail(ConfigError::MissingField, "sensor.secret");
    if (sensor["host"].isNull()) {
        strcpy(out.host, "hydronode.tech");
    } else if (!copyString(sensor["host"], out.host, sizeof(out.host))) {
        return fail(ConfigError::BadValue, "sensor.host");
    }

    JsonObjectConst wifi = doc["wifi"];
    if (!copyString(wifi["ssid"], out.ssid, sizeof(out.ssid))) return fail(ConfigError::MissingField, "wifi.ssid");
    // Open networks have no password.
    if (!copyString(wifi["pass"], out.pass, sizeof(out.pass), true)) return fail(ConfigError::MissingField, "wifi.pass");
    JsonObjectConst ip = wifi["ip"];
    if (!ip.isNull()) {
        out.staticIp = true;
        if (!parseIp(ip["address"], out.ip)) return fail(ConfigError::BadValue, "wifi.ip.address");
        if (!parseIp(ip["gateway"], out.gateway)) return fail(ConfigError::BadValue, "wifi.ip.gateway");
        if (!parseIp(ip["subnet"], out.subnet)) return fail(ConfigError::BadValue, "wifi.ip.subnet");
        if (!parseIp(ip["dns"].isNull() ? ip["gateway"] : ip["dns"], out.dns)) return fail(ConfigError::BadValue, "wifi.ip.dns");
    }

    if (!doc["interval"].is<uint32_t>()) return fail(ConfigError::MissingField, "interval");
    out.intervalSeconds = doc["interval"].as<uint32_t>();
    if (out.intervalSeconds < kMinIntervalSeconds || out.intervalSeconds > kMaxIntervalSeconds) {
        return fail(ConfigError::BadValue, "interval");
    }

    JsonObjectConst power = doc["power"];
    const char* mode = power["mode"] | "ALWAYS_ON";
    if (!parseMode(mode, out.mode)) return fail(ConfigError::BadValue, "power.mode");
    out.wakePin = -1;
    if (!power["wakePin"].isNull() && !validPin(power["wakePin"], out.wakePin)) return fail(ConfigError::BadValue, "power.wakePin");
    out.wakeLevel = (power["wakeLevel"] | 0) ? 1 : 0;
    out.sensorPowerPin = -1;
    if (!power["sensorPowerPin"].isNull() && !validPin(power["sensorPowerPin"], out.sensorPowerPin)) {
        return fail(ConfigError::BadValue, "power.sensorPowerPin");
    }
    out.fastReconnect = power["fastReconnect"] | true;
    if (out.mode == SleepMode::Hibernate) {
        out.fastReconnect = false;  // RTC memory is powered down
        if (out.wakePin >= 0) return fail(ConfigError::BadValue, "power.wakePin");
    }

    out.adcRangeMv = doc["adcMv"] | 3200;
    if (out.adcRangeMv < 1000 || out.adcRangeMv > 12000) return fail(ConfigError::BadValue, "adcMv");

    JsonArrayConst buses = doc["i2c"];
    if (buses.size() > kMaxI2cBuses) return fail(ConfigError::BadValue, "i2c");
    for (JsonObjectConst bus : buses) {
        I2cBusConfig& b = out.i2c[out.i2cCount++];
        if (!validPin(bus["sda"], b.sda) || !validPin(bus["scl"], b.scl) || b.sda == b.scl) {
            return fail(ConfigError::BadValue, "i2c.pins");
        }
        b.hz = bus["hz"] | 100000u;
        if (b.hz < 10000 || b.hz > 1000000) return fail(ConfigError::BadValue, "i2c.hz");
    }

    JsonArrayConst devices = doc["devices"];
    if (devices.size() == 0) return fail(ConfigError::MissingField, "devices");
    if (devices.size() > kMaxDevices) return fail(ConfigError::BadValue, "devices");
    for (JsonObjectConst dev : devices) {
        DeviceConfig& d = out.devices[out.deviceCount++];
        if (!copyString(dev["drv"], d.driver, sizeof(d.driver))) return fail(ConfigError::MissingField, "devices.drv");
        d.pin = -1;
        d.pin2 = -1;
        d.bus = -1;
        if (!dev["pin"].isNull() && !validPin(dev["pin"], d.pin)) return fail(ConfigError::BadValue, "devices.pin");
        if (!dev["pin2"].isNull() && !validPin(dev["pin2"], d.pin2)) return fail(ConfigError::BadValue, "devices.pin2");
        if (!dev["bus"].isNull()) {
            int bus = dev["bus"] | -1;
            if (bus < 0 || bus >= out.i2cCount) return fail(ConfigError::BadValue, "devices.bus");
            d.bus = int8_t(bus);
            int addr = dev["addr"] | 0;
            if (addr < 0x08 || addr > 0x77) return fail(ConfigError::BadValue, "devices.addr");
            d.address = uint8_t(addr);
        }
        // Everything but the WiFi signal is wired somewhere.
        if (d.pin < 0 && d.bus < 0 && strcmp(d.driver, "wifi") != 0) return fail(ConfigError::MissingField, "devices.pin");

        // Outputs (relays, LEDs) and a button that only switches locally send nothing.
        JsonArrayConst channels = dev["ch"];
        if (channels.size() > kMaxChannels) return fail(ConfigError::BadValue, "devices.ch");
        if (out.channelPoolUsed + channels.size() > kMaxTotalChannels) return fail(ConfigError::BadValue, "devices.ch.total");
        d.channels = &out.channelPool[out.channelPoolUsed];
        out.channelPoolUsed += channels.size();
        for (JsonObjectConst ch : channels) {
            ChannelConfig& c = d.channels[d.channelCount++];
            if (!copyString(ch["q"], c.q, sizeof(c.q))) return fail(ConfigError::MissingField, "devices.ch.q");
            if (!copyString(ch["type"], c.type, sizeof(c.type)) || !isValidType(c.type)) {
                return fail(ConfigError::BadValue, "devices.ch.type");
            }
            c.index = int8_t(ch["idx"] | -1);
            uint32_t every = ch["n"] | 1u;
            if (every < 1 || every > 65535) return fail(ConfigError::BadValue, "devices.ch.n");
            c.every = uint16_t(every);
            if (!ch["addr"].isNull()) {
                if (!copyString(ch["addr"], c.addr, sizeof(c.addr)) || !isHex16(c.addr)) {
                    return fail(ConfigError::BadValue, "devices.ch.addr");
                }
            }
        }

        JsonObjectConst opts = dev["opt"];
        for (JsonPairConst kv : opts) {
            if (d.optionCount >= kMaxOptions) return fail(ConfigError::BadValue, "devices.opt");
            OptionValue& o = d.options[d.optionCount++];
            if (strlen(kv.key().c_str()) >= sizeof(o.key)) return fail(ConfigError::BadValue, "devices.opt.key");
            strcpy(o.key, kv.key().c_str());
            if (kv.value().is<const char*>()) {
                if (!copyString(kv.value(), o.text, sizeof(o.text), true)) return fail(ConfigError::BadValue, "devices.opt.value");
            } else if (kv.value().is<bool>()) {
                o.number = kv.value().as<bool>() ? 1 : 0;
            } else if (kv.value().is<float>()) {
                o.number = kv.value().as<float>();
            } else {
                return fail(ConfigError::BadValue, "devices.opt.value");
            }
        }
    }

    // A wake pin only makes sense in modes that actually sleep.
    if (out.wakePin >= 0 && (out.mode == SleepMode::AlwaysOn || out.mode == SleepMode::ModemSleep)) {
        return fail(ConfigError::BadValue, "power.wakePin");
    }
    return ok();
}

ParseResult parseBlock(const uint8_t* block, size_t blockLen, Config& out) {
    const uint8_t* payload = nullptr;
    size_t payloadLen = 0;
    ParseResult r = parseHeader(block, blockLen, &payload, &payloadLen);
    if (r.error != ConfigError::Ok) return r;
    return parsePayload(reinterpret_cast<const char*>(payload), payloadLen, out);
}

const char* errorName(ConfigError error) {
    switch (error) {
        case ConfigError::Ok: return "OK";
        case ConfigError::NoPartition: return "NO_PARTITION";
        case ConfigError::BadMagic: return "NO_CONFIG";
        case ConfigError::BadSchema: return "BAD_SCHEMA";
        case ConfigError::BadLength: return "BAD_LENGTH";
        case ConfigError::BadCrc: return "BAD_CRC";
        case ConfigError::BadJson: return "BAD_JSON";
        case ConfigError::MissingField: return "MISSING_FIELD";
        case ConfigError::BadValue: return "BAD_VALUE";
    }
    return "UNKNOWN";
}

const char* sleepModeName(SleepMode mode) {
    switch (mode) {
        case SleepMode::AlwaysOn: return "ALWAYS_ON";
        case SleepMode::ModemSleep: return "MODEM_SLEEP";
        case SleepMode::LightSleep: return "LIGHT_SLEEP";
        case SleepMode::DeepSleep: return "DEEP_SLEEP";
        case SleepMode::Hibernate: return "HIBERNATE";
    }
    return "UNKNOWN";
}

}  // namespace hn
