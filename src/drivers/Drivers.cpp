// All sensor drivers. Each one is a thin adapter from a library (see
// catalog/libraries.json for versions and licenses) to hn::Driver.

#include <Adafruit_ADS1X15.h>
#include <Adafruit_AHTX0.h>
#include <Adafruit_BME280.h>
#include <Adafruit_BME680.h>
#include <Adafruit_BMP280.h>
#include <Adafruit_BMP3XX.h>
#include <Adafruit_DPS310.h>
#include <Adafruit_HTU21DF.h>
#include <Adafruit_INA219.h>
#include <Adafruit_INA260.h>
#include <Adafruit_LPS2X.h>
#include <Adafruit_LTR390.h>
#include <Adafruit_MCP9808.h>
#include <Adafruit_MS8607.h>
#include <Adafruit_PM25AQI.h>
#include <Adafruit_SCD30.h>
#include <Adafruit_SGP30.h>
#include <Adafruit_SGP40.h>
#include <Adafruit_SHT31.h>
#include <Adafruit_SHTC3.h>
#include <Adafruit_TMP117.h>
#include <Adafruit_TSL2591.h>
#include <Adafruit_VEML7700.h>
#include <Arduino.h>
#include <BH1750.h>
#include <DHT.h>
#include <DallasTemperature.h>
#include <INA226.h>
#include <NOxGasIndexAlgorithm.h>
#include <OneWire.h>
#include <SensirionI2CSen5x.h>
#include <SensirionI2CSgp41.h>
#include <SensirionI2cScd4x.h>
#include <SensirionI2cSht4x.h>
#include <VL53L0X.h>
#include <VOCGasIndexAlgorithm.h>
#include <math.h>
#include <string.h>

#if defined(ESP8266)
#include <ESP8266WiFi.h>
#include <SoftwareSerial.h>
#else
#include <WiFi.h>
#endif

#include "Driver.h"
#include "status/Status.h"

