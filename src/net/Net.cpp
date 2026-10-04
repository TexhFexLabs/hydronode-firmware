#include "Net.h"

#include <Arduino.h>
#include <string.h>

#if defined(ESP8266)
#include <ESP8266WiFi.h>
#else
#include <WiFi.h>
#include <esp_attr.h>
#endif

#include "status/Status.h"

namespace hn::net {

namespace {

constexpr uint32_t kFastMagic = 0x484E5746;  // "HNWF"

struct FastState {
    uint32_t magic;
    uint8_t bssid[6];
    int32_t channel;
};

#if defined(ESP8266)
// ESP8266: RTC user memory, word aligned. Slot 0..3 is used by the power module.
constexpr uint32_t kRtcSlot = 8;
FastState fast;
void loadFast() { ESP.rtcUserMemoryRead(kRtcSlot, reinterpret_cast<uint32_t*>(&fast), sizeof(fast)); }
void saveFast() { ESP.rtcUserMemoryWrite(kRtcSlot, reinterpret_cast<uint32_t*>(&fast), sizeof(fast)); }
#else
// Survives deep sleep (not hibernate, which powers RTC memory down).
RTC_DATA_ATTR FastState fast;
void loadFast() {}
void saveFast() {}
#endif

const char* reason(wl_status_t s) {
    switch (s) {
        case WL_NO_SSID_AVAIL: return "NO_SSID";
        case WL_CONNECT_FAILED: return "AUTH";
        case WL_CONNECTION_LOST: return "LOST";
        case WL_DISCONNECTED: return "TIMEOUT";
        default: return "TIMEOUT";
    }
}

const char* lastFailure = nullptr;
uint32_t connectDurationMs = 0;

bool waitConnected(uint32_t timeoutMs) {
    uint32_t start = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - start < timeoutMs) {
        wl_status_t s = WiFi.status();
        if (s == WL_CONNECT_FAILED || s == WL_NO_SSID_AVAIL) return false;
        delay(20);
    }
    return WiFi.status() == WL_CONNECTED;
}

}  // namespace

bool connect(const Config& cfg, uint32_t timeoutMs) {
    uint32_t start = millis();
    WiFi.persistent(false);
#if defined(ESP8266)
    WiFi.forceSleepWake();
    delay(1);
#endif
    WiFi.mode(WIFI_STA);
#if defined(CONFIG_IDF_TARGET_ESP32C3)
    // C3 SuperMini boards have a tiny chip antenna next to the regulator; at full power the
    // WPA handshake fails ("AUTH") or times out. 8.5 dBm is the value these boards connect with.
    if (strstr(cfg.board, "supermini")) WiFi.setTxPower(WIFI_POWER_8_5dBm);
#endif
    if (cfg.staticIp) {
        WiFi.config(IPAddress(cfg.ip), IPAddress(cfg.gateway), IPAddress(cfg.subnet), IPAddress(cfg.dns));
    }

    bool ok = false;
    bool usedFast = false;
    loadFast();
    if (cfg.fastReconnect && fast.magic == kFastMagic) {
        usedFast = true;
        WiFi.begin(cfg.ssid, cfg.pass, fast.channel, fast.bssid, true);
        ok = waitConnected(4000);
        if (!ok) {
            // Router moved to another channel or AP: fall back to a full scan.
            fast.magic = 0;
            saveFast();
            WiFi.disconnect(true);
            delay(50);
        }
    }
    if (!ok) {
        WiFi.begin(cfg.ssid, cfg.pass);
        ok = waitConnected(timeoutMs);
    }

    connectDurationMs = millis() - start;
    if (!ok) {
        lastFailure = reason(WiFi.status());
        status::line("ERR WIFI %s", lastFailure);
        // Stop the attempt that is still running, else the next begin() fails with
        // "sta is connecting, cannot set config" and the following round starts from a dirty state.
        WiFi.disconnect(true);
        return false;
    }
    if (cfg.fastReconnect) {
        memcpy(fast.bssid, WiFi.BSSID(), 6);
        fast.channel = WiFi.channel();
        fast.magic = kFastMagic;
        saveFast();
    }
    status::line("WIFI ok rssi=%d ms=%lu fast=%d", WiFi.RSSI(), (unsigned long)(millis() - start), usedFast ? 1 : 0);
    return true;
}

void off() {
#if defined(ESP8266)
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
    WiFi.forceSleepBegin();
#else
    WiFi.disconnect(true, false);
    WiFi.mode(WIFI_OFF);
#endif
}

bool connected() { return WiFi.status() == WL_CONNECTED; }

const char* lastError() { return lastFailure; }

uint32_t lastConnectMs() { return connectDurationMs; }

void setPowerSave(bool on) {
#if defined(ESP8266)
    WiFi.setSleepMode(on ? WIFI_MODEM_SLEEP : WIFI_NONE_SLEEP);
#else
    WiFi.setSleep(on ? WIFI_PS_MAX_MODEM : WIFI_PS_NONE);
#endif
}

}  // namespace hn::net
