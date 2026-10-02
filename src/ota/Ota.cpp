#include "Ota.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <HydroNode.h>
#include <string.h>

#include <new>

#include "ConfigMerge.h"
#include "OtaLogic.h"
#include "OtaStore.h"
#include "OtaVerify.h"
#include "config/ConfigStore.h"
#include "status/Status.h"

#if !defined(ESP8266)
#include <esp_ota_ops.h>
#endif

#if !defined(ESP8266)
// The Arduino core would otherwise declare new firmware valid right at start. Returning true
// leaves it "pending verify": any restart before ota::afterRound() confirms it brings the old
// firmware back (bootloader rollback).
extern "C" bool verifyRollbackLater() { return true; }
#endif

namespace hn::ota {

namespace {

enum class Phase : uint8_t { Idle, Verifying, Reporting };

constexpr uint8_t kMaxUnverifiedBoots = 3;
#if defined(ESP8266)
constexpr bool kFirmwareOta = false;
#else
constexpr bool kFirmwareOta = true;
#endif

Phase phase = Phase::Idle;
Pending pending{};
uint8_t attempt = 0;
uint32_t startedMs = 0;
// An offer from the last answer, carried out after the round. Only one per answer.
JsonDocument* offer = nullptr;
bool offerIsFirmware = false;

void copy(char* dst, size_t cap, const char* src) {
    strncpy(dst, src ? src : "", cap - 1);
    dst[cap - 1] = '\0';
}

[[noreturn]] void restart() {
    status::flush();
    delay(200);
    ESP.restart();
    while (true) delay(1000);
}

// Puts the backed-up config block back. True when it is on the board again.
bool restoreBackup() {
    uint8_t* block = static_cast<uint8_t*>(malloc(store::kBlockSize));
    if (!block) return false;
    size_t len = loadBackup(block, store::kBlockSize);
    bool ok = len > 0 && store::writeConfigBlock(block, len);
    memset(block, 0, store::kBlockSize);
    free(block);
    if (ok) clearBackup();
    return ok;
}

// Config update failed: back to the old block, remember why, start over with it.
[[noreturn]] void rollBackConfig(const char* reason) {
    status::line("OTA rollback config %s", reason);
    copy(pending.result, sizeof(pending.result), reason);
    savePending(pending);
    if (!restoreBackup()) status::line("ERR OTA no_backup");
    restart();
}

void setStateHeader(HydroNode& hydro) {
    char value[96];
    otaStateHeader(uint8_t(attempt + 1), VerifyMode(pending.mode), pending.job, value, sizeof(value));
    hydro.setExtraHeader("X-Ota-State", value);
}

void ack(HydroNode& hydro, const char* job, const char* result, const char* reason) {
    bool sent = hydro.sendOtaAck(job, result, reason);
    status::line("OTA ack %s %s %s", result, reason ? reason : "-", sent ? "sent" : "not_sent");
}

void storeOffer(JsonVariantConst value, bool firmware) {
    // While new firmware or config proves itself, nothing else is taken.
    if (phase == Phase::Verifying || !value.is<JsonObjectConst>()) return;
    if (!offer) offer = new JsonDocument();
    offer->set(value);
    offerIsFirmware = firmware;
}

void dropOffer() {
    if (offer) {
        delete offer;
        offer = nullptr;
    }
}

#if !defined(ESP8266)
void handleFirmwareOffer(HydroNode& hydro, JsonObjectConst o) {
    FirmwareOffer f{};
    f.job = o["job"];
    f.version = o["version"];
    f.family = o["family"];
    f.size = o["size"] | 0u;
    f.sha256 = o["sha256"];
    f.sig = o["sig"];
    f.keyId = o["keyId"];
    f.downgrade = o["downgrade"] | false;
    f.verify = parseVerifyMode(o["verify"] | "STRICT");
    f.url = o["url"];
    if (!f.job) return;  // nothing to answer to
    status::line("OTA offer %s %s", f.version ? f.version : "-", verifyModeName(f.verify));

    const esp_partition_t* next = esp_ota_get_next_update_partition(nullptr);
    const char* failure = next ? checkFirmwareOffer(f, HN_FAMILY, HN_FW_VERSION, next->size) : "no_space";
    char text[192];
    if (!failure && !signedText(f.version, f.family, f.sha256, f.size, f.downgrade, text, sizeof(text))) {
        failure = "bad_offer";
    }
    if (!failure && (!hasKey(f.keyId) || !verifySignature(f.keyId, text, f.sig))) failure = "signature_invalid";
    if (failure) {
        ack(hydro, f.job, "failed", failure);
        return;
    }

    esp_ota_handle_t handle = 0;
    if (esp_ota_begin(next, f.size, &handle) != ESP_OK) {
        ack(hydro, f.job, "failed", "no_space");
        return;
    }
    Sha256 sha;
    size_t written = 0;
    uint8_t lastTenth = 0;
    bool writeFailed = false;
    bool tooLarge = false;
    int lastStatus = 0;
    for (int tries = 0; tries < 3 && written < f.size && !writeFailed && !tooLarge; tries++) {
        HydroNode::DownloadResult r = hydro.downloadSigned(f.url, written, [&](const uint8_t* data, size_t n) {
            if (written + n > f.size) {
                tooLarge = true;
                return false;
            }
            if (esp_ota_write(handle, data, n) != ESP_OK) {
                writeFailed = true;
                return false;
            }
            sha.update(data, n);
            written += n;
            uint8_t tenth = uint8_t(written * 10 / f.size);
            if (tenth != lastTenth) {
                lastTenth = tenth;
                status::line("OTA download %u%%", unsigned(tenth) * 10);
            }
            return true;
        });
        lastStatus = r.status;
        // A refusal (404, 409, ...) does not get better by asking again.
        if (r.status >= 400 && r.status < 500) break;
    }
    if (writeFailed || tooLarge || written != f.size) {
        esp_ota_abort(handle);
        char reason[24];
        if (tooLarge) {
            copy(reason, sizeof(reason), "size_mismatch");
        } else if (writeFailed) {
            copy(reason, sizeof(reason), "write_failed");
        } else if (lastStatus >= 300) {
            snprintf(reason, sizeof(reason), "http_%d", lastStatus);
        } else {
            copy(reason, sizeof(reason), "timeout");
        }
        ack(hydro, f.job, "failed", reason);
        return;
    }
    char hex[65];
    sha.finishHex(hex);
    if (strcmp(hex, f.sha256) != 0) {
        esp_ota_abort(handle);
        ack(hydro, f.job, "failed", "sha256_mismatch");
        return;
    }
    if (esp_ota_end(handle) != ESP_OK) {
        ack(hydro, f.job, "failed", "image_invalid");
        return;
    }

    Pending p{};
    p.kind = PendingKind::Firmware;
    p.mode = uint8_t(f.verify);
    copy(p.job, sizeof(p.job), f.job);
    copy(p.fromVersion, sizeof(p.fromVersion), HN_FW_VERSION);
    copy(p.toVersion, sizeof(p.toVersion), f.version);
    if (!savePending(p) || esp_ota_set_boot_partition(next) != ESP_OK) {
        clearPending();
        ack(hydro, f.job, "failed", "set_boot_failed");
        return;
    }
    ack(hydro, f.job, "downloaded", nullptr);
    hydro.closeConnection();
    status::line("OTA restart into %s", f.version);
    restart();
}
#endif

void handleConfigOffer(HydroNode& hydro, const Config& cfg, JsonObjectConst o) {
    const char* job = o["job"];
    if (!job) return;
    uint32_t rev = o["rev"] | 0u;
    VerifyMode mode = parseVerifyMode(o["verify"] | "STRICT");
    status::line("OTA config r%lu %s", (unsigned long)rev, verifyModeName(mode));

    uint8_t* oldBlock = static_cast<uint8_t*>(malloc(store::kBlockSize));
    uint8_t* newBlock = static_cast<uint8_t*>(malloc(store::kBlockSize));
    Config* scratch = new (std::nothrow) Config;
    const char* failure = nullptr;
    size_t oldLen = 0;
    size_t newLen = 0;
    if (!oldBlock || !newBlock || !scratch) {
        failure = "no_space";
    } else if (!store::readConfigBlock(oldBlock, store::kBlockSize)) {
        failure = "config_invalid";
    } else {
        const uint8_t* payload = nullptr;
        size_t payloadLen = 0;
        if (parseHeader(oldBlock, store::kBlockSize, &payload, &payloadLen).error != ConfigError::Ok) {
            failure = "config_invalid";
        } else {
            oldLen = kHeaderSize + payloadLen;
            MergeResult m = mergeConfig(reinterpret_cast<const char*>(payload), payloadLen, o["config"], rev, newBlock,
                                        store::kBlockSize, *scratch);
            if (m.error) {
                failure = m.error;
                if (m.parse.error != ConfigError::Ok) {
                    status::line("ERR OTA config %s %s", errorName(m.parse.error), m.parse.detail);
                }
            }
            newLen = m.length;
        }
    }
    if (!failure && !saveBackup(oldBlock, oldLen)) failure = "no_space";
    if (!failure) {
        pending = Pending{};
        pending.kind = PendingKind::Config;
        pending.mode = uint8_t(mode);
        copy(pending.job, sizeof(pending.job), job);
        copy(pending.fromVersion, sizeof(pending.fromVersion), HN_FW_VERSION);
        copy(pending.toVersion, sizeof(pending.toVersion), HN_FW_VERSION);
        pending.fromRev = cfg.rev;
        pending.toRev = rev;
        if (!savePending(pending)) {
            failure = "no_space";
        } else if (!store::writeConfigBlock(newBlock, newLen)) {
            // Half written is worse than old: put the old block back.
            store::writeConfigBlock(oldBlock, oldLen);
            clearPending();
            failure = "write_failed";
        }
    }
    if (oldBlock) {
        memset(oldBlock, 0, store::kBlockSize);
        free(oldBlock);
    }
    if (newBlock) {
        memset(newBlock, 0, store::kBlockSize);
        free(newBlock);
    }
    if (scratch) {
        memset(static_cast<void*>(scratch), 0, sizeof(Config));
        delete scratch;
    }
    if (failure) {
        clearBackup();
        ack(hydro, job, "failed", failure);
        return;
    }
    ack(hydro, job, "config_applied", nullptr);
    hydro.closeConnection();
    status::line("OTA restart with config r%lu", (unsigned long)rev);
    restart();
}

}  // namespace

void begin(const Config& cfg, const ParseResult& configResult) {
    startedMs = millis();
    bool havePending = loadPending(pending);
#if !defined(ESP8266)
    esp_ota_img_states_t state = ESP_OTA_IMG_UNDEFINED;
    const esp_partition_t* running = esp_ota_get_running_partition();
    bool pendingVerify = running && esp_ota_get_state_partition(running, &state) == ESP_OK &&
                         state == ESP_OTA_IMG_PENDING_VERIFY;
#else
    bool pendingVerify = false;
#endif

    if (!havePending) {
#if !defined(ESP8266)
        // New firmware that did not come from a HydroNode job: nothing to prove it against.
        if (pendingVerify) esp_ota_mark_app_valid_cancel_rollback();
#endif
        return;
    }
    if (pending.result[0]) {
        phase = Phase::Reporting;
        status::line("OTA rolled back %s", pending.result);
        return;
    }

    if (pending.kind == PendingKind::Firmware) {
        bool isNew = strcmp(HN_FW_VERSION, pending.toVersion) == 0;
        if (isNew && pendingVerify) {
            phase = Phase::Verifying;
        } else if (!isNew) {
            // Back on the old firmware without a verdict: the new one never got that far.
            copy(pending.result, sizeof(pending.result), "boot_failed");
            savePending(pending);
            phase = Phase::Reporting;
            status::line("OTA rolled back boot_failed");
            return;
        } else {
            clearPending();  // already confirmed, only the record was left
            return;
        }
    } else if (pending.kind == PendingKind::Config) {
        if (configResult.error != ConfigError::Ok) rollBackConfig("config_invalid");
        phase = Phase::Verifying;
    } else {
        clearPending();
        return;
    }

    // A board that keeps crashing before the verdict (the ESP8266 has no bootloader rollback).
    if (++pending.boots > kMaxUnverifiedBoots) {
        if (pending.kind == PendingKind::Config) rollBackConfig("boot_failed");
    }
    savePending(pending);
    status::line("OTA verify %s %s r%lu", pending.kind == PendingKind::Firmware ? "firmware" : "config",
                 verifyModeName(VerifyMode(pending.mode)), (unsigned long)cfg.rev);
}

void attach(HydroNode& hydro, const Config& cfg) {
    char flags[24];
    firmwareFlags(kFirmwareOta, cfg.rev, flags, sizeof(flags));
    hydro.setFirmwareIdentity("hydronode", HN_FW_VERSION, flags);
    if (phase == Phase::Verifying) {
        if (pending.kind == PendingKind::Firmware) hydro.setResetReason("ota");
        setStateHeader(hydro);
    } else if (phase == Phase::Reporting) {
        char value[112];
        otaResultHeader(pending.result, pending.job, value, sizeof(value));
        hydro.setExtraHeader("X-Ota-Result", value);
    }
    hydro.onResponseKey("ota", [](JsonVariantConst value) {
        if (kFirmwareOta) storeOffer(value, true);
    });
    hydro.onResponseKey("config", [](JsonVariantConst value) { storeOffer(value, false); });
}

bool afterRound(HydroNode* hydro, const Config& cfg, const RoundReport& report) {
    bool delivered = report.bestStatus >= 200 && report.bestStatus < 300;

    if (phase == Phase::Reporting) {
        if (hydro && delivered) {
            hydro->clearExtraHeader("X-Ota-Result");
            clearPending();
            phase = Phase::Idle;
        }
    } else if (phase == Phase::Verifying) {
        attempt++;
        VerifyDecision d = decideVerify({VerifyMode(pending.mode), attempt, millis() - startedMs, report.wifiOk,
                                         report.bestStatus, report.failedDriver});
        if (d.step == VerifyStep::Retry) {
            status::line("OTA verify try %u failed %s", (unsigned)attempt, d.reason);
            if (hydro) setStateHeader(*hydro);
            return true;
        }
        if (d.step == VerifyStep::RollBack) {
            if (pending.kind == PendingKind::Config) rollBackConfig(d.reason);
#if !defined(ESP8266)
            status::line("OTA rollback firmware %s", d.reason);
            copy(pending.result, sizeof(pending.result), d.reason);
            savePending(pending);
            status::flush();
            esp_ota_mark_app_invalid_rollback_and_reboot();
            // Only returns when there is nothing to go back to: keep running what we have.
            clearPending();
            phase = Phase::Idle;
#endif
            return false;
        }
        // Verified.
#if !defined(ESP8266)
        if (pending.kind == PendingKind::Firmware) esp_ota_mark_app_valid_cancel_rollback();
#endif
        if (pending.kind == PendingKind::Config) clearBackup();
        char took[16];
        snprintf(took, sizeof(took), "%lus", (unsigned long)((millis() - startedMs) / 1000));
        status::line("OTA verified %s", took);
        if (hydro) {
            hydro->clearExtraHeader("X-Ota-State");
            ack(*hydro, pending.job, "verified", took);
        }
        clearPending();
        phase = Phase::Idle;
        return false;
    }

    if (offer && hydro && phase == Phase::Idle) {
        JsonObjectConst o = offer->as<JsonObjectConst>();
        bool firmware = offerIsFirmware;
#if !defined(ESP8266)
        if (firmware) handleFirmwareOffer(*hydro, o);
#endif
        if (!firmware) handleConfigOffer(*hydro, cfg, o);
    }
    dropOffer();
    return false;
}

}  // namespace hn::ota
