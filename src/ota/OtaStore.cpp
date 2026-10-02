#include "OtaStore.h"

#include <Arduino.h>
#include <string.h>

#include "config/Config.h"

#if !defined(ESP8266)
#include <Preferences.h>
#endif

namespace hn::ota {

namespace {

constexpr uint32_t kMagic = 0x484E4F54;  // "HNOT"

struct Record {
    uint32_t magic;
    uint32_t crc;
    Pending pending;
};

#if defined(ESP8266)
constexpr uint32_t kSector = 4096;
constexpr uint32_t kStateOffset = 0x3F5000;
constexpr uint32_t kBackupOffset = 0x3F6000;
constexpr size_t kBackupSize = 8192;

bool writeArea(uint32_t offset, size_t areaSize, const uint8_t* data, size_t len) {
    for (uint32_t s = 0; s < areaSize / kSector; s++) {
        if (!ESP.flashEraseSector(offset / kSector + s)) return false;
    }
    if (len == 0) return true;
    size_t padded = (len + 3) & ~size_t(3);
    uint8_t* buf = static_cast<uint8_t*>(malloc(padded));
    if (!buf) return false;
    memset(buf, 0xFF, padded);
    memcpy(buf, data, len);
    bool ok = ESP.flashWrite(offset, reinterpret_cast<uint32_t*>(buf), padded);
    memset(buf, 0, padded);
    free(buf);
    return ok;
}
#endif

uint32_t recordCrc(const Pending& p) { return crc32(reinterpret_cast<const uint8_t*>(&p), sizeof(p)); }

}  // namespace

bool loadPending(Pending& out) {
    Record r{};
#if defined(ESP8266)
    if (!ESP.flashRead(kStateOffset, reinterpret_cast<uint32_t*>(&r), sizeof(r))) return false;
#else
    Preferences prefs;
    if (!prefs.begin("hn-ota", true)) return false;
    size_t n = prefs.getBytes("pending", &r, sizeof(r));
    prefs.end();
    if (n != sizeof(r)) return false;
#endif
    if (r.magic != kMagic || r.crc != recordCrc(r.pending)) return false;
    out = r.pending;
    out.job[sizeof(out.job) - 1] = '\0';
    out.fromVersion[sizeof(out.fromVersion) - 1] = '\0';
    out.toVersion[sizeof(out.toVersion) - 1] = '\0';
    out.result[sizeof(out.result) - 1] = '\0';
    return out.kind != PendingKind::None;
}

bool savePending(const Pending& pending) {
    Record r{kMagic, recordCrc(pending), pending};
#if defined(ESP8266)
    return writeArea(kStateOffset, kSector, reinterpret_cast<const uint8_t*>(&r), sizeof(r));
#else
    Preferences prefs;
    if (!prefs.begin("hn-ota", false)) return false;
    bool ok = prefs.putBytes("pending", &r, sizeof(r)) == sizeof(r);
    prefs.end();
    return ok;
#endif
}

void clearPending() {
#if defined(ESP8266)
    writeArea(kStateOffset, kSector, nullptr, 0);
#else
    Preferences prefs;
    if (prefs.begin("hn-ota", false)) {
        prefs.remove("pending");
        prefs.end();
    }
#endif
}

bool saveBackup(const uint8_t* block, size_t len) {
#if defined(ESP8266)
    return len <= kBackupSize && writeArea(kBackupOffset, kBackupSize, block, len);
#else
    Preferences prefs;
    if (!prefs.begin("hn-ota", false)) return false;
    bool ok = prefs.putBytes("cfgbak", block, len) == len;
    prefs.end();
    return ok;
#endif
}

size_t loadBackup(uint8_t* out, size_t cap) {
#if defined(ESP8266)
    size_t len = cap < kBackupSize ? cap : kBackupSize;
    if (!ESP.flashRead(kBackupOffset, reinterpret_cast<uint32_t*>(out), len)) return 0;
    // The block carries its own length and CRC; an erased area has no magic.
    const uint8_t* payload = nullptr;
    size_t payloadLen = 0;
    if (parseHeader(out, len, &payload, &payloadLen).error != ConfigError::Ok) return 0;
    return kHeaderSize + payloadLen;
#else
    Preferences prefs;
    if (!prefs.begin("hn-ota", true)) return 0;
    size_t len = prefs.getBytesLength("cfgbak");
    size_t n = (len > 0 && len <= cap) ? prefs.getBytes("cfgbak", out, cap) : 0;
    prefs.end();
    return n;
#endif
}

void clearBackup() {
#if defined(ESP8266)
    writeArea(kBackupOffset, kBackupSize, nullptr, 0);
#else
    Preferences prefs;
    if (prefs.begin("hn-ota", false)) {
        prefs.remove("cfgbak");
        prefs.end();
    }
#endif
}

}  // namespace hn::ota
