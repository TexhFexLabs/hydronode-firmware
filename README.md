# HydroNode Firmware

Universal firmware for ESP32 and ESP8266 boards that report to [HydroNode](https://hydronode.tech).
One binary per chip family (ESP32, ESP32-S2, ESP32-S3, ESP32-C3, ESP32-C6, ESP8266). Everything
device specific (sensors, outputs, pins, interval, sleep mode, WiFi, sensor credentials) lives in
a small config block in its own flash partition (`hncfg`, on the ESP8266 at `0x3F8000`).

The HydroNode web app builds that block in your browser and flashes firmware and config
over USB (Web Serial). The WiFi password never leaves the browser.

## Layout

| Path | Content |
|---|---|
| `src/` | firmware |
| `src/actuators/` | relays, LEDs, switched outputs and buttons, driven by HydroNode commands |
| `src/scanner/` | I²C and 1-Wire scanner (`scan-*` environments), flashed from the wiring step |
| `catalog/boards.json` | chip families, boards, pin rules, sleep modes per family |
| `catalog/drivers.json` | supported sensors, buses, channels, options |
| `catalog/sleep-modes.json` | energy modes |
| `catalog/libraries.json` | every bundled library with exact version and SPDX license |
| `partitions/` | flash layout (`hncfg` at `0x390000`, 8 KB) |
| `tools/validate-catalog.mjs` | consistency check, writes `dist/catalog.json` for the web app |
| `tools/license-check.mjs` | license gate, writes `THIRD_PARTY_LICENSES.md` |
| `tools/encode-config.mjs` | reference encoder for the config block |
| `tools/package-release.py` | merged images, `manifest.json`, `SHA256SUMS`, release zip |
| `catalog/fixtures/` | golden config block shared with the web encoder |

## Build

```bash
pip install platformio
pio run                      # all chip families plus the scanners
pio test -e native           # host tests
node tools/validate-catalog.mjs --out dist
node tools/license-check.mjs --out dist/THIRD_PARTY_LICENSES.md
python tools/package-release.py   # merged images + manifest.json → dist/
```

A release tag `vX.Y.Z` (must match `version` in `platformio.ini`) runs the same steps in
CI and publishes `hydronode-firmware-X.Y.Z.zip` plus the Arduino core source.

## Serial status lines

The firmware prints machine readable lines at 115200 baud, all prefixed with `HN:`
(`BOOT`, `CFG`, `DEV`, `OUT`, `BTN`, `OW`, `ROUND`, `WIFI`, `SEND`, `SLEEP`, `ERR …`). The web
flasher shows them as a checklist. Secrets are never printed.

## Rounds and timing

The board measures in rounds, one per interval. A value can be sent every n-th round only
(`"n"` in the config). Rounds start on the interval, not interval plus awake time: the firmware
subtracts the time it was awake from the sleep. All values of a round share one TLS
connection; on the ESP8266 (160 MHz) the handshake takes about 3 s, every further value about
0.6 s. `catalog/boards.json` carries these timings per family, the web app adds them up to the
fastest interval a configuration can keep.

## Outputs and commands

Relays, LEDs and switched outputs listen to HydroNode commands named after the device
(`relay1`): `BOOL` switches, `UINT32` switches on for that many milliseconds (at most 24 h),
`<name>_level` (`UINT32`, 0–100) dims an LED. Commands arrive with the answer to a sent value,
so a board with outputs always sends at least one value (the WiFi signal costs nothing). A button
sends its press right away and can toggle an output on the same board.

## Scanner

`scan-<family>` builds a small firmware that answers serial commands: `I2C <sda> <scl>` lists
every I²C address that acknowledges, `OW <pin>` every 1-Wire ROM. The wiring step flashes it,
scans and suggests sensors from the catalog that use the found addresses.

## Licensing

The firmware source is MIT. Binaries also contain the Arduino core for ESP32 or ESP8266
(LGPL-2.1) and third-party libraries under MIT, BSD and Apache-2.0. Only libraries on
the allowlist in `catalog/libraries.json` may be linked; CI fails otherwise. Every
release ships `THIRD_PARTY_LICENSES.md` and the exact core source. You can rebuild and
relink the firmware from this repository with every dependency pinned.
