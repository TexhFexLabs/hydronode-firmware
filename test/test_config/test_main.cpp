#include <unity.h>

#include <stdio.h>
#include <string.h>

#include <string>
#include <vector>

#include <ArduinoJson.h>

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
    // An ADC range past 16 bits must not wrap into the valid range (70000 would be 4464).
    TEST_ASSERT_EQUAL(ConfigError::BadValue, parse(replace(kMinimal, "\"interval\":60", "\"interval\":60,\"adcMv\":70000"), cfg));
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

// Channels share one pool; each device gets its own slice, and the pool has a hard limit.
void test_channels_share_one_pool() {
    std::string devs;
    for (int i = 0; i < 6; i++) {
        if (i) devs += ",";
        devs += R"({"drv":"bme280","bus":0,"addr":118,"ch":[{"q":"t","type":"T)" + std::to_string(i) +
                R"("},{"q":"rh","type":"H)" + std::to_string(i) + R"("},{"q":"p","type":"P)" + std::to_string(i) + R"("}]})";
    }
    std::string json = replace(kMinimal, R"({"drv":"dht","pin":4,"ch":[{"q":"t","type":"TEMPERATURE"}]})", devs);
    json = replace(json, R"("interval":60,)", R"("interval":60,"i2c":[{"sda":21,"scl":22}],)");
    TEST_ASSERT_EQUAL(ConfigError::Ok, parse(json, cfg));
    TEST_ASSERT_EQUAL(18, cfg.channelPoolUsed);
    TEST_ASSERT_EQUAL_STRING("H0", cfg.devices[0].channels[1].type);
    TEST_ASSERT_EQUAL_STRING("P5", cfg.devices[5].channels[2].type);
    TEST_ASSERT_TRUE(cfg.devices[1].channels == cfg.devices[0].channels + 3);

    std::string many = devs;
    for (int i = 6; i < 16; i++) {
        many += R"(,{"drv":"bme280","bus":0,"addr":118,"ch":[{"q":"t","type":"T)" + std::to_string(i) +
                R"("},{"q":"rh","type":"H)" + std::to_string(i) + R"("},{"q":"p","type":"P)" + std::to_string(i) + R"("}]})";
    }
    std::string tooMany = replace(json, devs, many);  // 48 channels fit exactly on the ESP32 build
    TEST_ASSERT_EQUAL(ConfigError::Ok, parse(tooMany, cfg));
    tooMany = replace(tooMany, R"("type":"P15"}]})", R"("type":"P15"},{"q":"x","type":"EXTRA"}]})");
    TEST_ASSERT_EQUAL(ConfigError::BadValue, parse(tooMany, cfg));
}

