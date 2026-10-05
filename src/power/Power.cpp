#include "Power.h"

#if !defined(ESP8266)

#include <Arduino.h>
#include <driver/gpio.h>
#include <esp_sleep.h>
#include <esp_task_wdt.h>
#include <soc/soc_caps.h>
#include <sys/time.h>

#if SOC_RTCIO_INPUT_OUTPUT_SUPPORTED
#include <driver/rtc_io.h>
#endif

#include "status/Status.h"

namespace hn::power {

namespace {

// --- held pins --------------------------------------------------------------------------------

int8_t keptPins[kMaxDevices + 2];
uint8_t keptCount = 0;

void pull(gpio_num_t pin, bool high) {
#if SOC_RTCIO_INPUT_OUTPUT_SUPPORTED
    if (rtc_gpio_is_valid_gpio(pin)) {
        // ESP32, S2, S3: the RTC pull is the one that stays on in deep sleep.
        if (high) {
            rtc_gpio_pulldown_dis(pin);
            rtc_gpio_pullup_en(pin);
        } else {
            rtc_gpio_pullup_dis(pin);
            rtc_gpio_pulldown_en(pin);
        }
        return;
    }
#endif
    // C3, C6: digital pull. An external resistor is more reliable, the internal one is weak.
    if (high) {
        gpio_pulldown_dis(pin);
        gpio_pullup_en(pin);
    } else {
        gpio_pullup_dis(pin);
        gpio_pulldown_en(pin);
    }
}

// --- wake inputs ------------------------------------------------------------------------------

void armLight(const WakeInput* inputs, uint8_t count) {
    for (uint8_t i = 0; i < count; i++) {
        gpio_wakeup_enable(gpio_num_t(inputs[i].pin), inputs[i].level ? GPIO_INTR_HIGH_LEVEL : GPIO_INTR_LOW_LEVEL);
    }
    if (count) esp_sleep_enable_gpio_wakeup();
}

void disarmLight(const WakeInput* inputs, uint8_t count) {
    for (uint8_t i = 0; i < count; i++) gpio_wakeup_disable(gpio_num_t(inputs[i].pin));
}

// The input that goes on EXT0 (ESP32, S2, S3): the one whose level differs from the others, so
// EXT1 sees one level only. -1 without EXT0.
int8_t ext0Index(const WakeInput* inputs, uint8_t count) {
#if SOC_PM_SUPPORT_EXT0_WAKEUP
    if (count == 0) return -1;
    uint8_t high = 0;
    for (uint8_t i = 0; i < count; i++) high += inputs[i].level;
    bool minorityHigh = high * 2 < count;
    for (uint8_t i = 0; i < count; i++) {
        if (bool(inputs[i].level) == minorityHigh) return int8_t(i);
    }
    return 0;
#else
    (void)inputs;
    (void)count;
    return -1;
#endif
}

void armDeep(const WakeInput* inputs, uint8_t count) {
    if (count == 0) return;
    int8_t ext0 = ext0Index(inputs, count);
    uint64_t lowMask = 0, highMask = 0;
    for (uint8_t i = 0; i < count; i++) {
        gpio_num_t pin = gpio_num_t(inputs[i].pin);
        bool high = inputs[i].level == 1;
        pull(pin, !high);  // idle level: pulled away from the level that wakes
#if SOC_PM_SUPPORT_EXT0_WAKEUP
        if (i == ext0) {
            esp_sleep_enable_ext0_wakeup(pin, high ? 1 : 0);
            continue;
        }
#endif
        (high ? highMask : lowMask) |= 1ULL << inputs[i].pin;
    }
    (void)ext0;
#if SOC_PM_SUPPORT_EXT1_WAKEUP && !SOC_GPIO_SUPPORT_DEEPSLEEP_WAKEUP
    // ESP32, S2, S3: one EXT1 level for all remaining pins (the web app keeps it that way).
    // The RTC pulls need the RTC peripherals powered through the sleep.
    esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_PERIPH, ESP_PD_OPTION_ON);
    if (highMask) esp_sleep_enable_ext1_wakeup(highMask, ESP_EXT1_WAKEUP_ANY_HIGH);
#if CONFIG_IDF_TARGET_ESP32
    // The classic ESP32 has no "any low": with one pin, "all low" is the same.
    if (lowMask) esp_sleep_enable_ext1_wakeup(lowMask, ESP_EXT1_WAKEUP_ALL_LOW);
#else
    if (lowMask) esp_sleep_enable_ext1_wakeup(lowMask, ESP_EXT1_WAKEUP_ANY_LOW);
#endif
#elif SOC_GPIO_SUPPORT_DEEPSLEEP_WAKEUP
    // C3, C6: the low-power GPIOs wake the board directly, each with its own level.
    if (lowMask) esp_deep_sleep_enable_gpio_wakeup(lowMask, ESP_GPIO_WAKEUP_GPIO_LOW);
    if (highMask) esp_deep_sleep_enable_gpio_wakeup(highMask, ESP_GPIO_WAKEUP_GPIO_HIGH);
#else
    status::line("WARN WAKEPIN unsupported");
#endif
}

// --- RTC state --------------------------------------------------------------------------------

constexpr uint32_t kRoundMagic = 0x484E5244;   // "HNRD"
constexpr uint32_t kReportMagic = 0x484E5250;  // "HNRP"
RTC_DATA_ATTR uint32_t roundMagic = 0;
RTC_DATA_ATTR Rounds savedRounds = {0, 0};
RTC_DATA_ATTR uint32_t reportMagic = 0;
RTC_DATA_ATTR ReportState savedReport = {};
RTC_DATA_ATTR int64_t nextRoundUs = 0;

int64_t nowUs() {
    struct timeval tv;
    gettimeofday(&tv, nullptr);
    return int64_t(tv.tv_sec) * 1000000LL + tv.tv_usec;
}

}  // namespace

