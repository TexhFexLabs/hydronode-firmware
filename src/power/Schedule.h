#pragma once

// Pure rules of the round schedule, without hardware, so the host tests (pio test -e native)
// check them: which round reads which value, how long the board may nap, how sensors that
// calibrate themselves are told the real spacing of their rounds.

#include <stddef.h>
#include <stdint.h>

namespace hn::schedule {

// A value with `every` = n goes out in rounds 0, n, 2n, … (round = rounds since the last reset).
bool due(uint32_t round, uint16_t every);

// SCD41 self-calibration in single-shot operation counts shots and assumes one every 5 minutes.
// The periods (Sensirion defaults 44 h initial, 156 h standard) are scaled to the real spacing
// `shotSeconds` and rounded to the multiple of 4 hours the sensor takes (at least 4).
struct AscPeriods {
    uint16_t initialHours;
    uint16_t standardHours;
};
AscPeriods scd41AscPeriods(uint32_t shotSeconds);

// SCD30 measurement interval: 2 s while the board is awake (`dueSeconds` 0), else a little less
// than the spacing of its rounds, so a fresh value waits at each, at most 1800 s.
uint16_t scd30IntervalSeconds(uint32_t dueSeconds);

// A nap shorter than this is a plain delay: entering light sleep costs about a millisecond and a
// burst of current, and the USB serial of the C3/S3 drops out with every one.
constexpr uint32_t kMinNapMs = 40;

// After a wake-up by a pin (rain gauge tip, button press) in the middle of a deep sleep: run the
// round now when the next one is due within this, else go back to sleep for the rest.
constexpr uint32_t kRoundSoonMs = 2000;
enum class PinWake : uint8_t { Round, Send, Sleep };
// `pressed`: a button press is waiting to be sent; `wakePin`: the "wake up early" pin.
PinWake afterPinWake(int64_t untilRoundMs, bool pressed, bool wakePin, bool updateRunning);

// Internet time (ms) the next round starts at. Rounds land on anchor + n × interval; the slot
// this round served is the one nearest to when it started, so a timer that woke a little early
// (the ESP8266 light sleep runs up to 5 % short, long deep sleeps drift) does not run the same
// slot a second time. A round that took longer than the interval moves on to the next slot ahead.
uint64_t nextSlotMs(uint64_t anchorMs, uint64_t intervalMs, uint64_t roundStartMs, uint64_t nowMs);

// How long a round tries to join the WiFi. After two failed rounds in a row (router off, out of
// range) only a short try, so a dead network does not keep the radio on for 20 s every round.
uint32_t wifiTimeoutMs(uint16_t failuresInARow);

// The X-Device-Report header: what the last round cost and what went wrong, read by the fleet
// view. At most 240 characters; problems that do not fit are dropped from the end.
struct ReportData {
    uint32_t awakeMs;      // last round, CPU running
    uint32_t napMs;        // last round, light sleep while sensors measured
    uint32_t wifiMs;       // last round, until connected (0 = no connect)
    uint16_t pinWakes;     // wake-ups by a pin since the last round
    uint16_t wifiFailures; // failed connects since the last success
    const char* wifiError; // reason of the last failure (AUTH, NO_SSID, TIMEOUT, LOST)
};
// Writes `awake=…;nap=…;wifi=…[;wakes=n][;wfail=n:REASON][;sens=id:problem,…]` into `out`.
// `problems` is a comma list `id:problem` built by the caller (may be empty).
size_t formatReport(const ReportData& data, const char* problems, char* out, size_t cap);

}  // namespace hn::schedule
