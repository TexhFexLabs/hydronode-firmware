# Power and rounds

How the firmware spends as little time awake as it can, and how the catalog tells the web app,
the backend and the firmware the same thing about every sensor. Since 0.6.0.

## A round

1. **Only due sensors.** A value with `every: n` goes out in rounds 0, n, 2n, … (rounds count
   from the last reset). A sensor whose values are not due this round is not touched: a CO₂ value
   sent every 5th round wakes its sensor every 5th round.
2. **Start together, nap while they measure.** Each due sensor gets `begin()` if it is new since
   power-on, then its power-up time, then `start()`; what it has to wait for (a DS18B20
   conversion, two SCD41 shots, a fan run-up) is returned instead of `delay()`. In the sleeping
   modes the board light-sleeps meanwhile (`HN:` lines stay quiet), radio off.
3. **Read, then put the sensors back to sleep** (`sleep()`: SCD41 power-down, fan SET low, SEN5x
   idle or gas-only).
4. **Only then the radio.** A round with no value due does not connect at all. All values go out
   in one request with the time they were read (since 0.7.0). Always on and modem sleep with
   rounds of a minute or less keep the TLS connection for the next round (the handshake is 2 to
   3 s); the sleeping modes close it. After two failed connects in a row a round tries the WiFi
   for 8 s instead of 20 s.
5. **Sleep** as deep as the config says, until the next round on the internet-time grid. The slot
   a round served is the one nearest to when it started, so a timer that woke early never runs
   the same slot twice. A wake pin still held at its wake level (a stuck rain contact) is left
   unarmed until it lets go, instead of waking the board over and over.

A task watchdog (ESP32 family, 5 minutes) restarts a board whose loop stops coming back, for
example a library waiting forever on a sensor that left the bus. A sensor that did not answer at
start-up is tried again every round; I²C sensors whose library cannot tell a lost sensor from a
value are checked for an answer before each read (`missing` instead of a made-up value).

The serial line `HN:COST awake=… nap=… wifi=…` closes every round; the same numbers go to the
server with the next round (below).

## Per sensor

| Sensor | Between rounds | Per due round |
|---|---|---|
| SHT4x, SHT3x, AHT, HTU21D, SHTC3, MS8607, BME280, BMP280, BMP3xx, LPS22 | sleeps by itself (single shot / forced mode) | one measurement, ≤ 200 ms |
| BME680 | sleeps (forced mode) | measurement and heater napped, about 200 ms |