void startWatchdog() {
    esp_task_wdt_config_t config = {
        .timeout_ms = 5 * 60 * 1000,
        .idle_core_mask = (1u << portNUM_PROCESSORS) - 1,
        .trigger_panic = true,
    };
    if (esp_task_wdt_reconfigure(&config) != ESP_OK) esp_task_wdt_init(&config);
    esp_task_wdt_add(nullptr);
}

void feedWatchdog() { esp_task_wdt_reset(); }

const char* wakeReason() {
    switch (esp_sleep_get_wakeup_cause()) {
        case ESP_SLEEP_WAKEUP_TIMER: return "TIMER";
        case ESP_SLEEP_WAKEUP_EXT0:
        case ESP_SLEEP_WAKEUP_EXT1:
        case ESP_SLEEP_WAKEUP_GPIO: return "PIN";
        case ESP_SLEEP_WAKEUP_UNDEFINED: return "RESET";
        default: return "OTHER";
    }
}

void keepLevel(int8_t pin) {
    if (pin < 0) return;
    for (uint8_t i = 0; i < keptCount; i++) {
        if (keptPins[i] == pin) return;
    }
    if (keptCount < sizeof(keptPins)) keptPins[keptCount++] = pin;
}

void releaseLevel(int8_t pin) {
    if (pin >= 0) gpio_hold_dis(gpio_num_t(pin));
}

void holdLevels(bool hold) {
    for (uint8_t i = 0; i < keptCount; i++) {
        if (hold) gpio_hold_en(gpio_num_t(keptPins[i]));
        else gpio_hold_dis(gpio_num_t(keptPins[i]));
    }
}

void sensorsOn(const Config& cfg) {
    if (cfg.sensorPowerPin < 0) return;
    // pinMode first: digitalWrite does nothing on a pin it has not set up. A hold from the deep
    // sleep keeps the pad latched meanwhile, so the supply does not dip.
    pinMode(cfg.sensorPowerPin, OUTPUT);
    digitalWrite(cfg.sensorPowerPin, HIGH);
    keepLevel(cfg.sensorPowerPin);
    releaseLevel(cfg.sensorPowerPin);
}

void sensorsOff(const Config& cfg) {
    if (cfg.sensorPowerPin < 0) return;
    pinMode(cfg.sensorPowerPin, OUTPUT);
    digitalWrite(cfg.sensorPowerPin, LOW);
    keepLevel(cfg.sensorPowerPin);
}

uint32_t sleepMs(uint32_t intervalSeconds, uint32_t awakeMs) {
    uint32_t interval = intervalSeconds * 1000;
    return interval > awakeMs + 1000 ? interval - awakeMs : 1000;
}

