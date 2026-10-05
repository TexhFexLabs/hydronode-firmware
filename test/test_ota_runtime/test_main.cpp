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
// The block of oldJson with one piece replaced, as the web flasher would write it.
std::vector<uint8_t> blockWith(const char* from, const char* to) {
    std::string json(oldJson);
    json.replace(json.find(from), strlen(from), to);
    std::vector<uint8_t> b(8192);
    b.resize(ota::encodeBlock(json.c_str(), json.size(), b.data(), b.size()));
    return b;
}
// One start of the board with whatever is in flash now. True when it restarted.
bool boot() {
    ParseResult r = fake::config.empty() ? makeResult(ConfigError::NoPartition, "partition")
                                         : parseBlock(fake::config.data(), fake::config.size(), cfg);
    try { ota::begin(cfg, r); } catch (Restarted&) { return true; }
    return false;
}
DriverContext awake{SleepMode::AlwaysOn, 300, 1, false, 0};
// Answers, but its algorithm has nothing yet (or the sensor reads nothing).
class GasDriver : public Driver {
public:
    bool warming = false;
    GasDriver(const DeviceConfig& dev) : Driver(dev, awake) {}
    bool begin(bool) override { return true; }
    uint32_t tickMs() const override { return 1000; }
    void read(Reading* out) override { out[0] = {cfg_.channels[0].type, 0, false, warming}; }
};
class WifiDriver : public Driver {
public:
    WifiDriver(const DeviceConfig& dev) : Driver(dev, awake) {}
    bool begin(bool) override { return true; }
    bool afterConnect() const override { return true; }
    void read(Reading* out) override { out[0] = {cfg_.channels[0].type, -50, true}; }
};
}
void setUp() {
    fake::reset(); configBlock();
    ota::begin(cfg, makeResult(ConfigError::Ok, ""));
    for (auto& d : drivers) d = nullptr;
    for (auto& b : begun) b = false;
    hydro = nullptr; roundIndex = 0;
    coldSensors = true; poweredLong = false; poweredAt = 0; sensorsPowered = true;
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
    TEST_ASSERT_EQUAL_STRING("timeout", fake::record->result);
}
void test_firmware_cut_off_by_deadline_reports_timeout() {
    ota::Pending p{}; p.kind = ota::PendingKind::Firmware; strcpy(p.toVersion, HN_FW_VERSION); strcpy(p.job,"job");
    fake::record = p; fakeImageState = ESP_OTA_IMG_PENDING_VERIFY;
    ota::begin(cfg, makeResult(ConfigError::Ok, ""));
    restarted([] { delay(ota::kVerifyLimitMs); });
    TEST_ASSERT_EQUAL_STRING("timeout", fake::record->result);
    // The bootloader went back to the old firmware; it reports the reason once.
    fakeImageState = ESP_OTA_IMG_VALID; strcpy(fake::record->toVersion, "0.5.1");
    TEST_ASSERT_FALSE(boot());
    HydroNode client; ota::attach(client, cfg);
    TEST_ASSERT_TRUE(client.headers["X-Ota-Result"].find("timeout") != std::string::npos);
}
void test_usb_flash_after_config_rollback_is_kept() {
    pendingConfig(); fake::config = fake::backup;
    strcpy(fake::record->result, "boot_failed"); fake::record->reserved = 2;
    // Flashed over USB before the result was reported; NVS (record, backup) survives that.
    for (const char* rev : {"\"rev\":5", "\"rev\":1"}) {
        fake::config = blockWith("\"ssid\":\"Garden\"", "\"ssid\":\"Shed\"");
        fake::config = rev[7] == '5' ? blockWith("\"rev\":1", rev) : fake::config;
        auto flashed = fake::config;
        ota::Pending kept = *fake::record; auto backup = fake::backup;
        TEST_ASSERT_FALSE(boot());
        TEST_ASSERT_TRUE(fake::config == flashed);
        TEST_ASSERT_FALSE(fake::record.has_value());
        TEST_ASSERT_TRUE(fake::backup.empty());
        TEST_ASSERT_FALSE(ota::busy());
        fake::record = kept; fake::backup = backup;
    }
}
void test_usb_flash_during_config_verification_is_kept() {
    pendingConfig();
    fake::config = blockWith("\"rev\":1", "\"rev\":7");
    auto flashed = fake::config;
    TEST_ASSERT_FALSE(boot());
    TEST_ASSERT_TRUE(fake::config == flashed);
    TEST_ASSERT_EQUAL(7, cfg.rev);
    TEST_ASSERT_FALSE(fake::record.has_value());
    TEST_ASSERT_TRUE(fake::backup.empty());
    TEST_ASSERT_FALSE(ota::busy());
}
void test_rollback_without_backup_never_loops() {
    for (uint8_t reserved : {0, 1, 2}) {
        for (bool newConfig : {true, false}) {
            fake::reset(); configBlock(); pendingConfig();
            if (!newConfig) fake::config = blockWith("\"rev\":1", "\"rev\":9");
            strcpy(fake::record->result, "boot_failed"); fake::record->reserved = reserved;
            fake::backup.clear();
            int restarts = 0;
            for (int i = 0; i < 5; i++) restarts += boot() ? 1 : 0;
            TEST_ASSERT_TRUE_MESSAGE(restarts <= 1, "restart loop");
            TEST_ASSERT_FALSE(boot());
        }
    }
}
void test_plain_boot_does_not_read_a_missing_backup() {
    TEST_ASSERT_FALSE(boot());
    TEST_ASSERT_EQUAL(0, fake::backupLoads);
}
void test_new_firmware_with_other_version_spelling_is_verified() {
    ota::Pending p{}; p.kind = ota::PendingKind::Firmware; strcpy(p.toVersion, "v" HN_FW_VERSION); strcpy(p.job,"job");
    fake::record = p; fakeImageState = ESP_OTA_IMG_PENDING_VERIFY;
    TEST_ASSERT_FALSE(boot());
    TEST_ASSERT_TRUE(ota::busy());
    TEST_ASSERT_EQUAL_STRING("", fake::record->result);
    HydroNode client; ota::attach(client, cfg);
    TEST_ASSERT_TRUE(client.headers.find("X-Ota-State") != client.headers.end());
}
const char* kFirmwareOffer = R"({"job":"job","family":"esp32c3","version":"0.5.1","size":4,"sha256":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa","sig":"sig","keyId":"dev","url":"/image"})";
// One accepted answer of the server, with or without the offer in it.
void reply(HydroNode& client, const char* offerJson) {
    if (offerJson) {
        JsonDocument offer;
        deserializeJson(offer, offerJson);
        client.handlers["ota"](offer.as<JsonVariantConst>());
    }
    ota::afterSend(202);
}
void test_offer_waits_until_timed_output_finished() {
    HydroNode client; ota::attach(client, cfg);
    reply(client, kFirmwareOffer);
    fake::pulseLeft = 1400;
    ota::afterRound(&client,cfg,{true,202,nullptr});
    TEST_ASSERT_EQUAL(0,downloadedImages);
    fake::pulseLeft = 0;
    // Still open: the next round's answer carries it again, then it runs.
    reply(client, kFirmwareOffer);
    restarted([&] { ota::afterRound(&client,cfg,{true,202,nullptr}); });
    TEST_ASSERT_EQUAL(1,downloadedImages);
}
void test_offer_cancelled_during_pulse_is_never_carried_out() {
    HydroNode client; ota::attach(client, cfg);
    reply(client, kFirmwareOffer);
    fake::pulseLeft = 1400;
    ota::afterRound(&client,cfg,{true,202,nullptr});
    fake::pulseLeft = 0;
    // Cancelled on the server: the next round's answers no longer carry it.
    reply(client, nullptr);
    ota::afterRound(&client,cfg,{true,202,nullptr});
    TEST_ASSERT_EQUAL(0,downloadedImages);
    // Nor does a round without any answer bring the old offer back.
    ota::afterRound(&client,cfg,{true,0,nullptr});
    TEST_ASSERT_EQUAL(0,downloadedImages);
    TEST_ASSERT_EQUAL(0,client.acks.size());
}
void test_later_answer_without_offer_drops_it_within_the_round() {
    HydroNode client; ota::attach(client, cfg);
    reply(client, kFirmwareOffer);
    // Cancelled between two values of the same round.
    reply(client, nullptr);
    ota::afterRound(&client,cfg,{true,202,nullptr});
    TEST_ASSERT_EQUAL(0,downloadedImages);
    // A failed send says nothing about the job: an offer from this round's last answer stays.
    reply(client, kFirmwareOffer);
    ota::afterSend(503);
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
// Counts what the round does with it.
class CountingDriver : public Driver {
public:
    int begins = 0, starts = 0, reads = 0, sleeps = 0;
    explicit CountingDriver(const DeviceConfig& dev) : Driver(dev, awake) {}
    bool begin(bool) override { begins++; return true; }
    uint32_t start() override { starts++; return 750; }
    void read(Reading* out) override { reads++; out[0] = {cfg_.channels[0].type, 21.5f, true}; }
    void sleep() override { sleeps++; }
};
void test_a_value_sent_every_second_round_wakes_its_sensor_every_second_round() {
    cfg.channelPool[0].every = 2;
    CountingDriver probe(cfg.devices[0]);
    drivers[0] = &probe;
    HydroNode client; hydro = &client;
    // Deep sleep, timer wake-up without a power pin: the sensor slept powered and set up.
    cfg.mode = SleepMode::DeepSleep;
    coldSensors = false; poweredLong = true;
    roundIndex = 1;
    measureAndSend();
    TEST_ASSERT_EQUAL(0, probe.begins);  // not due: not even woken
    TEST_ASSERT_EQUAL(0, probe.reads);
    roundIndex = 2;
    uint32_t before = millis();
    measureAndSend();
    TEST_ASSERT_EQUAL(1, probe.begins);
    TEST_ASSERT_EQUAL(1, probe.starts);
    TEST_ASSERT_EQUAL(1, probe.reads);
    TEST_ASSERT_EQUAL(1, probe.sleeps);  // back to its low power state after reading
    TEST_ASSERT_TRUE(millis() - before >= 750);  // waited for the conversion
    cfg.mode = SleepMode::AlwaysOn; cfg.channelPool[0].every = 1; hydro = nullptr;
}
void test_report_header_names_what_went_wrong() {
    GasDriver gas(cfg.devices[0]);
    strcpy(cfg.devices[0].driver, "sgp40");
    drivers[0] = &gas;
    HydroNode client; hydro = &client;
    power::report().awakeMs = 4120;
    measureAndSend();
    std::string header = client.headers["X-Device-Report"];
    TEST_ASSERT_TRUE_MESSAGE(header.find("awake=4120") != std::string::npos, header.c_str());
    TEST_ASSERT_TRUE_MESSAGE(header.find("sens=sgp40:timeout") != std::string::npos, header.c_str());
    gas.warming = true;
    measureAndSend();
    header = client.headers["X-Device-Report"];
    TEST_ASSERT_TRUE_MESSAGE(header.find("sens=sgp40:warming") != std::string::npos, header.c_str());
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
    RUN_TEST(test_firmware_cut_off_by_deadline_reports_timeout);
    RUN_TEST(test_usb_flash_after_config_rollback_is_kept);
    RUN_TEST(test_usb_flash_during_config_verification_is_kept);
    RUN_TEST(test_rollback_without_backup_never_loops);
    RUN_TEST(test_plain_boot_does_not_read_a_missing_backup);
    RUN_TEST(test_new_firmware_with_other_version_spelling_is_verified);
    RUN_TEST(test_offer_waits_until_timed_output_finished);
    RUN_TEST(test_offer_cancelled_during_pulse_is_never_carried_out);
    RUN_TEST(test_later_answer_without_offer_drops_it_within_the_round);
    RUN_TEST(test_measurement_reports_broken_gas_and_missing_driver);
    RUN_TEST(test_a_value_sent_every_second_round_wakes_its_sensor_every_second_round);
    RUN_TEST(test_report_header_names_what_went_wrong);
    return UNITY_END();
}
