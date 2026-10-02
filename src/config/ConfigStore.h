#pragma once

// Reads and writes the raw "hncfg" block on the board: the ESP32 family keeps it in its own
// partition, the ESP8266 (no partition table) in a fixed 8 KB area at the end of the unused file
// system region of the 4 MB layout. The release manifest carries the same offset.

#include <stddef.h>
#include <stdint.h>

namespace hn::store {

constexpr size_t kBlockSize = 8192;

// Fills `out` (kBlockSize bytes) with the block. False when the partition is missing or unreadable.
bool readConfigBlock(uint8_t* out, size_t cap);

// Erases the area and writes `len` bytes (rest stays 0xFF). Verified by reading back.
bool writeConfigBlock(const uint8_t* block, size_t len);

}  // namespace hn::store
