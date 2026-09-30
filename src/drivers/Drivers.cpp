// All sensor drivers. Each one is a thin adapter from a library (see
// catalog/libraries.json for versions and licenses) to hn::Driver.

#include <Adafruit_AHTX0.h>
#include <Adafruit_BME280.h>
#include <Adafruit_BMP280.h>
#include <Adafruit_HTU21DF.h>
#include <Adafruit_SHT31.h>
#include <Arduino.h>
#include <BH1750.h>
#include <DHT.h>
#include <DallasTemperature.h>
#include <OneWire.h>
#include <SensirionI2cSht4x.h>
#include <math.h>
#include <string.h>

#include "Driver.h"
#include "status/Status.h"

namespace hn {

void Driver::fill(Reading* out, const char* q, float value, bool ok) const {
    for (uint8_t i = 0; i < cfg_.channelCount; i++) {
        if (strcmp(cfg_.channels[i].q, q) == 0) {
            out[i] = {cfg_.channels[i].type, value, ok && isfinite(value)};
        }
    }
}

namespace {

// --- 1-Wire -------------------------------------------------------------------

bool parseRom(const char* hex, DeviceAddress out) {
    for (int i = 0; i < 8; i++) {
        char byte[3] = {hex[2 * i], hex[2 * i + 1], 0};
        out[i] = uint8_t(strtoul(byte, nullptr, 16));
    }
    return true;
}

class Ds18b20 : public Driver {
public:
    explicit Ds18b20(const DeviceConfig& cfg) : Driver(cfg), wire_(cfg.pin), sensors_(&wire_) {}

    bool begin() override {
        sensors_.begin();
        sensors_.setWaitForConversion(true);
        uint8_t count = sensors_.getDeviceCount();
        // Print every ROM address so the web app can map probes to types.
        DeviceAddress addr;
        for (uint8_t i = 0; i < count; i++) {
            if (sensors_.getAddress(addr, i)) {
                status::line("OW pin=%d idx=%u addr=%02X%02X%02X%02X%02X%02X%02X%02X", cfg_.pin, i, addr[0], addr[1],
                             addr[2], addr[3], addr[4], addr[5], addr[6], addr[7]);
            }
        }
        return count > 0;
    }

    void read(Reading* out) override {
        sensors_.requestTemperatures();
        for (uint8_t i = 0; i < cfg_.channelCount; i++) {
            const ChannelConfig& ch = cfg_.channels[i];
            float t = DEVICE_DISCONNECTED_C;
            if (ch.addr[0]) {
                DeviceAddress addr;
                parseRom(ch.addr, addr);
                t = sensors_.getTempC(addr);
            } else {
                t = sensors_.getTempCByIndex(ch.index < 0 ? i : ch.index);
            }
            out[i] = {ch.type, t, t != DEVICE_DISCONNECTED_C && t > -55.5f && t < 125.5f};
        }
    }

    uint32_t warmupMs() const override { return 750; }

private:
    OneWire wire_;
    DallasTemperature sensors_;
};

// --- DHT ------------------------------------------------------------------------

class Dht : public Driver {
public:
    explicit Dht(const DeviceConfig& cfg) : Driver(cfg), dht_(cfg.pin, model(cfg)) {}

    bool begin() override {
        dht_.begin();
        return true;
    }

    void read(Reading* out) override {
        float t = dht_.readTemperature(false, true);
        float rh = dht_.readHumidity();
        fill(out, "t", t, !isnan(t));
        fill(out, "rh", rh, !isnan(rh));
    }

    uint32_t warmupMs() const override { return 2000; }

private:
    static uint8_t model(const DeviceConfig& cfg) {
        const OptionValue* o = cfg.option("model");
        return (o && strcmp(o->text, "DHT11") == 0) ? DHT11 : DHT22;
    }

    DHT dht_;
};

// --- I2C sensors -----------------------------------------------------------------

class Sht4x : public Driver {
public:
    Sht4x(const DeviceConfig& cfg, TwoWire* bus) : Driver(cfg), bus_(bus) {}

    bool begin() override {
        sht_.begin(*bus_, cfg_.address);
        sht_.softReset();
        delay(10);
        uint32_t serial = 0;
        return sht_.serialNumber(serial) == 0;
    }

    void read(Reading* out) override {
        float t = NAN, rh = NAN;
        bool ok = sht_.measureHighPrecision(t, rh) == 0;
        fill(out, "t", t, ok);
        fill(out, "rh", rh, ok);
    }

    uint32_t warmupMs() const override { return 10; }

private:
    TwoWire* bus_;
    SensirionI2cSht4x sht_;
};

class Sht3x : public Driver {
public:
    Sht3x(const DeviceConfig& cfg, TwoWire* bus) : Driver(cfg), sht_(bus) {}

    bool begin() override { return sht_.begin(cfg_.address); }

    void read(Reading* out) override {
        float t = NAN, rh = NAN;
        bool ok = sht_.readBoth(&t, &rh);
        fill(out, "t", t, ok);
        fill(out, "rh", rh, ok);
    }

    uint32_t warmupMs() const override { return 10; }

private:
    Adafruit_SHT31 sht_;
};

class Aht : public Driver {
public:
    Aht(const DeviceConfig& cfg, TwoWire* bus) : Driver(cfg), bus_(bus) {}

    bool begin() override { return aht_.begin(bus_, 0, cfg_.address); }

