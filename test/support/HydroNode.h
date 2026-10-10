#pragma once
#include <ArduinoJson.h>
#include <HydroNodeDeviceConfig.h>  // the library's plain struct (../hydronode-library/src)
#include <functional>
#include <map>
#include <string>
#include <vector>
struct HydroNodeValue {
    const char* type;
    float value;
};
class HydroNode {
public:
    HydroNode(const char* = "", const char* = "", const char* = "") {}
    std::map<std::string, std::string> headers;
    std::map<std::string, std::function<void(JsonVariantConst)>> handlers;
    std::vector<std::string> errors;
    std::vector<std::string> acks;
    int ingestStatus = 202;
    void begin() {}
    bool syncTime() { return true; }
    uint64_t epochMs() { return 0; }
    void closeConnection() {}
    void setFirmwareIdentity(const char*, const char*, const char*) {}
    void setResetReason(const char*) {}
    HydroNodeDeviceConfig deviceConfig;
    std::string powerState;
    void setDeviceConfig(const HydroNodeDeviceConfig& c) { deviceConfig = c; }
    void setPowerState(const char* state) { powerState = state ? state : ""; }
    void setExtraHeader(const char* name, const char* value) { headers[name] = value; }
    void clearExtraHeader(const char* name) { headers.erase(name); }
    void onResponseKey(const char* key, std::function<void(JsonVariantConst)> fn) { handlers[key] = fn; }
    void clearReadErrors() { errors.clear(); }
    void reportReadError(const char* id) { errors.push_back(id); }
    int sendValue(const char*, float) { return ingestStatus; }
    int sendValues(const HydroNodeValue*, size_t count, int* codes = nullptr, uint32_t = 0) {
        for (size_t i = 0; codes && i < count; i++) codes[i] = ingestStatus;
        return ingestStatus;
    }
    bool sendOtaAck(const char*, const char* result, const char*) { acks.push_back(result); return true; }
    struct DownloadResult { int status; size_t bytes; size_t total; };
    DownloadResult downloadSigned(const char*, size_t, std::function<bool(const uint8_t*, size_t)> fn) {
        uint8_t bytes[4] = {};
        fn(bytes, 4);
        return {200, 4, 4};
    }
};
