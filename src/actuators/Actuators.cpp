#include "Actuators.h"

#include <Arduino.h>
#include <HydroNode.h>
#include <string.h>

#if !defined(ESP8266)
#include <Preferences.h>
#include <driver/gpio.h>
#endif

#include "status/Status.h"

namespace hn::act {

namespace {

// Longest timed switch-on a command may ask for. Anything longer is almost certainly a typo
// (ms instead of s), and a pump that runs for a day is worse than one that stops early.
constexpr uint32_t kMaxPulseMs = 24UL * 3600 * 1000;
constexpr uint32_t kDebounceMs = 30;

struct Output {
    const DeviceConfig* dev;
    uint8_t pin;
    bool activeLow;
    bool dim;
    bool on;
    uint8_t level;   // brightness when on, 0..255 (dimmable LEDs)
    bool pulsing;
    uint32_t offAt;  // millis() when a timed switch-on ends
};

struct Button {
    const DeviceConfig* dev;
    uint8_t pin;
    bool pressedLevel;
    bool pressed;     // debounced state
    bool raw;
    uint32_t changedAt;
    int8_t target;    // output toggled on press, -1 for none
    const char* type; // measurement type reported on press, nullptr for none
    uint8_t queued;   // presses not sent yet
};

// The last state of every output survives a restart (an update, a crash, the watchdog): a vent
// relay that was on is on again before the first round. ESP32 family: NVS, written only when the
// state changes. ESP8266: RTC memory (slots 64..72), which survives a restart but not a power cut;
// then the start state from the config applies.
#if defined(ESP8266)
constexpr uint32_t kRtcOutSlot = 64;
constexpr uint32_t kRtcOutMagic = 0x484E4F55;  // "HNOU"
#endif

Output outputs[kMaxDevices];
uint32_t savedState[kMaxDevices];  // last value written per output, to skip unchanged writes
uint8_t outputCount = 0;
Button buttons[kMaxDevices];
uint8_t buttonCount = 0;

bool isOutput(const char* driver) {
    return strcmp(driver, "relay") == 0 || strcmp(driver, "led") == 0 || strcmp(driver, "output") == 0;
}

uint16_t nameHash(const char* name) {
    uint32_t h = 2166136261u;
    for (const char* c = name; c && *c; c++) h = (h ^ uint8_t(*c)) * 16777619u;
    return uint16_t(h ^ (h >> 16));
}

// Name hash, level and on/off in one word: a slot only restores the output it was written for.
uint32_t packState(const Output& o) {
    return (uint32_t(nameHash(o.dev->text("cmd", o.dev->driver))) << 16) | (uint32_t(o.level) << 8) | (o.on ? 1 : 0);
}

void remember(uint8_t index) {
    if (index >= outputCount) return;
    uint32_t value = packState(outputs[index]);
    if (value == savedState[index]) return;
    savedState[index] = value;
#if defined(ESP8266)
    uint32_t magic = kRtcOutMagic;
    ESP.rtcUserMemoryWrite(kRtcOutSlot, &magic, sizeof(magic));
    ESP.rtcUserMemoryWrite(kRtcOutSlot + 1 + index, &value, sizeof(value));
#else
    Preferences prefs;
    if (prefs.begin("hn-out", false)) {
        char key[6];
        snprintf(key, sizeof(key), "o%u", unsigned(index));
        prefs.putUInt(key, value);
        prefs.end();
    }
#endif
}

// The stored state of output `index`, when it belongs to this output.
bool recall(uint8_t index, const Output& o, bool& on, uint8_t& level) {
#if defined(ESP8266)
    uint32_t magic = 0;
    uint32_t value = 0;
    ESP.rtcUserMemoryRead(kRtcOutSlot, &magic, sizeof(magic));
    if (magic != kRtcOutMagic) return false;
    ESP.rtcUserMemoryRead(kRtcOutSlot + 1 + index, &value, sizeof(value));
#else
    Preferences prefs;
    if (!prefs.begin("hn-out", true)) return false;
    char key[6];
    snprintf(key, sizeof(key), "o%u", unsigned(index));
    uint32_t value = prefs.getUInt(key, 0);
    prefs.end();
#endif
    if (value == 0 || (value >> 16) != nameHash(o.dev->text("cmd", o.dev->driver))) return false;
    on = (value & 1) != 0;
    level = uint8_t(value >> 8);
    return true;
}

uint8_t indexOf(const Output& o) { return uint8_t(&o - outputs); }

void apply(Output& o) {
    if (o.dim) {
        uint8_t duty = o.on ? o.level : 0;
        analogWrite(o.pin, o.activeLow ? 255 - duty : duty);
    } else {
        digitalWrite(o.pin, o.on != o.activeLow ? HIGH : LOW);
    }
}

void set(Output& o, bool on) {
    o.pulsing = false;
    o.on = on;
    apply(o);
    remember(indexOf(o));
    status::line("OUT %s %s", o.dev->text("cmd", o.dev->driver), on ? "on" : "off");
}

void pulse(Output& o, uint32_t ms) {
    if (ms == 0) {
        set(o, false);
        return;
    }
    if (ms > kMaxPulseMs) ms = kMaxPulseMs;
    o.on = true;
    o.pulsing = true;
    o.offAt = millis() + ms;
    apply(o);
    // A pulse does not outlive a restart: remembered as off.
    o.on = false;
    remember(indexOf(o));
    o.on = true;
    status::line("OUT %s on %lums", o.dev->text("cmd", o.dev->driver), (unsigned long)ms);
}

int8_t findOutput(const char* name) {
    if (!name || !name[0]) return -1;
    for (uint8_t i = 0; i < outputCount; i++) {
        if (strcmp(outputs[i].dev->text("cmd", ""), name) == 0) return int8_t(i);
    }
    return -1;
}

}  // namespace

bool isActuator(const char* driver) { return isOutput(driver) || strcmp(driver, "button") == 0; }

void begin(const Config& cfg) {
    outputCount = 0;
    buttonCount = 0;
#if defined(ESP8266)
    analogWriteRange(255);
#endif
    for (uint8_t i = 0; i < cfg.deviceCount; i++) {
        const DeviceConfig& dev = cfg.devices[i];
        if (dev.pin < 0) continue;
        if (isOutput(dev.driver)) {
            Output& o = outputs[outputCount++];
            o = {};
            o.dev = &dev;
            o.pin = uint8_t(dev.pin);
            o.activeLow = strcmp(dev.text("active", "HIGH"), "LOW") == 0;
            o.dim = strcmp(dev.driver, "led") == 0 && dev.number("dim", 0) != 0;
            o.level = 255;
            o.on = strcmp(dev.text("start", "OFF"), "ON") == 0;
            uint8_t index = uint8_t(outputCount - 1);
            savedState[index] = 0;
            bool restored = recall(index, o, o.on, o.level);
            if (restored) savedState[index] = packState(o);
#if !defined(ESP8266)
            gpio_hold_dis(gpio_num_t(o.pin));
#endif
            // Level first, then output: an active-low relay must not click on during boot.
            if (!o.dim) digitalWrite(o.pin, o.on != o.activeLow ? HIGH : LOW);
            pinMode(o.pin, OUTPUT);
            apply(o);
            status::line("OUT %s %s pin=%d%s", dev.text("cmd", dev.driver), o.on ? "on" : "off", dev.pin,
                         restored ? " restored" : "");
        } else if (strcmp(dev.driver, "button") == 0) {
            Button& b = buttons[buttonCount++];
            b = {};
            b.dev = &dev;
            b.pin = uint8_t(dev.pin);
            b.pressedLevel = strcmp(dev.text("press", "LOW"), "HIGH") == 0;
            pinMode(b.pin, strcmp(dev.text("pull", "UP"), "UP") == 0 ? INPUT_PULLUP : INPUT);
            b.raw = b.pressed = digitalRead(b.pin) == (b.pressedLevel ? HIGH : LOW);
            b.type = dev.channelCount > 0 ? dev.channels[0].type : nullptr;
            b.target = -1;
        }
    }
    // Outputs exist now, so buttons can find the one they toggle.
    for (uint8_t i = 0; i < buttonCount; i++) buttons[i].target = findOutput(buttons[i].dev->text("toggles", ""));
    for (uint8_t i = 0; i < buttonCount; i++) {
        status::line("BTN pin=%d reports=%s toggles=%s", buttons[i].pin, buttons[i].type ? buttons[i].type : "-",
                     buttons[i].target >= 0 ? outputs[buttons[i].target].dev->text("cmd", "-") : "-");
    }
}

void attach(HydroNode& hydro) {
    for (uint8_t i = 0; i < outputCount; i++) {
        Output* o = &outputs[i];
        const char* name = o->dev->text("cmd", nullptr);
        if (!name) continue;
        if (o->dev->number("toggle", 1) != 0) {
            hydro.onBool(name, [o](bool on) { set(*o, on); });
        }
        if (o->dev->number("timed", 1) != 0) {
            hydro.onUInt32(name, [o](uint32_t ms) { pulse(*o, ms); });
        }
        if (o->dim) {
            hydro.onUInt32(String(name) + "_level", [o](uint32_t percent) {
                if (percent > 100) percent = 100;
                o->level = uint8_t(percent * 255 / 100);
                set(*o, percent > 0);
            });
        }
    }
}

void service() {
    uint32_t now = millis();
    for (uint8_t i = 0; i < outputCount; i++) {
        Output& o = outputs[i];
        if (o.pulsing && int32_t(now - o.offAt) >= 0) set(o, false);
    }
    for (uint8_t i = 0; i < buttonCount; i++) {
        Button& b = buttons[i];
        bool raw = digitalRead(b.pin) == (b.pressedLevel ? HIGH : LOW);
        if (raw != b.raw) {
            b.raw = raw;
            b.changedAt = now;
        } else if (raw != b.pressed && now - b.changedAt >= kDebounceMs) {
            b.pressed = raw;
            if (!raw) continue;  // release
            status::line("BTN pin=%d pressed", b.pin);
            if (b.target >= 0) set(outputs[b.target], !outputs[b.target].on);
            if (b.type && b.queued < 255) b.queued++;
        }
    }
}

bool busy() {
    if (buttonCount > 0) return true;
    for (uint8_t i = 0; i < outputCount; i++) {
        if (outputs[i].pulsing) return true;
    }
    return false;
}

uint32_t pendingMs() {
    uint32_t now = millis();
    uint32_t next = 0;
    for (uint8_t i = 0; i < outputCount; i++) {
        if (!outputs[i].pulsing) continue;
        int32_t left = int32_t(outputs[i].offAt - now);
        uint32_t ms = left > 0 ? uint32_t(left) : 1;
        if (next == 0 || ms < next) next = ms;
    }
    return next;
}

const char* takePress() {
    for (uint8_t i = 0; i < buttonCount; i++) {
        if (buttons[i].queued > 0) {
            buttons[i].queued--;
            return buttons[i].type;
        }
    }
    return nullptr;
}

void holdForSleep(bool hold) {
#if !defined(ESP8266)
    for (uint8_t i = 0; i < outputCount; i++) {
        if (outputs[i].dim) continue;
        if (hold) gpio_hold_en(gpio_num_t(outputs[i].pin));
        else gpio_hold_dis(gpio_num_t(outputs[i].pin));
    }
#else
    (void)hold;
#endif
}

}  // namespace hn::act
