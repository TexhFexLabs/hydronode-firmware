// Host tests for the battery rules of firmware 0.8.0: the measurement the thresholds watch, what
// SAVE skips, the X-Device-Config tokens, the INA charge counter, the MAX1704x registers, the
// LC709203F APA table and CRC-8. With a HydroNode-Library checkout next to this repository
// (../hydronode-library) it also runs the guard the firmware feeds, HydroNodeBatteryGuard.

#include <math.h>
#include <string.h>
#include <unity.h>

#include <string>

#include "config/Config.h"
#include "power/Battery.h"

#if __has_include(<HydroNodeBatteryGuard.h>)
#include <HydroNodeBatteryGuard.h>
// One implementation: the library's source, compiled into the test.
#include <HydroNodeBatteryGuard.cpp>
#define HN_HAVE_GUARD 1
#endif

using namespace hn;
using namespace hn::battery;

void setUp() {}
void tearDown() {}

namespace {

Config cfg;

ParseResult load(const std::string& battery, const std::string& devices) {
    std::string json = R"({"v":1,"board":"b","sensor":{"id":"i","secret":"s"},"wifi":{"ssid":"n","pass":""},)"
                       R"("interval":300,"i2c":[{"sda":6,"scl":7}],)" +
                       (battery.empty() ? std::string() : "\"battery\":" + battery + ",") + "\"devices\":" + devices + "}";
    return parsePayload(json.data(), json.size(), cfg);
}

const char* kLipo = R"({"src":"bat","chem":"lipo","cells":1,"mah":2000,"save":3500,"rec":3300,"sby":3200,"res":3600,"rev":4})";
const char* kGauge = R"([{"drv":"sht4x","bus":0,"addr":68,"ch":[{"q":"t","type":"TEMPERATURE"}]},)"
                     R"({"drv":"max1704x","bus":0,"addr":54,"opt":{"chip":"MAX17049"},"ch":[{"q":"pct","type":"BATTERY_PERCENTAGE"},{"q":"v","type":"BATTERY_VOLTAGE"}]}])";

}  // namespace

void test_measurement_is_the_first_battery_voltage() {
    TEST_ASSERT_EQUAL(ConfigError::Ok, load(kLipo, kGauge).error);
    Measurement m = measurement(cfg);
    TEST_ASSERT_EQUAL(1, m.device);
    TEST_ASSERT_EQUAL(1, m.channel);
    TEST_ASSERT_TRUE(guards(cfg));
    TEST_ASSERT_FALSE(missingMeasurement(cfg));
    TEST_ASSERT_EQUAL_STRING("max17049", gaugeToken(cfg));
    TEST_ASSERT_EQUAL_STRING("bat", sourceToken(cfg.battery.source));
}

void test_thresholds_need_source_values_and_a_measurement() {
    const char* noBattery = R"([{"drv":"sht4x","bus":0,"addr":68,"ch":[{"q":"t","type":"TEMPERATURE"}]}])";
    TEST_ASSERT_EQUAL(ConfigError::Ok, load(kLipo, noBattery).error);
    TEST_ASSERT_FALSE(guards(cfg));
    TEST_ASSERT_TRUE(missingMeasurement(cfg));  // err=no_measurement
    TEST_ASSERT_NULL(gaugeToken(cfg));

    TEST_ASSERT_EQUAL(ConfigError::Ok, load(R"({"src":"usb","save":3500,"rec":3300,"sby":3200,"res":3600})", kGauge).error);
    TEST_ASSERT_FALSE(guards(cfg));
    TEST_ASSERT_FALSE(missingMeasurement(cfg));
    TEST_ASSERT_EQUAL_STRING("usb", sourceToken(cfg.battery.source));

    TEST_ASSERT_EQUAL(ConfigError::Ok, load(R"({"src":"solar"})", kGauge).error);
    TEST_ASSERT_FALSE(guards(cfg));  // no thresholds
    TEST_ASSERT_EQUAL_STRING("solar", sourceToken(cfg.battery.source));

    TEST_ASSERT_EQUAL(ConfigError::Ok, load("", kGauge).error);
    TEST_ASSERT_FALSE(guards(cfg));
}

