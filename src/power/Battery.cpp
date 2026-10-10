#include "Battery.h"

#include <math.h>
#include <string.h>

namespace hn::battery {

Measurement measurement(const Config& cfg) {
    for (uint8_t i = 0; i < cfg.deviceCount; i++) {
        for (uint8_t c = 0; c < cfg.devices[i].channelCount; c++) {
            if (strcmp(cfg.devices[i].channels[c].type, "BATTERY_VOLTAGE") == 0) return {int8_t(i), int8_t(c)};
        }
    }
    return {-1, -1};
}

bool guards(const Config& cfg) {
    const BatteryConfig& b = cfg.battery;
    return b.present && b.source != PowerSource::Usb && b.thresholds && measurement(cfg).device >= 0;
}

bool missingMeasurement(const Config& cfg) {
    const BatteryConfig& b = cfg.battery;
    return b.present && b.source != PowerSource::Usb && b.thresholds && measurement(cfg).device < 0;
}

const char* sourceToken(PowerSource source) {
    switch (source) {
        case PowerSource::Battery: return "bat";
        case PowerSource::Solar: return "solar";
        default: return "usb";
    }
}

const char* gaugeToken(const Config& cfg) {
    Measurement m = measurement(cfg);
    if (m.device < 0) return nullptr;
    const DeviceConfig& d = cfg.devices[m.device];
    if (strcmp(d.driver, "battery") == 0) return "adc";
    if (strcmp(d.driver, "max1704x") == 0) {
        switch (max1704xChip(d.text("chip", "MAX17048"))) {
            case Max1704x::Max17043: return "max17043";
            case Max1704x::Max17049: return "max17049";
            default: return "max17048";
        }
    }
    return d.driver;  // lc709203f, bq27441, ina219, ina226, ina260, ads1115, ...
}

uint16_t everyInSave(uint16_t every) {
    if (every <= 1) return 1;
    return every > 32767 ? 65535 : uint16_t(every * 2);
}

bool restsInSave(const char* driver) {
    static const char* const resting[] = {"sgp30", "sgp40", "sgp41", "pms5003", "pmsa003i", "sen5x"};
    for (const char* id : resting) {
        if (strcmp(driver, id) == 0) return true;
    }
    return false;
}

// --- charge -------------------------------------------------------------------------------------

uint16_t fullPerCellMv(Chemistry chemistry) { return chemistry == Chemistry::LiFePO4 ? 3550 : 4150; }

namespace {

struct Point {
    uint16_t mv;
    uint8_t pct;
};

// Resting voltage per cell to charge, a typical discharge curve at room temperature.
constexpr Point kLithium[] = {{3000, 0}, {3300, 5}, {3600, 20}, {3700, 40}, {3800, 60},
                              {3900, 75}, {4000, 88}, {4100, 96}, {4200, 100}};
constexpr Point kLfp[] = {{2800, 0}, {3000, 5}, {3200, 20}, {3250, 40}, {3300, 70}, {3350, 90}, {3450, 100}};

template <size_t N>
float interpolate(const Point (&curve)[N], float mv) {
    if (mv <= curve[0].mv) return curve[0].pct;
    for (size_t i = 1; i < N; i++) {
        if (mv <= curve[i].mv) {
            float f = (mv - curve[i - 1].mv) / float(curve[i].mv - curve[i - 1].mv);
            return curve[i - 1].pct + f * (curve[i].pct - curve[i - 1].pct);
        }
    }
    return curve[N - 1].pct;
}

}  // namespace

float percentFromVoltage(uint16_t packMv, uint8_t cells, Chemistry chemistry) {
    float perCell = float(packMv) / (cells ? cells : 1);
    return chemistry == Chemistry::LiFePO4 ? interpolate(kLfp, perCell) : interpolate(kLithium, perCell);
}

float countCharge(float mah, float currentA, uint32_t seconds, uint16_t packMv, uint32_t capacityMah, uint8_t cells,
                  Chemistry chemistry) {
    float capacity = float(capacityMah);
    if (capacityMah == 0) return NAN;
    uint32_t full = uint32_t(fullPerCellMv(chemistry)) * (cells ? cells : 1);
    // Charger done: voltage at full and no current flowing out. Counting starts over at 100 %.
    if (packMv >= full && currentA >= -0.005f) return capacity;
    if (isnan(mah) || !isfinite(currentA)) return capacity * percentFromVoltage(packMv, cells, chemistry) / 100.0f;
    // A gap of more than two days (board off, clock lost) counts nothing.
    if (seconds > 2u * 24 * 3600) seconds = 0;
    float next = mah + currentA * 1000.0f * float(seconds) / 3600.0f;
    if (next < 0) next = 0;
    if (next > capacity) next = capacity;
    return next;
}

float restCharge(float mah, uint32_t restUa, uint32_t sleptSeconds) {
    if (isnan(mah) || restUa == 0 || sleptSeconds > 2u * 24 * 3600) return mah;
    float next = mah - float(restUa) / 1000.0f * float(sleptSeconds) / 3600.0f;
    return next < 0 ? 0 : next;
}

// --- clock --------------------------------------------------------------------------------------

uint32_t sleptMs(int64_t sleptAtUs, int64_t nowUs, uint32_t awakeMs, uint32_t plannedMs) {
    if (sleptAtUs <= 0 || nowUs < sleptAtUs) return plannedMs;
    int64_t ms = (nowUs - sleptAtUs) / 1000 - int64_t(awakeMs);
    if (ms <= 0) return 0;
    return ms >= int64_t(plannedMs) ? plannedMs : uint32_t(ms);
}

// --- gauges -------------------------------------------------------------------------------------

Max1704x max1704xChip(const char* option) {
    if (option && strcmp(option, "MAX17043") == 0) return Max1704x::Max17043;
    if (option && strcmp(option, "MAX17049") == 0) return Max1704x::Max17049;
    return Max1704x::Max17048;
}

float max1704xPackMv(uint16_t vcell, Max1704x chip) {
    switch (chip) {
        case Max1704x::Max17043: return float(vcell >> 4) * 1.25f;
        case Max1704x::Max17049: return float(vcell) * 0.078125f * 2.0f;
        default: return float(vcell) * 0.078125f;
    }
}

float max1704xPercent(uint16_t soc) { return float(soc) / 256.0f; }

float max1704xRatePerHour(uint16_t crate) { return float(int16_t(crate)) * 0.208f; }

uint16_t lc709203fApa(uint32_t capacityMah) {
    struct Apa {
        uint16_t mah;
        uint8_t apa;
    };
    static const Apa table[] = {{100, 0x08}, {200, 0x0B}, {500, 0x10}, {1000, 0x19}, {2000, 0x2D}, {3000, 0x36}};
    constexpr size_t n = sizeof(table) / sizeof(table[0]);
    if (capacityMah <= table[0].mah) return table[0].apa;
    for (size_t i = 1; i < n; i++) {
        if (capacityMah <= table[i].mah) {
            uint32_t span = table[i].mah - table[i - 1].mah;
            uint32_t into = capacityMah - table[i - 1].mah;
            uint32_t step = table[i].apa - table[i - 1].apa;
            return uint16_t(table[i - 1].apa + (step * into + span / 2) / span);
        }
    }
    return table[n - 1].apa;
}

uint16_t lc709203fProfile(Chemistry chemistry) { return chemistry == Chemistry::LiIon ? 1 : 0; }

uint8_t crc8(const uint8_t* data, size_t len) {
    uint8_t crc = 0;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int k = 0; k < 8; k++) crc = (crc & 0x80) ? uint8_t((crc << 1) ^ 0x07) : uint8_t(crc << 1);
    }
    return crc;
}

}  // namespace hn::battery
