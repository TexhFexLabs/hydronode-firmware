#include <unity.h>

#include <stdio.h>
#include <string.h>

#include <string>
#include <vector>

#include "config/Config.h"

using namespace hn;

namespace {

std::vector<uint8_t> readFile(const char* path) {
    std::vector<uint8_t> data;
    FILE* f = fopen(path, "rb");
    if (!f) return data;
    uint8_t buf[512];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) data.insert(data.end(), buf, buf + n);
    fclose(f);
    return data;
}

std::vector<uint8_t> block(const std::string& json) {
    std::vector<uint8_t> b(kHeaderSize + json.size());
    memcpy(b.data(), "HNC1", 4);
    b[4] = 1; b[5] = 0; b[6] = 0; b[7] = 0;
    uint32_t len = json.size();
    uint32_t crc = crc32(reinterpret_cast<const uint8_t*>(json.data()), json.size());
    for (int i = 0; i < 4; i++) {
        b[8 + i] = uint8_t(len >> (8 * i));
        b[12 + i] = uint8_t(crc >> (8 * i));
    }
    memcpy(b.data() + kHeaderSize, json.data(), json.size());
    return b;
}

const std::string kMinimal =
    R"({"v":1,"board":"esp32-devkitc","sensor":{"id":"550e8400-e29b-41d4-a716-446655440000","secret":"s"},)"
    R"("wifi":{"ssid":"net","pass":""},"interval":60,"devices":[{"drv":"dht","pin":4,"ch":[{"q":"t","type":"TEMPERATURE"}]}]})";

std::string replace(std::string s, const std::string& from, const std::string& to) {
    size_t at = s.find(from);
    TEST_ASSERT_TRUE_MESSAGE(at != std::string::npos, from.c_str());
    return s.replace(at, from.size(), to);
}

ConfigError parse(const std::string& json, Config& cfg) {
    auto b = block(json);
    return parseBlock(b.data(), b.size(), cfg).error;
}

Config cfg;

}  // namespace

void test_crc32_matches_zlib() {
    const char* s = "123456789";
    TEST_ASSERT_EQUAL_HEX32(0xCBF43926, crc32(reinterpret_cast<const uint8_t*>(s), 9));
}

// The fixture is produced by tools/encode-config.mjs, the web encoder must match it byte for byte.
void test_golden_fixture_parses() {
    auto data = readFile("catalog/fixtures/config-v1.bin");
    TEST_ASSERT_TRUE_MESSAGE(!data.empty(), "fixture missing, run pio test from the project root");
    ParseResult r = parseBlock(data.data(), data.size(), cfg);
    TEST_ASSERT_EQUAL_STRING("", r.detail);
    TEST_ASSERT_EQUAL(ConfigError::Ok, r.error);
    TEST_ASSERT_EQUAL_STRING("esp32c3-supermini", cfg.board);
    TEST_ASSERT_EQUAL_STRING("Gärten WLAN", cfg.ssid);
    TEST_ASSERT_EQUAL_STRING("hydronode.tech", cfg.host);
    TEST_ASSERT_EQUAL(300, cfg.intervalSeconds);
    TEST_ASSERT_EQUAL(SleepMode::DeepSleep, cfg.mode);
    TEST_ASSERT_EQUAL(3, cfg.wakePin);
    TEST_ASSERT_EQUAL(10, cfg.sensorPowerPin);
    TEST_ASSERT_EQUAL(1, cfg.i2cCount);
    TEST_ASSERT_EQUAL(4, cfg.deviceCount);
    TEST_ASSERT_EQUAL_STRING("ds18b20", cfg.devices[0].driver);
    TEST_ASSERT_EQUAL(0, cfg.devices[0].channels[0].index);
    TEST_ASSERT_EQUAL_STRING("28FF641E8716045C", cfg.devices[0].channels[1].addr);
    TEST_ASSERT_EQUAL(68, cfg.devices[1].address);
    TEST_ASSERT_EQUAL(0, cfg.devices[1].bus);
    TEST_ASSERT_EQUAL_STRING("DHT22", cfg.devices[2].option("model")->text);
    TEST_ASSERT_EQUAL_FLOAT(2600, cfg.devices[3].number("dry", 0));
}

void test_minimal_config_defaults() {
    TEST_ASSERT_EQUAL(ConfigError::Ok, parse(kMinimal, cfg));
    TEST_ASSERT_EQUAL(SleepMode::AlwaysOn, cfg.mode);
    TEST_ASSERT_EQUAL(-1, cfg.wakePin);
    TEST_ASSERT_EQUAL(-1, cfg.sensorPowerPin);
    TEST_ASSERT_EQUAL_STRING("hydronode.tech", cfg.host);
    TEST_ASSERT_EQUAL_STRING("", cfg.pass);
    TEST_ASSERT_FALSE(cfg.staticIp);
}

