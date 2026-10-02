#pragma once

// What an update leaves behind for the next boot: the job being verified, and a copy of the old
// config to go back to. ESP32 family: NVS (namespace "hn-ota"). ESP8266: two flash areas in the
// unused file system region next to the config (0x3F5000 state, 0x3F6000 backup).

#include <stddef.h>
#include <stdint.h>

namespace hn::ota {

enum class PendingKind : uint8_t { None = 0, Firmware = 1, Config = 2 };

struct Pending {
    PendingKind kind;
    uint8_t mode;          // VerifyMode
    uint8_t boots;         // starts of the new firmware/config without verification
    uint8_t reserved;
    char job[40];
    char fromVersion[16];
    char toVersion[16];
    uint32_t fromRev;
    uint32_t toRev;
    // Set when the update was rolled back: the reason the old firmware/config reports once in
    // X-Ota-Result, then the record is cleared.
    char result[48];
};

bool loadPending(Pending& out);
bool savePending(const Pending& pending);
void clearPending();

// The old config block, kept until the new one is verified.
bool saveBackup(const uint8_t* block, size_t len);
// Copies the backup into `out`; returns its length, 0 when there is none.
size_t loadBackup(uint8_t* out, size_t cap);
void clearBackup();

}  // namespace hn::ota
