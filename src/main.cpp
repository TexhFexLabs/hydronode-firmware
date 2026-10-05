// HydroNode universal firmware.
//
// Boot → read the "hncfg" partition → measure, send, sleep according to the configured power
// mode. Nothing here is board specific; the web flasher decides everything through the config
// block.
//
// A round wakes only the sensors that have a value due (a value sent every 5th round wakes its
// sensor every 5th round), starts their measurements together and naps in light sleep while they
// run (a CO₂ shot, a fan run-up), reads them, puts them back to sleep and only then switches the
// radio on. A round with nothing due does not connect at all. Between rounds the board sleeps as
// deep as the config allows; gas sensors that learn continuously get their short readings from
// light sleep, a rain gauge tip or a button press wakes the board for a moment.

#include <Arduino.h>
#include <HydroNode.h>
#include <Wire.h>

#if !defined(ESP8266)
#include <soc/soc_caps.h>
#endif

#include "actuators/Actuators.h"
#include "config/Config.h"
#include "config/ConfigStore.h"
#include "drivers/Driver.h"
#include "net/Net.h"
#include "ota/Ota.h"
#include "power/Power.h"
#include "power/Schedule.h"
#include "status/Status.h"

using namespace hn;

namespace {

constexpr uint32_t kNoConfigRepeatMs = 10000;
// A round never waits longer than this for its sensors (a fan run-up is 30 s, two CO₂ shots 10 s).
constexpr uint32_t kRoundLimitMs = 90000;

Config cfg;
Driver* drivers[kMaxDevices] = {};
DriverContext contexts[kMaxDevices] = {};
bool begun[kMaxDevices] = {};
uint32_t nextTick[kMaxDevices] = {};
TwoWire* buses[kMaxI2cBuses] = {};
HydroNode* hydro = nullptr;
bool configOk = false;
uint32_t cycleStart = 0;
uint32_t roundIndex = 0;  // measuring rounds since reset; a value with every = n goes out when roundIndex % n == 0
// Internet time (ms) the first round started at. Sleeping rounds are aligned to it, so the drift of
// the sleep timer (the ESP8266 light sleep runs up to 5 % short) never adds up. 0 = not known yet.
uint64_t anchorMs = 0;
// ESP8266 light sleep stops the clock: the next round fetches the time again before signing.
bool clockStopped = false;
ParseResult configError = makeResult(ConfigError::Ok, "");

// Sensor supply: when it came on, and whether it has been on long enough that no sensor needs
// its power-up time (a timer wake-up without a power pin: the sensors slept powered).
uint32_t poweredAt = 0;
bool poweredLong = false;
bool coldSensors = true;  // the next begin() finds the sensors just powered
bool sensorsPowered = true;  // false only between rounds with a switched supply

// Pins that wake the board: the "wake up early" pin, buttons, rain gauges. `wakeInputs` are the
// ones armed for the next sleep: all of them except those held at their wake level.
power::WakeInput allInputs[power::kMaxWakeInputs];
uint8_t allInputCount = 0;
power::WakeInput wakeInputs[power::kMaxWakeInputs];
uint8_t wakeInputCount = 0;
#if !defined(ESP8266)
// A bit per entry of allInputs held at its wake level when the board last went to sleep. Kept
// through deep sleep, so the inputs armed then and the ones read after the wake-up are the same.
RTC_DATA_ATTR uint8_t heldInputs = 0;
#else
uint8_t heldInputs = 0;
#endif

// What this round costs, for the next X-Device-Report.
uint32_t roundStart = 0;
uint32_t roundNapMs = 0;
uint32_t roundWifiMs = 0;

bool sleeping() { return cfg.mode == SleepMode::LightSleep || cfg.mode == SleepMode::DeepSleep || cfg.mode == SleepMode::Hibernate; }
bool restartsEachCycle() { return cfg.mode == SleepMode::DeepSleep || cfg.mode == SleepMode::Hibernate; }

bool anyTicking() {
    for (uint8_t i = 0; i < cfg.deviceCount; i++) {
        if (drivers[i] && drivers[i]->tickMs()) return true;
    }
    return false;
}

// Sensors are switched off between rounds only when nothing samples in the background.
bool cyclesSensorPower() { return cfg.sensorPowerPin >= 0 && sleeping() && !anyTicking(); }

ParseResult loadConfig() {
    // Heap, not .bss: the ESP32-S2 and the ESP8266 have no RAM to spare for a static 8 KB buffer.
    size_t len = kHeaderSize + kMaxPayload;
    uint8_t* block = static_cast<uint8_t*>(malloc(len));
    if (!block) return makeResult(ConfigError::NoPartition, "memory");
    ParseResult result = makeResult(ConfigError::NoPartition, "hncfg");
    if (store::readConfigBlock(block, len)) result = parseBlock(block, len, cfg);
    memset(block, 0, len);  // the block holds the WiFi password
    free(block);
    return result;
}

bool connectedForSend() { return hydro && net::connected(); }

// Always on and modem sleep with short rounds keep the TLS connection for the next round: the
// handshake is 2 to 3 s of a 4 s round. The ESP8266 needs its heap back between rounds.
bool keepConnection() {
#if defined(ESP8266)
    return false;
#else
    return (cfg.mode == SleepMode::AlwaysOn || cfg.mode == SleepMode::ModemSleep) && cfg.intervalSeconds <= 60;
#endif
}

// The smallest "every" of a device: how many rounds lie between two rounds that read it.
uint16_t deviceEvery(uint8_t i) {
    uint16_t every = 0;
    for (uint8_t c = 0; c < cfg.devices[i].channelCount; c++) {
        uint16_t e = cfg.devices[i].channels[c].every ? cfg.devices[i].channels[c].every : 1;
        if (every == 0 || e < every) every = e;
    }
    return every ? every : 1;
}

bool due(const ChannelConfig& ch) { return schedule::due(roundIndex, ch.every); }

bool deviceDue(uint8_t i) {
    for (uint8_t c = 0; c < cfg.devices[i].channelCount; c++) {
        if (due(cfg.devices[i].channels[c])) return true;
    }
    return false;
}

// --- clients and sending ----------------------------------------------------------------------

void ensureClient() {
    if (!hydro) {
        hydro = new HydroNode(cfg.sensorId, cfg.secret, cfg.host);
        hydro->begin();
        act::attach(*hydro);
        ota::attach(*hydro, cfg);
    } else if (clockStopped) {
        clockStopped = !hydro->syncTime();
    }
}

// Connects for a send. Failed attempts are counted for the report of the next round that gets
// through, with the reason (wrong password, network gone, timeout).
bool connectForSend() {
    if (net::connected()) return true;
    power::ReportState& r = power::report();
    bool ok = net::connect(cfg, schedule::wifiTimeoutMs(r.wifiFailures));
    roundWifiMs += net::lastConnectMs();
    if (!ok) {
        if (r.wifiFailures < 0xFFFF) r.wifiFailures++;
        strncpy(r.wifiError, net::lastError() ? net::lastError() : "UNKNOWN", sizeof(r.wifiError) - 1);
        r.wifiError[sizeof(r.wifiError) - 1] = '\0';
        power::saveReport();
        return false;
    }
    // Modem sleep keeps the radio dozing between rounds. The other modes want the round over
    // fast: no doze between the frames of DNS, TLS and the request.
    net::setPowerSave(cfg.mode == SleepMode::ModemSleep);
    return true;
}

// X-Device-Report: the last round's awake time, naps and connect time, pin wake-ups and failed
// connects since, and what went wrong with the sensors this round.
void attachReport(const char* problems) {
    const power::ReportState& r = power::report();
    schedule::ReportData data{r.awakeMs, r.napMs, r.wifiMs, r.pinWakes, r.wifiFailures, r.wifiError};
    char header[241];
    schedule::formatReport(data, problems, header, sizeof(header));
    hydro->setExtraHeader("X-Device-Report", header);
}

// Once a value got through, the report has been seen: start counting again.
void reportDelivered() {
    power::ReportState& r = power::report();
    r.pinWakes = 0;
    r.wifiFailures = 0;
    r.wifiError[0] = '\0';
    power::saveReport();
}

// Button presses go out right away.
void sendPresses() {
    bool sent = false;
    for (const char* type = act::takePress(); type; type = act::takePress()) {
        if (!connectedForSend()) continue;  // no connection: the press still toggled locally
        int code = hydro->sendValue(type, 1);
        ota::afterSend(code);
        status::line("SEND %s %d", type, code);
        if (code >= 200 && code < 300) reportDelivered();
        sent = true;
    }
    if (sent && !keepConnection()) hydro->closeConnection();
}

// A press that woke a sleeping board: connect, send it, take the commands of the answer.
void sendPressesNow() {
    if (!act::hasPresses()) return;
    if (!connectForSend()) {
        while (act::takePress()) {}  // toggled locally already; nothing to send it with
        return;
    }
    ensureClient();
    attachReport("");
    sendPresses();
}

// --- waking inputs ----------------------------------------------------------------------------

bool isWakePin(int8_t pin) { return cfg.wakePin >= 0 && pin == cfg.wakePin; }

const DeviceConfig* rainAt(int8_t pin) {
    for (uint8_t i = 0; i < cfg.deviceCount; i++) {
        if (strcmp(cfg.devices[i].driver, "rain") == 0 && cfg.devices[i].pin == pin) return &cfg.devices[i];
    }
    return nullptr;
}

void armInputs() {
    wakeInputCount = 0;
    for (uint8_t i = 0; i < allInputCount; i++) {
        if (!(heldInputs & (1u << i))) wakeInputs[wakeInputCount++] = allInputs[i];
    }
}

// Pins that wake the board in this power mode. The ESP8266 has none (its deep sleep timer needs
// RST); hibernate wakes on its timer only. `fresh`: after a reset nothing counts as held.
void collectWakeInputs(bool fresh) {
    allInputCount = 0;
    if (fresh) heldInputs = 0;
#if !defined(ESP8266)
    if (sleeping() && cfg.mode != SleepMode::Hibernate) {
        // Set up as inputs, pulled away from the level that wakes, so their level can be read
        // before the drivers start (rain gauges) or when nothing else owns them (wake pin).
        if (cfg.wakePin >= 0) {
            pinMode(cfg.wakePin, cfg.wakeLevel ? INPUT_PULLDOWN : INPUT_PULLUP);
            allInputs[allInputCount++] = {cfg.wakePin, cfg.wakeLevel};
        }
        allInputCount += act::wakeInputs(allInputs + allInputCount, power::kMaxWakeInputs - allInputCount);
        for (uint8_t i = 0; i < cfg.deviceCount && allInputCount < power::kMaxWakeInputs; i++) {
            if (strcmp(cfg.devices[i].driver, "rain") != 0) continue;
            pinMode(cfg.devices[i].pin, INPUT_PULLUP);
            allInputs[allInputCount++] = {cfg.devices[i].pin, 0};
        }
    }
#endif
    armInputs();
}

// Right before a sleep: a pin still at its wake level (a rain contact stuck closed, a shorted
// cable, the wake pin left on) is not armed, else it would wake the board straight back up, over
// and over. It is armed again once it lets go.
void checkHeldInputs() {
    uint8_t held = 0;
    for (uint8_t i = 0; i < allInputCount; i++) {
        if (digitalRead(allInputs[i].pin) == (allInputs[i].level ? HIGH : LOW)) held |= uint8_t(1u << i);
    }
    if (held == heldInputs) return;
    for (uint8_t i = 0; i < allInputCount; i++) {
        if ((held & ~heldInputs) & (1u << i)) status::line("WARN WAKE pin=%d held, not armed", allInputs[i].pin);
    }
    heldInputs = held;
    armInputs();
}

// Waits until a rain gauge's reed contact opens again, so its level does not wake the board
// straight back up. A bucket tips in well under a second.
void waitReleased(int8_t pin, uint8_t level) {
    pinMode(pin, level ? INPUT : INPUT_PULLUP);  // after a deep sleep it is not set up yet
    for (uint32_t start = millis(); millis() - start < 1000;) {
        if (digitalRead(pin) != (level ? HIGH : LOW)) {
            delay(30);  // contact bounce
            if (digitalRead(pin) != (level ? HIGH : LOW)) return;
        }
        delay(5);
    }
}

// After a sleep: counts a rain tip, handles a button press. Returns whether the "wake up early"
// pin fired.
bool handleWakeInputs(bool afterDeepSleep) {
    if (wakeInputCount == 0) return false;
    uint32_t active = power::activeInputs(wakeInputs, wakeInputCount, afterDeepSleep);
    if (!active) return false;
    bool early = false;
    power::ReportState& r = power::report();
    if (r.pinWakes < 0xFFFF) r.pinWakes++;
    for (uint8_t i = 0; i < wakeInputCount; i++) {
        if (!(active & (1u << i))) continue;
        int8_t pin = wakeInputs[i].pin;
        if (isWakePin(pin)) {
            early = true;
        } else if (rainAt(pin)) {
            addSleepTips(pin, 1);
            status::line("TIP pin=%d", pin);
            waitReleased(pin, wakeInputs[i].level);
        } else {
            act::wakePress(pin);
        }
    }
    power::saveReport();
    return early;
}

// --- waiting -----------------------------------------------------------------------------------

void runTicks() {
    uint32_t now = millis();
    for (uint8_t i = 0; i < cfg.deviceCount; i++) {
        Driver* d = drivers[i];
        if (!d || !begun[i] || !d->tickMs()) continue;
        if (int32_t(now - nextTick[i]) < 0) continue;
        d->tick();
        nextTick[i] += d->tickMs();
        if (int32_t(millis() - nextTick[i]) >= 0) nextTick[i] = millis() + d->tickMs();  // fell behind
    }
}

uint32_t untilNextTick(uint32_t limit) {
    uint32_t now = millis();
    for (uint8_t i = 0; i < cfg.deviceCount; i++) {
        if (!drivers[i] || !begun[i] || !drivers[i]->tickMs()) continue;
        int32_t left = int32_t(nextTick[i] - now);
        uint32_t ms = left > 0 ? uint32_t(left) : 0;
        if (ms < limit) limit = ms;
    }
    return limit;
}

// Waits `ms` and keeps the background going: gas sensor readings, timed outputs, buttons.
// `nap`: the board light-sleeps in between (sleeping modes, radio off), waking for those and
// for the wake inputs. Returns early when the "wake up early" pin fires.
bool pause(uint32_t ms, bool nap) {
#if defined(ESP8266)
    // millis() stands still in the ESP8266's light sleep: sleep up to the next timed switch-off
    // at a time and tell the outputs how long it was, so a pulse ends on time.
    if (nap && ms >= schedule::kMinNapMs) {
        for (uint32_t left = ms; left;) {
            uint32_t pending = act::pendingMs();
            uint32_t step = pending && pending < left ? pending : left;
            if (step >= schedule::kMinNapMs) {
                power::lightSleep(cfg, step, nullptr, 0, true);
                act::advance(step);
            } else {
                delay(step);
                act::service();
            }
            left -= step;
        }
        clockStopped = true;
        return false;
    }
#endif
    uint32_t start = millis();
    for (;;) {
        power::feedWatchdog();
        runTicks();
        act::service();
        if (net::connected()) sendPresses();
        int32_t left = int32_t(ms - (millis() - start));
        if (left <= 0) return false;
        uint32_t step = untilNextTick(uint32_t(left));
        uint32_t pending = act::pendingMs();
        if (pending && pending < step) step = pending;
        if (nap && step >= schedule::kMinNapMs && !net::connected()) {
            uint32_t before = millis();
            // Pulse interrupts off while asleep: a tip wakes the board and is counted once, here.
            for (uint8_t i = 0; i < cfg.deviceCount; i++) {
                if (drivers[i] && begun[i]) drivers[i]->beforeSleep();
            }
            checkHeldInputs();
            power::lightSleep(cfg, step, wakeInputs, wakeInputCount, true);
            for (uint8_t i = 0; i < cfg.deviceCount; i++) {
                if (drivers[i] && begun[i]) drivers[i]->afterSleep();
            }
            roundNapMs += millis() - before;
            if (handleWakeInputs(false)) return true;
            sendPressesNow();
            if (act::hasPresses() == false && net::connected()) net::off();
        } else {
            // Awake: buttons are polled, so short steps while there are any.
            if (act::busy()) step = min<uint32_t>(step, 10);
            delay(step ? step : 1);
        }
    }
}

// --- sensors -----------------------------------------------------------------------------------

void startBuses() {
    for (uint8_t i = 0; i < cfg.i2cCount; i++) {
        if (buses[i]) continue;
#if defined(ESP8266)
        buses[i] = i == 0 ? &Wire : nullptr;  // one software I2C bus
#elif SOC_HP_I2C_NUM > 1
        buses[i] = i == 0 ? &Wire : &Wire1;
#else
        buses[i] = i == 0 ? &Wire : nullptr;  // C3/C6 have one I2C controller
#endif
        if (!buses[i]) {
            status::line("ERR CONFIG BAD_VALUE i2c.second_bus");
            continue;
        }
#if defined(ESP8266)
        buses[i]->begin(cfg.i2c[i].sda, cfg.i2c[i].scl);
        buses[i]->setClock(cfg.i2c[i].hz);
#else
        buses[i]->begin(cfg.i2c[i].sda, cfg.i2c[i].scl, cfg.i2c[i].hz);
#endif
    }
}

void createDrivers() {
    for (uint8_t i = 0; i < cfg.deviceCount; i++) {
        if (act::isActuator(cfg.devices[i].driver)) continue;  // set up in act::begin()
        contexts[i] = {cfg.mode, cfg.intervalSeconds, deviceEvery(i), cfg.sensorPowerPin >= 0, cfg.adcRangeMv};
        drivers[i] = createDriver(cfg.devices[i], buses, contexts[i]);
        if (!drivers[i]) status::line("ERR SENSOR %s unknown", cfg.devices[i].driver);
    }
}

void powerSensors() {
    power::sensorsOn(cfg);
    startBuses();
    sensorsPowered = true;
    poweredAt = millis();
    poweredLong = false;
    coldSensors = true;
    for (uint8_t i = 0; i < cfg.deviceCount; i++) begun[i] = false;
}

enum class Step : uint8_t { Idle, Begin, PowerUp, Started, Ready, Done };

struct Run {
    Step step;
    uint32_t at;
    bool due;
};

uint32_t since(uint32_t base, uint32_t ms) { return poweredLong ? millis() : base + ms; }

// Brings the devices of this round to "ready": begin() where needed (after the sensor's boot
// time), its power-up time, start(), the waits poll() asks for. The board naps meanwhile. Devices
// that are not due but have never been set up since power-on get begin() and sleep() only, so
// they rest in their lowest power state.
void prepareDevices(Run runs[], bool napAllowed) {
    uint32_t limit = millis() + kRoundLimitMs;
    for (;;) {
        bool pending = false;
        uint32_t wait = UINT32_MAX;
        for (uint8_t i = 0; i < cfg.deviceCount; i++) {
            Run& r = runs[i];
            if (r.step == Step::Idle || r.step == Step::Ready || r.step == Step::Done) continue;
            pending = true;
            uint32_t now = millis();
            if (int32_t(now - r.at) < 0) {
                wait = min<uint32_t>(wait, r.at - now);
                continue;
            }
            Driver* d = drivers[i];
            switch (r.step) {
                case Step::Begin: {
                    bool ok = d->begin(coldSensors);
                    begun[i] = ok;  // a sensor that did not answer is tried again next round
                    nextTick[i] = millis() + d->tickMs();
                    status::line("DEV %s %s", cfg.devices[i].driver, ok ? "ok" : "missing");
                    if (!ok) {
                        r.step = Step::Done;
                    } else if (!r.due) {
                        d->sleep();
                        r.step = Step::Done;
                    } else {
                        r.step = Step::PowerUp;
                        r.at = since(poweredAt, d->powerUpMs());
                    }
                    break;
                }
                case Step::PowerUp:
                    r.at = millis() + d->start();
                    r.step = Step::Started;
                    break;
                case Step::Started: {
                    uint32_t more = d->poll();
                    if (more == 0) r.step = Step::Ready;
                    else r.at = millis() + more;
                    break;
                }
                default:
                    break;
            }
            wait = 0;  // something moved: look again right away
        }
        if (!pending) return;
        if (int32_t(millis() - limit) >= 0) {
            status::line("WARN ROUND sensors still busy after %lus", (unsigned long)(kRoundLimitMs / 1000));
            for (uint8_t i = 0; i < cfg.deviceCount; i++) {
                if (runs[i].step != Step::Idle && runs[i].step != Step::Done) runs[i].step = Step::Ready;
            }
            return;
        }
        if (wait) pause(wait, napAllowed && wait >= schedule::kMinNapMs);
    }
}

// Gas sensors compensate with the temperature and humidity another sensor measured.
void updateAmbient(const Reading readings[][kMaxChannels]) {
    for (uint8_t i = 0; i < cfg.deviceCount; i++) {
        if (!drivers[i] || drivers[i]->tickMs()) continue;
        for (uint8_t c = 0; c < cfg.devices[i].channelCount; c++) {
            const Reading& r = readings[i][c];
            if (!r.ok) continue;
            if (strcmp(cfg.devices[i].channels[c].q, "t") == 0) ambient().t = r.value;
            if (strcmp(cfg.devices[i].channels[c].q, "rh") == 0) ambient().rh = r.value;
        }
    }
}

void readDevice(uint8_t i, Reading readings[][kMaxChannels]) {
    for (uint8_t c = 0; c < kMaxChannels; c++) readings[i][c] = {nullptr, 0, false};
    if (drivers[i]) drivers[i]->read(readings[i]);
}

// "scd4x:missing,pms5003:warming" for the devices with a due value that did not arrive.
void collectProblems(const Reading readings[][kMaxChannels], const Run runs[], char* out, size_t cap) {
    size_t used = 0;
    out[0] = '\0';
    for (uint8_t i = 0; i < cfg.deviceCount; i++) {
        if (!runs[i].due || act::isActuator(cfg.devices[i].driver)) continue;
        const char* problem = nullptr;
        for (uint8_t c = 0; c < cfg.devices[i].channelCount; c++) {
            const Reading& r = readings[i][c];
            if (!due(cfg.devices[i].channels[c]) || r.ok) continue;
            problem = r.warming ? problem::kWarming : nullptr;
            if (!problem) problem = drivers[i] && drivers[i]->problem() ? drivers[i]->problem() : problem::kTimeout;
            if (!drivers[i]) problem = "unknown";
            break;
        }
        if (!problem) continue;
        int n = snprintf(out + used, cap - used, "%s%s:%s", used ? "," : "", cfg.devices[i].driver, problem);
        if (n < 0 || size_t(n) >= cap - used) break;
        used += size_t(n);
    }
}

// One round: the due sensors measure, then the values go out over one connection. The report
// tells an update in verification how it went.
ota::RoundReport measureAndSend() {
    ota::RoundReport report{false, 0, nullptr};
    Reading readings[kMaxDevices][kMaxChannels] = {};
    Run runs[kMaxDevices] = {};
    bool anyDue = false;
    bool persistent = !restartsEachCycle();  // drivers live on between rounds

    for (uint8_t i = 0; i < cfg.deviceCount; i++) {
        if (!drivers[i]) continue;
        runs[i].due = deviceDue(i);
        anyDue = anyDue || runs[i].due;
        if (runs[i].due) drivers[i]->resetProblem();  // what goes wrong this round, not before
    }
    if (anyDue && cyclesSensorPower() && !sensorsPowered) powerSensors();
    for (uint8_t i = 0; i < cfg.deviceCount && sensorsPowered; i++) {
        Driver* d = drivers[i];
        if (!d) continue;
        // Set up once per power-up even when not due: it then rests in its low power state.
        bool setUp = !begun[i] && (coldSensors || persistent);
        bool touch = (runs[i].due && !d->afterConnect()) || setUp;
        if (!touch) continue;
        if (!begun[i]) {
            runs[i].step = Step::Begin;
            runs[i].at = since(poweredAt, d->bootMs());
        } else {
            runs[i].step = Step::PowerUp;
            runs[i].at = since(poweredAt, d->powerUpMs());
        }
        if (d->afterConnect()) runs[i].due = false;  // read once connected
    }
    bool napAllowed = sleeping();
#if defined(ESP8266)
    napAllowed = false;  // millis() stops in its light sleep
#endif
    prepareDevices(runs, napAllowed);
    coldSensors = false;

    // Sensors that compensate with another one's temperature (TDS, ultrasonic) read after it.
    for (uint8_t pass = 0; pass < 2; pass++) {
        for (uint8_t i = 0; i < cfg.deviceCount; i++) {
            if (runs[i].step == Step::Ready && drivers[i] && drivers[i]->usesAmbient() == (pass == 1)) readDevice(i, readings);
        }
        if (pass == 0) updateAmbient(readings);
    }
    uint32_t readAt = millis();
    for (uint8_t i = 0; i < cfg.deviceCount; i++) {
        if (runs[i].step == Step::Ready && drivers[i]) drivers[i]->sleep();
        if (drivers[i] && drivers[i]->afterConnect()) runs[i].due = deviceDue(i);
    }
    status::line("ROUND %lu", (unsigned long)roundIndex);
    if (!anyDue) return report;

    if (!connectForSend()) return report;
    report.wifiOk = true;
    ensureClient();
    // Values that only exist with a connection (WiFi signal).
    for (uint8_t i = 0; i < cfg.deviceCount; i++) {
        if (drivers[i] && drivers[i]->afterConnect() && runs[i].due) {
            for (uint8_t c = 0; c < kMaxChannels; c++) readings[i][c] = {nullptr, 0, false};
            drivers[i]->begin(false);
            drivers[i]->read(readings[i]);
        }
    }

    char problems[200];
    collectProblems(readings, runs, problems, sizeof(problems));
    attachReport(problems);

    // Drivers that failed this round travel with every value (X-Device-Status readErr=…).
    hydro->clearReadErrors();
    for (uint8_t i = 0; i < cfg.deviceCount; i++) {
        for (uint8_t c = 0; c < cfg.devices[i].channelCount; c++) {
            const Reading& r = readings[i][c];
            if (!due(cfg.devices[i].channels[c]) || act::isActuator(cfg.devices[i].driver)) continue;
            // Only confirmed communication with a warming algorithm is exempt. Missing drivers
            // and failed gas sensor initialization must block Strict verification too.
            if (!r.ok && !r.warming) {
                hydro->reportReadError(cfg.devices[i].driver);
                if (!report.failedDriver) report.failedDriver = cfg.devices[i].driver;
            }
        }
    }

    // All due values in one request, stamped with the time they were read: they arrive together
    // and land on the same point in time.
    HydroNodeValue batch[kMaxTotalChannels];
    int codes[kMaxTotalChannels];
    size_t count = 0;
    for (uint8_t i = 0; i < cfg.deviceCount; i++) {
        for (uint8_t c = 0; c < cfg.devices[i].channelCount; c++) {
            const Reading& r = readings[i][c];
            if (!r.type || !due(cfg.devices[i].channels[c])) continue;
            if (!r.ok) {
                bool settling = r.warming;
                status::line("%s SENSOR %s %s", settling ? "WAIT" : "ERR", cfg.devices[i].driver,
                             cfg.devices[i].channels[c].type);
                continue;
            }
            if (count < kMaxTotalChannels) batch[count++] = {r.type, r.value};
        }
    }
    if (count) {
        int code = hydro->sendValues(batch, count, codes, millis() - readAt);
        ota::afterSend(code);
        for (size_t k = 0; k < count; k++) status::line("SEND %s %d", batch[k].type, codes[k]);
        if (code == 401 || code == 403) status::line("ERR AUTH %d", code);
        report.bestStatus = code;
        if (code >= 200 && code < 300) reportDelivered();
        act::service();  // a command in the answer may have started a short pulse
    }
    sendPresses();
    if (!keepConnection()) hydro->closeConnection();
    return report;
}

// What the round cost, for the report of the next one.
void noteRoundCost() {
    power::ReportState& r = power::report();
    uint32_t total = millis() - roundStart;
    r.napMs = roundNapMs;
    r.awakeMs = total > roundNapMs ? total - roundNapMs : 0;
    r.wifiMs = roundWifiMs;
    power::saveReport();
    status::line("COST awake=%lu nap=%lu wifi=%lu", (unsigned long)r.awakeMs, (unsigned long)r.napMs,
                 (unsigned long)r.wifiMs);
}

// One round, and again while an update in verification asks for another try. Afterwards a
// waiting update offer is carried out (it may restart the board).
void runRound() {
    // After a deep sleep the round started at boot.
    roundStart = restartsEachCycle() ? 0 : millis();
    roundNapMs = 0;
    roundWifiMs = 0;
    ota::RoundReport report = measureAndSend();
    while (ota::afterRound(hydro, cfg, report)) {
        pause(ota::kRetryPauseMs, false);
        report = measureAndSend();
    }
    noteRoundCost();
}

// Milliseconds until the next round should start. With internet time from the last send, rounds
// land on anchor + n × interval however much the sleep timer drifts. Without it, the interval
// minus the time awake.
uint32_t untilNextRound() {
    uint32_t fallback = power::sleepMs(cfg.intervalSeconds, millis() - cycleStart);
    // A round that sent nothing did not fetch the time; after a stopped clock it would be off.
    uint64_t now = hydro && !clockStopped ? hydro->epochMs() : 0;
    if (now == 0) return fallback;
    uint64_t interval = uint64_t(cfg.intervalSeconds) * 1000;
    uint64_t started = now - (millis() - cycleStart);  // this round's start
    if (anchorMs == 0 || anchorMs > started || now - anchorMs > 30ULL * 24 * 3600 * 1000) anchorMs = started;
    uint64_t wait = schedule::nextSlotMs(anchorMs, interval, started, now) - now;
    return wait < 1000 ? 1000 : uint32_t(wait);
}

void sensorsOffForSleep() {
    if (!cyclesSensorPower()) return;
    // The bus pins let go first: with their pull-ups on they would feed the unpowered sensors
    // through their protection diodes. powerSensors() starts the buses again.
    for (uint8_t i = 0; i < cfg.i2cCount; i++) {
        if (!buses[i]) continue;
#if !defined(ESP8266)
        buses[i]->end();
#endif
        pinMode(cfg.i2c[i].sda, INPUT);
        pinMode(cfg.i2c[i].scl, INPUT);
        buses[i] = nullptr;
    }
    power::sensorsOff(cfg);
    sensorsPowered = false;
}

[[noreturn]] void sleepDeep(uint32_t ms) {
    net::off();
    sensorsOffForSleep();
    checkHeldInputs();
    power::saveRounds({roundIndex, anchorMs});
    power::saveReport();
    power::deepSleep(cfg, ms, wakeInputs, wakeInputCount);
}

// A deep sleep was cut short by a pin: count the tip, send the press, or start the round when
// it is due anyway. Returns only when the round should run now.
void afterPinWake() {
    bool early = handleWakeInputs(true);
    int64_t left = power::untilNextRoundMs();
    switch (schedule::afterPinWake(left, act::hasPresses(), early, ota::busy())) {
        case schedule::PinWake::Round:
            return;
        case schedule::PinWake::Send:
            sendPressesNow();
            if (hydro) hydro->closeConnection();
            break;
        case schedule::PinWake::Sleep:
            break;
    }
    // The press may have taken a few seconds: sleep only what is left until the round.
    left = power::untilNextRoundMs();
    sleepDeep(left > 1000 ? uint32_t(left) : 1000);
}

void printConfigError() {
    status::line("ERR CONFIG %s %s", errorName(configError.error), configError.detail);
}

}  // namespace

