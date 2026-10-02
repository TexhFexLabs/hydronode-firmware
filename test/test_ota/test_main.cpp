#include <unity.h>

#include <string.h>

#include <string>
#include <vector>

#include "config/Config.h"
#include "ota/ConfigMerge.h"
#include "ota/OtaLogic.h"

using namespace hn;
using namespace hn::ota;

namespace {

const char* kSha = "3f0a1b2c3d4e5f60718293a4b5c6d7e8f9011223344556677889900aabbccdde";

const std::string kOldJson =
    R"({"v":1,"rev":14,"board":"esp32c3-devkitm","sensor":{"id":"550e8400-e29b-41d4-a716-446655440000",)"
    R"("secret":"topsecret","host":"hydronode.tech"},"wifi":{"ssid":"Garden","pass":"wifi-password","ip":null},)"
    R"("interval":600,"power":{"mode":"ALWAYS_ON"},"i2c":[],"devices":[{"drv":"dht","pin":4,"ch":[{"q":"t","type":"TEMPERATURE"}]}]})";

FirmwareOffer offer() {
    return FirmwareOffer{"job-1", "0.5.1", "esp32c3", 1360128, kSha, "c2ln", "dev-2026-10", false,
                         VerifyMode::Strict, "/api/device-ota/v1/image?job=job-1"};
}

VerifyInput input(VerifyMode mode, uint8_t attempt, int status, const char* failed, uint32_t elapsed = 10000) {
    return VerifyInput{mode, attempt, elapsed, true, status, failed};
}

Config scratch;

}  // namespace

void setUp() {}
void tearDown() {}

