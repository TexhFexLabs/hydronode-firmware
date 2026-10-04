// Host tests for the round schedule: which round reads which value, the self-calibration
// periods of an SCD41 in single shots, the SCD30 interval, the decision after a pin wake-up and
// the X-Device-Report header.

#include <string.h>
#include <unity.h>

#include "power/Schedule.h"

using namespace hn::schedule;

void setUp() {}
void tearDown() {}

void test_due_every_nth_round() {
    TEST_ASSERT_TRUE(due(0, 5));
    TEST_ASSERT_FALSE(due(1, 5));
    TEST_ASSERT_FALSE(due(4, 5));
    TEST_ASSERT_TRUE(due(5, 5));
    TEST_ASSERT_TRUE(due(7, 1));
    TEST_ASSERT_TRUE(due(7, 0));  // 0 counts as every round
}

void test_scd41_asc_periods_follow_the_shot_spacing() {
    AscPeriods fiveMinutes = scd41AscPeriods(300);  // Sensirion's assumption: unchanged
    TEST_ASSERT_EQUAL_UINT16(44, fiveMinutes.initialHours);
    TEST_ASSERT_EQUAL_UINT16(156, fiveMinutes.standardHours);
    AscPeriods oneMinute = scd41AscPeriods(60);  // five times the shots: five times the count
    TEST_ASSERT_EQUAL_UINT16(220, oneMinute.initialHours);
    TEST_ASSERT_EQUAL_UINT16(780, oneMinute.standardHours);
    AscPeriods hourly = scd41AscPeriods(3600);  // never below 4 h, always a multiple of 4
    TEST_ASSERT_EQUAL_UINT16(4, hourly.initialHours);
    TEST_ASSERT_EQUAL_UINT16(12, hourly.standardHours);
    TEST_ASSERT_EQUAL_UINT16(44, scd41AscPeriods(0).initialHours);
}

void test_scd30_interval() {
    TEST_ASSERT_EQUAL_UINT16(2, scd30IntervalSeconds(0));       // awake
    TEST_ASSERT_EQUAL_UINT16(270, scd30IntervalSeconds(300));   // a fresh value at each round
    TEST_ASSERT_EQUAL_UINT16(2, scd30IntervalSeconds(1));
    TEST_ASSERT_EQUAL_UINT16(1800, scd30IntervalSeconds(7200));  // the sensor's maximum
}

void test_pin_wake_decision() {
    TEST_ASSERT_EQUAL(int(PinWake::Sleep), int(afterPinWake(60000, false, false, false)));  // a rain tip
    TEST_ASSERT_EQUAL(int(PinWake::Send), int(afterPinWake(60000, true, false, false)));    // a press
    TEST_ASSERT_EQUAL(int(PinWake::Round), int(afterPinWake(1500, false, false, false)));   // due anyway
    TEST_ASSERT_EQUAL(int(PinWake::Round), int(afterPinWake(60000, false, true, false)));   // wake early
    TEST_ASSERT_EQUAL(int(PinWake::Round), int(afterPinWake(60000, true, false, true)));    // update proving itself
}

void test_report_header() {
    char out[241];
    ReportData d{4120, 9800, 2300, 0, 0, nullptr};
    formatReport(d, "", out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("awake=4120;nap=9800;wifi=2300", out);

    ReportData failing{900, 0, 0, 3, 2, "AUTH"};
    formatReport(failing, "scd4x:missing,pms5003:warming", out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("awake=900;nap=0;wifi=0;wakes=3;wfail=2:AUTH;sens=scd4x:missing,pms5003:warming", out);

    // Problems that do not fit are cut at a whole entry.
    char small[49];
    formatReport(d, "scd4x:missing,pms5003:warming", small, sizeof(small));
    TEST_ASSERT_EQUAL_STRING("awake=4120;nap=9800;wifi=2300;sens=scd4x:missing", small);
    TEST_ASSERT_TRUE(strlen(small) < sizeof(small));
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_due_every_nth_round);
    RUN_TEST(test_scd41_asc_periods_follow_the_shot_spacing);
    RUN_TEST(test_scd30_interval);
    RUN_TEST(test_pin_wake_decision);
    RUN_TEST(test_report_header);
    return UNITY_END();
}
