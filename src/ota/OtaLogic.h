#pragma once

// Rules of updates over the air that need no hardware: version order, the signed text, the
// checks an offer must pass before anything is downloaded, the verification after a restart and
// the header values. Compiled for the host in the native tests, so no Arduino headers here.

#include <stddef.h>
#include <stdint.h>

namespace hn::ota {

// STRICT: valid after a signed ingest and every configured sensor read. INGEST ("Lenient" in the
// app): valid after the first signed ingest.
enum class VerifyMode : uint8_t { Strict, Lenient };

VerifyMode parseVerifyMode(const char* name);
const char* verifyModeName(VerifyMode mode);

// Version order after semver 2.0, the same as the backend (Semver.java): numeric core, then a
// pre-release ranks below its release ("0.5.1-dev" < "0.5.1"), build metadata is ignored. A
// version that does not parse sorts below every valid one. Negative when a is older.
int compareVersions(const char* a, const char* b);

// Longest version name the backend sends (its columns hold 32 characters). Longer offers are
// refused as bad_offer instead of being cut short in the update record.
constexpr size_t kMaxVersionLength = 32;

// The flags part of X-Firmware: "ota cfg=14" on boards that update firmware over the air,
// "cfg=3" on the ESP8266 (config only).
void firmwareFlags(bool otaCapable, uint32_t rev, char* out, size_t cap);

// The text a release key signs (one line, no newline):
//   hydronode-ota-v1|<version>|<family>|<sha256 hex lowercase>|<size>|<downgrade 0|1>
// False when it does not fit or the hash is not 64 hex digits.
bool signedText(const char* version, const char* family, const char* sha256Hex, uint32_t size, bool downgrade,
                char* out, size_t cap);

struct FirmwareOffer {
    const char* job;
    const char* version;
    const char* family;
    uint32_t size;
    const char* sha256;
    const char* sig;
    const char* keyId;
    bool downgrade;
    VerifyMode verify;
    const char* url;  // path on the HydroNode host, e.g. /api/device-ota/v1/image?job=<id>
};

// Everything about an offer that can be checked before the signature: nullptr when it may go
// ahead, otherwise the failure code sent with the ack (bad_offer, family_mismatch, same_version,
// downgrade_not_allowed, no_space). A version longer than kMaxVersionLength is a bad_offer.
const char* checkFirmwareOffer(const FirmwareOffer& offer, const char* ownFamily, const char* ownVersion,
                               uint32_t slotSize);

constexpr uint8_t kVerifyAttempts = 3;
constexpr uint32_t kVerifyLimitMs = 120000;

// One verification attempt in the first wake cycle after an update.
struct VerifyInput {
    VerifyMode mode;
    uint8_t attempt;           // 1-based
    uint32_t elapsedMs;        // since the new firmware or config started
    bool wifiOk;
    int ingestStatus;          // best HTTP status of this round's values, <= 0 transport error
    const char* failedDriver;  // first driver that did not read, nullptr when all did
};

enum class VerifyStep : uint8_t { Verified, Retry, RollBack };

struct VerifyDecision {
    VerifyStep step;
    // Why it failed: wifi_failed, server_unreachable, ingest_failed:<status>,
    // sensor_read_failed:<driver>, timeout. Empty when verified.
    char reason[48];
};

// Up to kVerifyAttempts attempts within kVerifyLimitMs, then roll back with the last reason (or
// "timeout" when the time ran out first). A server that cannot be reached also rolls back: the
// backend offers the job again later and does not count that round.
VerifyDecision decideVerify(const VerifyInput& in);

// X-Ota-State while the new firmware or config proves itself: verifying;try=1;mode=STRICT;job=<id>
void otaStateHeader(uint8_t attempt, VerifyMode mode, const char* job, char* out, size_t cap);

// X-Ota-Result, sent once after a rollback: rolled_back;<reason>;job=<id>
void otaResultHeader(const char* reason, const char* job, char* out, size_t cap);

}  // namespace hn::ota
