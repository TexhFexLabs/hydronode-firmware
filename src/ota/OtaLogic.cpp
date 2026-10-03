#include "OtaLogic.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace hn::ota {

namespace {

void copy(char* out, size_t cap, const char* text) {
    if (cap == 0) return;
    strncpy(out, text, cap - 1);
    out[cap - 1] = '\0';
}

bool isHex64(const char* s) {
    if (!s || strlen(s) != 64) return false;
    for (const char* c = s; *c; c++) {
        bool hex = (*c >= '0' && *c <= '9') || (*c >= 'a' && *c <= 'f');
        if (!hex) return false;
    }
    return true;
}

bool empty(const char* s) { return !s || !*s; }

bool digit(char c) { return c >= '0' && c <= '9'; }

bool identifierChar(char c) {
    return digit(c) || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '.' || c == '-';
}

struct Version {
    unsigned long core[3];
    const char* pre;  // pre-release identifiers, not terminated
    size_t preLen;    // 0: a release
};

// "v1.2.3-rc.1+build" → core 1.2.3, pre "rc.1". False for anything else, like the backend.
bool parseVersion(const char* text, Version& out) {
    if (!text) return false;
    const char* p = text;
    while (*p == ' ') p++;
    if (*p == 'v') p++;
    size_t len = strlen(p);
    while (len > 0 && p[len - 1] == ' ') len--;
    const char* plus = static_cast<const char*>(memchr(p, '+', len));
    size_t end = plus ? size_t(plus - p) : len;
    const char* dash = static_cast<const char*>(memchr(p, '-', end));
    size_t coreEnd = dash ? size_t(dash - p) : end;
    size_t i = 0;
    for (int part = 0; part < 3; part++) {
        size_t start = i;
        unsigned long value = 0;
        while (i < coreEnd && digit(p[i]) && i - start < 9) value = value * 10 + unsigned(p[i++] - '0');
        if (i == start || (i < coreEnd && digit(p[i]))) return false;  // empty or longer than 9 digits
        out.core[part] = value;
        if (part < 2) {
            if (i >= coreEnd || p[i] != '.') return false;
            i++;
        }
    }
    if (i != coreEnd) return false;
    out.pre = nullptr;
    out.preLen = 0;
    if (dash) {
        out.pre = dash + 1;
        out.preLen = end - coreEnd - 1;
        if (out.preLen == 0) return false;
        for (size_t k = 0; k < out.preLen; k++) {
            if (!identifierChar(out.pre[k])) return false;
        }
    }
    return true;
}

bool numeric(const char* s, size_t len) {
    if (len == 0) return false;
    for (size_t i = 0; i < len; i++) {
        if (!digit(s[i])) return false;
    }
    return true;
}

// Semver identifier order: numbers by value, numbers below text, text by ASCII.
int compareIdentifier(const char* a, size_t lenA, const char* b, size_t lenB) {
    bool na = numeric(a, lenA);
    bool nb = numeric(b, lenB);
    if (na && nb) {
        while (lenA > 1 && *a == '0') a++, lenA--;
        while (lenB > 1 && *b == '0') b++, lenB--;
        if (lenA != lenB) return lenA < lenB ? -1 : 1;
        int c = memcmp(a, b, lenA);
        return c < 0 ? -1 : c > 0 ? 1 : 0;
    }
    if (na) return -1;
    if (nb) return 1;
    int c = memcmp(a, b, lenA < lenB ? lenA : lenB);
    if (c != 0) return c < 0 ? -1 : 1;
    if (lenA != lenB) return lenA < lenB ? -1 : 1;
    return 0;
}

}  // namespace

VerifyMode parseVerifyMode(const char* name) {
    return name && strcmp(name, "INGEST") == 0 ? VerifyMode::Lenient : VerifyMode::Strict;
}

const char* verifyModeName(VerifyMode mode) { return mode == VerifyMode::Lenient ? "INGEST" : "STRICT"; }

