#pragma once

// Public keys a firmware image must be signed with (ECDSA P-256, PEM). A list, so a new key can be
// rolled out before the old one stops signing. The private release key lives offline on a
// YubiKey; neither the server nor GitHub ever sees it.
//
// Development builds (-DHN_OTA_DEV_KEY, env esp32c3-dev and friends) additionally trust the local
// dev key from tools/dev-keys/ (make-dev-key.sh, never committed).

namespace hn::ota {

struct TrustedKey {
    const char* id;
    const char* pem;
};

// Release keys. Empty until the YubiKey key is created: then a release build takes no firmware
// over the air at all, which is the safe default.
static const TrustedKey kReleaseKeys[] = {
    // {"prod-2026-10", "-----BEGIN PUBLIC KEY-----\n...\n-----END PUBLIC KEY-----\n"},
    // YubiKey public key goes here.
    {nullptr, nullptr},
};

}  // namespace hn::ota

#if defined(HN_OTA_DEV_KEY)
#include "../../tools/dev-keys/ota-dev.pub.h"  // HN_OTA_DEV_KEY_ID, HN_OTA_DEV_KEY_PEM
#endif