namespace hn {

void Driver::fill(Reading* out, const char* q, float value, bool ok, bool warming) const {
    for (uint8_t i = 0; i < cfg_.channelCount; i++) {
        if (strcmp(cfg_.channels[i].q, q) == 0) {
            out[i] = {cfg_.channels[i].type, value, ok && isfinite(value), warming};
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
        measured_ = ok_;  // initialization already confirmed sensor communication
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
        bool warming = ok_ && measured_ && millis() - started_ <= 15000;
        fill(out, "eco2", sgp_.eCO2, ok, warming);
        fill(out, "tvoc", sgp_.TVOC, ok, warming);
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

    bool begin() override {
        started_ = millis();
        return sampled_ = ok_ = sgp_.begin(bus_);
    }

    bool continuous() const override { return true; }

    void tick() override {
        if (!ok_) return;
        const Ambient& a = ambient();
        uint16_t raw = sgp_.measureRaw(isnan(a.t) ? 25.0f : a.t, isnan(a.rh) ? 50.0f : a.rh);
        sampled_ = raw != 0;
        if (sampled_) voc_ = algorithm_.process(raw);
    }

    // The VOC index is 0 while the algorithm learns the room (about the first 45 s).
    void read(Reading* out) override {
        bool healthy = ok_ && sampled_;
        fill(out, "voc", voc_, healthy && voc_ > 0, healthy && voc_ == 0 && millis() - started_ < 60000);
    }

private:
    TwoWire* bus_;
    Adafruit_SGP40 sgp_;
    VOCGasIndexAlgorithm algorithm_;
    uint32_t started_ = 0;
    bool sampled_ = false;
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
        sampled_ = ok_;
        started_ = millis();
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
            sampled_ = sgp_.executeConditioning(rhTicks(a.rh), tTicks(a.t), srawVoc) == 0;
            return;
        }
        sampled_ = sgp_.measureRawSignals(rhTicks(a.rh), tTicks(a.t), srawVoc, srawNox) == 0;
        if (sampled_) {
            voc_ = vocAlgorithm_.process(srawVoc);
            nox_ = noxAlgorithm_.process(srawNox);
        }
    }

    // Both indices stay 0 while their algorithm learns (VOC ~45 s, NOx ~5 min).
    void read(Reading* out) override {
        bool healthy = ok_ && sampled_;
        fill(out, "voc", voc_, healthy && voc_ > 0, healthy && voc_ == 0 && millis() - started_ < 60000);
        fill(out, "nox", nox_, healthy && nox_ > 0, healthy && nox_ == 0 && millis() - started_ < 360000);
    }

private:
    TwoWire* bus_;
    SensirionI2CSgp41 sgp_;
    VOCGasIndexAlgorithm vocAlgorithm_;
    NOxGasIndexAlgorithm noxAlgorithm_;
    int32_t voc_ = 0;
    int32_t nox_ = 0;
    uint8_t conditioning_ = 0;
    uint32_t started_ = 0;
    bool sampled_ = false;
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

class Ms8607 : public Driver {
public:
    Ms8607(const DeviceConfig& cfg, TwoWire* bus) : Driver(cfg), bus_(bus) {}

    bool begin() override { return ok_ = ms_.begin(bus_); }

    void read(Reading* out) override {
        sensors_event_t t, p, rh;
        bool ok = ok_ && ms_.getEvent(&p, &t, &rh);
        fill(out, "t", t.temperature, ok);
        fill(out, "rh", rh.relative_humidity, ok);
        fill(out, "p", p.pressure, ok);
    }

    uint32_t warmupMs() const override { return 15; }

private:
    TwoWire* bus_;
    Adafruit_MS8607 ms_;
    bool ok_ = false;
};

class Bmp3xx : public Driver {
public:
    Bmp3xx(const DeviceConfig& cfg, TwoWire* bus) : Driver(cfg), bus_(bus) {}

    // performReading() runs one forced measurement; 2x pressure, 1x temperature, no filter is
    // Bosch's weather setting.
    bool begin() override {
        ok_ = bmp_.begin_I2C(cfg_.address, bus_);
        if (ok_) {
            bmp_.setTemperatureOversampling(BMP3_NO_OVERSAMPLING);
            bmp_.setPressureOversampling(BMP3_OVERSAMPLING_2X);
            bmp_.setIIRFilterCoeff(BMP3_IIR_FILTER_DISABLE);
        }
        return ok_;
    }

    void read(Reading* out) override {
        bool ok = ok_ && bmp_.performReading();
        fill(out, "t", bmp_.temperature, ok);
        fill(out, "p", bmp_.pressure / 100.0, ok);
    }

    uint32_t warmupMs() const override { return 10; }

private:
    TwoWire* bus_;
    Adafruit_BMP3XX bmp_;
    bool ok_ = false;
};

class Dps310 : public Driver {
public:
    Dps310(const DeviceConfig& cfg, TwoWire* bus) : Driver(cfg), bus_(bus) {}

    bool begin() override {
        ok_ = dps_.begin_I2C(cfg_.address, bus_);
        if (ok_) {
            dps_.configurePressure(DPS310_1HZ, DPS310_8SAMPLES);
            dps_.configureTemperature(DPS310_1HZ, DPS310_2SAMPLES);
            dps_.setMode(DPS310_CONT_PRESTEMP);
        }
        return ok_;
    }

    void read(Reading* out) override {
        bool ok = ok_;
        for (uint32_t start = millis(); ok && !dps_.pressureAvailable(); delay(10)) {
            if (millis() - start > 1500) ok = false;
        }
        sensors_event_t t, p;
        ok = ok && dps_.getEvents(&t, &p);
        fill(out, "t", t.temperature, ok);
        fill(out, "p", p.pressure, ok);
    }

private:
    TwoWire* bus_;
    Adafruit_DPS310 dps_;
    bool ok_ = false;
};

class Lps22 : public Driver {
public:
    Lps22(const DeviceConfig& cfg, TwoWire* bus) : Driver(cfg), bus_(bus) {}

    // One-shot: the sensor measures on request and powers down in between.
    bool begin() override {
        ok_ = lps_.begin_I2C(cfg_.address, bus_);
        if (ok_) lps_.setDataRate(LPS22_RATE_ONE_SHOT);
        return ok_;
    }

    void read(Reading* out) override {
        sensors_event_t p, t;
        bool ok = ok_ && lps_.getEvent(&p, &t);
        fill(out, "t", t.temperature, ok);
        fill(out, "p", p.pressure, ok);
    }

    uint32_t warmupMs() const override { return 10; }

private:
    TwoWire* bus_;
    Adafruit_LPS22 lps_;
    bool ok_ = false;
};

class Shtc3 : public Driver {
public:
    Shtc3(const DeviceConfig& cfg, TwoWire* bus) : Driver(cfg), bus_(bus) {}

    bool begin() override { return ok_ = shtc_.begin(bus_); }

    void read(Reading* out) override {
        sensors_event_t rh, t;
        bool ok = ok_ && shtc_.getEvent(&rh, &t);
        fill(out, "t", t.temperature, ok);
        fill(out, "rh", rh.relative_humidity, ok);
    }

    uint32_t warmupMs() const override { return 10; }

private:
    TwoWire* bus_;
    Adafruit_SHTC3 shtc_;
    bool ok_ = false;
};

class Mcp9808 : public Driver {
public:
    Mcp9808(const DeviceConfig& cfg, TwoWire* bus) : Driver(cfg), bus_(bus) {}

    // 0.0625 °C resolution (250 ms conversion); shut down between readings.
    bool begin() override {
        ok_ = mcp_.begin(cfg_.address, bus_);
        if (ok_) {
            mcp_.setResolution(3);
            mcp_.shutdown();
        }
        return ok_;
    }

    void read(Reading* out) override {
        if (!ok_) {
            fill(out, "t", NAN, false);
            return;
        }
        mcp_.wake();
        delay(260);
        float t = mcp_.readTempC();
        mcp_.shutdown();
        fill(out, "t", t, !isnan(t));
    }

private:
    TwoWire* bus_;
    Adafruit_MCP9808 mcp_;
    bool ok_ = false;
};

class Tmp117 : public Driver {
public:
    Tmp117(const DeviceConfig& cfg, TwoWire* bus) : Driver(cfg), bus_(bus) {}

    bool begin() override { return ok_ = tmp_.begin(cfg_.address, bus_); }

    void read(Reading* out) override {
        sensors_event_t t;
        bool ok = ok_ && tmp_.getEvent(&t);
        fill(out, "t", t.temperature, ok);
    }

    // Continuous mode with 8 averages delivers a value every second.
    uint32_t warmupMs() const override { return 1100; }

private:
    TwoWire* bus_;
    Adafruit_TMP117 tmp_;
    bool ok_ = false;
};

class Tsl2591 : public Driver {
public:
    Tsl2591(const DeviceConfig& cfg, TwoWire* bus) : Driver(cfg), bus_(bus) {}

    bool begin() override {
        ok_ = tsl_.begin(bus_, cfg_.address);
        if (ok_) {
            tsl_.setTiming(TSL2591_INTEGRATIONTIME_100MS);
            tsl_.disable();
        }
        return ok_;
    }

    // Starts at medium gain and steps down while the sensor saturates, from shade to full sun.
    void read(Reading* out) override {
        if (!ok_) {
            fill(out, "lux", NAN, false);
            return;
        }
        const tsl2591Gain_t gains[] = {TSL2591_GAIN_HIGH, TSL2591_GAIN_MED, TSL2591_GAIN_LOW};
        float lux = NAN;
        for (tsl2591Gain_t gain : gains) {
            tsl_.setGain(gain);
            tsl_.enable();
            uint32_t both = tsl_.getFullLuminosity();
            tsl_.disable();
            uint16_t ir = both >> 16, full = both & 0xFFFF;
            if (full < 36000 && ir < 36000) {
                lux = tsl_.calculateLux(full, ir);
                break;
            }
        }
        fill(out, "lux", lux, !isnan(lux) && lux >= 0);
    }

private:
    TwoWire* bus_;
    Adafruit_TSL2591 tsl_{2591};
    bool ok_ = false;
};

class Ltr390 : public Driver {
public:
    Ltr390(const DeviceConfig& cfg, TwoWire* bus) : Driver(cfg), bus_(bus) {}

    bool begin() override {
        ok_ = ltr_.begin(bus_);
        if (ok_) {
            ltr_.setGain(LTR390_GAIN_3);
            ltr_.setResolution(LTR390_RESOLUTION_18BIT);  // 100 ms per conversion
        }
        return ok_;
    }

    void read(Reading* out) override {
        bool want = has("uv"), wantLux = has("lux");
        if (want) {
            uint32_t uvs = 0;
            bool ok = sample(LTR390_MODE_UVS, uvs);
            // Datasheet: 2300 counts per UV index at gain 18 and 20 bit (400 ms); here gain 3,
            // 18 bit (100 ms): 2300 / 6 / 4.
            fill(out, "uv", uvs / (2300.0f / 24.0f), ok);
        }
        if (wantLux) {
            uint32_t als = 0;
            bool ok = sample(LTR390_MODE_ALS, als);
            // Datasheet: lux = 0.6 × ALS / (gain × integration time in units of 100 ms).
            fill(out, "lux", 0.6f * als / 3.0f, ok);
        }
    }

private:
    bool has(const char* q) const {
        for (uint8_t i = 0; i < cfg_.channelCount; i++) {
            if (strcmp(cfg_.channels[i].q, q) == 0) return true;
        }
        return false;
    }

    bool sample(ltr390_mode_t mode, uint32_t& value) {
        if (!ok_) return false;
        ltr_.setMode(mode);
        ltr_.enable(true);
        bool ready = false;
        for (uint32_t start = millis(); millis() - start < 400 && !(ready = ltr_.newDataAvailable());) delay(10);
        if (ready) value = mode == LTR390_MODE_UVS ? ltr_.readUVS() : ltr_.readALS();
        ltr_.enable(false);
        return ready;
    }

    TwoWire* bus_;
    Adafruit_LTR390 ltr_;
    bool ok_ = false;
};

class Scd30 : public Driver {
public:
    Scd30(const DeviceConfig& cfg, TwoWire* bus) : Driver(cfg), bus_(bus) {}

    // Measures every 2 s on its own and keeps calibrating; the board only collects the result.
    bool begin() override {
        ok_ = scd_.begin(cfg_.address, bus_);
        if (ok_) scd_.setMeasurementInterval(2);
        return ok_;
    }

    void read(Reading* out) override {
        bool ok = ok_;
        for (uint32_t start = millis(); ok && !scd_.dataReady(); delay(100)) {
            if (millis() - start > 3000) ok = false;
        }
        ok = ok && scd_.read() && scd_.CO2 > 0;
        fill(out, "co2", scd_.CO2, ok);
        fill(out, "t", scd_.temperature, ok);
        fill(out, "rh", scd_.relative_humidity, ok);
    }

private:
    TwoWire* bus_;
    Adafruit_SCD30 scd_;
    bool ok_ = false;
};

void fillParticles(const Driver& d, Reading* out, const PM25_AQI_Data& data, bool ok);

class Pmsa003i : public Driver {
public:
    Pmsa003i(const DeviceConfig& cfg, TwoWire* bus) : Driver(cfg), bus_(bus) {}

    bool begin() override { return ok_ = aqi_.begin_I2C(bus_); }

    void read(Reading* out) override {
        PM25_AQI_Data data{};
        bool ok = ok_ && aqi_.read(&data);
        fillParticles(*this, out, data, ok);
    }

    // The fan needs about 30 s after power-up before the counts are stable.
    uint32_t warmupMs() const override { return 30000; }


private:
    TwoWire* bus_;
    Adafruit_PM25AQI aqi_;
    bool ok_ = false;
};

class Pms5003 : public Driver {
public:
    explicit Pms5003(const DeviceConfig& cfg) : Driver(cfg) {}

    // Only the sensor's TX is needed: it sends a frame every second on its own.
    bool begin() override {
#if defined(ESP8266)
        serial_.begin(9600, SWSERIAL_8N1, cfg_.pin, -1);
        ok_ = aqi_.begin_UART(&serial_);
#else
        Serial1.begin(9600, SERIAL_8N1, cfg_.pin, -1);
        ok_ = aqi_.begin_UART(&Serial1);
#endif
        return ok_;
    }

    void read(Reading* out) override {
        PM25_AQI_Data data{};
        bool ok = false;
        // A frame takes up to a second; try for two.
        for (uint32_t start = millis(); ok_ && !ok && millis() - start < 2500; delay(50)) ok = aqi_.read(&data);
        fillParticles(*this, out, data, ok);
    }

    uint32_t warmupMs() const override { return 30000; }


private:
#if defined(ESP8266)
    SoftwareSerial serial_;
#endif
    Adafruit_PM25AQI aqi_;
    bool ok_ = false;
};

class Sen5x : public Driver {
public:
    Sen5x(const DeviceConfig& cfg, TwoWire* bus) : Driver(cfg), bus_(bus) {}

    bool begin() override {
        sen_.begin(*bus_);
        ok_ = sen_.startMeasurement() == 0;
        return ok_;
    }

    void read(Reading* out) override {
        bool ready = false;
        for (uint32_t start = millis(); ok_ && !ready && millis() - start < 1500; delay(50)) {
            if (sen_.readDataReady(ready) != 0) break;
        }
        float pm1 = NAN, pm25 = NAN, pm4 = NAN, pm10 = NAN, rh = NAN, t = NAN, voc = NAN, nox = NAN;
        bool ok = ready && sen_.readMeasuredValues(pm1, pm25, pm4, pm10, rh, t, voc, nox) == 0;
        fill(out, "pm1", pm1, ok);
        fill(out, "pm25", pm25, ok);
        fill(out, "pm4", pm4, ok);
        fill(out, "pm10", pm10, ok);
        fill(out, "t", t, ok);
        fill(out, "rh", rh, ok);
        // NaN on the SEN54 (no NOx) and while the indices learn.
        fill(out, "voc", voc, ok);
        fill(out, "nox", nox, ok);
    }

    uint32_t warmupMs() const override { return 1100; }

private:
    TwoWire* bus_;
    SensirionI2CSen5x sen_;
    bool ok_ = false;
};

class Ina226 : public Driver {
public:
    Ina226(const DeviceConfig& cfg, TwoWire* bus) : Driver(cfg), ina_(cfg.address, bus) {}

    bool begin() override {
        ok_ = ina_.begin() && ina_.isConnected();
        if (ok_) ok_ = ina_.setMaxCurrentShunt(cfg_.number("maxA", 0.8f), cfg_.number("shunt", 0.1f)) == INA226_ERR_NONE;
        return ok_;
    }

    void read(Reading* out) override {
        fill(out, "v", ina_.getBusVoltage(), ok_);
        fill(out, "i", ina_.getCurrent(), ok_);
        fill(out, "w", ina_.getPower(), ok_);
    }

    uint32_t warmupMs() const override { return 5; }

private:
    INA226 ina_;
    bool ok_ = false;
};

class Ina260 : public Driver {
public:
    Ina260(const DeviceConfig& cfg, TwoWire* bus) : Driver(cfg), bus_(bus) {}

    bool begin() override { return ok_ = ina_.begin(cfg_.address, bus_); }

    void read(Reading* out) override {
        fill(out, "v", ina_.readBusVoltage() / 1000.0f, ok_);
        fill(out, "i", ina_.readCurrent() / 1000.0f, ok_);
        fill(out, "w", ina_.readPower() / 1000.0f, ok_);
    }

    uint32_t warmupMs() const override { return 5; }

private:
    TwoWire* bus_;
    Adafruit_INA260 ina_;
    bool ok_ = false;
};

class Ads1115 : public Driver {
public:
    Ads1115(const DeviceConfig& cfg, TwoWire* bus) : Driver(cfg), bus_(bus) {}

    // ±4.096 V range: covers 0..3.3 V inputs (never more than the supply on any input).
    bool begin() override {
        ok_ = ads_.begin(cfg_.address, bus_);
        if (ok_) ads_.setGain(GAIN_ONE);
        return ok_;
    }

    void read(Reading* out) override {
        const char* inputs[] = {"a0", "a1", "a2", "a3"};
        for (uint8_t ch = 0; ch < 4; ch++) {
            bool wanted = false;
            for (uint8_t i = 0; i < cfg_.channelCount; i++) wanted = wanted || strcmp(cfg_.channels[i].q, inputs[ch]) == 0;
            if (!wanted) continue;
            float volts = ok_ ? ads_.computeVolts(ads_.readADC_SingleEnded(ch)) : NAN;
            fill(out, inputs[ch], volts, ok_);
        }
    }

    uint32_t warmupMs() const override { return 5; }

private:
    TwoWire* bus_;
    Adafruit_ADS1115 ads_;
    bool ok_ = false;
};

// Distance sensors also report a fill level when "empty" and "full" distances are set: a sensor
// above a tank sees the water surface come closer as it fills.
void fillDistance(const Driver& d, Reading* out, float cm, bool ok);

class Vl53l0x : public Driver {
public:
    Vl53l0x(const DeviceConfig& cfg, TwoWire* bus) : Driver(cfg), bus_(bus) {}

    bool begin() override {
        tof_.setBus(bus_);
        tof_.setTimeout(500);
        ok_ = tof_.init();
        return ok_;
    }

    void read(Reading* out) override {
        uint32_t sum = 0;
        uint8_t n = 0;
        for (uint8_t i = 0; ok_ && i < 3; i++) {
            uint16_t mm = tof_.readRangeSingleMillimeters();
            if (!tof_.timeoutOccurred() && mm < 8190) {
                sum += mm;
                n++;
            }
        }
        fillDistance(*this, out, n ? sum / 10.0f / n : NAN, n > 0);
    }

    uint32_t warmupMs() const override { return 5; }


private:
    TwoWire* bus_;
    VL53L0X tof_;
    bool ok_ = false;
};

class Hcsr04 : public Driver {
public:
    explicit Hcsr04(const DeviceConfig& cfg) : Driver(cfg) {}

    bool begin() override {
        pinMode(cfg_.pin, OUTPUT);
        digitalWrite(cfg_.pin, LOW);
        pinMode(cfg_.pin2, INPUT);
        return true;
    }

    // Median of five echoes; the speed of sound follows the air temperature when another sensor
    // measures it (331.3 + 0.606 × °C m/s).
    void read(Reading* out) override {
        float samples[5];
        uint8_t n = 0;
        for (uint8_t i = 0; i < 5; i++) {
            digitalWrite(cfg_.pin, HIGH);
            delayMicroseconds(10);
            digitalWrite(cfg_.pin, LOW);
            unsigned long us = pulseIn(cfg_.pin2, HIGH, 30000);
            if (us > 0) samples[n++] = us;
            delay(60);  // let the last echo die away
        }
        if (n == 0) {
            fillDistance(*this, out, NAN, false);
            return;
        }
        for (uint8_t i = 1; i < n; i++) {
            for (uint8_t j = i; j > 0 && samples[j] < samples[j - 1]; j--) {
                float tmp = samples[j];
                samples[j] = samples[j - 1];
                samples[j - 1] = tmp;
            }
        }
        float t = isnan(ambient().t) ? 20.0f : ambient().t;
        float cmPerUs = (331.3f + 0.606f * t) / 10000.0f;
        fillDistance(*this, out, samples[n / 2] * cmPerUs / 2.0f, true);
    }

    uint32_t warmupMs() const override { return 50; }

};

// Pulse counters: a water flow meter (YF-S201 and friends) or a tipping-bucket rain gauge. Every
// pulse is counted in an interrupt, so the CPU must keep running (Always on, Modem sleep).
class Pulses : public Driver {
public:
    explicit Pulses(const DeviceConfig& cfg) : Driver(cfg) {}

    bool begin() override {
        pinMode(cfg_.pin, INPUT_PULLUP);
        attachInterruptArg(digitalPinToInterrupt(cfg_.pin), onPulse, this, FALLING);
        since_ = millis();
        return true;
    }

    bool continuous() const override { return true; }

    void read(Reading* out) override {
        noInterrupts();
        uint32_t count = count_;
        count_ = 0;
        interrupts();
        uint32_t now = millis();
        float minutes = (now - since_) / 60000.0f;
        since_ = now;
        if (strcmp(cfg_.driver, "rain") == 0) {
            fill(out, "rain", count * cfg_.number("mm", 0.2794f), true);
        } else {
            float liters = count / cfg_.number("k", 450);
            fill(out, "vol", liters, true);
            fill(out, "flow", minutes > 0 ? liters / minutes : 0, minutes > 0);
        }
    }

private:
    static void IRAM_ATTR onPulse(void* self) { static_cast<Pulses*>(self)->count_++; }

    volatile uint32_t count_ = 0;
    uint32_t since_ = 0;
};

class WifiSignal : public Driver {
public:
    explicit WifiSignal(const DeviceConfig& cfg) : Driver(cfg) {}

    bool begin() override { return true; }
    bool afterConnect() const override { return true; }

    void read(Reading* out) override {
        bool ok = WiFi.status() == WL_CONNECTED;
        fill(out, "rssi", ok ? WiFi.RSSI() : NAN, ok);
    }
};

// Environmental (atmospheric) concentrations, µg/m³.
void fillParticles(const Driver& d, Reading* out, const PM25_AQI_Data& data, bool ok) {
    d.fill(out, "pm1", data.pm10_env, ok);
    d.fill(out, "pm25", data.pm25_env, ok);
    d.fill(out, "pm10", data.pm100_env, ok);
}

void fillDistance(const Driver& d, Reading* out, float cm, bool ok) {
    d.fill(out, "dist", cm, ok);
    float empty = d.cfg().number("empty", 0), full = d.cfg().number("full", 0);
    bool level = empty != full;
    float pct = level ? (empty - cm) / (empty - full) * 100.0f : NAN;
    d.fill(out, "lvl", constrain(pct, 0.0f, 100.0f), ok && level);
}

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
        } else if (strcmp(cfg_.driver, "ph") == 0) {
            // Two-point calibration with pH 7 and pH 4 buffer solutions.
            float v7 = cfg_.number("v7", 1500), v4 = cfg_.number("v4", 2030);
            float ph = v7 == v4 ? NAN : 7.0f - 3.0f * (mv - v7) / (v4 - v7);
            fill(out, "ph", ph, !isnan(ph) && ph > -1 && ph < 15);
        } else if (strcmp(cfg_.driver, "tds") == 0) {
            // Gravity TDS probe: cubic fit from the sensor's data sheet, compensated to 25 °C with
            // the water (or air) temperature another sensor measured, then scaled by the factor
            // from a calibration solution.
            float t = isnan(ambient().t) ? 25.0f : ambient().t;
            float v = mv / 1000.0f / (1.0f + 0.02f * (t - 25.0f));
            float tds = (133.42f * v * v * v - 255.86f * v * v + 857.39f * v) * 0.5f * cfg_.number("k", 1);
            fill(out, "tds", max(tds, 0.0f), true);
            fill(out, "ec", max(tds, 0.0f) * 2.0f / 1000.0f, true);  // µS/cm → mS/cm
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
    if (strcmp(id, "soil") == 0 || strcmp(id, "analog") == 0 || strcmp(id, "battery") == 0 || strcmp(id, "ph") == 0 ||
        strcmp(id, "tds") == 0) {
        return new Analog(cfg, adcRangeMv);
    }
    if (strcmp(id, "pms5003") == 0) return new Pms5003(cfg);
    if (strcmp(id, "hcsr04") == 0) return new Hcsr04(cfg);
    if (strcmp(id, "flow") == 0 || strcmp(id, "rain") == 0) return new Pulses(cfg);
    if (strcmp(id, "wifi") == 0) return new WifiSignal(cfg);

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
    if (strcmp(id, "ms8607") == 0) return new Ms8607(cfg, bus);
    if (strcmp(id, "bmp3xx") == 0) return new Bmp3xx(cfg, bus);
    if (strcmp(id, "dps310") == 0) return new Dps310(cfg, bus);
    if (strcmp(id, "lps22") == 0) return new Lps22(cfg, bus);
    if (strcmp(id, "shtc3") == 0) return new Shtc3(cfg, bus);
    if (strcmp(id, "mcp9808") == 0) return new Mcp9808(cfg, bus);
    if (strcmp(id, "tmp117") == 0) return new Tmp117(cfg, bus);
    if (strcmp(id, "tsl2591") == 0) return new Tsl2591(cfg, bus);
    if (strcmp(id, "ltr390") == 0) return new Ltr390(cfg, bus);
    if (strcmp(id, "scd30") == 0) return new Scd30(cfg, bus);
    if (strcmp(id, "pmsa003i") == 0) return new Pmsa003i(cfg, bus);
    if (strcmp(id, "sen5x") == 0) return new Sen5x(cfg, bus);
    if (strcmp(id, "ina226") == 0) return new Ina226(cfg, bus);
    if (strcmp(id, "ina260") == 0) return new Ina260(cfg, bus);
    if (strcmp(id, "ads1115") == 0) return new Ads1115(cfg, bus);
    if (strcmp(id, "vl53l0x") == 0) return new Vl53l0x(cfg, bus);
    return nullptr;
}

}  // namespace hn