void test_divider_and_ina_tokens() {
    const char* divider = R"([{"drv":"battery","pin":1,"opt":{"divider":2},"ch":[{"q":"v","type":"BATTERY_VOLTAGE"}]}])";
    TEST_ASSERT_EQUAL(ConfigError::Ok, load(kLipo, divider).error);
    TEST_ASSERT_EQUAL_STRING("adc", gaugeToken(cfg));
    const char* ina = R"([{"drv":"ina226","bus":0,"addr":64,"ch":[{"q":"v","type":"BATTERY_VOLTAGE"},{"q":"pct","type":"BATTERY_PERCENTAGE"}]}])";
    TEST_ASSERT_EQUAL(ConfigError::Ok, load(kLipo, ina).error);
    TEST_ASSERT_EQUAL_STRING("ina226", gaugeToken(cfg));
}

void test_save_doubles_every_and_rests_gas_and_dust() {
    TEST_ASSERT_EQUAL(1, everyInSave(1));
    TEST_ASSERT_EQUAL(1, everyInSave(0));
    TEST_ASSERT_EQUAL(4, everyInSave(2));
    TEST_ASSERT_EQUAL(10, everyInSave(5));
    TEST_ASSERT_EQUAL(65535, everyInSave(40000));
    TEST_ASSERT_TRUE(restsInSave("sgp41"));
    TEST_ASSERT_TRUE(restsInSave("pms5003"));
    TEST_ASSERT_TRUE(restsInSave("sen5x"));
    TEST_ASSERT_FALSE(restsInSave("scd4x"));
    TEST_ASSERT_FALSE(restsInSave("max1704x"));
}

void test_percent_from_voltage() {
    TEST_ASSERT_EQUAL_FLOAT(0, percentFromVoltage(2900, 1, Chemistry::LiPo));
    TEST_ASSERT_EQUAL_FLOAT(100, percentFromVoltage(4250, 1, Chemistry::LiPo));
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 50, percentFromVoltage(3750, 1, Chemistry::LiPo));
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 50, percentFromVoltage(7500, 2, Chemistry::LiIon));
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 70, percentFromVoltage(3300, 1, Chemistry::LiFePO4));
}

void test_rest_charge_while_asleep() {
    // 5 min asleep at 50 µA: 50 * 300 / 3600 = 4.17 µAh, a mAh counter barely moves.
    TEST_ASSERT_FLOAT_WITHIN(0.0005f, 1000.0f - 0.004167f, restCharge(1000.0f, 50, 300));
    // A board that draws 8 mA asleep (dev board regulator) for an hour: 8 mAh.
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 992.0f, restCharge(1000.0f, 8000, 3600));
    TEST_ASSERT_TRUE(isnan(restCharge(NAN, 50, 300)));
    TEST_ASSERT_EQUAL_FLOAT(1000.0f, restCharge(1000.0f, 0, 300));          // no slp: nothing
    TEST_ASSERT_EQUAL_FLOAT(1000.0f, restCharge(1000.0f, 50, 3u * 24 * 3600));  // clock lost
    TEST_ASSERT_EQUAL_FLOAT(0.0f, restCharge(1.0f, 100000, 3600));             // never below 0
}

void test_slept_ms_counts_what_really_passed() {
    const int64_t at = 1000LL * 1000 * 1000;  // went to sleep at 1000 s system time
    // Timer wake-up after 300 s, 40 ms awake since: the whole sleep.
    TEST_ASSERT_EQUAL_UINT32(300000, sleptMs(at, at + 300040LL * 1000, 40, 300000));
    // A rain pulse after 1 s ends the sleep early: 1 s, not 300 s.
    TEST_ASSERT_EQUAL_UINT32(1000, sleptMs(at, at + 1040LL * 1000, 40, 300000));
    // Never more than planned (timer a little late), never negative.
    TEST_ASSERT_EQUAL_UINT32(300000, sleptMs(at, at + 302000LL * 1000, 40, 300000));
    TEST_ASSERT_EQUAL_UINT32(0, sleptMs(at, at + 10LL * 1000, 40, 300000));
    // No usable start time: the planned sleep, as before.
    TEST_ASSERT_EQUAL_UINT32(300000, sleptMs(0, at, 40, 300000));
    TEST_ASSERT_EQUAL_UINT32(300000, sleptMs(at, at - 1, 40, 300000));
}

