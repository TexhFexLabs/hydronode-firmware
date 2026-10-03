#pragma once

// Signature and hash checks for firmware images (ESP32 family; the ESP8266 takes config only).

#include <stddef.h>
#include <stdint.h>

namespace hn::ota {

// True when the key is trusted by this build.
bool hasKey(const char* keyId);

// ECDSA P-256 over SHA-256 of `text`, signature as Base64 of the DER encoding.
bool verifySignature(const char* keyId, const char* text, const char* sigBase64);

// SHA-256 of a stream, fed chunk by chunk while the image is written.
class Sha256 {
public:
    Sha256();
    ~Sha256();
    void update(const uint8_t* data, size_t len);
    // Lowercase hex, 64 characters + terminator.
    void finishHex(char out[65]);

private:
    void* ctx_;
};

}  // namespace hn::ota