void test_every_and_outputs() {
    // Default: every value every round.
    TEST_ASSERT_EQUAL(ConfigError::Ok, parse(kMinimal, cfg));
    TEST_ASSERT_EQUAL_UINT16(1, cfg.devices[0].channels[0].every);
    TEST_ASSERT_EQUAL_INT8(-1, cfg.devices[0].pin2);

    // Per value: every 3rd round.
    TEST_ASSERT_EQUAL(ConfigError::Ok, parse(replace(kMinimal, "\"type\":\"TEMPERATURE\"", "\"type\":\"TEMPERATURE\",\"n\":3"), cfg));
    TEST_ASSERT_EQUAL_UINT16(3, cfg.devices[0].channels[0].every);
    TEST_ASSERT_EQUAL(ConfigError::BadValue, parse(replace(kMinimal, "\"type\":\"TEMPERATURE\"", "\"type\":\"TEMPERATURE\",\"n\":0"), cfg));
    TEST_ASSERT_EQUAL(ConfigError::BadValue, parse(replace(kMinimal, "\"type\":\"TEMPERATURE\"", "\"type\":\"TEMPERATURE\",\"n\":70000"), cfg));

    // A relay sends nothing and carries text and true/false options.
    const std::string relay =
        R"({"drv":"relay","pin":5,"opt":{"cmd":"relay1","active":"LOW","start":"OFF","toggle":true,"timed":false,"dim":0},"ch":[]})";
    TEST_ASSERT_EQUAL(ConfigError::Ok, parse(replace(kMinimal, "\"devices\":[", std::string("\"devices\":[") + relay + ","), cfg));
    const DeviceConfig& d = cfg.devices[0];
    TEST_ASSERT_EQUAL_STRING("relay", d.driver);
    TEST_ASSERT_EQUAL_UINT8(0, d.channelCount);
    TEST_ASSERT_EQUAL_UINT8(6, d.optionCount);
    TEST_ASSERT_EQUAL_STRING("relay1", d.text("cmd", "x"));
    TEST_ASSERT_EQUAL_FLOAT(1, d.number("toggle", -1));
    TEST_ASSERT_EQUAL_FLOAT(0, d.number("timed", -1));
    TEST_ASSERT_EQUAL_STRING("fallback", d.text("missing", "fallback"));

    // The WiFi signal needs no pin; any other device does.
    TEST_ASSERT_EQUAL(ConfigError::Ok, parse(replace(kMinimal, "\"drv\":\"dht\",\"pin\":4", "\"drv\":\"wifi\""), cfg));
    TEST_ASSERT_EQUAL(ConfigError::MissingField, parse(replace(kMinimal, "\"pin\":4,", ""), cfg));

    // HC-SR04: trigger and echo pin.
    TEST_ASSERT_EQUAL(ConfigError::Ok, parse(replace(kMinimal, "\"pin\":4", "\"pin\":4,\"pin2\":5"), cfg));
    TEST_ASSERT_EQUAL_INT8(5, cfg.devices[0].pin2);
    TEST_ASSERT_EQUAL(ConfigError::BadValue, parse(replace(kMinimal, "\"pin\":4", "\"pin\":4,\"pin2\":99"), cfg));
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

// Firmware 0.8.0: the battery block, pinned like config-v1 (catalog/fixtures/config-v1-battery.*).
void test_battery_fixture_parses() {
    auto data = readFile("catalog/fixtures/config-v1-battery.bin");
    TEST_ASSERT_TRUE_MESSAGE(!data.empty(), "fixture missing, run pio test from the project root");
    TEST_ASSERT_TRUE(data.size() <= kHeaderSize + kMaxPayload);
    ParseResult r = parseBlock(data.data(), data.size(), cfg);
    TEST_ASSERT_EQUAL_STRING("", r.detail);
    TEST_ASSERT_EQUAL(ConfigError::Ok, r.error);
    const BatteryConfig& b = cfg.battery;
    TEST_ASSERT_TRUE(b.present);
    TEST_ASSERT_EQUAL(PowerSource::Solar, b.source);
    TEST_ASSERT_EQUAL(Chemistry::LiPo, b.chemistry);
    TEST_ASSERT_EQUAL(1, b.cells);
    TEST_ASSERT_EQUAL(2000, b.capacityMah);
    TEST_ASSERT_TRUE(b.thresholds);
    TEST_ASSERT_EQUAL(3500, b.saveMv);
    TEST_ASSERT_EQUAL(3300, b.recoveryMv);
    TEST_ASSERT_EQUAL(3200, b.standbyMv);
    TEST_ASSERT_EQUAL(3600, b.resumeMv);
    TEST_ASSERT_EQUAL(7, b.rev);
    TEST_ASSERT_EQUAL_STRING("max1704x", cfg.devices[1].driver);
    TEST_ASSERT_EQUAL(4, cfg.devices[1].channels[2].every);
}

void test_battery_block_defaults_and_errors() {
    TEST_ASSERT_EQUAL(ConfigError::Ok, parse(kMinimal, cfg));
    TEST_ASSERT_FALSE(cfg.battery.present);
    TEST_ASSERT_EQUAL(PowerSource::Usb, cfg.battery.source);
    TEST_ASSERT_EQUAL(1, cfg.battery.cells);

    std::string usb = replace(kMinimal, "\"interval\":60", "\"interval\":60,\"battery\":{\"src\":\"usb\"}");
    TEST_ASSERT_EQUAL(ConfigError::Ok, parse(usb, cfg));
    TEST_ASSERT_TRUE(cfg.battery.present);
    TEST_ASSERT_FALSE(cfg.battery.thresholds);

    // Defaults: battery source, one cell, no revision.
    std::string bare = replace(kMinimal, "\"interval\":60", "\"interval\":60,\"battery\":{}");
    TEST_ASSERT_EQUAL(ConfigError::Ok, parse(bare, cfg));
    TEST_ASSERT_EQUAL(PowerSource::Battery, cfg.battery.source);
    TEST_ASSERT_EQUAL(0, cfg.battery.rev);

    auto with = [](const char* block) {
        return replace(kMinimal, "\"interval\":60", std::string("\"interval\":60,\"battery\":") + block);
    };
    TEST_ASSERT_EQUAL(ConfigError::BadValue, parse(with(R"({"src":"mains"})"), cfg));
    TEST_ASSERT_EQUAL(ConfigError::BadValue, parse(with(R"({"chem":"lead"})"), cfg));
    TEST_ASSERT_EQUAL(ConfigError::BadValue, parse(with(R"({"cells":0})"), cfg));
    TEST_ASSERT_EQUAL(ConfigError::BadValue, parse(with(R"({"cells":17})"), cfg));
    TEST_ASSERT_EQUAL(ConfigError::BadValue, parse(with(R"({"mah":0})"), cfg));
    TEST_ASSERT_EQUAL(ConfigError::MissingField, parse(with(R"({"save":3500,"rec":3300})"), cfg));
    ParseResult r = makeResult(ConfigError::Ok, "");
    auto b = block(with(R"({"save":3500,"rec":3300,"sby":3251,"res":3600})"));
    r = parseBlock(b.data(), b.size(), cfg);
    TEST_ASSERT_EQUAL(ConfigError::BadValue, r.error);
    TEST_ASSERT_EQUAL_STRING("battery.sby", r.detail);
    // 2S LiFePO4 preset.
    TEST_ASSERT_EQUAL(ConfigError::Ok, parse(with(R"({"chem":"lifepo4","cells":2,"save":6200,"rec":6000,"sby":5600,"res":6400})"), cfg));
    // Custom chemistry: only the gaps.
    TEST_ASSERT_EQUAL(ConfigError::Ok, parse(with(R"({"chem":"custom","save":2400,"rec":2300,"sby":2200,"res":2450})"), cfg));
}

// The rule table backend, web, library, station and firmware share (test/vectors/, a copy of
// hydronode-backend/src/test/resources/devicesettings/threshold-rules-vectors.json).
void test_threshold_rules_match_the_shared_table() {
    auto data = readFile("test/vectors/threshold-rules-vectors.json");
    TEST_ASSERT_TRUE_MESSAGE(!data.empty(), "vectors missing, run pio test from the project root");
    JsonDocument doc;
    TEST_ASSERT_FALSE(deserializeJson(doc, data.data(), data.size()));
    int checked = 0;
    for (JsonObjectConst c : doc["cases"].as<JsonArrayConst>()) {
        const char* chem = c["chemistry"] | "";
        Chemistry chemistry = strcmp(chem, "LI_ION") == 0 ? Chemistry::LiIon
                              : strcmp(chem, "LIFEPO4") == 0 ? Chemistry::LiFePO4
                              : strcmp(chem, "LIPO") == 0 ? Chemistry::LiPo
                                                          : Chemistry::Unknown;
        uint8_t broken = checkThresholds(c["save"], c["recovery"], c["standby"], c["resume"], c["cells"], chemistry);
        uint8_t expected = 0;
        for (const char* field : c["expected"].as<JsonArrayConst>()) {
            if (strcmp(field, "save") == 0) expected |= 1;
            if (strcmp(field, "recovery") == 0) expected |= 2;
            if (strcmp(field, "standby") == 0) expected |= 4;
            if (strcmp(field, "resume") == 0) expected |= 8;
        }
        TEST_ASSERT_EQUAL_HEX8_MESSAGE(expected, broken, c["name"].as<const char*>());
        checked++;
    }
    TEST_ASSERT_EQUAL(16, checked);
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
    RUN_TEST(test_channels_share_one_pool);
    RUN_TEST(test_every_and_outputs);
    RUN_TEST(test_type_rule_matches_backend);
    RUN_TEST(test_battery_fixture_parses);
    RUN_TEST(test_battery_block_defaults_and_errors);
    RUN_TEST(test_threshold_rules_match_the_shared_table);
    return UNITY_END();
}
