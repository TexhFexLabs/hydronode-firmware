#include "ConfigMerge.h"

#include <string.h>

namespace hn::ota {

namespace {

void writeU32(uint8_t* p, uint32_t v) {
    for (int i = 0; i < 4; i++) p[i] = uint8_t(v >> (8 * i));
}

MergeResult failed(const char* error) {
    MergeResult r{error, 0, makeResult(ConfigError::Ok, "")};
    return r;
}

}  // namespace

size_t encodeBlock(const char* json, size_t len, uint8_t* out, size_t cap) {
    if (len == 0 || len > kMaxPayload || kHeaderSize + len > cap) return 0;
    memcpy(out, "HNC1", 4);
    out[4] = uint8_t(kConfigSchema);
    out[5] = uint8_t(kConfigSchema >> 8);
    out[6] = 0;
    out[7] = 0;
    writeU32(out + 8, uint32_t(len));
    writeU32(out + 12, crc32(reinterpret_cast<const uint8_t*>(json), len));
    memmove(out + kHeaderSize, json, len);
    return kHeaderSize + len;
}

MergeResult mergeConfig(const char* oldJson, size_t oldLen, JsonVariantConst offered, uint32_t rev, uint8_t* out,
                        size_t cap, Config& scratch) {
    if (!offered.is<JsonObjectConst>() || rev == 0) return failed("config_invalid");

    JsonDocument old;
    if (deserializeJson(old, oldJson, oldLen)) return failed("config_invalid");
    if (!old["sensor"].is<JsonObjectConst>() || !old["wifi"].is<JsonObjectConst>()) return failed("config_invalid");

    JsonDocument merged;
    for (JsonPairConst kv : offered.as<JsonObjectConst>()) {
        // Credentials and network always come from the board, whatever the offer says.
        if (strcmp(kv.key().c_str(), "sensor") == 0 || strcmp(kv.key().c_str(), "wifi") == 0) continue;
        merged[kv.key()] = kv.value();
    }
    merged["v"] = kConfigSchema;
    merged["rev"] = rev;
    merged["sensor"] = old["sensor"];
    merged["wifi"] = old["wifi"];

    // Serialize behind the header; encodeBlock then fills the header in place.
    if (cap <= kHeaderSize) return failed("config_too_large");
    size_t len = measureJson(merged);
    if (len == 0 || len > kMaxPayload || kHeaderSize + len > cap) return failed("config_too_large");
    char* json = reinterpret_cast<char*>(out + kHeaderSize);
    serializeJson(merged, json, cap - kHeaderSize);

    MergeResult result{nullptr, 0, parsePayload(json, len, scratch)};
    if (result.parse.error != ConfigError::Ok) {
        memset(out, 0, cap);  // holds the WiFi password
        result.error = "config_invalid";
        return result;
    }
    result.length = encodeBlock(json, len, out, cap);
    return result;
}

}  // namespace hn::ota