void setup() {
    cycleStart = millis();
    power::startWatchdog();
    const char* wake = power::wakeReason();
    status::begin();
    // Gives a serial monitor time to attach after a reset; a wake-up from sleep goes on.
    if (strcmp(wake, "RESET") == 0) delay(50);
    status::line("BOOT fw=%s family=%s wake=%s heap=%u", HN_FW_VERSION, HN_FAMILY, wake, (unsigned)ESP.getFreeHeap());

    configError = loadConfig();
    configOk = configError.error == ConfigError::Ok;
    // An update waiting for its verdict; a new config that does not parse goes back to the old one.
    ota::begin(cfg, configError);
    if (!configOk) {
        printConfigError();
        return;
    }
    // ESP8266 intervals longer than one deep sleep: intermediate wake-ups end here.
    power::resumeLongSleep(cfg);
    bool timer = strcmp(wake, "TIMER") == 0;
    bool pin = strcmp(wake, "PIN") == 0;
    power::Rounds rounds = power::loadRounds(!(timer || pin));
    roundIndex = rounds.index;
    anchorMs = rounds.anchorMs;
    power::loadReport(!(timer || pin));
    status::line("CFG ok board=%s devices=%u mode=%s interval=%lu rev=%lu", cfg.board, cfg.deviceCount,
                 sleepModeName(cfg.mode), (unsigned long)cfg.intervalSeconds, (unsigned long)cfg.rev);

    act::begin(cfg);
    startBuses();
    createDrivers();
    collectWakeInputs(!(timer || pin));
    if (pin && restartsEachCycle()) afterPinWake();

    // After a sleep without a power pin the sensors stayed powered and settled: no power-up wait
    // and no full set-up. After power-on or reset they start from scratch.
    if (cyclesSensorPower()) {
        sensorsPowered = false;  // switched on in the rounds that read something
    } else if (cfg.sensorPowerPin >= 0) {
        powerSensors();  // awake modes, or background sampling: the supply stays on
    } else {
        poweredAt = 0;
        poweredLong = timer || pin;
        coldSensors = !(timer || pin);
    }
    if (cfg.mode == SleepMode::AlwaysOn || cfg.mode == SleepMode::ModemSleep) connectForSend();
}

