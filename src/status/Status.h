#pragma once

// Machine readable status lines for the web flasher's serial monitor.
// Every line starts with "HN:" followed by a keyword, e.g.
//   HN:BOOT fw=0.1.0 family=esp32c3 wake=TIMER
//   HN:CFG ok devices=2 mode=DEEP_SLEEP interval=300
//   HN:WIFI ok rssi=-61 ms=812
//   HN:SEND TEMPERATURE 202
//   HN:ERR WIFI NO_SSID
// Secrets are never printed. Keywords are a contract with the web app.

namespace hn::status {

void begin();
void line(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void flush();

}  // namespace hn::status
