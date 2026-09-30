#include "Net.h"

#include <Arduino.h>
#include <WiFi.h>
#include <esp_attr.h>
#include <string.h>

#include "status/Status.h"

namespace hn::net {

namespace {

constexpr uint32_t kFastMagic = 0x484E5746;  // "HNWF"

struct FastState {
    uint32_t magic;
    uint8_t bssid[6];
    int32_t channel;
};

// Survives deep sleep (not hibernate, which powers RTC memory down).
RTC_DATA_ATTR FastState fast;

const char* reason(wl_status_t s) {
    switch (s) {
        case WL_NO_SSID_AVAIL: return "NO_SSID";
        case WL_CONNECT_FAILED: return "AUTH";
        case WL_CONNECTION_LOST: return "LOST";
        case WL_DISCONNECTED: return "TIMEOUT";
        default: return "TIMEOUT";
    }
}

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
    WiFi.mode(WIFI_STA);
    if (cfg.staticIp) {
        WiFi.config(IPAddress(cfg.ip), IPAddress(cfg.gateway), IPAddress(cfg.subnet), IPAddress(cfg.dns));
    }

    bool ok = false;
    bool usedFast = false;
    if (cfg.fastReconnect && fast.magic == kFastMagic) {
        usedFast = true;
        WiFi.begin(cfg.ssid, cfg.pass, fast.channel, fast.bssid, true);
        ok = waitConnected(4000);
        if (!ok) {
            // Router moved to another channel or AP: fall back to a full scan.
            fast.magic = 0;
            WiFi.disconnect(true);
            delay(50);
        }
    }
    if (!ok) {
        WiFi.begin(cfg.ssid, cfg.pass);
        ok = waitConnected(timeoutMs);
    }

    if (!ok) {
        status::line("ERR WIFI %s", reason(WiFi.status()));
        return false;
    }
    if (cfg.fastReconnect) {
        memcpy(fast.bssid, WiFi.BSSID(), 6);
        fast.channel = WiFi.channel();
        fast.magic = kFastMagic;
    }
    status::line("WIFI ok rssi=%d ms=%lu fast=%d", WiFi.RSSI(), (unsigned long)(millis() - start), usedFast ? 1 : 0);
    return true;
}

void off() {
    WiFi.disconnect(true, false);
    WiFi.mode(WIFI_OFF);
}

bool connected() { return WiFi.status() == WL_CONNECTED; }

void setPowerSave(bool on) { WiFi.setSleep(on ? WIFI_PS_MAX_MODEM : WIFI_PS_NONE); }

}  // namespace hn::net