The BME280 starts without the library's 100 ms wait for a normal-mode value it never reads: chip
ID, a reset only after power-up, the calibration words. Sensors with a start-up time after power-on
(Bosch 2 ms, LPS22 5 ms, SHTC3 1 ms, TMP117 2 ms, SCD4x 1 s, SCD30 2 s) get it before the first
command; the catalog's `bootMs`/`powerUpMs` are the same numbers the firmware waits.
| BH1750, MCP9808, TMP117, DPS310, TSL2591, LTR390 | shut down / standby | one-shot, the board naps during the conversion |
| VEML7700 | shut down, 0.5 µA | gain and integration time stepped like the library's auto mode, every step napped (0.2 s in daylight, up to 3.5 s in the dark) |
| INA219, INA226, INA260, ADS1115 | power-down / single shot | triggered conversion |
| DS18B20 | idle | conversion napped: 12 bit (0.0625 °C, 750 ms) when the board stays awake, 10 bit (0.25 °C, 188 ms) when it sleeps between rounds; ±0.5 °C either way |
| SCD41 (sleeping modes) | power-down, 0.4 µA | wake, one shot thrown away, one shot kept (2 × 5 s napped; the library's call would wait them awake) |
| SCD40, SCD41 awake | measures every 5 s by itself | waits (napping) for the next value, then reads it |
| SCD30 | measures by itself at 0.9 × the round spacing (max 1800 s) | reads the waiting value. Never reset after a sleep (Adafruit's `begin()` would) |
| PMS5003, PMSA003I | fan asleep with SET wired (held low through deep sleep), else running | fan on, 30 s run-up napped; the PMS5003 drops buffered frames and reads the next one (≤ 2.3 s awake) |
| SEN5x | idle, or gas-only mode when VOC/NOx are sent (indices keep learning on the sensor) | full measurement, 10 s run-up napped |
| SGP40, SGP41 (VOC) | light sleep: a reading every 10 s with the heater off (Sensirion low power); the SGP40 measures 30 ms (the library waits 250 ms) | latest index |
| SGP30, SGP41 NOx | awake modes only: a reading every second | latest value |
| Rain gauge | each tip wakes the board ~20 ms, counted in RTC memory; awake, contact bounce within 50 ms counts once | the sum since the last round |
| Flow meter | awake modes only (hundreds of pulses per second) | the sum |
| Button | a press wakes the board, toggles locally, goes out right away | — |
| Relay, LED, output | level held through light and deep sleep (ESP32 family) | commands arrive with the round |

## The catalog says the same

`catalog/drivers.json` per driver: `bootMs` (power-on → accepts commands), `powerUpMs`
(power-on → first valid value), `waitMs` (napped per due round), `readMs` (busy reading),
`sleepUa` / `activeMa` (for the battery estimate), `ticks` (samples in the background),
`wakesBoard` (wakes the board from a pin), and `sleep` rules:

```jsonc
{ "modes": ["DEEP_SLEEP", "HIBERNATE"], "when": { "model": "SCD40" },
  "blocked": "cannot measure once per wake-up", "fix": "An SCD41 measures once per round …" }
```

A rule applies when its `modes`, optional `families` and every `when` condition match (option
values; `$pin`, `$pin2`, `$powerPin`: whether that pin is wired; `$channel`: whether that value is
sent). `blocked` rules a mode out, `minFirmware` names the first firmware that runs it,
`note` explains a trade-off, `waitMs`/`sleepUa` replace the numbers in that mode.
`boards.json`: per family `current` (CPU, WiFi, modem sleep, light sleep), per board `sleepUa`
(whole board in deep sleep: regulator, USB chip, power LED). `tools/validate-catalog.mjs` checks
all of it.

## Pins that wake the board

The "wake up early" pin, buttons and rain gauges wake the board from light sleep (any GPIO) and
deep sleep (RTC/LP pins only, `wakePins` in `boards.json`):

| Family | Deep sleep wake |
|---|---|
| ESP32 | EXT0 (one pin, either level) + EXT1 (one more pin when it wakes on LOW; any number on HIGH) |
| ESP32-S2, S3 | EXT0 (one pin) + EXT1 (any number, one shared level) |
| ESP32-C3, C6 | GPIO wake on the LP pins (C3: 0–5, C6: 0–7), each with its own level |
| ESP8266 | none: its RST is taken by the deep sleep timer |

A pin wake-up in the middle of a deep sleep counts the tip or sends the press and goes back to
sleep for the rest of the interval; the round runs right away only when it is due within 2 s,
for the "wake up early" pin, or while an update proves itself. Hibernate wakes on the timer only.

## Held pins

The sensor supply pin, a fan's SET pin and the outputs keep their level through light and deep
sleep (`power::keepLevel`). After a deep sleep the firmware writes the level first, then
releases the hold, so no relay clicks on wake-up. ESP32, S2, S3 and C3 need
`gpio_deep_sleep_hold_en()` for that, the C6 holds single pins. Hibernate keeps the RTC
peripherals powered while a pin is held.

## What the board reports

Every request of a round carries `X-Device-Report`, read by the fleet view:

```
X-Device-Report: awake=4120;nap=9800;wifi=2300;wakes=3;wfail=2:AUTH;sens=scd4x:missing,pms5003:warming
```

| Key | Meaning |
|---|---|
| `awake` | ms the CPU ran in the last round |
| `nap` | ms of light sleep in the last round while sensors measured |
| `wifi` | ms until connected in the last round (0: no connect) |
| `wakes` | pin wake-ups since the last delivered value |
| `wfail` | failed connects since the last delivered value, and the last reason (`AUTH` wrong password, `NO_SSID` network not found, `TIMEOUT`, `LOST`) |
| `sens` | devices with a due value that did not arrive: `missing` (no answer at start-up), `timeout` (no measurement in time), `range` (outside the measuring range), `warming` (still settling) |

Reset reasons (brownout, watchdog, panic) stay in `X-Device-Status`.

## Battery, gauges and thresholds (0.8.0)

A config may carry a `battery` block. Without it, or with `"src": "usb"`, the board runs as before
and reports `src=usb` with its interval.

```json
"battery": { "src": "solar", "chem": "lipo", "cells": 1, "mah": 2000,
             "save": 3500, "rec": 3300, "sby": 3200, "res": 3600, "rev": 7 }
```

`src` is `usb`, `bat` or `solar`, `chem` `lipo`, `li_ion`, `lifepo4` or `custom`, thresholds are pack
millivolts (volts per cell × cells), all four or none. `rev` is the settings revision the backend
wrote; the board reports it back. `slp` (optional, after `rev`) is the current between rounds in
µA, only with INA charge counting (see Gauges). The parser checks the same rules as backend, web, library and
station (`test/vectors/threshold-rules-vectors.json`):

- Standby + 50 ≤ Recovery, Recovery + 50 ≤ Save, Recovery + 100 ≤ Resume ≤ Save + 400 (pack mV)
- each value inside the chemistry's per cell range: LiPo and Li-ion 2.80 to 4.10 V, LiFePO4 2.50 to
  3.40 V, none given counts as LiPo, `custom` checks the gaps only

### What the board does

The thresholds watch the first value of type `BATTERY_VOLTAGE` (ADC divider `battery`, a gauge,
an INA). That device is read every round, even when its own value goes out every n-th round only.
The state machine is `HydroNodeBatteryGuard` from HydroNode-Library 1.8.0, the class a sketch uses
too:

| State | Enters | Leaves | The board |
|---|---|---|---|
| NORMAL | | | runs as configured |
| SAVE | below Save | at Save + 150 mV | twice the interval, values sent every n-th round (n > 1) every 2n-th, gas and dust sensors (SGP30/40/41, PMS5003, PMSA003I, SEN5x) rest |
| RECOVERY | below Recovery, or 3 invalid readings | 2 valid readings at Resume or more, 60 s apart | one last round with `pwr=recovery`, outputs off, then deep sleep for the interval with WiFi off, reading only the battery |
| STANDBY | 2 valid readings below Standby in Recovery | a valid reading at Resume, then as Recovery | deep sleep for an hour, only the battery |

An invalid reading never leads to Standby. After a reset the first reading decides like a station
boot: at Resume or more the board runs, below it waits in Recovery. Outputs switch off without
forgetting their state: once the battery recovered they come back as they were. The guard's
memory, a clock that counts deep sleep and the INA charge counter live in RTC memory
(`power::BatteryState`; ESP8266 RTC slots 24 to 35). On the ESP32 the clock adds the time a deep
sleep really lasted (the system time keeps running), so a rain pulse or a button that ends the sleep
early does not move it ahead. With thresholds running a hibernate keeps that
memory powered. An ESP8266 sleeps deep in Recovery only when its config uses deep sleep (GPIO16
wired to RST); otherwise it naps in light sleep with the radio off.

Thresholds without a `BATTERY_VOLTAGE` value are not applied; the board reports
`err=no_measurement` and prints `WARN BATTERY`.

### Gauges

| Driver | Chip | Values | Needs |
|---|---|---|---|
| `max1704x` | MAX17043, MAX17048, MAX17049 (option `chip`, 0x36) | voltage, level, charge rate (not 43) | nothing, keeps its model while the battery is connected |
| `lc709203f` | LC709203F (0x0B) | voltage, level | capacity (pack size, APA from the data sheet table) and chemistry (profile: Li-ion 1, else 0), written only when the chip holds other values. Every transfer carries a CRC-8 |
| `bq27441` | BQ27441-G1 (0x55) | voltage, level, current | capacity up to 32767 mAh (signed 16 bit), written once as design capacity through the chip's config update mode when it differs. Larger values print `WARN BQ27441` and keep the chip's own; builder and server refuse them |
| `ina219`, `ina226`, `ina260` | option `battery` adds the value `pct` | level by counting charge | capacity. Positive current charges (IN+ to the board, IN− to the battery). A full battery (4.15 V per cell, LiFePO4 3.55 V, nothing flowing out) resets the count to 100 %, an unknown start begins from the voltage. The INA only measures while the board is awake: its current counts the time awake, the battery block's `slp` (µA between rounds, board and sensors at rest, worked out by the builder from the catalog) counts the time asleep |

All are register drivers in `src/drivers/Drivers.cpp`, no extra library. Gauges carry `kind: gauge`
and `minFirmware: 0.8.0` in the catalog, the INA level `minFirmware` on its channel. A gauge on the
switched sensor supply forgets its charge model; the catalog's sleep rules say so.

### What the board reports

The first request after every boot carries `X-Device-Config` (HydroNode-Library 1.8.0):

```
X-Device-Config: v=1 rev=7 int=300 save=3500 rec=3300 sby=3200 res=3600 src=solar gauge=max17048 cells=1 mah=2000 pwr=normal
```

and every request `pwr=` in `X-Device-Status`. Changes arrive as a config update over the air with
a new `battery` block; the firmware never takes the library's `settings` answer key.
