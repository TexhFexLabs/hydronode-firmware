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

// Release keys. prod-2026-10: YubiKey 5C, PIV slot 9c, created 2026-10-03
// (tools/release-keys/prod-2026-10.pub.pem). The private key never leaves the YubiKey.
static const TrustedKey kReleaseKeys[] = {
    {"prod-2026-10",
     "-----BEGIN PUBLIC KEY-----\n"
     "MFkwEwYHKoZIzj0CAQYIKoZIzj0DAQcDQgAE88gGpg45wbZmTXOQTTYidp6b1Dlt\n"
     "BEeh3cSXjMWSx2OXX9HeUOJljW6zXMS2d7TXHvzMV61hdNkKhU6vQpTlYA==\n"
     "-----END PUBLIC KEY-----\n"},
    {nullptr, nullptr},
};

}  // namespace hn::ota

#if defined(HN_OTA_DEV_KEY)
#include "../../tools/dev-keys/ota-dev.pub.h"  // HN_OTA_DEV_KEY_ID, HN_OTA_DEV_KEY_PEM
#endif
