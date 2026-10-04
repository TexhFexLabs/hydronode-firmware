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
4. **Only then the radio.** A round with no value due does not connect at all. All values share
   one TLS connection.
5. **Sleep** as deep as the config says, until the next round on the internet-time grid.

The serial line `HN:COST awake=… nap=… wifi=…` closes every round; the same numbers go to the
server with the next round (below).

## Per sensor

| Sensor | Between rounds | Per due round |
|---|---|---|
| SHT4x, SHT3x, AHT, HTU21D, SHTC3, MS8607, BME280, BMP280, BMP3xx, LPS22, BME680 | sleeps by itself (single shot / forced mode) | one measurement, ≤ 200 ms |
| BH1750, MCP9808, TMP117, DPS310, VEML7700, TSL2591, LTR390 | shut down / standby | one-shot, the board naps during the conversion |
| INA219, INA226, INA260, ADS1115 | power-down / single shot | triggered conversion |
| DS18B20 | idle | 750 ms conversion, napped |
| SCD41 (sleeping modes) | power-down, 0.4 µA | wake, one shot thrown away, one shot kept (2 × 5 s napped) |
| SCD40, SCD41 awake | measures every 5 s by itself | reads the latest value |
| SCD30 | measures by itself at 0.9 × the round spacing (max 1800 s) | reads the waiting value. Never reset after a sleep (Adafruit's `begin()` would) |
| PMS5003, PMSA003I | fan asleep with SET wired (held low through deep sleep), else running | fan on, 30 s run-up napped |
| SEN5x | idle, or gas-only mode when VOC/NOx are sent (indices keep learning on the sensor) | full measurement, 10 s run-up napped |
| SGP40, SGP41 (VOC) | light sleep: a reading every 10 s with the heater off (Sensirion low power) | latest index |
| SGP30, SGP41 NOx | awake modes only: a reading every second | latest value |
| Rain gauge | each tip wakes the board ~20 ms, counted in RTC memory | the sum since the last round |
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