void test_rejects_bad_header() {
    auto b = block(kMinimal);
    b[0] = 'X';
    TEST_ASSERT_EQUAL(ConfigError::BadMagic, parseBlock(b.data(), b.size(), cfg).error);

    b = block(kMinimal);
    b[4] = 2;
    TEST_ASSERT_EQUAL(ConfigError::BadSchema, parseBlock(b.data(), b.size(), cfg).error);

    b = block(kMinimal);
    b[kHeaderSize + 3] ^= 1;
    TEST_ASSERT_EQUAL(ConfigError::BadCrc, parseBlock(b.data(), b.size(), cfg).error);

    b = block(kMinimal);
    TEST_ASSERT_EQUAL(ConfigError::BadLength, parseBlock(b.data(), b.size() - 1, cfg).error);

    std::vector<uint8_t> erased(8192, 0xFF);
    TEST_ASSERT_EQUAL(ConfigError::BadMagic, parseBlock(erased.data(), erased.size(), cfg).error);
}

void test_rejects_invalid_values() {
    TEST_ASSERT_EQUAL(ConfigError::BadValue, parse(replace(kMinimal, "\"interval\":60", "\"interval\":5"), cfg));
    TEST_ASSERT_EQUAL(ConfigError::BadValue, parse(replace(kMinimal, "\"TEMPERATURE\"", "\"temperature\""), cfg));
    TEST_ASSERT_EQUAL(ConfigError::MissingField, parse(replace(kMinimal, "\"ssid\":\"net\"", "\"ssid\":\"\""), cfg));
    TEST_ASSERT_EQUAL(ConfigError::MissingField, parse(replace(kMinimal, "\"secret\":\"s\"", "\"secret\":\"\""), cfg));
    TEST_ASSERT_EQUAL(ConfigError::BadValue, parse(replace(kMinimal, "\"pin\":4", "\"pin\":99"), cfg));
    TEST_ASSERT_EQUAL(ConfigError::BadValue,
                      parse(replace(kMinimal, "\"interval\":60", "\"interval\":60,\"power\":{\"mode\":\"TURBO\"}"), cfg));
    // A wake pin in a mode that never sleeps is a configuration mistake.
    TEST_ASSERT_EQUAL(ConfigError::BadValue,
                      parse(replace(kMinimal, "\"interval\":60", "\"interval\":60,\"power\":{\"mode\":\"ALWAYS_ON\",\"wakePin\":3}"), cfg));
    // Hibernate powers down RTC memory: no pin wake, no fast reconnect.
    TEST_ASSERT_EQUAL(ConfigError::BadValue,
                      parse(replace(kMinimal, "\"interval\":60", "\"interval\":60,\"power\":{\"mode\":\"HIBERNATE\",\"wakePin\":3}"), cfg));
    // I2C device must reference an existing bus.
    TEST_ASSERT_EQUAL(ConfigError::BadValue,
                      parse(replace(kMinimal, "\"pin\":4", "\"bus\":0,\"addr\":68"), cfg));
    TEST_ASSERT_EQUAL(ConfigError::BadJson, parse("{not json", cfg));
}

void test_hibernate_disables_fast_reconnect() {
    TEST_ASSERT_EQUAL(ConfigError::Ok,
                      parse(replace(kMinimal, "\"interval\":60", "\"interval\":600,\"power\":{\"mode\":\"HIBERNATE\"}"), cfg));
    TEST_ASSERT_FALSE(cfg.fastReconnect);
}

void test_static_ip() {
    std::string json = replace(kMinimal, "\"pass\":\"\"",
                               "\"pass\":\"x\",\"ip\":{\"address\":\"192.168.1.50\",\"gateway\":\"192.168.1.1\",\"subnet\":\"255.255.255.0\"}");
    TEST_ASSERT_EQUAL(ConfigError::Ok, parse(json, cfg));
    TEST_ASSERT_TRUE(cfg.staticIp);
    TEST_ASSERT_EQUAL(50, cfg.ip[3]);
    TEST_ASSERT_EQUAL(1, cfg.dns[3]);  // falls back to the gateway
    TEST_ASSERT_EQUAL(ConfigError::BadValue, parse(replace(json, "192.168.1.50", "192.168.1.500"), cfg));
}

void test_type_rule_matches_backend() {
    TEST_ASSERT_TRUE(isValidType("TEMPERATURE2"));
    TEST_ASSERT_TRUE(isValidType("SOIL_MOISTURE"));
    TEST_ASSERT_FALSE(isValidType("2TEMP"));
    TEST_ASSERT_FALSE(isValidType("TEMP-1"));
    TEST_ASSERT_FALSE(isValidType(""));
    std::string longest(64, 'A');
    TEST_ASSERT_TRUE(isValidType(longest.c_str()));
    TEST_ASSERT_FALSE(isValidType((longest + "A").c_str()));
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_crc32_matches_zlib);
    RUN_TEST(test_golden_fixture_parses);
    RUN_TEST(test_minimal_config_defaults);
    RUN_TEST(test_rejects_bad_header);
    RUN_TEST(test_rejects_invalid_values);
    RUN_TEST(test_hibernate_disables_fast_reconnect);
    RUN_TEST(test_static_ip);
    RUN_TEST(test_type_rule_matches_backend);
    return UNITY_END();
}
