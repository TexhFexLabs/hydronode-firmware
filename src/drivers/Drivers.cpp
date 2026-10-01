// All sensor drivers. Each one is a thin adapter from a library (see
// catalog/libraries.json for versions and licenses) to hn::Driver.

#include <Adafruit_AHTX0.h>
#include <Adafruit_BME280.h>
#include <Adafruit_BME680.h>
#include <Adafruit_BMP280.h>
#include <Adafruit_HTU21DF.h>
#include <Adafruit_INA219.h>
#include <Adafruit_SGP30.h>
#include <Adafruit_SGP40.h>
#include <Adafruit_SHT31.h>
#include <Adafruit_VEML7700.h>
#include <Arduino.h>
#include <BH1750.h>
#include <DHT.h>
#include <DallasTemperature.h>
#include <NOxGasIndexAlgorithm.h>
#include <OneWire.h>
#include <SensirionI2CSgp41.h>
#include <SensirionI2cScd4x.h>
#include <SensirionI2cSht4x.h>
#include <VOCGasIndexAlgorithm.h>
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

Ambient& ambient() {
    static Ambient value{NAN, NAN};
    return value;
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

    // requestTemperatures() waits for the 750 ms conversion itself.
    uint32_t warmupMs() const override { return 10; }

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

    // Datasheets: no command within 1 s after power-up.
    uint32_t warmupMs() const override { return 1200; }

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

    // Bosch's "weather monitoring" setting: forced mode, 1x oversampling, no filter. The sensor
    // sleeps between readings, which keeps self-heating and current down.
    bool begin() override {
        ok_ = bme_.begin(cfg_.address, bus_);
        if (ok_) {
            bme_.setSampling(Adafruit_BME280::MODE_FORCED, Adafruit_BME280::SAMPLING_X1, Adafruit_BME280::SAMPLING_X1,
                             Adafruit_BME280::SAMPLING_X1, Adafruit_BME280::FILTER_OFF);
        }
        return ok_;
    }

    void read(Reading* out) override {
        bool ok = ok_ && bme_.takeForcedMeasurement();
        fill(out, "t", bme_.readTemperature(), ok);
        fill(out, "rh", bme_.readHumidity(), ok);
        fill(out, "p", bme_.readPressure() / 100.0f, ok);
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

    bool begin() override {
        ok_ = bmp_.begin(cfg_.address);
        if (ok_) {
            bmp_.setSampling(Adafruit_BMP280::MODE_FORCED, Adafruit_BMP280::SAMPLING_X1, Adafruit_BMP280::SAMPLING_X1,
                             Adafruit_BMP280::FILTER_OFF);
        }
        return ok_;
    }

    void read(Reading* out) override {
        bool ok = ok_ && bmp_.takeForcedMeasurement();
        fill(out, "t", bmp_.readTemperature(), ok);
        fill(out, "p", bmp_.readPressure() / 100.0f, ok);
    }

    uint32_t warmupMs() const override { return 10; }

private:
    Adafruit_BMP280 bmp_;
    bool ok_ = false;
};

class Bh1750 : public Driver {
public:
    Bh1750(const DeviceConfig& cfg, TwoWire* bus) : Driver(cfg), bus_(bus), meter_(cfg.address) {}

    // One-time mode: the sensor measures once and powers down, instead of drawing 120 µA nonstop.
    bool begin() override { return meter_.begin(BH1750::ONE_TIME_HIGH_RES_MODE, cfg_.address, bus_); }

    void read(Reading* out) override {
        bool ok = meter_.configure(BH1750::ONE_TIME_HIGH_RES_MODE);
        for (uint32_t start = millis(); ok && !meter_.measurementReady() && millis() - start < 300;) delay(10);
        float lux = ok ? meter_.readLightLevel() : -1;
        fill(out, "lux", lux, lux >= 0);
    }

private:
    TwoWire* bus_;
    BH1750 meter_;
};

class Bme680 : public Driver {
public:
    Bme680(const DeviceConfig& cfg, TwoWire* bus) : Driver(cfg), bme_(bus) {}

    bool begin() override {
        ok_ = bme_.begin(cfg_.address);
        if (ok_) {
            bme_.setTemperatureOversampling(BME680_OS_2X);
            bme_.setHumidityOversampling(BME680_OS_1X);
            bme_.setPressureOversampling(BME680_OS_4X);
            bme_.setIIRFilterSize(BME680_FILTER_SIZE_0);
            bme_.setGasHeater(320, 150);  // 320 °C for 150 ms, Bosch's default profile
        }
        return ok_;
    }

    void read(Reading* out) override {
        bool ok = ok_ && bme_.performReading();
        fill(out, "t", bme_.temperature, ok);
        fill(out, "rh", bme_.humidity, ok);
        fill(out, "p", bme_.pressure / 100.0f, ok);
        fill(out, "gas", bme_.gas_resistance / 1000.0f, ok && bme_.gas_resistance > 0);
    }

    uint32_t warmupMs() const override { return 10; }

private:
    Adafruit_BME680 bme_;
    bool ok_ = false;
};

// SHT-style ticks for Sensirion gas sensors: RH 0..100 % and T -45..130 °C over 0..65535.
uint16_t rhTicks(float rh) { return isnan(rh) ? 0x8000 : uint16_t(constrain(rh, 0.0f, 100.0f) * 65535.0f / 100.0f); }
uint16_t tTicks(float t) { return isnan(t) ? 0x6666 : uint16_t((constrain(t, -45.0f, 130.0f) + 45.0f) * 65535.0f / 175.0f); }

class Sgp30 : public Driver {
public:
    Sgp30(const DeviceConfig& cfg, TwoWire* bus) : Driver(cfg), bus_(bus) {}

    bool begin() override {
        ok_ = sgp_.begin(bus_) && sgp_.IAQinit();
        started_ = millis();
        return ok_;
    }

    bool continuous() const override { return true; }

    void tick() override {
        if (!ok_) return;
        const Ambient& a = ambient();
        if (!isnan(a.t) && !isnan(a.rh)) {
            // Absolute humidity in mg/m³ (Sensirion application note).
            float absolute = 216.7f * (a.rh / 100.0f * 6.112f * expf(17.62f * a.t / (243.12f + a.t)) / (273.15f + a.t));
            sgp_.setHumidity(uint32_t(absolute * 1000.0f));
        }
        measured_ = sgp_.IAQmeasure();
    }

    void read(Reading* out) override {
        // The first 15 s after IAQinit() report the fixed start values 400 ppm / 0 ppb.
        bool ok = ok_ && measured_ && millis() - started_ > 15000;
        fill(out, "eco2", sgp_.eCO2, ok);
        fill(out, "tvoc", sgp_.TVOC, ok);
    }

private:
    TwoWire* bus_;
    Adafruit_SGP30 sgp_;
    uint32_t started_ = 0;
    bool ok_ = false;
    bool measured_ = false;
};

class Sgp40 : public Driver {
public:
    Sgp40(const DeviceConfig& cfg, TwoWire* bus) : Driver(cfg), bus_(bus) {}

    bool begin() override { return ok_ = sgp_.begin(bus_); }

    bool continuous() const override { return true; }

    void tick() override {
        if (!ok_) return;
        const Ambient& a = ambient();
        voc_ = sgp_.measureVocIndex(isnan(a.t) ? 25.0f : a.t, isnan(a.rh) ? 50.0f : a.rh);
    }

    // The VOC index is 0 while the algorithm learns the room (about the first 45 s).
    void read(Reading* out) override { fill(out, "voc", voc_, ok_ && voc_ > 0); }

private:
    TwoWire* bus_;
    Adafruit_SGP40 sgp_;
    int32_t voc_ = 0;
    bool ok_ = false;
};

class Sgp41 : public Driver {
public:
    Sgp41(const DeviceConfig& cfg, TwoWire* bus) : Driver(cfg), bus_(bus) {}

    bool begin() override {
        sgp_.begin(*bus_);
        uint16_t serial[3];
        ok_ = sgp_.getSerialNumber(serial) == 0;
        conditioning_ = 10;  // seconds of NOx conditioning after power-up, per datasheet
        return ok_;
    }

    bool continuous() const override { return true; }

    void tick() override {
        if (!ok_) return;
        const Ambient& a = ambient();
        uint16_t srawVoc = 0, srawNox = 0;
        if (conditioning_ > 0) {
            conditioning_--;
            sgp_.executeConditioning(rhTicks(a.rh), tTicks(a.t), srawVoc);
            return;
        }
        if (sgp_.measureRawSignals(rhTicks(a.rh), tTicks(a.t), srawVoc, srawNox) == 0) {
            voc_ = vocAlgorithm_.process(srawVoc);
            nox_ = noxAlgorithm_.process(srawNox);
        }
    }

    // Both indices stay 0 while their algorithm learns (VOC ~45 s, NOx ~5 min).
    void read(Reading* out) override {
        fill(out, "voc", voc_, ok_ && voc_ > 0);
        fill(out, "nox", nox_, ok_ && nox_ > 0);
    }

private:
    TwoWire* bus_;
    SensirionI2CSgp41 sgp_;
    VOCGasIndexAlgorithm vocAlgorithm_;
    NOxGasIndexAlgorithm noxAlgorithm_;
    int32_t voc_ = 0;
    int32_t nox_ = 0;
    uint8_t conditioning_ = 0;
    bool ok_ = false;
};

class Scd4x : public Driver {
public:
    // Deep sleep restarts the board for every reading: the SCD41 then measures once on request
    // (single shot). Otherwise the sensor measures every 5 s on its own and keeps calibrating.
    Scd4x(const DeviceConfig& cfg, TwoWire* bus, bool singleShot) : Driver(cfg), bus_(bus), singleShot_(singleShot) {}

    bool begin() override {
        scd_.begin(*bus_, cfg_.address);
        scd_.wakeUp();
        ok_ = scd_.stopPeriodicMeasurement() == 0;  // in case it still runs from before a reset
        delay(500);
        if (ok_ && !singleShot_) ok_ = scd_.startPeriodicMeasurement() == 0;
        return ok_;
    }

    void read(Reading* out) override {
        uint16_t co2 = 0;
        float t = NAN, rh = NAN;
        bool ok = false;
        if (ok_ && singleShot_) {
            ok = scd_.measureAndReadSingleShot(co2, t, rh) == 0;
        } else if (ok_) {
            bool ready = false;
            for (uint32_t start = millis(); millis() - start < 6000; delay(100)) {
                if (scd_.getDataReadyStatus(ready) == 0 && ready) break;
            }
            ok = ready && scd_.readMeasurement(co2, t, rh) == 0;
        }
        ok = ok && co2 > 0;
        fill(out, "co2", co2, ok);
        fill(out, "t", t, ok);
        fill(out, "rh", rh, ok);
    }

    // The first periodic result arrives 5 s after the start.
    uint32_t warmupMs() const override { return singleShot_ ? 0 : 5000; }

private:
    TwoWire* bus_;
    SensirionI2cScd4x scd_;
    bool singleShot_;
    bool ok_ = false;
};

class Veml7700 : public Driver {
public:
    Veml7700(const DeviceConfig& cfg, TwoWire* bus) : Driver(cfg), bus_(bus) {}

    bool begin() override { return ok_ = veml_.begin(bus_); }

    void read(Reading* out) override {
        // Picks gain and integration time for the current light, from moonlight to sunlight.
        float lux = ok_ ? veml_.readLux(VEML_LUX_AUTO) : -1;
        fill(out, "lux", lux, lux >= 0);
    }

    uint32_t warmupMs() const override { return 5; }

private:
    TwoWire* bus_;
    Adafruit_VEML7700 veml_;
    bool ok_ = false;
};

class Ina219 : public Driver {
public:
    Ina219(const DeviceConfig& cfg, TwoWire* bus) : Driver(cfg), bus_(bus), ina_(cfg.address) {}

    bool begin() override {
        ok_ = ina_.begin(bus_);
        const OptionValue* range = cfg_.option("range");
        if (ok_ && range && strcmp(range->text, "32V_1A") == 0) ina_.setCalibration_32V_1A();
        else if (ok_ && range && strcmp(range->text, "16V_400mA") == 0) ina_.setCalibration_16V_400mA();
        return ok_;
    }

    void read(Reading* out) override {
        // Load voltage: bus voltage plus the drop over the shunt.
        float volts = ina_.getBusVoltage_V() + ina_.getShuntVoltage_mV() / 1000.0f;
        fill(out, "v", volts, ok_);
        fill(out, "i", ina_.getCurrent_mA() / 1000.0f, ok_);
        fill(out, "w", ina_.getPower_mW() / 1000.0f, ok_);
    }

    uint32_t warmupMs() const override { return 5; }

private:
    TwoWire* bus_;
    Adafruit_INA219 ina_;
    bool ok_ = false;
};

// --- Analog ------------------------------------------------------------------------

float readMillivolts(int8_t pin, uint16_t rangeMv) {
    uint32_t sum = 0;
#if defined(ESP8266)
    // A0 reads 0..1023 over the board's divider (NodeMCU and D1 mini: 0..3.2 V).
    for (int i = 0; i < 8; i++) sum += analogRead(A0);
    return sum / 8.0f * rangeMv / 1023.0f;
#else
    (void)rangeMv;
    for (int i = 0; i < 8; i++) sum += analogReadMilliVolts(pin);
    return sum / 8.0f;
#endif
}

class Analog : public Driver {
public:
    Analog(const DeviceConfig& cfg, uint16_t rangeMv) : Driver(cfg), rangeMv_(rangeMv) {}

    bool begin() override {
#if !defined(ESP8266)
        analogSetPinAttenuation(cfg_.pin, ADC_11db);
#endif
        return true;
    }

    void read(Reading* out) override {
        float mv = readMillivolts(cfg_.pin, rangeMv_);
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

private:
    uint16_t rangeMv_;
};

}  // namespace

Driver* createDriver(const DeviceConfig& cfg, TwoWire* buses[kMaxI2cBuses], uint16_t adcRangeMv,
                     bool restartsEachCycle) {
    const char* id = cfg.driver;
    if (strcmp(id, "ds18b20") == 0) return new Ds18b20(cfg);
    if (strcmp(id, "dht") == 0) return new Dht(cfg);
    if (strcmp(id, "soil") == 0 || strcmp(id, "analog") == 0 || strcmp(id, "battery") == 0) return new Analog(cfg, adcRangeMv);

    if (cfg.bus < 0 || !buses[cfg.bus]) return nullptr;
    TwoWire* bus = buses[cfg.bus];
    if (strcmp(id, "sht4x") == 0) return new Sht4x(cfg, bus);
    if (strcmp(id, "sht3x") == 0) return new Sht3x(cfg, bus);
    if (strcmp(id, "aht") == 0) return new Aht(cfg, bus);
    if (strcmp(id, "htu21d") == 0) return new Htu21d(cfg, bus);
    if (strcmp(id, "bme280") == 0) return new Bme280(cfg, bus);
    if (strcmp(id, "bmp280") == 0) return new Bmp280(cfg, bus);
    if (strcmp(id, "bh1750") == 0) return new Bh1750(cfg, bus);
    if (strcmp(id, "bme680") == 0) return new Bme680(cfg, bus);
    if (strcmp(id, "sgp30") == 0) return new Sgp30(cfg, bus);
    if (strcmp(id, "sgp40") == 0) return new Sgp40(cfg, bus);
    if (strcmp(id, "sgp41") == 0) return new Sgp41(cfg, bus);
    if (strcmp(id, "scd4x") == 0) return new Scd4x(cfg, bus, restartsEachCycle);
    if (strcmp(id, "veml7700") == 0) return new Veml7700(cfg, bus);
    if (strcmp(id, "ina219") == 0) return new Ina219(cfg, bus);
    return nullptr;
}

}  // namespace hn
