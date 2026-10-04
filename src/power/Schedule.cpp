#include "Schedule.h"

#include <stdio.h>
#include <string.h>

namespace hn::schedule {

bool due(uint32_t round, uint16_t every) { return round % (every ? every : 1) == 0; }

namespace {
uint16_t roundToFour(double hours) {
    long rounded = long(hours / 4.0 + 0.5) * 4;
    if (rounded < 4) rounded = 4;
    if (rounded > 65532) rounded = 65532;
    return uint16_t(rounded);
}
}  // namespace

AscPeriods scd41AscPeriods(uint32_t shotSeconds) {
    if (shotSeconds == 0) shotSeconds = 300;
    double scale = 300.0 / shotSeconds;
    return {roundToFour(44.0 * scale), roundToFour(156.0 * scale)};
}

uint16_t scd30IntervalSeconds(uint32_t dueSeconds) {
    if (dueSeconds == 0) return 2;
    uint32_t interval = dueSeconds * 9 / 10;
    if (interval < 2) interval = 2;
    if (interval > 1800) interval = 1800;
    return uint16_t(interval);
}

PinWake afterPinWake(int64_t untilRoundMs, bool pressed, bool wakePin, bool updateRunning) {
    if (wakePin || updateRunning || untilRoundMs <= int64_t(kRoundSoonMs)) return PinWake::Round;
    return pressed ? PinWake::Send : PinWake::Sleep;
}

size_t formatReport(const ReportData& d, const char* problems, char* out, size_t cap) {
    if (cap == 0) return 0;
    int n = snprintf(out, cap, "awake=%lu;nap=%lu;wifi=%lu", (unsigned long)d.awakeMs, (unsigned long)d.napMs,
                     (unsigned long)d.wifiMs);
    auto append = [&](const char* fmt, auto... args) {
        if (n < 0 || size_t(n) >= cap) return;
        int m = snprintf(out + n, cap - size_t(n), fmt, args...);
        if (m > 0) n += m;
    };
    if (d.pinWakes) append(";wakes=%u", unsigned(d.pinWakes));
    if (d.wifiFailures) append(";wfail=%u:%s", unsigned(d.wifiFailures), d.wifiError ? d.wifiError : "UNKNOWN");
    if (problems && problems[0] && n >= 0 && size_t(n) + 7 < cap) {
        // Whole entries only: cut at the last comma that still fits (and the terminating 0).
        size_t room = cap - size_t(n) - 7;
        size_t len = strlen(problems);
        if (len > room) {
            len = room;
            while (len > 0 && problems[len] != ',') len--;
        }
        if (len > 0) {
            memcpy(out + n, ";sens=", 6);
            memcpy(out + n + 6, problems, len);
            n += int(6 + len);
            out[n] = '\0';
        }
    }
    if (n < 0) n = 0;
    if (size_t(n) >= cap) n = int(cap - 1);
    out[n] = '\0';
    return size_t(n);
}

}  // namespace hn::schedule