void test_charge_counter() {
    // Unknown start: from the voltage.
    TEST_ASSERT_FLOAT_WITHIN(0.1f, 1000, countCharge(NAN, -0.1f, 0, 3750, 2000, 1, Chemistry::LiPo));
    // 100 mA out for an hour: 100 mAh less.
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 900, countCharge(1000, -0.1f, 3600, 3750, 2000, 1, Chemistry::LiPo));
    // 500 mA in for 30 min: 250 mAh more.
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 1250, countCharge(1000, 0.5f, 1800, 3900, 2000, 1, Chemistry::LiPo));
    // Never below 0 or above the capacity.
    TEST_ASSERT_EQUAL_FLOAT(0, countCharge(50, -1.0f, 3600, 3300, 2000, 1, Chemistry::LiPo));
    TEST_ASSERT_EQUAL_FLOAT(2000, countCharge(1990, 1.0f, 3600, 4000, 2000, 1, Chemistry::LiPo));
    // Charger done (full voltage, nothing flowing out): back to 100 %, whatever the count said.
    TEST_ASSERT_EQUAL_FLOAT(2000, countCharge(1200, 0.02f, 600, 4160, 2000, 1, Chemistry::LiPo));
    TEST_ASSERT_EQUAL_FLOAT(4000, countCharge(1200, 0.0f, 600, 7100, 4000, 2, Chemistry::LiFePO4));
    // Full voltage while discharging hard is no full battery.
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 1100, countCharge(1200, -0.1f, 3600, 4160, 2000, 1, Chemistry::LiPo));
    // A gap of days counts nothing; no capacity, no percent.
    TEST_ASSERT_EQUAL_FLOAT(1000, countCharge(1000, -0.1f, 3u * 24 * 3600, 3750, 2000, 1, Chemistry::LiPo));
    TEST_ASSERT_TRUE(isnan(countCharge(1000, -0.1f, 60, 3750, 0, 1, Chemistry::LiPo)));
}

void test_max1704x_registers() {
    TEST_ASSERT_EQUAL(Max1704x::Max17048, max1704xChip(nullptr));
    TEST_ASSERT_EQUAL(Max1704x::Max17043, max1704xChip("MAX17043"));
    // MAX17048: 0xB400 * 78.125 µV = 3600 mV.
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 3600, max1704xPackMv(0xB400, Max1704x::Max17048));
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 7200, max1704xPackMv(0xB400, Max1704x::Max17049));
    // MAX17043: 12 bit in the upper bits, 1.25 mV: 0xB400 >> 4 = 2880 * 1.25 = 3600 mV.
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 3600, max1704xPackMv(0xB400, Max1704x::Max17043));
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 87.5f, max1704xPercent(0x5780));
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 2.08f, max1704xRatePerHour(10));
    TEST_ASSERT_FLOAT_WITHIN(0.001f, -2.08f, max1704xRatePerHour(uint16_t(-10)));
}

void test_lc709203f_apa_and_profile() {
    TEST_ASSERT_EQUAL_HEX16(0x08, lc709203fApa(50));
    TEST_ASSERT_EQUAL_HEX16(0x08, lc709203fApa(100));
    TEST_ASSERT_EQUAL_HEX16(0x0B, lc709203fApa(200));
    TEST_ASSERT_EQUAL_HEX16(0x10, lc709203fApa(500));
    TEST_ASSERT_EQUAL_HEX16(0x19, lc709203fApa(1000));
    TEST_ASSERT_EQUAL_HEX16(0x2D, lc709203fApa(2000));
    TEST_ASSERT_EQUAL_HEX16(0x36, lc709203fApa(3000));
    TEST_ASSERT_EQUAL_HEX16(0x36, lc709203fApa(10000));
    TEST_ASSERT_EQUAL_HEX16(0x23, lc709203fApa(1500));  // halfway between 0x19 and 0x2D
    TEST_ASSERT_EQUAL(0, lc709203fProfile(Chemistry::LiPo));
    TEST_ASSERT_EQUAL(1, lc709203fProfile(Chemistry::LiIon));
}

