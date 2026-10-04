# HydroNode Firmware

Universal firmware for ESP32 and ESP8266 boards that report to [HydroNode](https://hydronode.tech).
One binary per chip family (ESP32, ESP32-S2, ESP32-S3, ESP32-C3, ESP32-C6, ESP8266). Everything
device specific (sensors, outputs, pins, interval, sleep mode, WiFi, sensor credentials) lives in
a small config block in its own flash partition (`hncfg`, on the ESP8266 at `0x3F8000`).

The HydroNode web app builds that block in your browser and flashes firmware and config
over USB (Web Serial). The WiFi password never leaves the browser in readable form.

From 0.5.0 on, that USB flash is the last one a board needs. The ESP32 family (ESP32, S2, S3, C3,
C6) then takes signed firmware updates and config updates over the air from the HydroNode fleet
view; the ESP8266 takes config updates. A board that fails its check after an update goes back
to what it ran before by itself. See [Updates over the air](#updates-over-the-air).

## Layout

| Path | Content |
|---|---|
| `src/` | firmware |
| `src/actuators/` | relays, LEDs, switched outputs and buttons, driven by HydroNode commands |
| `src/ota/` | updates over the air: offer checks, signed download, config merge, verify and rollback, trusted keys (`OtaKeys.h`) |
| `src/scanner/` | I²C and 1-Wire scanner (`scan-*` environments), flashed from the wiring step |
| `catalog/boards.json` | chip families, boards, pin rules, sleep modes, wake pins and currents per family, deep sleep current per board |
| `catalog/drivers.json` | supported sensors, buses, channels, options, timing and currents, sleep rules per power mode ([docs/POWER.md](docs/POWER.md)), `minFirmware` for drivers newer than 0.5.0 |
| `catalog/sleep-modes.json` | energy modes |
| `catalog/libraries.json` | every bundled library with exact version and SPDX license |
| `partitions/` | flash layout (`hncfg` at `0x390000`, 8 KB) |
| `tools/validate-catalog.mjs` | consistency check, writes `dist/catalog.json` for the web app |
| `tools/license-check.mjs` | license gate, writes `THIRD_PARTY_LICENSES.md` |
| `tools/encode-config.mjs` | reference encoder for the config block |
| `tools/package-release.py` | merged images, bare app images for OTA, `manifest.json`, `SHA256SUMS`, release zip |
| `tools/sign-release.sh` | signs every OTA image of a release (YubiKey or a PEM key), writes the `ota` blocks into the manifest |
| `tools/dev-keys/` | local dev signing key for tests (`make-dev-key.sh`, keys gitignored) |
| `tools/check-image-size.py` | fails when an image uses more than 85 % of its OTA slot |
| `catalog/fixtures/` | golden config block shared with the web encoder |

## Build

```bash
pip install platformio
pio run                      # all chip families plus the scanners
pio test -e native           # host tests (config, OTA logic)
pio test -e native-ota       # OTA state machine and measurement loop on simulated hardware
node tools/validate-catalog.mjs --out dist
node tools/license-check.mjs --out dist/THIRD_PARTY_LICENSES.md
python tools/package-release.py   # merged images + manifest.json → dist/
```

A release tag `vX.Y.Z` (must match `version` in `platformio.ini`) runs the same steps in
CI and publishes `hydronode-firmware-X.Y.Z.zip` plus the Arduino core source. The OTA images are
signed afterwards, locally, with the release key on the YubiKey (see [docs/OTA.md](docs/OTA.md)).

For local OTA tests, build a firmware that trusts the dev key:

```bash
bash tools/dev-keys/make-dev-key.sh            # once, keys stay local
pio run -e esp32c3-dev                         # also esp32-dev, esp32s3-dev
python tools/package-release.py
bash tools/sign-release.sh --key tools/dev-keys/ota-dev.pem
```

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

## Updates over the air

From 0.5.0 on, the ESP32 family takes signed firmware updates over the air and every family, the
ESP8266 included, takes config updates. Getting there takes one USB flash with the device builder;
a board on an older version shows "needs USB once" in the fleet view.

| | ESP32, S2, S3, C3, C6 | ESP8266 |
|---|---|---|
| Firmware over the air | yes, signed images only | no, no room for a second app slot |
| Config over the air | yes | yes |

- The update is offered in the reply to a sent value and taken after the round, so no reading is
  lost. The board downloads the image itself, nothing reaches into its network.
- Every image is signed with ECDSA P-256. The board checks the signature against the keys compiled
  into the firmware before it writes a byte, and the SHA-256 while it writes.
- New firmware or config has to prove itself in its first wake cycle (Strict or Lenient),
  otherwise the board goes back to what it ran before and reports why.
- WiFi and the sensor secret never travel with a config update. The board keeps its own.
- Outputs keep their last state over a restart.
- Release builds trust only the keys in `src/ota/OtaKeys.h`: the release key `prod-2026-10`
  (`tools/release-keys/prod-2026-10.pub.pem`), which lives on a YubiKey.

Details, the wire format, signing with the YubiKey and the local dev key: [docs/OTA.md](docs/OTA.md).

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
