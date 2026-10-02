#include "Power.h"

#if !defined(ESP8266)

#include <Arduino.h>
#include <driver/gpio.h>
#include <esp_sleep.h>
#include <soc/soc_caps.h>

#if SOC_RTCIO_INPUT_OUTPUT_SUPPORTED
#include <driver/rtc_io.h>
#endif

#include "status/Status.h"

namespace hn::power {

namespace {

void armWakePin(const Config& cfg, bool deep) {
    if (cfg.wakePin < 0) return;
    gpio_num_t pin = gpio_num_t(cfg.wakePin);
    bool high = cfg.wakeLevel == 1;

    if (!deep) {
        pinMode(cfg.wakePin, high ? INPUT_PULLDOWN : INPUT_PULLUP);
        gpio_wakeup_enable(pin, high ? GPIO_INTR_HIGH_LEVEL : GPIO_INTR_LOW_LEVEL);
        esp_sleep_enable_gpio_wakeup();
        return;
    }

#if SOC_PM_SUPPORT_EXT0_WAKEUP
    // ESP32, S2, S3: RTC GPIO with internal pull that stays active in deep sleep.
    esp_sleep_enable_ext0_wakeup(pin, high ? 1 : 0);
    if (high) {
        rtc_gpio_pullup_dis(pin);
        rtc_gpio_pulldown_en(pin);
    } else {
        rtc_gpio_pulldown_dis(pin);
        rtc_gpio_pullup_en(pin);
    }
#elif SOC_GPIO_SUPPORT_DEEPSLEEP_WAKEUP
    // C3, C6: digital GPIO wake on the low-power pins. An external pull
    // resistor is recommended, the internal one is weak.
    esp_deep_sleep_enable_gpio_wakeup(1ULL << cfg.wakePin,
                                      high ? ESP_GPIO_WAKEUP_GPIO_HIGH : ESP_GPIO_WAKEUP_GPIO_LOW);
    if (high) {
        gpio_pullup_dis(pin);
        gpio_pulldown_en(pin);
    } else {
        gpio_pulldown_dis(pin);
        gpio_pullup_en(pin);
    }
#else
    status::line("WARN WAKEPIN unsupported");
#endif
}

}  // namespace

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

void sensorsOn(const Config& cfg) {
    if (cfg.sensorPowerPin < 0) return;
    gpio_hold_dis(gpio_num_t(cfg.sensorPowerPin));
    pinMode(cfg.sensorPowerPin, OUTPUT);
    digitalWrite(cfg.sensorPowerPin, HIGH);
}

void sensorsOff(const Config& cfg) {
    if (cfg.sensorPowerPin < 0) return;
    digitalWrite(cfg.sensorPowerPin, LOW);
    gpio_hold_en(gpio_num_t(cfg.sensorPowerPin));
}

uint32_t sleepMs(uint32_t intervalSeconds, uint32_t awakeMs) {
    uint32_t interval = intervalSeconds * 1000;
    return interval > awakeMs + 1000 ? interval - awakeMs : 1000;
}

namespace {
constexpr uint32_t kRoundMagic = 0x484E5244;  // "HNRD"
RTC_DATA_ATTR uint32_t roundMagic = 0;
RTC_DATA_ATTR Rounds savedRounds = {0, 0};
}  // namespace

Rounds loadRounds(bool fresh) {
    if (fresh || roundMagic != kRoundMagic) return {0, 0};
    return savedRounds;
}

void saveRounds(const Rounds& rounds) {
    roundMagic = kRoundMagic;
    savedRounds = rounds;
}

void lightSleep(const Config& cfg, uint32_t ms) {
    status::line("SLEEP LIGHT %lu.%02lu", (unsigned long)(ms / 1000), (unsigned long)(ms % 1000 / 10));
    status::flush();
    esp_sleep_enable_timer_wakeup(uint64_t(ms) * 1000ULL);
    armWakePin(cfg, false);
    esp_light_sleep_start();
    esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
}

void deepSleep(const Config& cfg, uint32_t ms) {
    bool hibernate = cfg.mode == SleepMode::Hibernate;
    status::line("SLEEP %s %lu.%02lu", hibernate ? "HIBERNATE" : "DEEP", (unsigned long)(ms / 1000),
                 (unsigned long)(ms % 1000 / 10));
    status::flush();
    esp_sleep_enable_timer_wakeup(uint64_t(ms) * 1000ULL);
    if (hibernate) {
        // Power down everything that is not needed for the timer.
#if SOC_PM_SUPPORT_RTC_PERIPH_PD
        esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_PERIPH, ESP_PD_OPTION_OFF);
#endif
#if SOC_PM_SUPPORT_RTC_SLOW_MEM_PD
        esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_SLOW_MEM, ESP_PD_OPTION_OFF);
#endif
#if SOC_PM_SUPPORT_RTC_FAST_MEM_PD
        esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_FAST_MEM, ESP_PD_OPTION_OFF);
#endif
    } else {
        armWakePin(cfg, true);
    }
#if CONFIG_IDF_TARGET_ESP32
    // Classic ESP32 only keeps held pads through deep sleep with this switch.
    if (cfg.sensorPowerPin >= 0) gpio_deep_sleep_hold_en();
#endif
    esp_deep_sleep_start();
}

void resumeLongSleep(const Config&) {}

}  // namespace hn::power

#endif  // !ESP8266
