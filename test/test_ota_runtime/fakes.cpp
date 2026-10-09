#include <Arduino.h>
#include <Ticker.h>
#include <HydroNode.h>
#include <esp_ota_ops.h>
#include "fakes.h"
#include "config/ConfigStore.h"
#include "ota/OtaVerify.h"
#include "actuators/Actuators.h"
#include "drivers/Driver.h"
#include "power/Power.h"
#include "net/Net.h"
#include "status/Status.h"
namespace fake {
std::optional<hn::ota::Pending> record;
std::vector<uint8_t> config, backup;
bool interruptWrite = false, interruptCleanup = false;
uint32_t pulseLeft = 0;
unsigned backupLoads = 0;
void reset() {
    record.reset(); config.clear(); backup.clear(); interruptWrite = false; interruptCleanup = false; pulseLeft = 0; backupLoads = 0;
    Ticker::callback = nullptr; fakeMillis = 0; rolledBack = false;
    fakeImageState = ESP_OTA_IMG_VALID; downloadedImages = 0;
}
}
uint32_t millis() { return fakeMillis; }
void delay(uint32_t ms) {
    fakeMillis += ms;
    if (Ticker::callback && fakeMillis >= Ticker::deadline) {
        auto fn = Ticker::callback;
        Ticker::callback = nullptr;
        fn();
    }
}
namespace hn::store {
bool readConfigBlock(uint8_t* out, size_t cap) {
    if (fake::config.empty()) return false;
    memset(out, 0xff, cap);
    memcpy(out, fake::config.data(), min(cap, fake::config.size()));
    return true;
}
bool writeConfigBlock(const uint8_t* block, size_t len) {
    if (fake::interruptWrite) { fake::config.clear(); throw Restarted{}; }
    fake::config.assign(block, block + len);
    return true;
}
}
namespace hn::ota {
bool loadPending(Pending& out) { if (!fake::record) return false; out = *fake::record; return true; }
bool savePending(const Pending& p) { fake::record = p; return true; }
void clearPending() { fake::record.reset(); }
bool saveBackup(const uint8_t* p, size_t n) { fake::backup.assign(p, p+n); return true; }
bool hasBackup() { return !fake::backup.empty(); }
size_t loadBackup(uint8_t* p, size_t cap) {
    fake::backupLoads++;
    if (fake::backup.empty() || fake::backup.size() > cap) return 0;
    memcpy(p, fake::backup.data(), fake::backup.size()); return fake::backup.size();
}
void clearBackup() { if (fake::interruptCleanup) throw Restarted{}; fake::backup.clear(); }
bool hasKey(const char*) { return true; }
bool verifySignature(const char*, const char*, const char*) { return true; }
Sha256::Sha256() : ctx_(nullptr) {}
Sha256::~Sha256() {}
void Sha256::update(const uint8_t*, size_t) {}
void Sha256::finishHex(char out[65]) { memset(out, 'a', 64); out[64] = 0; }
}
namespace hn::act {
bool isActuator(const char* id) { return strcmp(id,"relay") == 0 || strcmp(id,"button") == 0; }
void begin(const Config&, bool) {}
void attach(HydroNode&) {}
void service() {}
void advance(uint32_t) {}
bool busy() { return fake::pulseLeft > 0; }
uint32_t pendingMs() { return fake::pulseLeft; }
const char* takePress() { return nullptr; }
uint8_t wakeInputs(power::WakeInput*, uint8_t) { return 0; }
bool wakePress(int8_t) { return false; }
bool hasPresses() { return false; }
void safeOff() {}
void resume() {}
}
namespace hn::net {
bool connect(const Config&, uint32_t) { return true; }
bool connected() { return true; }
void off() {}
void setPowerSave(bool) {}
const char* lastError() { return nullptr; }
uint32_t lastConnectMs() { return 0; }
}
namespace hn::power {
void startWatchdog() {}
void feedWatchdog() {}
const char* wakeReason() { return "RESET"; }
void sensorsOn(const Config&) {}
void sensorsOff(const Config&) {}
uint32_t sleepMs(uint32_t, uint32_t) { return 1000; }
void keepLevel(int8_t) {}
void releaseLevel(int8_t) {}
void holdLevels(bool) {}
void lightSleep(const Config&, uint32_t ms, const WakeInput*, uint8_t, bool) { delay(ms); }
[[noreturn]] void deepSleep(const Config&, uint32_t, const WakeInput*, uint8_t) { ESP.restart(); }
uint32_t activeInputs(const WakeInput*, uint8_t, bool) { return 0; }
void setNextRound(uint32_t) {}
int64_t untilNextRoundMs() { return 60000; }
ReportState& report() { static ReportState r{}; return r; }
void loadReport(bool) {}
void saveReport() {}
Rounds loadRounds(bool) { return {0,0}; }
void saveRounds(const Rounds&) {}
void resumeLongSleep(const Config&) {}
BatteryState& batteryState() { static BatteryState s{}; return s; }
void loadBattery(bool) {}
void saveBattery() {}
uint64_t clockMs() { return millis(); }
}
namespace hn::status { void begin() {} void line(const char*, ...) {} void flush() {} }
namespace hn {
Ambient& ambient() { static Ambient a{}; return a; }
Driver* createDriver(const DeviceConfig&, TwoWire*[], const DriverContext&) { return nullptr; }
void Driver::fill(Reading*, const char*, float, bool, bool) {}
bool Driver::sends(const char*) const { return true; }
void addSleepTips(int8_t, uint16_t) {}
}
