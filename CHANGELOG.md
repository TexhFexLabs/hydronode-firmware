# Changelog

## 0.9.0 (not released yet)

- Outputs (relay, LED, switched output) can report their state: "Report output state" in the device
  builder, off by default. The output sends `1`/`0` on its own type (`RELAY1_STATE`) whenever it
  changes: by command, by a button, at the end of a timed pulse, and once after a fresh start.
  Without the option the output sends nothing, as before. Needs firmware 0.9.0 (`minFirmware`).
- A reported state the server did not take (429, no network, a server error) stays open and goes out
  again with the next round, until it is accepted. Only the newest state of an output stays open; a
  newer change replaces it. A refused value (400, 401) is not sent again, button presses are not
  repeated.

Earlier versions are described in the release notes on GitHub.
