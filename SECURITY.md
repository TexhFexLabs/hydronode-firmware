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

The HydroNode web flasher never sends the WiFi password to any server. It builds
the config block in the browser and writes it over USB.
