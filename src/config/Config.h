#pragma once

// Device config as written by the web flasher into the "hncfg" partition.
//
// Block layout (all integers little endian):
//   0   4  magic "HNC1"
//   4   2  schema version (1)
//   6   2  flags (reserved, 0)
//   8   4  payload length in bytes
//   12  4  CRC32 (IEEE, as zlib) over the payload
//   16  n  payload: UTF-8 JSON
// The rest of the partition is 0xFF.
//
// This file must not include Arduino headers: it is compiled for the host in
// the native unit tests, and its byte format is shared with the web encoder
// (catalog/fixtures/*).

#include <stddef.h>
#include <stdint.h>

namespace hn {

constexpr uint16_t kConfigSchema = 1;
constexpr size_t kHeaderSize = 16;
constexpr size_t kMaxPayload = 8192 - kHeaderSize;
#if defined(ESP8266)
constexpr uint8_t kMaxDevices = 8;  // ~80 KB RAM, TLS needs the heap
#else
constexpr uint8_t kMaxDevices = 16;
#endif
constexpr uint8_t kMaxChannels = 8;  // per device (8 probes on one 1-Wire pin)
// All channels share one pool: a fixed 8 per device cost 5.8 KB on the ESP8266, where every
// kilobyte is missing from the TLS handshake. Mirrors `maxChannels` in catalog/boards.json.
#if defined(ESP8266)
constexpr uint8_t kMaxTotalChannels = 16;
#else
constexpr uint8_t kMaxTotalChannels = 48;
#endif
constexpr uint8_t kMaxOptions = 4;
constexpr uint8_t kMaxI2cBuses = 2;
constexpr uint32_t kMinIntervalSeconds = 10;
constexpr uint32_t kMaxIntervalSeconds = 7 * 24 * 3600;

enum class SleepMode : uint8_t { AlwaysOn, ModemSleep, LightSleep, DeepSleep, Hibernate };

enum class ConfigError : uint8_t {
    Ok,
    NoPartition,
    BadMagic,
    BadSchema,
    BadLength,
    BadCrc,
    BadJson,
    MissingField,
    BadValue,
};

struct ChannelConfig {
    char q[8];        // quantity key from the catalog ("t", "rh", ...)
    char type[65];    // HydroNode measurement type, [A-Z][A-Z0-9_]{0,63}
    int8_t index;     // position on a 1-Wire bus, -1 when addr is used
    char addr[17];    // 1-Wire ROM address as 16 hex chars, empty when unused
};

struct OptionValue {
    char key[16];
    float number;
    char text[16];
};

struct DeviceConfig {
    char driver[16];
    int8_t pin;       // data/analog pin, -1 for I2C devices
    int8_t bus;       // I2C bus index, -1 for non-I2C devices
    uint8_t address;  // I2C address, 0 when unused
    ChannelConfig* channels;  // slice of Config::channelPool
    uint8_t channelCount;
    OptionValue options[kMaxOptions];
    uint8_t optionCount;

    const OptionValue* option(const char* key) const;
    float number(const char* key, float fallback) const;
};

struct I2cBusConfig {
    int8_t sda;
    int8_t scl;
    uint32_t hz;
};

struct Config {
    char board[40];
    char sensorId[40];
    char secret[96];
    char host[64];
    char ssid[33];
    char pass[65];
    bool staticIp;
    uint8_t ip[4];
    uint8_t gateway[4];
    uint8_t subnet[4];
    uint8_t dns[4];
    uint32_t intervalSeconds;
    SleepMode mode;
    int8_t wakePin;          // -1 = timer only
    uint8_t wakeLevel;       // 0 = wake on LOW, 1 = wake on HIGH
    int8_t sensorPowerPin;   // -1 = sensors always powered
    bool fastReconnect;
    uint16_t adcRangeMv;     // ESP8266 A0: millivolts at full scale (board divider), else unused
    I2cBusConfig i2c[kMaxI2cBuses];
    uint8_t i2cCount;
    DeviceConfig devices[kMaxDevices];
    uint8_t deviceCount;
    ChannelConfig channelPool[kMaxTotalChannels];
    uint8_t channelPoolUsed;
};

struct ParseResult {
    ConfigError error;
    char detail[48];  // field name or reason, never a secret value
};

// A result with a short detail text (copied, truncated to fit).
ParseResult makeResult(ConfigError error, const char* detail);

uint32_t crc32(const uint8_t* data, size_t len);

// Checks the header and returns the payload range inside `block`.
ParseResult parseHeader(const uint8_t* block, size_t blockLen, const uint8_t** payload, size_t* payloadLen);

// Parses and validates the JSON payload into `out`.
ParseResult parsePayload(const char* json, size_t len, Config& out);

// Both steps in one.
ParseResult parseBlock(const uint8_t* block, size_t blockLen, Config& out);

const char* errorName(ConfigError error);
const char* sleepModeName(SleepMode mode);
bool isValidType(const char* type);

}  // namespace hn
