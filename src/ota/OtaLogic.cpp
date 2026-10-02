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

}  // namespace

VerifyMode parseVerifyMode(const char* name) {
    return name && strcmp(name, "INGEST") == 0 ? VerifyMode::Lenient : VerifyMode::Strict;
}

const char* verifyModeName(VerifyMode mode) { return mode == VerifyMode::Lenient ? "INGEST" : "STRICT"; }

int compareVersions(const char* a, const char* b) {
    const char* pa = a ? a : "";
    const char* pb = b ? b : "";
    for (int part = 0; part < 3; part++) {
        long na = strtol(pa, const_cast<char**>(&pa), 10);
        long nb = strtol(pb, const_cast<char**>(&pb), 10);
        if (na != nb) return na < nb ? -1 : 1;
        if (*pa == '.') pa++;
        if (*pb == '.') pb++;
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
    if (strcmp(offer.family, ownFamily) != 0) return "family_mismatch";
    int order = compareVersions(offer.version, ownVersion);
    if (order == 0) return "same_version";
    if (order < 0 && !offer.downgrade) return "downgrade_not_allowed";
    if (offer.size > slotSize) return "no_space";
    return nullptr;
}

VerifyDecision decideVerify(const VerifyInput& in) {
    VerifyDecision d{VerifyStep::Verified, ""};
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