void test_firmware_flags() {
    char out[24];
    firmwareFlags(true, 14, out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("ota cfg=14", out);
    firmwareFlags(false, 3, out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("cfg=3", out);
    firmwareFlags(true, 0, out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("ota cfg=1", out);
}

void test_signed_text() {
    char out[192];
    TEST_ASSERT_TRUE(signedText("0.5.1", "esp32c3", kSha, 1360128, false, out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING(
        "hydronode-ota-v1|0.5.1|esp32c3|3f0a1b2c3d4e5f60718293a4b5c6d7e8f9011223344556677889900aabbccdde|1360128|0", out);
    TEST_ASSERT_TRUE(signedText("0.4.0", "esp32", kSha, 10, true, out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("|1", out + strlen(out) - 2);
    TEST_ASSERT_FALSE(signedText("0.5.1", "esp32c3", "ABC", 1, false, out, sizeof(out)));
    // Upper case hex is not the canonical form.
    std::string upper(kSha);
    upper[0] = 'F';
    upper[1] = 'F';
    upper[2] = 'A';
    TEST_ASSERT_FALSE(signedText("0.5.1", "esp32c3", upper.c_str(), 1, false, out, sizeof(out)));
    TEST_ASSERT_FALSE(signedText("0.5.1", "esp32c3", kSha, 1, false, out, 20));
}

void test_versions_compare_by_number() {
    TEST_ASSERT_TRUE(compareVersions("0.4.2", "0.5.0") < 0);
    TEST_ASSERT_TRUE(compareVersions("0.10.0", "0.9.9") > 0);
    TEST_ASSERT_EQUAL(0, compareVersions("0.5.0", "0.5.0"));
    TEST_ASSERT_TRUE(compareVersions("1.0", "0.9.9") > 0);
}

void test_offer_checks() {
    TEST_ASSERT_NULL(checkFirmwareOffer(offer(), "esp32c3", "0.5.0", 0x1C0000));
    FirmwareOffer f = offer();
    f.family = "esp32s3";
    TEST_ASSERT_EQUAL_STRING("family_mismatch", checkFirmwareOffer(f, "esp32c3", "0.5.0", 0x1C0000));
    TEST_ASSERT_EQUAL_STRING("same_version", checkFirmwareOffer(offer(), "esp32c3", "0.5.1", 0x1C0000));
    TEST_ASSERT_EQUAL_STRING("downgrade_not_allowed", checkFirmwareOffer(offer(), "esp32c3", "0.6.0", 0x1C0000));
    f = offer();
    f.downgrade = true;
    TEST_ASSERT_NULL(checkFirmwareOffer(f, "esp32c3", "0.6.0", 0x1C0000));
    TEST_ASSERT_EQUAL_STRING("no_space", checkFirmwareOffer(offer(), "esp32c3", "0.5.0", 1000000));
    f = offer();
    f.url = "https://evil.example/image";
    TEST_ASSERT_EQUAL_STRING("bad_offer", checkFirmwareOffer(f, "esp32c3", "0.5.0", 0x1C0000));
    f = offer();
    f.sig = "";
    TEST_ASSERT_EQUAL_STRING("bad_offer", checkFirmwareOffer(f, "esp32c3", "0.5.0", 0x1C0000));
}

void test_verify_strict_needs_every_sensor() {
    TEST_ASSERT_EQUAL(int(VerifyStep::Verified), int(decideVerify(input(VerifyMode::Strict, 1, 202, nullptr)).step));
    VerifyDecision d = decideVerify(input(VerifyMode::Strict, 1, 202, "bme280"));
    TEST_ASSERT_EQUAL(int(VerifyStep::Retry), int(d.step));
    d = decideVerify(input(VerifyMode::Strict, 3, 202, "bme280"));
    TEST_ASSERT_EQUAL(int(VerifyStep::RollBack), int(d.step));
    TEST_ASSERT_EQUAL_STRING("sensor_read_failed:bme280", d.reason);
}

void test_verify_lenient_only_needs_the_ingest() {
    TEST_ASSERT_EQUAL(int(VerifyStep::Verified), int(decideVerify(input(VerifyMode::Lenient, 1, 202, "bme280")).step));
    VerifyDecision d = decideVerify(input(VerifyMode::Lenient, 3, 401, nullptr));
    TEST_ASSERT_EQUAL(int(VerifyStep::RollBack), int(d.step));
    TEST_ASSERT_EQUAL_STRING("ingest_failed:401", d.reason);
}

void test_verify_server_errors_and_time() {
    VerifyDecision d = decideVerify(input(VerifyMode::Strict, 1, 503, nullptr));
    TEST_ASSERT_EQUAL(int(VerifyStep::Retry), int(d.step));
    d = decideVerify(input(VerifyMode::Strict, 3, -3, nullptr));
    TEST_ASSERT_EQUAL(int(VerifyStep::RollBack), int(d.step));
    TEST_ASSERT_EQUAL_STRING("server_unreachable", d.reason);
    // Out of time before the third try.
    d = decideVerify(input(VerifyMode::Strict, 2, 202, "ds18b20", 125000));
    TEST_ASSERT_EQUAL(int(VerifyStep::RollBack), int(d.step));
    TEST_ASSERT_EQUAL_STRING("timeout", d.reason);
    VerifyInput noWifi = input(VerifyMode::Lenient, 3, 0, nullptr);
    noWifi.wifiOk = false;
    TEST_ASSERT_EQUAL_STRING("wifi_failed", decideVerify(noWifi).reason);
}

void test_headers() {
    char out[112];
    otaStateHeader(1, VerifyMode::Strict, "job-1", out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("verifying;try=1;mode=STRICT;job=job-1", out);
    otaStateHeader(2, VerifyMode::Lenient, "job-1", out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("verifying;try=2;mode=INGEST;job=job-1", out);
    otaResultHeader("sensor_read_failed:bme280", "job-1", out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("rolled_back;sensor_read_failed:bme280;job=job-1", out);
    TEST_ASSERT_EQUAL(int(VerifyMode::Lenient), int(parseVerifyMode("INGEST")));
    TEST_ASSERT_EQUAL(int(VerifyMode::Strict), int(parseVerifyMode("anything")));
}

void test_config_merge_keeps_wifi_and_secret() {
    JsonDocument offered;
    deserializeJson(offered,
                    R"({"v":1,"board":"esp32c3-devkitm","interval":300,"power":{"mode":"DEEP_SLEEP"},"i2c":[],)"
                    R"("devices":[{"drv":"dht","pin":4,"ch":[{"q":"t","type":"TEMPERATURE"}]},)"
                    R"({"drv":"wifi","ch":[{"q":"rssi","type":"RSSI"}]}],)"
                    R"("sensor":{"id":"other","secret":"stolen"},"wifi":{"ssid":"evil","pass":"x"}})");
    std::vector<uint8_t> out(8192);
    MergeResult r = mergeConfig(kOldJson.data(), kOldJson.size(), offered.as<JsonVariantConst>(), 15, out.data(),
                                out.size(), scratch);
    TEST_ASSERT_NULL(r.error);
    TEST_ASSERT_TRUE(r.length > kHeaderSize);

    Config parsed;
    TEST_ASSERT_EQUAL(int(ConfigError::Ok), int(parseBlock(out.data(), r.length, parsed).error));
    TEST_ASSERT_EQUAL_UINT32(15, parsed.rev);
    TEST_ASSERT_EQUAL_UINT32(300, parsed.intervalSeconds);
    TEST_ASSERT_EQUAL(int(SleepMode::DeepSleep), int(parsed.mode));
    TEST_ASSERT_EQUAL(2, parsed.deviceCount);
    TEST_ASSERT_EQUAL_STRING("550e8400-e29b-41d4-a716-446655440000", parsed.sensorId);
    TEST_ASSERT_EQUAL_STRING("topsecret", parsed.secret);
    TEST_ASSERT_EQUAL_STRING("Garden", parsed.ssid);
    TEST_ASSERT_EQUAL_STRING("wifi-password", parsed.pass);
}

void test_config_merge_rejects_what_the_firmware_would_not_run() {
    JsonDocument offered;
    deserializeJson(offered, R"({"board":"esp32c3-devkitm","interval":5,"devices":[]})");
    std::vector<uint8_t> out(8192);
    MergeResult r = mergeConfig(kOldJson.data(), kOldJson.size(), offered.as<JsonVariantConst>(), 15, out.data(),
                                out.size(), scratch);
    TEST_ASSERT_EQUAL_STRING("config_invalid", r.error);
    TEST_ASSERT_EQUAL(int(ConfigError::BadValue), int(r.parse.error));

    JsonDocument notObject;
    deserializeJson(notObject, "[1,2]");
    r = mergeConfig(kOldJson.data(), kOldJson.size(), notObject.as<JsonVariantConst>(), 15, out.data(), out.size(),
                    scratch);
    TEST_ASSERT_EQUAL_STRING("config_invalid", r.error);

    JsonDocument ok;
    deserializeJson(ok, R"({"board":"esp32c3-devkitm","interval":60,"devices":[{"drv":"dht","pin":4,"ch":[]}]})");
    r = mergeConfig(kOldJson.data(), kOldJson.size(), ok.as<JsonVariantConst>(), 15, out.data(), 64, scratch);
    TEST_ASSERT_EQUAL_STRING("config_too_large", r.error);
}

void test_config_without_rev_is_revision_one() {
    std::vector<uint8_t> out(8192);
    std::string json = kOldJson;
    json.replace(json.find("\"rev\":14,"), 9, "");
    size_t len = encodeBlock(json.data(), json.size(), out.data(), out.size());
    Config parsed;
    TEST_ASSERT_EQUAL(int(ConfigError::Ok), int(parseBlock(out.data(), len, parsed).error));
    TEST_ASSERT_EQUAL_UINT32(1, parsed.rev);
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_firmware_flags);
    RUN_TEST(test_signed_text);
    RUN_TEST(test_versions_compare_by_number);
    RUN_TEST(test_offer_checks);
    RUN_TEST(test_verify_strict_needs_every_sensor);
    RUN_TEST(test_verify_lenient_only_needs_the_ingest);
    RUN_TEST(test_verify_server_errors_and_time);
    RUN_TEST(test_headers);
    RUN_TEST(test_config_merge_keeps_wifi_and_secret);
    RUN_TEST(test_config_merge_rejects_what_the_firmware_would_not_run);
    RUN_TEST(test_config_without_rev_is_revision_one);
    return UNITY_END();
}
