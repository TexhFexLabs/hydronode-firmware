#include <unity.h>
#include <Ticker.h>
#include <esp_ota_ops.h>
#include "fakes.h"
#include "ota/ConfigMerge.h"
#include "ota/OtaLogic.h"
// Exercise the real measurement loop, not a second implementation of its error rules.
#include "../../src/main.cpp"

namespace {
const char* oldJson = R"({"v":1,"rev":1,"board":"esp32c3-devkitm","sensor":{"id":"sensor","secret":"secret"},"wifi":{"ssid":"Garden","pass":"password"},"interval":300,"power":{"mode":"ALWAYS_ON"},"devices":[{"drv":"dht","pin":4,"ch":[{"q":"t","type":"TEMPERATURE"}]}]})";
void configBlock() {
    fake::config.resize(8192);
    size_t n = ota::encodeBlock(oldJson, strlen(oldJson), fake::config.data(), fake::config.size());
    fake::config.resize(n);
    TEST_ASSERT_EQUAL(int(ConfigError::Ok), int(parseBlock(fake::config.data(), n, cfg).error));
}
void pendingConfig() {
    ota::Pending p{}; p.kind = ota::PendingKind::Config; p.fromRev = 1; p.toRev = 2; strcpy(p.job, "job");
    fake::record = p; fake::backup = fake::config;
    std::string updated(oldJson);
    updated.replace(updated.find("\"rev\":1"), 7, "\"rev\":2");
    fake::config.resize(8192);
    size_t n = ota::encodeBlock(updated.c_str(), updated.size(), fake::config.data(), fake::config.size());
    fake::config.resize(n);
    parseBlock(fake::config.data(), n, cfg);
}
template<class Fn> void restarted(Fn fn) {
    bool reset = false;
    try { fn(); } catch (Restarted&) { reset = true; }
    TEST_ASSERT_TRUE(reset);
}
class GasDriver : public Driver {
public:
    bool warming = false;
    GasDriver(const DeviceConfig& dev) : Driver(dev) {}
    bool begin() override { return false; }
    bool continuous() const override { return true; }
    void read(Reading* out) override { out[0] = {cfg_.channels[0].type, 0, false, warming}; }
};
class WifiDriver : public Driver {
public:
    WifiDriver(const DeviceConfig& dev) : Driver(dev) {}
    bool begin() override { return true; }
    bool afterConnect() const override { return true; }
    void read(Reading* out) override { out[0] = {cfg_.channels[0].type, -50, true}; }
};
}
void setUp() {
    fake::reset(); configBlock();
    ota::begin(cfg, makeResult(ConfigError::Ok, ""));
    for (auto& d : drivers) d = nullptr;
    hydro = nullptr; roundIndex = 0;
}
void tearDown() { Ticker::callback = nullptr; }
void test_interrupted_config_restoration_resumes_on_boot() {
    pendingConfig(); fake::record->result[0] = 0;
    fake::config.clear(); fake::interruptWrite = true;
    restarted([] { ota::begin(cfg, makeResult(ConfigError::BadCrc, "crc")); });
    TEST_ASSERT_EQUAL(1, fake::record->reserved);
    TEST_ASSERT_FALSE(fake::backup.empty());
    fake::interruptWrite = false;
    restarted([] { ota::begin(cfg, makeResult(ConfigError::BadCrc, "crc")); });
    TEST_ASSERT_EQUAL(2, fake::record->reserved);
    TEST_ASSERT_FALSE(fake::config.empty());
    TEST_ASSERT_FALSE(fake::backup.empty());
    parseBlock(fake::config.data(), fake::config.size(), cfg);
    ota::begin(cfg, makeResult(ConfigError::Ok, ""));
    HydroNode client; ota::attach(client, cfg);
    TEST_ASSERT_TRUE(client.headers.find("X-Ota-Result") != client.headers.end());
    ota::afterRound(&client, cfg, {true,202,nullptr});
    TEST_ASSERT_FALSE(fake::record.has_value());
    TEST_ASSERT_TRUE(fake::backup.empty());
}
void test_reset_before_config_write_never_confirms_old_revision() {
    pendingConfig(); fake::config = fake::backup;
    parseBlock(fake::config.data(), fake::config.size(), cfg);
    restarted([] { ota::begin(cfg, makeResult(ConfigError::Ok, "")); });
    TEST_ASSERT_EQUAL_STRING("write_failed", fake::record->result);
    TEST_ASSERT_EQUAL(2, fake::record->reserved);
}
void test_confirmed_config_survives_reset_during_cleanup() {
    pendingConfig(); ota::begin(cfg, makeResult(ConfigError::Ok, ""));
    HydroNode client; ota::attach(client, cfg);
    fake::interruptCleanup = true;
    restarted([&] { ota::afterRound(&client,cfg,{true,202,nullptr}); });
    TEST_ASSERT_EQUAL(3, fake::record->reserved);
    fake::interruptCleanup = false;
    ota::begin(cfg, makeResult(ConfigError::Ok, ""));
    TEST_ASSERT_EQUAL(2,cfg.rev);
    TEST_ASSERT_FALSE(fake::record.has_value());
    TEST_ASSERT_TRUE(fake::backup.empty());
}
void test_lost_state_record_still_recovers_config_backup() {
    fake::backup = fake::config; fake::config.clear();
    restarted([] { ota::begin(cfg, makeResult(ConfigError::BadCrc, "crc")); });
    TEST_ASSERT_FALSE(fake::config.empty());
}
void test_firmware_with_invalid_config_rolls_back_at_boot() {
    ota::Pending p{}; p.kind = ota::PendingKind::Firmware; strcpy(p.toVersion, HN_FW_VERSION); strcpy(p.job,"job");
    fake::record = p; fakeImageState = ESP_OTA_IMG_PENDING_VERIFY;
    restarted([] { ota::begin(cfg, makeResult(ConfigError::BadValue, "mode")); });
    TEST_ASSERT_TRUE(rolledBack);
    TEST_ASSERT_EQUAL_STRING("config_invalid", fake::record->result);
}
void test_blocked_call_is_cut_off_after_two_minutes() {
    pendingConfig(); ota::begin(cfg, makeResult(ConfigError::Ok, ""));
    restarted([] { delay(ota::kVerifyLimitMs); });
    // On the next start a still-unconfirmed config is restored immediately.
    restarted([] { ota::begin(cfg, makeResult(ConfigError::Ok, "")); });
    TEST_ASSERT_EQUAL_STRING("boot_failed", fake::record->result);
}
void test_offer_waits_until_timed_output_finished() {
    HydroNode client; ota::attach(client, cfg);
    JsonDocument offer;
    deserializeJson(offer, R"({"job":"job","family":"esp32c3","version":"0.5.1","size":4,"sha256":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa","sig":"sig","keyId":"dev","url":"/image"})");
    client.handlers["ota"](offer.as<JsonVariantConst>());
    fake::pulseLeft = 1400;
    ota::afterRound(&client,cfg,{true,202,nullptr});
    TEST_ASSERT_EQUAL(0,downloadedImages);
    fake::pulseLeft = 0;
    restarted([&] { ota::afterRound(&client,cfg,{true,202,nullptr}); });
    TEST_ASSERT_EQUAL(1,downloadedImages);
}
void test_measurement_reports_broken_gas_and_missing_driver() {
    cfg.deviceCount = 2;
    cfg.devices[1] = cfg.devices[0];
    cfg.devices[1].channels = &cfg.channelPool[1];
    cfg.channelPool[1] = cfg.channelPool[0];
    strcpy(cfg.devices[0].driver,"sgp40"); strcpy(cfg.devices[1].driver,"wifi");
    GasDriver gas(cfg.devices[0]); WifiDriver wifi(cfg.devices[1]);
    drivers[0] = &gas; drivers[1] = &wifi;
    HydroNode client; hydro = &client;
    auto report = measureAndSend();
    TEST_ASSERT_EQUAL_STRING("sgp40",report.failedDriver);
    TEST_ASSERT_EQUAL(1,client.errors.size());
    gas.warming = true;
    report = measureAndSend();
    TEST_ASSERT_NULL(report.failedDriver);
    TEST_ASSERT_EQUAL(0,client.errors.size());
    drivers[0] = nullptr;
    report = measureAndSend();
    TEST_ASSERT_EQUAL_STRING("sgp40",report.failedDriver);
    hydro = nullptr;
}
int main() {
    UNITY_BEGIN();
    RUN_TEST(test_interrupted_config_restoration_resumes_on_boot);
    RUN_TEST(test_reset_before_config_write_never_confirms_old_revision);
    RUN_TEST(test_confirmed_config_survives_reset_during_cleanup);
    RUN_TEST(test_lost_state_record_still_recovers_config_backup);
    RUN_TEST(test_firmware_with_invalid_config_rolls_back_at_boot);
    RUN_TEST(test_blocked_call_is_cut_off_after_two_minutes);
    RUN_TEST(test_offer_waits_until_timed_output_finished);
    RUN_TEST(test_measurement_reports_broken_gas_and_missing_driver);
    return UNITY_END();
}