void test_crc8() {
    // CRC-8/SMBUS check value.
    const uint8_t check[] = {'1', '2', '3', '4', '5', '6', '7', '8', '9'};
    TEST_ASSERT_EQUAL_HEX8(0xF4, crc8(check, sizeof(check)));
    // LC709203F frame: write 0x0001 to register 0x15 (power mode), address byte 0x16 first.
    const uint8_t write[] = {0x16, 0x15, 0x01, 0x00};
    TEST_ASSERT_EQUAL_HEX8(0x64, crc8(write, sizeof(write)));
}

#if HN_HAVE_GUARD
// What main.cpp does with the guard, round by round: thresholds from the battery block, the
// battery reading in pack mV, the clock in seconds, the memory through deep sleep.
void test_guard_from_the_battery_block() {
    TEST_ASSERT_EQUAL(ConfigError::Ok, load(kLipo, kGauge).error);
    const BatteryConfig& b = cfg.battery;
    HydroNodeBatteryGuard guard;
    guard.setThresholds({b.saveMv, b.recoveryMv, b.standbyMv, b.resumeMv});
    TEST_ASSERT_EQUAL(HydroNodeBatteryGuard::NORMAL, guard.update(3900, true, 0));
    TEST_ASSERT_EQUAL(HydroNodeBatteryGuard::SAVE, guard.update(3450, true, 300));
    TEST_ASSERT_EQUAL(600u, guard.sleepSeconds(300));
    // Last round before Recovery: it changed from a running state.
    TEST_ASSERT_EQUAL(HydroNodeBatteryGuard::RECOVERY, guard.update(3250, true, 900));
    TEST_ASSERT_TRUE(guard.changed());
    TEST_ASSERT_FALSE(guard.radioAllowed());
    TEST_ASSERT_EQUAL_STRING("recovery", guard.stateName());

    // Deep sleep: the memory goes through RTC memory (power::BatteryState::guard, 16 bytes).
    uint8_t rtc[16];
    HydroNodeBatteryGuard::Memory memory = guard.memory();
    TEST_ASSERT_TRUE(sizeof(memory) <= sizeof(rtc));
    memcpy(rtc, &memory, sizeof(memory));
    HydroNodeBatteryGuard woke;
    woke.setThresholds({b.saveMv, b.recoveryMv, b.standbyMv, b.resumeMv});
    HydroNodeBatteryGuard::Memory restored;
    memcpy(&restored, rtc, sizeof(restored));
    TEST_ASSERT_TRUE(woke.restore(restored));
    TEST_ASSERT_EQUAL(HydroNodeBatteryGuard::RECOVERY, woke.state());
    TEST_ASSERT_EQUAL(HydroNodeBatteryGuard::STANDBY, (woke.update(3150, true, 1200), woke.update(3150, true, 1500)));
    TEST_ASSERT_EQUAL(3600u, woke.sleepSeconds(300));
    // Resume: two valid readings at 3.6 V or more, 60 s apart.
    TEST_ASSERT_EQUAL(HydroNodeBatteryGuard::RECOVERY, woke.update(3700, true, 5100));
    TEST_ASSERT_EQUAL(HydroNodeBatteryGuard::NORMAL, woke.update(3700, true, 5400));
    TEST_ASSERT_EQUAL_STRING("normal", woke.stateName());
}
#endif

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_measurement_is_the_first_battery_voltage);
    RUN_TEST(test_thresholds_need_source_values_and_a_measurement);
    RUN_TEST(test_divider_and_ina_tokens);
    RUN_TEST(test_save_doubles_every_and_rests_gas_and_dust);
    RUN_TEST(test_percent_from_voltage);
    RUN_TEST(test_charge_counter);
    RUN_TEST(test_slept_ms_counts_what_really_passed);
    RUN_TEST(test_rest_charge_while_asleep);
    RUN_TEST(test_max1704x_registers);
    RUN_TEST(test_lc709203f_apa_and_profile);
    RUN_TEST(test_crc8);
#if HN_HAVE_GUARD
    RUN_TEST(test_guard_from_the_battery_block);
#endif
    return UNITY_END();
}