    void read(Reading* out) override {
        sensors_event_t humidity, temp;
        bool ok = aht_.getEvent(&humidity, &temp);
        fill(out, "t", temp.temperature, ok);
        fill(out, "rh", humidity.relative_humidity, ok);
    }

    uint32_t warmupMs() const override { return 40; }

private:
    TwoWire* bus_;
    Adafruit_AHTX0 aht_;
};

class Htu21d : public Driver {
public:
    Htu21d(const DeviceConfig& cfg, TwoWire* bus) : Driver(cfg), bus_(bus) {}

    bool begin() override { return htu_.begin(bus_); }

    void read(Reading* out) override {
        float t = htu_.readTemperature();
        float rh = htu_.readHumidity();
        fill(out, "t", t, !isnan(t));
        fill(out, "rh", rh, !isnan(rh));
    }

    uint32_t warmupMs() const override { return 15; }

private:
    TwoWire* bus_;
    Adafruit_HTU21DF htu_;
};

class Bme280 : public Driver {
public:
    Bme280(const DeviceConfig& cfg, TwoWire* bus) : Driver(cfg), bus_(bus) {}

    bool begin() override { return ok_ = bme_.begin(cfg_.address, bus_); }

    void read(Reading* out) override {
        fill(out, "t", bme_.readTemperature(), ok_);
        fill(out, "rh", bme_.readHumidity(), ok_);
        fill(out, "p", bme_.readPressure() / 100.0f, ok_);
    }

    uint32_t warmupMs() const override { return 10; }

private:
    TwoWire* bus_;
    Adafruit_BME280 bme_;
    bool ok_ = false;
};

class Bmp280 : public Driver {
public:
    Bmp280(const DeviceConfig& cfg, TwoWire* bus) : Driver(cfg), bmp_(bus) {}

    bool begin() override { return ok_ = bmp_.begin(cfg_.address); }

    void read(Reading* out) override {
        fill(out, "t", bmp_.readTemperature(), ok_);
        fill(out, "p", bmp_.readPressure() / 100.0f, ok_);
    }

    uint32_t warmupMs() const override { return 10; }

private:
    Adafruit_BMP280 bmp_;
    bool ok_ = false;
};

class Bh1750 : public Driver {
public:
    Bh1750(const DeviceConfig& cfg, TwoWire* bus) : Driver(cfg), bus_(bus), meter_(cfg.address) {}

    bool begin() override { return meter_.begin(BH1750::CONTINUOUS_HIGH_RES_MODE, cfg_.address, bus_); }

    void read(Reading* out) override {
        float lux = meter_.readLightLevel();
        fill(out, "lux", lux, lux >= 0);
    }

    uint32_t warmupMs() const override { return 180; }

private:
    TwoWire* bus_;
    BH1750 meter_;
};

// --- Analog ------------------------------------------------------------------------

float readMillivolts(int8_t pin) {
    uint32_t sum = 0;
    for (int i = 0; i < 8; i++) sum += analogReadMilliVolts(pin);
    return sum / 8.0f;
}

class Analog : public Driver {
public:
    explicit Analog(const DeviceConfig& cfg) : Driver(cfg) {}

    bool begin() override {
        analogSetPinAttenuation(cfg_.pin, ADC_11db);
        return true;
    }

    void read(Reading* out) override {
        float mv = readMillivolts(cfg_.pin);
        if (strcmp(cfg_.driver, "soil") == 0) {
            float dry = cfg_.number("dry", 2600), wet = cfg_.number("wet", 1100);
            float pct = dry == wet ? NAN : (dry - mv) / (dry - wet) * 100.0f;
            fill(out, "m", constrain(pct, 0.0f, 100.0f), !isnan(pct));
        } else if (strcmp(cfg_.driver, "battery") == 0) {
            fill(out, "v", mv * cfg_.number("divider", 2) / 1000.0f, true);
        } else {
            fill(out, "x", mv * cfg_.number("scale", 1) + cfg_.number("offset", 0), true);
        }
    }

    uint32_t warmupMs() const override { return 100; }
};

}  // namespace

Driver* createDriver(const DeviceConfig& cfg, TwoWire* buses[kMaxI2cBuses]) {
    const char* id = cfg.driver;
    if (strcmp(id, "ds18b20") == 0) return new Ds18b20(cfg);
    if (strcmp(id, "dht") == 0) return new Dht(cfg);
    if (strcmp(id, "soil") == 0 || strcmp(id, "analog") == 0 || strcmp(id, "battery") == 0) return new Analog(cfg);

    if (cfg.bus < 0 || !buses[cfg.bus]) return nullptr;
    TwoWire* bus = buses[cfg.bus];
    if (strcmp(id, "sht4x") == 0) return new Sht4x(cfg, bus);
    if (strcmp(id, "sht3x") == 0) return new Sht3x(cfg, bus);
    if (strcmp(id, "aht") == 0) return new Aht(cfg, bus);
    if (strcmp(id, "htu21d") == 0) return new Htu21d(cfg, bus);
    if (strcmp(id, "bme280") == 0) return new Bme280(cfg, bus);
    if (strcmp(id, "bmp280") == 0) return new Bmp280(cfg, bus);
    if (strcmp(id, "bh1750") == 0) return new Bh1750(cfg, bus);
    return nullptr;
}

}  // namespace hn
