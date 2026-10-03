# Security Policy

## Reporting a vulnerability

Please report suspected vulnerabilities privately to **security@hydronode.tech**.
Do not open a public GitHub issue and do not include real HydroNode credentials
or WiFi passwords.

Include the affected firmware version (printed on boot as `HN:BOOT fw=…`), the
board, impact, reproduction steps and any proposed mitigation. You should receive
an acknowledgement within seven days. A fix and coordinated disclosure timeline
will be agreed after triage.

## What the firmware stores

The device config (WiFi credentials, sensor ID, device secret, pins) lives in the
`hncfg` flash partition in plain text. Flash encryption is not enabled. Anyone with
physical access to the device can read it. If a device is lost, rotate the device
secret in HydroNode and change the WiFi password if needed.

The HydroNode web flasher never sends the WiFi password to any server in readable
form. It builds the config block in the browser and writes it over USB. If the user
saves a network with a passkey, the browser encrypts it first and HydroNode keeps
only the encrypted copy.

## Updates over the air

- **Signed firmware.** Every OTA image is signed with ECDSA P-256 over the text
  `hydronode-ota-v1|<version>|<family>|<sha256>|<size>|<downgrade>`. The board
  verifies the signature against the public keys compiled into `src/ota/OtaKeys.h`
  before it writes anything, and the SHA-256 of the streamed image before it
  switches the boot slot. Downgrades need a signed flag.
- **Offline release key.** The private release key is generated on a YubiKey
  (PIV) and never leaves it. Releases are signed locally by the maintainer with
  `tools/sign-release.sh --key yubikey`. CI, GitHub and the HydroNode server only
  ever hold the public key. The server checks the same signature before it offers
  an image.
- **Key rotation.** The firmware trusts a list of keys. A new key ships in a
  firmware release signed with the old one; afterwards the old key can be removed.
  If you suspect the release key is compromised, report it as a vulnerability.
- **Dev key.** `tools/dev-keys/` creates a local test key. Only the `*-dev`
  environments trust it (`-DHN_OTA_DEV_KEY`); release builds never do. The key
  files are gitignored.
- **Config updates** are not signed separately. They arrive over TLS in the reply
  to an HMAC-signed request, like commands, and never contain WiFi credentials or
  the device secret. A config the firmware cannot parse is refused.
- **Rollback.** New firmware stays "pending verify" until it passes its check in
  the first wake cycle; any restart before that boots the previous firmware. The
  previous config is kept until the new one is confirmed. The bootloader and the
  partition table are never written over the air. Anti-rollback eFuses stay off.