int compareVersions(const char* a, const char* b) {
    Version va{};
    Version vb{};
    bool okA = parseVersion(a, va);
    bool okB = parseVersion(b, vb);
    if (!okA || !okB) {
        if (!okA && !okB) return 0;
        return okA ? 1 : -1;
    }
    for (int i = 0; i < 3; i++) {
        if (va.core[i] != vb.core[i]) return va.core[i] < vb.core[i] ? -1 : 1;
    }
    if (va.preLen == 0 || vb.preLen == 0) {
        if (va.preLen == 0 && vb.preLen == 0) return 0;
        return va.preLen == 0 ? 1 : -1;  // the release ranks above its pre-releases
    }
    const char* pa = va.pre;
    const char* pb = vb.pre;
    const char* endA = va.pre + va.preLen;
    const char* endB = vb.pre + vb.preLen;
    while (pa < endA && pb < endB) {
        const char* dotA = static_cast<const char*>(memchr(pa, '.', size_t(endA - pa)));
        const char* dotB = static_cast<const char*>(memchr(pb, '.', size_t(endB - pb)));
        size_t lenA = size_t((dotA ? dotA : endA) - pa);
        size_t lenB = size_t((dotB ? dotB : endB) - pb);
        int c = compareIdentifier(pa, lenA, pb, lenB);
        if (c != 0) return c;
        pa += lenA + (dotA ? 1 : 0);
        pb += lenB + (dotB ? 1 : 0);
        if (!dotA || !dotB) {
            // One list ended: the shorter one ranks lower ("rc" < "rc.1").
            if (!dotA && !dotB) return 0;
            return dotA ? 1 : -1;
        }
    }
    return 0;
}

void firmwareFlags(bool otaCapable, uint32_t rev, char* out, size_t cap) {
    snprintf(out, cap, "%scfg=%lu", otaCapable ? "ota " : "", (unsigned long)(rev ? rev : 1));
}

bool signedText(const char* version, const char* family, const char* sha256Hex, uint32_t size, bool downgrade,
                char* out, size_t cap) {
    if (empty(version) || empty(family) || !isHex64(sha256Hex)) return false;
    int n = snprintf(out, cap, "hydronode-ota-v1|%s|%s|%s|%lu|%d", version, family, sha256Hex, (unsigned long)size,
                     downgrade ? 1 : 0);
    return n > 0 && size_t(n) < cap;
}

const char* checkFirmwareOffer(const FirmwareOffer& offer, const char* ownFamily, const char* ownVersion,
                               uint32_t slotSize) {
    if (empty(offer.job) || empty(offer.version) || empty(offer.family) || empty(offer.sig) ||
        empty(offer.keyId) || empty(offer.url) || offer.url[0] != '/' || offer.size == 0 ||
        !isHex64(offer.sha256)) {
        return "bad_offer";
    }
    if (strlen(offer.version) > kMaxVersionLength) return "bad_offer";
    if (strcmp(offer.family, ownFamily) != 0) return "family_mismatch";
    int order = compareVersions(offer.version, ownVersion);
    if (order == 0) return "same_version";
    if (order < 0 && !offer.downgrade) return "downgrade_not_allowed";
    if (offer.size > slotSize) return "no_space";
    return nullptr;
}

VerifyDecision decideVerify(const VerifyInput& in) {
    VerifyDecision d{VerifyStep::Verified, ""};
    if (in.elapsedMs >= kVerifyLimitMs) {
        d.step = VerifyStep::RollBack;
        copy(d.reason, sizeof(d.reason), "timeout");
        return d;
    }
    if (!in.wifiOk) {
        copy(d.reason, sizeof(d.reason), "wifi_failed");
    } else if (in.ingestStatus <= 0 || in.ingestStatus >= 500) {
        copy(d.reason, sizeof(d.reason), "server_unreachable");
    } else if (in.ingestStatus < 200 || in.ingestStatus >= 300) {
        snprintf(d.reason, sizeof(d.reason), "ingest_failed:%d", in.ingestStatus);
    } else if (in.mode == VerifyMode::Strict && !empty(in.failedDriver)) {
        snprintf(d.reason, sizeof(d.reason), "sensor_read_failed:%s", in.failedDriver);
    } else {
        return d;
    }
    if (in.attempt < kVerifyAttempts && in.elapsedMs < kVerifyLimitMs) {
        d.step = VerifyStep::Retry;
        return d;
    }
    d.step = VerifyStep::RollBack;
    if (in.attempt < kVerifyAttempts) copy(d.reason, sizeof(d.reason), "timeout");
    return d;
}

void otaStateHeader(uint8_t attempt, VerifyMode mode, const char* job, char* out, size_t cap) {
    snprintf(out, cap, "verifying;try=%u;mode=%s;job=%s", (unsigned)attempt, verifyModeName(mode), job ? job : "");
}

void otaResultHeader(const char* reason, const char* job, char* out, size_t cap) {
    snprintf(out, cap, "rolled_back;%s;job=%s", reason && *reason ? reason : "unknown", job ? job : "");
}

}  // namespace hn::ota
