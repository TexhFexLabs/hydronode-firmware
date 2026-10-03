#include "OtaVerify.h"

#if !defined(ESP8266)

#include <mbedtls/base64.h>
#include <mbedtls/md.h>
#include <mbedtls/pk.h>
#include <mbedtls/sha256.h>
#include <stdio.h>
#include <string.h>

#include "OtaKeys.h"

namespace hn::ota {

namespace {

const char* pemFor(const char* keyId) {
    if (!keyId || !*keyId) return nullptr;
    for (const TrustedKey& key : kReleaseKeys) {
        if (key.id && strcmp(key.id, keyId) == 0) return key.pem;
    }
#if defined(HN_OTA_DEV_KEY)
    if (strcmp(HN_OTA_DEV_KEY_ID, keyId) == 0) return HN_OTA_DEV_KEY_PEM;
#endif
    return nullptr;
}

}  // namespace

bool hasKey(const char* keyId) { return pemFor(keyId) != nullptr; }

bool verifySignature(const char* keyId, const char* text, const char* sigBase64) {
    const char* pem = pemFor(keyId);
    if (!pem || !text || !sigBase64) return false;

    unsigned char sig[160];
    size_t sigLen = 0;
    if (mbedtls_base64_decode(sig, sizeof(sig), &sigLen, reinterpret_cast<const unsigned char*>(sigBase64),
                              strlen(sigBase64)) != 0) {
        return false;
    }
    unsigned char hash[32];
    if (mbedtls_sha256(reinterpret_cast<const unsigned char*>(text), strlen(text), hash, 0) != 0) return false;

    mbedtls_pk_context pk;
    mbedtls_pk_init(&pk);
    bool ok = mbedtls_pk_parse_public_key(&pk, reinterpret_cast<const unsigned char*>(pem), strlen(pem) + 1) == 0 &&
              mbedtls_pk_can_do(&pk, MBEDTLS_PK_ECDSA) &&
              mbedtls_pk_verify(&pk, MBEDTLS_MD_SHA256, hash, sizeof(hash), sig, sigLen) == 0;
    mbedtls_pk_free(&pk);
    return ok;
}

Sha256::Sha256() : ctx_(new mbedtls_sha256_context) {
    auto* ctx = static_cast<mbedtls_sha256_context*>(ctx_);
    mbedtls_sha256_init(ctx);
    mbedtls_sha256_starts(ctx, 0);
}

Sha256::~Sha256() {
    auto* ctx = static_cast<mbedtls_sha256_context*>(ctx_);
    mbedtls_sha256_free(ctx);
    delete ctx;
}

void Sha256::update(const uint8_t* data, size_t len) {
    mbedtls_sha256_update(static_cast<mbedtls_sha256_context*>(ctx_), data, len);
}

void Sha256::finishHex(char out[65]) {
    unsigned char hash[32];
    mbedtls_sha256_finish(static_cast<mbedtls_sha256_context*>(ctx_), hash);
    for (int i = 0; i < 32; i++) snprintf(out + 2 * i, 3, "%02x", hash[i]);
    out[64] = '\0';
}

}  // namespace hn::ota

#endif  // !ESP8266
