// Host tests for the state report of an output (firmware 0.9.0, "Report output state"): only
// changes are queued, in order, and a full queue keeps the newest. A state the server did not
// take stays open for the next round, only the newest one.

#include <unity.h>

#include "actuators/StateQueue.h"

using hn::act::StateQueue;

void setUp() {}
void tearDown() {}

void test_unknown_start_queues_the_first_state() {
    StateQueue q;
    q.reset(-1);
    TEST_ASSERT_TRUE(q.push(false));
    bool on = true;
    TEST_ASSERT_TRUE(q.pop(on));
    TEST_ASSERT_FALSE(on);
    TEST_ASSERT_FALSE(q.pending());
}

void test_known_state_is_not_queued_again() {
    StateQueue q;
    q.reset(1);
    TEST_ASSERT_FALSE(q.push(true));
    TEST_ASSERT_FALSE(q.pending());
    TEST_ASSERT_TRUE(q.push(false));
    TEST_ASSERT_FALSE(q.push(false));
    TEST_ASSERT_EQUAL_UINT8(1, q.count);
}

void test_a_pulse_sends_on_then_off() {
    StateQueue q;
    q.reset(0);
    TEST_ASSERT_TRUE(q.push(true));
    TEST_ASSERT_TRUE(q.push(false));
    bool on = false;
    TEST_ASSERT_TRUE(q.pop(on));
    TEST_ASSERT_TRUE(on);
    TEST_ASSERT_TRUE(q.pop(on));
    TEST_ASSERT_FALSE(on);
    TEST_ASSERT_FALSE(q.pop(on));
}

void test_full_queue_drops_the_oldest_and_keeps_the_newest() {
    StateQueue q;
    q.reset(0);
    for (int i = 0; i < 11; i++) q.push(i % 2 == 0);  // on, off, on, … ends on "on" (i = 10)
    TEST_ASSERT_EQUAL_UINT8(StateQueue::kCapacity, q.count);
    bool on = false;
    bool newest = false;
    uint8_t taken = 0;
    bool previous = false;
    while (q.pop(on)) {
        if (taken > 0) TEST_ASSERT_TRUE(on != previous);  // still alternating, in order
        previous = on;
        newest = on;
        taken++;
    }
    TEST_ASSERT_EQUAL_UINT8(8, taken);
    TEST_ASSERT_TRUE(newest);
}

void test_reset_forgets_waiting_changes() {
    StateQueue q;
    q.reset(0);
    q.push(true);
    q.reset(1);
    TEST_ASSERT_FALSE(q.pending());
    TEST_ASSERT_FALSE(q.push(true));
}

void test_a_refused_state_goes_out_again_next_round() {
    StateQueue q;
    q.reset(0);
    TEST_ASSERT_TRUE(q.push(true));
    bool on = false;
    TEST_ASSERT_TRUE(q.pop(on));
    q.failed(on);                     // 429
    TEST_ASSERT_FALSE(q.pending());   // not again in this round
    TEST_ASSERT_FALSE(q.push(true));  // same state: still the open one, nothing new
    TEST_ASSERT_TRUE(q.reopen());
    TEST_ASSERT_TRUE(q.pop(on));
    TEST_ASSERT_TRUE(on);
    q.failed(on);  // network error: open again
    TEST_ASSERT_TRUE(q.reopen());
    TEST_ASSERT_TRUE(q.pop(on));  // accepted this time
    TEST_ASSERT_FALSE(q.reopen());
    TEST_ASSERT_FALSE(q.pending());
}

void test_only_the_newest_state_stays_open() {
    StateQueue q;
    q.reset(0);
    q.push(true);
    bool on = false;
    q.pop(on);
    q.failed(on);                     // "on" refused
    TEST_ASSERT_TRUE(q.push(false));  // switched off locally: replaces the open "on"
    TEST_ASSERT_FALSE(q.reopen());
    TEST_ASSERT_TRUE(q.pop(on));
    TEST_ASSERT_FALSE(on);
    q.failed(on);
    TEST_ASSERT_TRUE(q.reopen());
    TEST_ASSERT_TRUE(q.pop(on));
    TEST_ASSERT_FALSE(on);
    TEST_ASSERT_FALSE(q.pending());
}

void test_a_refused_state_with_a_newer_one_waiting_is_dropped() {
    StateQueue q;
    q.reset(0);
    q.push(true);
    q.push(false);
    bool on = false;
    q.pop(on);     // "on"
    q.failed(on);  // refused, but "off" waits behind it
    TEST_ASSERT_FALSE(q.reopen());
    TEST_ASSERT_TRUE(q.pop(on));
    TEST_ASSERT_FALSE(on);
    TEST_ASSERT_FALSE(q.pending());
}

void test_retryable_codes() {
    TEST_ASSERT_TRUE(hn::act::retryable(429));
    TEST_ASSERT_TRUE(hn::act::retryable(503));
    TEST_ASSERT_TRUE(hn::act::retryable(-1));
    TEST_ASSERT_TRUE(hn::act::retryable(-3));
    TEST_ASSERT_FALSE(hn::act::retryable(202));
    TEST_ASSERT_FALSE(hn::act::retryable(400));
    TEST_ASSERT_FALSE(hn::act::retryable(401));
    TEST_ASSERT_FALSE(hn::act::retryable(-4));
}

void test_reset_forgets_an_open_state() {
    StateQueue q;
    q.reset(0);
    q.push(true);
    bool on = false;
    q.pop(on);
    q.failed(on);
    q.reset(1);
    TEST_ASSERT_FALSE(q.reopen());
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_unknown_start_queues_the_first_state);
    RUN_TEST(test_known_state_is_not_queued_again);
    RUN_TEST(test_a_pulse_sends_on_then_off);
    RUN_TEST(test_full_queue_drops_the_oldest_and_keeps_the_newest);
    RUN_TEST(test_reset_forgets_waiting_changes);
    RUN_TEST(test_a_refused_state_goes_out_again_next_round);
    RUN_TEST(test_only_the_newest_state_stays_open);
    RUN_TEST(test_a_refused_state_with_a_newer_one_waiting_is_dropped);
    RUN_TEST(test_retryable_codes);
    RUN_TEST(test_reset_forgets_an_open_state);
    return UNITY_END();
}
