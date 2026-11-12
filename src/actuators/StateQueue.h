#pragma once

// The states an output with "Report output state" still has to send (firmware 0.9.0).
//
// One output, up to eight changes in order: a 300 ms pulse inside a round is an "on" and an
// "off", and both belong in the history. Only a change is queued; the same state twice in a row
// is not. When the queue is full the oldest change goes, the newest always stays.
//
// A state the server did not take (429, no network, server error) stays open and goes out again
// with the next round, until it is accepted. Only the newest state of an output stays open: a
// newer change replaces it, and an open state is dropped while a newer one waits.
//
// No Arduino headers: the native unit tests compile it on the host.

#include <stdint.h>

namespace hn::act {

struct StateQueue {
    uint8_t bits = 0;   // bit i = state of the i-th waiting change, oldest in bit 0
    uint8_t count = 0;  // waiting changes, 0..8
    int8_t last = -1;   // last state queued (or known to the server), -1 = unknown
    int8_t open = -1;   // state not taken by the server, sent again next round; -1 = none

    static constexpr uint8_t kCapacity = 8;

    // Forgets what waits. `known`: the state the server already has (-1 = none).
    void reset(int8_t known) {
        bits = 0;
        count = 0;
        last = known;
        open = -1;
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
        open = -1;  // the newer change counts
        return true;
    }

    // The server did not take `on` (just popped). It stays open unless a newer change waits.
    void failed(bool on) { open = count > 0 ? -1 : (on ? 1 : 0); }

    // Next round: the open state waits again (popped like any change). True when re-queued.
    bool reopen() {
        if (open < 0) return false;
        int8_t value = open;
        open = -1;
        if (count > 0) return false;  // a newer change waits and replaces it
        bits = value ? 1u : 0u;
        count = 1;
        last = value;
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

// Send results after which a state stays open: 429 (rate limit), a server error, and the
// library's network errors (-1 network down, -2 no time yet, -3 connection failed). A refused
// value (400, 401, an invalid type) is not sent again.
inline bool retryable(int code) {
    return code == 429 || code >= 500 || code == -1 || code == -2 || code == -3;
}

}  // namespace hn::act
