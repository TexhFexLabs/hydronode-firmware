#pragma once

// Pure battery rules of the firmware, without hardware, so the host tests (pio test -e native)
// check them: which value measures the battery, what SAVE skips, the gauge numbers (MAX1704x
// registers, LC709203F APA and CRC-8) and the charge counter of an INA with "battery" on.
//
// The state machine itself (NORMAL, SAVE, RECOVERY, STANDBY) is HydroNodeBatteryGuard from the
// HydroNode-Library, the same class a sketch uses. main.cpp feeds it.

#include <stddef.h>
#include <stdint.h>

#include "config/Config.h"

namespace hn::battery {

// The value the thresholds watch: the first channel of type BATTERY_VOLTAGE (ADC divider, gauge
// or INA). device = -1 when there is none.
struct Measurement {
    int8_t device;
    int8_t channel;
};
Measurement measurement(const Config& cfg);

// Thresholds run: a battery block with a battery or solar source, all four thresholds, and a
// value that measures the battery.
bool guards(const Config& cfg);

// Thresholds are set but nothing measures the battery: the board reports err=no_measurement.
bool missingMeasurement(const Config& cfg);

// X-Device-Config tokens: src=usb|bat|solar, gauge=max17048|lc709203f|bq27441|ina226|adc|...
const char* sourceToken(PowerSource source);
// The chip that measures the battery (the measurement's device), nullptr when there is none.
const char* gaugeToken(const Config& cfg);

// SAVE (below the save threshold): the interval doubles, values sent every n-th round (n > 1) go
// every 2n-th, gas and dust sensors rest.
uint16_t everyInSave(uint16_t every);
bool restsInSave(const char* driver);

// --- charge -------------------------------------------------------------------------------------

// Voltage a cell reads when its charger finished (LiPo/Li-ion 4.15 V, LiFePO4 3.55 V).
uint16_t fullPerCellMv(Chemistry chemistry);

// State of charge from the resting voltage, 0..100 (rough, for a start without counting).
float percentFromVoltage(uint16_t packMv, uint8_t cells, Chemistry chemistry);

// Coulomb counting with an INA: `mah` the charge so far (NAN = not known yet), `currentA` the
// current measured now (positive charges the battery), `seconds` since the last count. A full
// battery (voltage at full, not discharging) sets it to the capacity, an unknown one starts from
// the voltage. Returns the new charge in mAh, between 0 and the capacity.
float countCharge(float mah, float currentA, uint32_t seconds, uint16_t packMv, uint32_t capacityMah, uint8_t cells,
                  Chemistry chemistry);

// What the board and the sensors drew while the board slept: `restUa` (the battery block's
// "slp", worked out by the builder from the catalog) for `sleptSeconds`. The INA only measures
// while the board is awake, so its current counts only the time awake. NAN stays NAN, a gap of
// more than two days counts nothing (as countCharge), never below 0.
float restCharge(float mah, uint32_t restUa, uint32_t sleptSeconds);

// --- clock --------------------------------------------------------------------------------------

// How long a deep sleep really lasted, for the battery clock (guard's 60 s, charge counter). The
// ESP32 system time keeps running through deep sleep: wake-up time `nowUs` minus the time it went
// to sleep, minus `awakeMs` since the wake-up. Never more than the planned sleep and never below
// 0, so a pin that ends the sleep early counts only the time that passed. Without a usable start
// time (0, or later than now) it counts the planned sleep, as a timer wake-up would.
uint32_t sleptMs(int64_t sleptAtUs, int64_t nowUs, uint32_t awakeMs, uint32_t plannedMs);

// --- gauges -------------------------------------------------------------------------------------

enum class Max1704x : uint8_t { Max17043, Max17048, Max17049 };
Max1704x max1704xChip(const char* option);
// VCELL register to pack mV: MAX17043 12 bit at 1.25 mV, MAX17048 78.125 µV per cell, MAX17049
// the same for each of its two cells.
float max1704xPackMv(uint16_t vcell, Max1704x chip);
// SOC register: high byte percent, low byte 1/256 %.
float max1704xPercent(uint16_t soc);
// CRATE register (MAX17048/49): 0.208 %/h per bit, signed.
float max1704xRatePerHour(uint16_t crate);

// LC709203F "APA" (adjustment pack application) for a design capacity, interpolated from the
// data sheet table (100 mAh 0x08 ... 3000 mAh 0x36).
uint16_t lc709203fApa(uint32_t capacityMah);
// Battery profile register: 1 for Li-ion (3.6 V cells), 0 for LiPo (3.7 V).
uint16_t lc709203fProfile(Chemistry chemistry);
// CRC-8 (polynomial 0x07, start 0) over the bytes of a transfer including the address bytes.
uint8_t crc8(const uint8_t* data, size_t len);

}  // namespace hn::battery
