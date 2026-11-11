#pragma once

// The states an output with "Report output state" still has to send (firmware 0.9.0).
//
// One output, up to eight changes in order: a 300 ms pulse inside a round is an "on" and an
// "off", and both belong in the history. Only a change is queued; the same state twice in a row
// is not. When the queue is full the oldest change goes, the newest always stays.
//
// No Arduino headers: the native unit tests compile it on the host.

#include <stdint.h>

namespace hn::act {

struct StateQueue {
    uint8_t bits = 0;   // bit i = state of the i-th waiting change, oldest in bit 0
    uint8_t count = 0;  // waiting changes, 0..8
    int8_t last = -1;   // last state queued (or known to the server), -1 = unknown

    static constexpr uint8_t kCapacity = 8;

    // Forgets what waits. `known`: the state the server already has (-1 = none).
    void reset(int8_t known) {
        bits = 0;
        count = 0;
        last = known;
    }

    // Queues `on` when it differs from the last state. True when queued.
    bool push(bool on) {
        int8_t value = on ? 1 : 0;
        if (value == last) return false;
        last = value;
        if (count == kCapacity) {
            bits >>= 1;
            count--;
        }
        if (on) bits |= uint8_t(1u << count);
        else bits &= uint8_t(~(1u << count));
        count++;
        return true;
    }

    // Takes the oldest waiting state. False when nothing waits.
    bool pop(bool& on) {
        if (count == 0) return false;
        on = (bits & 1u) != 0;
        bits >>= 1;
        count--;
        return true;
    }

    bool pending() const { return count > 0; }
};

}  // namespace hn::act
