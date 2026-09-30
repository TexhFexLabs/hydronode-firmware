# HydroNode Firmware

Universal firmware for ESP32 boards that report to [HydroNode](https://hydronode.tech).
One binary per chip family (ESP32, ESP32-S2, ESP32-S3, ESP32-C3, ESP32-C6). Everything
device specific (sensors, pins, interval, sleep mode, WiFi, sensor credentials) lives in
a small config block in its own flash partition (`hncfg`).

The HydroNode web app builds that block in your browser and flashes firmware and config
over USB (Web Serial). The WiFi password never leaves the browser.

## Layout

| Path | Content |
|---|---|
| `src/` | firmware |
| `catalog/boards.json` | chip families, boards, pin rules, sleep modes per family |
| `catalog/drivers.json` | supported sensors, buses, channels, options |
| `catalog/sleep-modes.json` | energy modes |
| `catalog/libraries.json` | every bundled library with exact version and SPDX license |
| `partitions/` | flash layout (`hncfg` at `0x390000`, 8 KB) |
| `tools/validate-catalog.mjs` | consistency check, writes `dist/catalog.json` for the web app |
| `tools/license-check.mjs` | license gate, writes `THIRD_PARTY_LICENSES.md` |

## Build

```bash
pip install platformio
pio run                      # all chip families
pio test -e native           # host tests
node tools/validate-catalog.mjs --out dist
node tools/license-check.mjs --out dist/THIRD_PARTY_LICENSES.md
```

## Licensing

The firmware source is MIT. Binaries also contain the Arduino core for ESP32
(LGPL-2.1) and third-party libraries under MIT, BSD and Apache-2.0. Only libraries on
the allowlist in `catalog/libraries.json` may be linked; CI fails otherwise. Every
release ships `THIRD_PARTY_LICENSES.md` and the exact core source. You can rebuild and
relink the firmware from this repository with every dependency pinned.
