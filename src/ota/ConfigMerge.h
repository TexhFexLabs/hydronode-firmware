#pragma once

// A config update over the air carries the device config without WiFi, sensor ID and secret: those
// never leave the board. The new block is the offered config plus "sensor" and "wifi" from the
// block on the board, with the new revision. Host-testable, no Arduino headers.

#include <ArduinoJson.h>
#include <stddef.h>
#include <stdint.h>

#include "config/Config.h"

namespace hn::ota {

struct MergeResult {
    const char* error;  // nullptr, or a code for the ack: config_invalid, config_too_large
    size_t length;      // bytes written to `out` (block incl. header)
    ParseResult parse;  // why the merged config does not parse, when error = config_invalid
};

// Builds the complete "hncfg" block (header + JSON) for the offered config into `out`.
// `oldJson` is the payload of the current block. `scratch` is filled with the parsed result,
// which also proves the firmware accepts it.
MergeResult mergeConfig(const char* oldJson, size_t oldLen, JsonVariantConst offered, uint32_t rev, uint8_t* out,
                        size_t cap, Config& scratch);

// Header (magic, schema, length, CRC) + payload. Returns the block length, 0 when it does not fit.
size_t encodeBlock(const char* json, size_t len, uint8_t* out, size_t cap);

}  // namespace hn::ota