uint32_t activeInputs(const WakeInput* inputs, uint8_t count, bool afterDeepSleep) {
    uint32_t active = 0;
    esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
    if (!afterDeepSleep) {
        if (cause != ESP_SLEEP_WAKEUP_GPIO) return 0;  // the timer ended the light sleep
        for (uint8_t i = 0; i < count; i++) {
            if (digitalRead(inputs[i].pin) == (inputs[i].level ? HIGH : LOW)) active |= 1u << i;
        }
        return active;
    }
#if SOC_PM_SUPPORT_EXT0_WAKEUP
    int8_t ext0 = ext0Index(inputs, count);
    if (cause == ESP_SLEEP_WAKEUP_EXT0 && ext0 >= 0) active |= 1u << ext0;
#endif
    uint64_t pins = 0;
#if SOC_PM_SUPPORT_EXT1_WAKEUP
    if (cause == ESP_SLEEP_WAKEUP_EXT1) pins |= esp_sleep_get_ext1_wakeup_status();
#endif
#if SOC_GPIO_SUPPORT_DEEPSLEEP_WAKEUP
    if (cause == ESP_SLEEP_WAKEUP_GPIO) pins |= esp_sleep_get_gpio_wakeup_status();
#endif
    for (uint8_t i = 0; i < count; i++) {
        if ((pins >> inputs[i].pin) & 1ULL) active |= 1u << i;
    }
    return active;
}

void lightSleep(const Config& cfg, uint32_t ms, const WakeInput* inputs, uint8_t count, bool quiet) {
    (void)cfg;
    if (!quiet) status::line("SLEEP LIGHT %lu.%02lu", (unsigned long)(ms / 1000), (unsigned long)(ms % 1000 / 10));
    status::flush();
    esp_sleep_enable_timer_wakeup(uint64_t(ms) * 1000ULL);
    armLight(inputs, count);
    holdLevels(true);
    esp_light_sleep_start();
    holdLevels(false);
    disarmLight(inputs, count);
    esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
}

void deepSleep(const Config& cfg, uint32_t ms, const WakeInput* inputs, uint8_t count) {
    bool hibernate = cfg.mode == SleepMode::Hibernate;
    status::line("SLEEP %s %lu.%02lu", hibernate ? "HIBERNATE" : "DEEP", (unsigned long)(ms / 1000),
                 (unsigned long)(ms % 1000 / 10));
    status::flush();
    setNextRound(ms);
    esp_sleep_enable_timer_wakeup(uint64_t(ms) * 1000ULL);
    if (hibernate) {
        // Power down what the timer does not need. Held pins (sensor supply, fan SET) need the RTC
        // peripherals on the ESP32/S2/S3; they stay on then, everything else goes off.
#if SOC_PM_SUPPORT_RTC_PERIPH_PD
        if (keptCount == 0) esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_PERIPH, ESP_PD_OPTION_OFF);
#endif
#if SOC_PM_SUPPORT_RTC_SLOW_MEM_PD
        esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_SLOW_MEM, ESP_PD_OPTION_OFF);
#endif
#if SOC_PM_SUPPORT_RTC_FAST_MEM_PD
        esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_FAST_MEM, ESP_PD_OPTION_OFF);
#endif
    } else {
        armDeep(inputs, count);
    }
    holdLevels(true);
#if !SOC_GPIO_SUPPORT_HOLD_SINGLE_IO_IN_DSLP
    // ESP32, S2, S3, C3: digital pads keep a held level through deep sleep only with this switch.
    if (keptCount) gpio_deep_sleep_hold_en();
#endif
    esp_deep_sleep_start();
}

void setNextRound(uint32_t inMs) { nextRoundUs = nowUs() + int64_t(inMs) * 1000; }

int64_t untilNextRoundMs() { return nextRoundUs ? (nextRoundUs - nowUs()) / 1000 : 0; }

Rounds loadRounds(bool fresh) {
    if (fresh || roundMagic != kRoundMagic) return {0, 0};
    return savedRounds;
}

void saveRounds(const Rounds& rounds) {
    roundMagic = kRoundMagic;
    savedRounds = rounds;
}

ReportState& report() { return savedReport; }

void loadReport(bool fresh) {
    if (fresh || reportMagic != kReportMagic) {
        savedReport = {};
        reportMagic = kReportMagic;
    }
}

void saveReport() { reportMagic = kReportMagic; }

void resumeLongSleep(const Config&) {}

}  // namespace hn::power

#endif  // !ESP8266
