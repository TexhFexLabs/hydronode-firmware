# Updates over the air

Firmware 0.5.0 and later update over the air. One USB flash with the device builder is needed to get
there; after that the fleet view in HydroNode sends new firmware and new config to the board. The
user guide is at <https://hydronode.tech/docs/features/fleet>.

| | ESP32, S2, S3, C3, C6 | ESP8266 |
|---|---|---|
| Firmware over the air | yes, signed images only | no (no second app slot) |
| Config over the air | yes | yes |
| Rollback | bootloader, plus the check below | old config from flash |

## What the board reports

Every request carries the version and how the board is doing (HydroNode-Library 1.6.0):

```
X-Firmware: hydronode/0.5.0 esp32c3 ota cfg=14
X-Device-Status: boot=12;reset=poweron;uptime=45;rssi=-61;net=wifi;readErr=
```

`ota` appears only on the ESP32 family. `cfg` is the config revision, counted up by every flash and
every config update. `readErr` lists the drivers that did not answer in this round.

## Firmware update

1. The answer to a sent value carries an offer next to `commands`:
   `{"ota": {"job", "version", "family", "size", "sha256", "sig", "keyId", "downgrade", "verify", "url"}}`.
2. After the round (every value is out), the board checks the offer: own chip family, a newer version
   (or a signed downgrade flag), the size fits the free slot, and the signature verifies with a key
   compiled into the firmware. Any failure is reported with `POST /api/webhook/sensor-ota-ack`
   (`failed` + `bad_offer`, `family_mismatch`, `same_version`, `downgrade_not_allowed`, `no_space`,
   `signature_invalid`) and nothing changes.
3. The image streams into the inactive slot (`downloadSigned`, resumed with `Range` up to three times)
   while its SHA-256 is computed. A mismatch (`sha256_mismatch`), an HTTP error (`http_<code>`) or a
   stalled download (`timeout`) aborts; the running firmware stays.
4. Boot slot switched, ack `downloaded`, restart.

## Config update

`{"config": {"job", "rev", "verify", "config"}}`: the device config without WiFi, sensor ID and
secret. The board copies those three from its current config, writes the new block (same format as
the web flasher, `HNC1` + CRC), keeps the old block (ESP32: NVS, ESP8266: flash next to the
config), acks `config_applied` and restarts. A merged config the firmware would not run is refused
with `config_invalid` before anything is written.

## The first wake cycle decides

New firmware starts "pending verify" (`verifyRollbackLater()` returns true, so the Arduino core does
not confirm it on its own). Until it is confirmed, every request carries
`X-Ota-State: verifying;try=1;mode=STRICT;job=<id>`.

- **Strict** (`STRICT`): a signed ingest with a 2xx answer and every configured sensor read.
- **Lenient** (`INGEST`): a signed ingest with a 2xx answer.

Up to three tries, 15 s apart, at most two minutes. The board does not sleep before the verdict.

- Passed: firmware marked valid (`esp_ota_mark_app_valid_cancel_rollback`), old config dropped, ack
  `verified` with the time it took (`41s`).
- Failed: `esp_ota_mark_app_invalid_rollback_and_reboot()`, or the old config written back and a
  restart. The old firmware/config then sends `X-Ota-Result: rolled_back;<reason>;job=<id>` once.

Reasons: `sensor_read_failed:<driver>`, `ingest_failed:<status>`, `server_unreachable` (5xx or no
answer while WiFi works, the backend offers the job again later), `wifi_failed`, `timeout`,
`config_invalid` (new config does not parse), `boot_failed` (crashed before the verdict: on the
ESP32 the bootloader rolls back, on the ESP8266 the fourth unverified start restores the old config).

Outputs (relays, LEDs) keep their last state over every restart: NVS on the ESP32 family, RTC memory
on the ESP8266 (not over a power cut, then the start state from the config applies).

## What the server does

The fleet view in HydroNode creates one job per board and change. In short, the backend:

- An offer stands in every reply while the job is offered and counts once per round. A board that
  does not take it within three rounds fails the job ("Offered 3 times, not taken").
- A download without progress for 10 minutes is offered again. No report within three intervals
  plus five minutes after the restart fails the job ("No report after restart").
- A rollback with `server_unreachable`, `http_5xx` or `ingest_failed:5xx` is the server's fault: the
  job is queued again, up to five times.
- A job can be cancelled while it is queued or offered. The board only takes an offer the latest
  reply still carries, so a cancelled job is never applied late.
- After `verified`, or a report of the target version without `X-Ota-State`, the job is installed
  and the saved setup follows the board (firmware version, config and revision).
- One open job per board. Firmware and config in one change run as one job, firmware first.

## Signing

The text signed per image (UTF-8, no newline):

```
hydronode-ota-v1|<version>|<family>|<sha256 hex lowercase>|<size>|<downgrade 0|1>
```

ECDSA P-256 over SHA-256, DER, Base64. The manifest carries it per image:
`images.<family>.ota = {file, sha256, size, sig, keyId, downgrade}`. `file` is the bare app
(`<family>-app.bin`), not the merged USB image. The firmware trusts the keys in `src/ota/OtaKeys.h`
(a list, so keys can be rotated). The backend checks the same signature before it offers an image.

### Release key on a YubiKey

The private release key is created on the YubiKey (PIV slot 9c) and never leaves it.

```bash
python tools/package-release.py                       # after the CI build of the tag
bash tools/sign-release.sh --key yubikey --pub prod.pub.pem --key-id prod-2026-10 [--slot 9c]
```

`yubico-piv-tool` asks for the PIN and signs on the key; every signature is verified with `--pub`
before the manifest is written. SHA256SUMS and the release zip are rebuilt; `firmware.lock` in the
backend then pins the new zip. The current release key is `prod-2026-10` (YubiKey PIV slot 9c,
public key in `tools/release-keys/` and `kReleaseKeys`). A new key goes into `kReleaseKeys` in a
release signed with the old one before the old one is retired.

### Dev key (local tests)

```bash
bash tools/dev-keys/make-dev-key.sh              # ota-dev.pem, .pub.pem, .pub.h (gitignored)
pio run -e esp32c3-dev                           # trusts the dev key (-DHN_OTA_DEV_KEY)
python tools/package-release.py
bash tools/sign-release.sh --key tools/dev-keys/ota-dev.pem
```

## Limits

- Images may use at most 85 % of the 1.75 MB slot (`tools/check-image-size.py`, also in CI).
- The partition table and the bootloader never change over the air.
- Anti-rollback through eFuses stays off.

## Recovery and verification checks

The device defers a blocking update while a timed output is active. Normal rounds still service
its off deadline. The offer is taken after a later round when all timed outputs have finished.

A config rollback records restoration in progress before writing flash. The backup stays until
restoration succeeds and its result reaches HydroNode. A reset during restoration resumes it at
boot. The loaded revision must match the offered revision before it can be confirmed.
Confirmation is saved before backup cleanup, so a reset during cleanup keeps the verified config.
A missing ESP8266 state record also restores a surviving backup. Any second boot before
config confirmation restores the old config.

Strict verification distinguishes a gas algorithm warming up from failed sensor communication.
Failed initialization, failed raw samples and missing drivers block confirmation. A two-minute
timer restarts a stalled verification even when a driver or network call does not return.
The next boot rolls back unconfirmed firmware or restores unconfirmed config. A timer reset is
reported as `boot_failed` when no more specific verdict could be saved.

Run `pio test -e native-ota` to exercise the real OTA state machine and measurement loop against
simulated flash, resets and network responses. Run `python tools/test-sign-release.py` to verify
that signing an explicit manifest uses that release's version.
