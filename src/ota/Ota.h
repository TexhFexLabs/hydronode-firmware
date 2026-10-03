#pragma once

// Updates over the air: firmware (ESP32 family) and config (all families, ESP8266 included).
//
// An offer arrives in the answer to a sent value ("ota" or "config" next to "commands") and is
// carried out after the round, when every value is out. The new firmware or config then has to
// prove itself in its first wake cycle (up to 3 tries, at most 2 minutes): Strict needs a signed
// ingest and every sensor read, Lenient only the ingest. Otherwise the board goes back to what
// it ran before and reports why with the next value.

#include "config/Config.h"

class HydroNode;

namespace hn::ota {

// Call at boot, right after loading the config. Finds an update that waits for verification or
// for its rollback to be reported; restores the old config (and restarts) when a new one does
// not parse or keeps crashing.
void begin(const Config& cfg, const ParseResult& configResult);

// Call once the HydroNode client exists: firmware identity, update hooks, X-Ota-* headers.
void attach(HydroNode& hydro, const Config& cfg);

// Call after every sendValue() with its status. An accepted answer (202) without an offer drops
// an offer kept from an earlier answer: only what the server still offers is carried out.
void afterSend(int status);

// What one round achieved, for the verification.
struct RoundReport {
    bool wifiOk;
    int bestStatus;            // 202 when a value got through, else the last status (<= 0 transport)
    const char* failedDriver;  // first driver that did not read, nullptr when all did
};

// Call after each round. True when the round has to be repeated now (verification retry, after a
// short pause). Carries out a waiting offer, which may restart the board.
bool afterRound(HydroNode* hydro, const Config& cfg, const RoundReport& report);

// Seconds to wait before a verification retry (the backend takes one value per type every 10 s).
constexpr uint32_t kRetryPauseMs = 15000;

}  // namespace hn::ota