void loop() {
    power::feedWatchdog();
    if (!configOk) {
        // Keep repeating so a monitor that attaches late still sees the reason.
        delay(kNoConfigRepeatMs);
        printConfigError();
        return;
    }

    runRound();
    roundIndex++;

    switch (cfg.mode) {
        case SleepMode::AlwaysOn:
        case SleepMode::ModemSleep:
            pause(power::sleepMs(cfg.intervalSeconds, millis() - cycleStart), false);
            cycleStart += cfg.intervalSeconds * 1000;
            // Fell behind (a slow round): start the next one from now instead of catching up.
            if (int32_t(millis() - cycleStart) > int32_t(cfg.intervalSeconds * 1000)) cycleStart = millis();
            poweredLong = true;
            break;

        case SleepMode::LightSleep: {
            uint32_t sleepFor = untilNextRound();
            net::off();
            if (cyclesSensorPower()) sensorsOffForSleep();
            else poweredLong = true;
            status::line("SLEEP LIGHT %lu.%02lu", (unsigned long)(sleepFor / 1000), (unsigned long)(sleepFor % 1000 / 10));
            pause(sleepFor, true);  // gas sensor readings, tips and presses wake it meanwhile
            cycleStart = millis();
            break;
        }

        case SleepMode::DeepSleep:
        case SleepMode::Hibernate: {
            // A timed output switches off on time: stay (napping, radio off) until it did.
            if (act::pendingMs()) {
                if (hydro) hydro->closeConnection();
                net::off();
            }
            for (uint32_t left = act::pendingMs(); left; left = act::pendingMs()) pause(left, true);
            sleepDeep(untilNextRound());
        }
    }
}
