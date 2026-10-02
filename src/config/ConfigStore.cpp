#include "ConfigStore.h"

#include <Arduino.h>
#include <string.h>

#if !defined(ESP8266)
#include <esp_partition.h>
#endif

namespace hn::store {

namespace {

#if defined(ESP8266)
constexpr uint32_t kConfigOffset8266 = 0x3F8000;
constexpr uint32_t kSector = 4096;
#else
constexpr uint8_t kConfigSubtype = 0x40;

const esp_partition_t* partition() {
    return esp_partition_find_first(ESP_PARTITION_TYPE_DATA, esp_partition_subtype_t(kConfigSubtype), "hncfg");
}
#endif

}  // namespace

bool readConfigBlock(uint8_t* out, size_t cap) {
    size_t len = cap < kBlockSize ? cap : kBlockSize;
#if defined(ESP8266)
    return ESP.flashRead(kConfigOffset8266, reinterpret_cast<uint32_t*>(out), len);
#else
    const esp_partition_t* part = partition();
    if (!part) return false;
    if (part->size < len) len = part->size;
    return esp_partition_read(part, 0, out, len) == ESP_OK;
#endif
}

bool writeConfigBlock(const uint8_t* block, size_t len) {
    if (len == 0 || len > kBlockSize) return false;
    // Flash writes go in whole 4-byte words; pad with 0xFF like erased flash.
    size_t padded = (len + 3) & ~size_t(3);
    uint8_t* buf = static_cast<uint8_t*>(malloc(padded));
    if (!buf) return false;
    memset(buf, 0xFF, padded);
    memcpy(buf, block, len);
    bool ok;
#if defined(ESP8266)
    ok = ESP.flashEraseSector(kConfigOffset8266 / kSector) && ESP.flashEraseSector(kConfigOffset8266 / kSector + 1) &&
         ESP.flashWrite(kConfigOffset8266, reinterpret_cast<uint32_t*>(buf), padded);
#else
    const esp_partition_t* part = partition();
    ok = part && esp_partition_erase_range(part, 0, part->size) == ESP_OK &&
         esp_partition_write(part, 0, buf, padded) == ESP_OK;
#endif
    if (ok) {
        // Read back: a block that does not match would leave the board without config.
        uint8_t* check = static_cast<uint8_t*>(malloc(padded));
        ok = check && readConfigBlock(check, padded) && memcmp(check, buf, padded) == 0;
        if (check) {
            memset(check, 0, padded);
            free(check);
        }
    }
    memset(buf, 0, padded);  // holds the WiFi password
    free(buf);
    return ok;
}

}  // namespace hn::store
