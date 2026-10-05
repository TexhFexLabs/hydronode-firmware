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
#include <driver/gpio.h>
#include <esp_attr.h>
#endif

#include "Driver.h"
#include "power/Power.h"
#include "power/Schedule.h"
#include "status/Status.h"

// Every driver follows the same round: begin() once per boot, then per due round start() →
// (nap) → poll() → read() → sleep(). Sensors that can measure on request do so and power down in
// between; the ones that cannot say so in the catalog's sleep rules, which keep them out of the
// power modes they do not work in. Waits the sensor needs (a conversion, a fan run-up, a CO₂
// shot) are returned from start()/poll() instead of delay(), so the board naps meanwhile.

namespace hn {

void Driver::fill(Reading* out, const char* q, float value, bool ok, bool warming) {
    for (uint8_t i = 0; i < cfg_.channelCount; i++) {
        if (strcmp(cfg_.channels[i].q, q) == 0) {
            bool valid = ok && isfinite(value);
            out[i] = {cfg_.channels[i].type, value, valid, warming};
            if (ok && !valid && !problem_) problem_ = problem::kRange;
            if (warming && !problem_) problem_ = problem::kWarming;
        }
    }
}

// The sensor still answers on its address. Several libraries cannot tell a sensor that left the
// bus from a reading: a failed read leaves old bytes in their buffer and they turn those into a
// value, or they wait forever for a ready bit.
static bool answers(TwoWire* bus, uint8_t address) {
    bus->beginTransmission(address);
    return bus->endTransmission() == 0;
}

bool Driver::stillThere(TwoWire* bus) {
    if (answers(bus, cfg_.address)) return true;
    setProblem(problem::kMissing);
    return false;
}

bool Driver::sends(const char* q) const {
    for (uint8_t i = 0; i < cfg_.channelCount; i++) {
        if (strcmp(cfg_.channels[i].q, q) == 0) return true;
    }
    return false;
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
    Ds18b20(const DeviceConfig& cfg, const DriverContext& ctx) : Driver(cfg, ctx), wire_(cfg.pin), sensors_(&wire_) {}

    bool begin(bool) override {
        sensors_.begin();
        sensors_.setWaitForConversion(false);  // the board naps during the 750 ms conversion
        uint8_t count = sensors_.getDeviceCount();
        // Print every ROM address so the web app can map probes to types.
        DeviceAddress addr;
        for (uint8_t i = 0; i < count; i++) {
            if (sensors_.getAddress(addr, i)) {
                status::line("OW pin=%d idx=%u addr=%02X%02X%02X%02X%02X%02X%02X%02X", cfg_.pin, i, addr[0], addr[1],
                             addr[2], addr[3], addr[4], addr[5], addr[6], addr[7]);
            }
        }
        if (count == 0) {
            setProblem(problem::kMissing);
            return false;
        }
        // The library writes the probes (and their EEPROM) only when the setting changes.
        sensors_.setResolution(bits());
        return true;
    }

    uint32_t start() override {
        sensors_.requestTemperatures();
        return DallasTemperature::millisToWaitForConversion(bits()) + 10;  // 12 bit 760 ms, 10 bit 198 ms
    }

    void read(Reading* out) override {
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
            bool ok = t != DEVICE_DISCONNECTED_C && t > -55.5f && t < 125.5f;
            if (!ok) setProblem(t == DEVICE_DISCONNECTED_C ? problem::kTimeout : problem::kRange);
            out[i] = {ch.type, t, ok};
        }
    }

private:
    // Accurate to ±0.5 °C at any resolution. Awake boards read 12 bit (0.0625 °C, 750 ms); boards
    // that sleep between rounds read 10 bit (0.25 °C, 188 ms), a quarter of the time on.
    uint8_t bits() const { return ctx_.sleeps() ? 10 : 12; }

    OneWire wire_;
    DallasTemperature sensors_;
};

// --- DHT ------------------------------------------------------------------------

class Dht : public Driver {
public:
    Dht(const DeviceConfig& cfg, const DriverContext& ctx) : Driver(cfg, ctx), dht_(cfg.pin, model(cfg)) {}

    bool begin(bool) override {
        dht_.begin();
        return true;  // a DHT has no "are you there"; a failed read says it
    }

    void read(Reading* out) override {
        float t = dht_.readTemperature(false, true);
        float rh = dht_.readHumidity();
        if ((sends("t") && isnan(t)) || (sends("rh") && isnan(rh))) setProblem(problem::kTimeout);
        fill(out, "t", t, !isnan(t));
        fill(out, "rh", rh, !isnan(rh));
    }

    // Datasheets: no command within 1 s after power-up.
    uint32_t powerUpMs() const override { return 1200; }

private:
    static uint8_t model(const DeviceConfig& cfg) {
        const OptionValue* o = cfg.option("model");
        return (o && strcmp(o->text, "DHT11") == 0) ? DHT11 : DHT22;
    }

    DHT dht_;
};

// --- I2C sensors -----------------------------------------------------------------
// Most of these measure on request and sleep in between by themselves (single shot, forced or
// one-time modes); their begin() only checks that they answer.

class Sht4x : public Driver {
public:
    Sht4x(const DeviceConfig& cfg, const DriverContext& ctx, TwoWire* bus) : Driver(cfg, ctx), bus_(bus) {}

    bool begin(bool cold) override {
        sht_.begin(*bus_, cfg_.address);
        if (cold) sht_.softReset();  // waits the 1 ms the sensor needs (and more)
        uint32_t serial = 0;
        bool ok = sht_.serialNumber(serial) == 0;
        if (!ok) setProblem(problem::kMissing);
        return ok;
    }

    void read(Reading* out) override {
        float t = NAN, rh = NAN;
        bool ok = sht_.measureHighPrecision(t, rh) == 0;
        if (!ok) setProblem(problem::kTimeout);
        fill(out, "t", t, ok);
        fill(out, "rh", rh, ok);
    }

private:
    TwoWire* bus_;
    SensirionI2cSht4x sht_;
};

class Sht3x : public Driver {
public:
    Sht3x(const DeviceConfig& cfg, const DriverContext& ctx, TwoWire* bus) : Driver(cfg, ctx), sht_(bus) {}

    bool begin(bool) override {
        bool ok = sht_.begin(cfg_.address);
        if (!ok) setProblem(problem::kMissing);
        return ok;
    }

    void read(Reading* out) override {
        float t = NAN, rh = NAN;
        bool ok = sht_.readBoth(&t, &rh);
        if (!ok) setProblem(problem::kTimeout);
        fill(out, "t", t, ok);
        fill(out, "rh", rh, ok);
    }

private:
    Adafruit_SHT31 sht_;
};

class Aht : public Driver {
public:
    Aht(const DeviceConfig& cfg, const DriverContext& ctx, TwoWire* bus) : Driver(cfg, ctx), bus_(bus) {}

    bool begin(bool) override {
        bool ok = aht_.begin(bus_, 0, cfg_.address);
        if (!ok) setProblem(problem::kMissing);
        return ok;
    }

    void read(Reading* out) override {
        sensors_event_t humidity{}, temp{};
        bool ok = stillThere(bus_) && aht_.getEvent(&humidity, &temp);  // getEvent() spins on a lost sensor
        if (!ok) setProblem(problem::kTimeout);
        fill(out, "t", temp.temperature, ok);
        fill(out, "rh", humidity.relative_humidity, ok);
    }

    uint32_t powerUpMs() const override { return 40; }

private:
    TwoWire* bus_;
    Adafruit_AHTX0 aht_;
};

class Htu21d : public Driver {
public:
    Htu21d(const DeviceConfig& cfg, const DriverContext& ctx, TwoWire* bus) : Driver(cfg, ctx), bus_(bus) {}

    bool begin(bool) override {
        bool ok = htu_.begin(bus_);
        if (!ok) setProblem(problem::kMissing);
        return ok;
    }

    void read(Reading* out) override {
        float t = htu_.readTemperature();
        float rh = htu_.readHumidity();
        if (isnan(t) && isnan(rh)) setProblem(problem::kTimeout);
        fill(out, "t", t, !isnan(t));
        fill(out, "rh", rh, !isnan(rh));
    }

    uint32_t powerUpMs() const override { return 15; }

private:
    TwoWire* bus_;
    Adafruit_HTU21DF htu_;
};

// Adafruit's begin() resets the BME280, switches on the library's default profile (normal mode,
// 16x) and waits 100 ms for its first value, which this driver never reads: it measures in forced
// mode. In deep sleep that is 100 ms awake at every wake-up. Here: the chip ID, a reset only after
// power-up (asleep the sensor keeps its registers), the calibration words as the library reads
// them, nothing more. The values come from the library's own compensation as before.
class Bme280Device : public Adafruit_BME280 {
public:
    bool attach(uint8_t address, TwoWire* wire, bool cold) {
        delete i2c_dev;
        _i2caddr = address;
        i2c_dev = new Adafruit_I2CDevice(address, wire);
        if (!i2c_dev->begin()) return false;
        _sensorID = read8(BME280_REGISTER_CHIPID);
        if (_sensorID != 0x60) return false;
        if (cold) {
            write8(BME280_REGISTER_SOFTRESET, 0xB6);
            delay(3);  // start-up after a reset: 2 ms
        }
        // The sensor copies its calibration from NVM after a reset (a few ms).
        for (uint32_t start = millis(); isReadingCalibration(); delay(1)) {
            if (millis() - start > 50) return false;
        }
        readCoefficients();
        return true;
    }
};

class Bme280 : public Driver {
public:
    Bme280(const DeviceConfig& cfg, const DriverContext& ctx, TwoWire* bus) : Driver(cfg, ctx), bus_(bus) {}

    // Bosch's "weather monitoring" setting: forced mode, 1x oversampling, no filter. The sensor
    // sleeps between readings, which keeps self-heating and current down.
    bool begin(bool cold) override {
        ok_ = bme_.attach(cfg_.address, bus_, cold);
        if (ok_) {
            bme_.setSampling(Adafruit_BME280::MODE_FORCED, Adafruit_BME280::SAMPLING_X1, Adafruit_BME280::SAMPLING_X1,
                             Adafruit_BME280::SAMPLING_X1, Adafruit_BME280::FILTER_OFF);
        } else {
            setProblem(problem::kMissing);
        }
        return ok_;
    }

    void read(Reading* out) override {
        bool here = ok_ && stillThere(bus_);
        bool ok = here && bme_.takeForcedMeasurement();
        if (here && !ok) setProblem(problem::kTimeout);
        fill(out, "t", bme_.readTemperature(), ok);
        fill(out, "rh", bme_.readHumidity(), ok);
        fill(out, "p", bme_.readPressure() / 100.0f, ok);
    }

    uint32_t bootMs() const override { return 2; }  // Bosch: 2 ms start-up after power-on

private:
    TwoWire* bus_;
    Bme280Device bme_;
    bool ok_ = false;
};

class Bmp280 : public Driver {
public:
    Bmp280(const DeviceConfig& cfg, const DriverContext& ctx, TwoWire* bus) : Driver(cfg, ctx), bus_(bus), bmp_(bus) {}

    bool begin(bool) override {
        ok_ = bmp_.begin(cfg_.address);
        if (ok_) {
            bmp_.setSampling(Adafruit_BMP280::MODE_FORCED, Adafruit_BMP280::SAMPLING_X1, Adafruit_BMP280::SAMPLING_X1,
                             Adafruit_BMP280::FILTER_OFF);
        } else {
            setProblem(problem::kMissing);
        }
        return ok_;
    }

    void read(Reading* out) override {
        bool here = ok_ && stillThere(bus_);
        bool ok = here && bmp_.takeForcedMeasurement();
        if (here && !ok) setProblem(problem::kTimeout);
        fill(out, "t", bmp_.readTemperature(), ok);
        fill(out, "p", bmp_.readPressure() / 100.0f, ok);
    }

    uint32_t bootMs() const override { return 2; }  // Bosch: 2 ms start-up after power-on

private:
    TwoWire* bus_;
    Adafruit_BMP280 bmp_;
    bool ok_ = false;
};

class Bh1750 : public Driver {
public:
    Bh1750(const DeviceConfig& cfg, const DriverContext& ctx, TwoWire* bus) : Driver(cfg, ctx), bus_(bus), meter_(cfg.address) {}

    // One-time mode: the sensor measures once and powers down, instead of drawing 120 µA nonstop.
    bool begin(bool) override {
        ok_ = meter_.begin(BH1750::ONE_TIME_HIGH_RES_MODE, cfg_.address, bus_);
        if (!ok_) setProblem(problem::kMissing);
        return ok_;
    }

    uint32_t start() override {
        started_ = ok_ && meter_.configure(BH1750::ONE_TIME_HIGH_RES_MODE);
        return started_ ? 180 : 0;  // high resolution conversion, at most 180 ms
    }

    void read(Reading* out) override {
        float lux = started_ ? meter_.readLightLevel() : -1;
        if (lux < 0) setProblem(problem::kTimeout);
        fill(out, "lux", lux, lux >= 0);
    }

private:
    TwoWire* bus_;
    BH1750 meter_;
    bool ok_ = false;
    bool started_ = false;
};

class Bme680 : public Driver {
public:
    Bme680(const DeviceConfig& cfg, const DriverContext& ctx, TwoWire* bus) : Driver(cfg, ctx), bme_(bus) {}

    bool begin(bool) override {
        ok_ = bme_.begin(cfg_.address);
        if (ok_) {
            bme_.setTemperatureOversampling(BME680_OS_2X);
            bme_.setHumidityOversampling(BME680_OS_1X);
            bme_.setPressureOversampling(BME680_OS_4X);
            bme_.setIIRFilterSize(BME680_FILTER_SIZE_0);
            // 320 °C for 150 ms, Bosch's default profile. Without a gas value, no heater at all.
            if (sends("gas")) bme_.setGasHeater(320, 150);
            else bme_.setGasHeater(0, 0);
        } else {
            setProblem(problem::kMissing);
        }
        return ok_;
    }

    // The measurement (and the heater) runs while the board naps; performReading() would wait
    // for it awake.
    uint32_t start() override {
        started_ = ok_ && bme_.beginReading() != 0;
        int left = started_ ? bme_.remainingReadingMillis() : 0;
        return left > 0 ? uint32_t(left) : 0;
    }

    void read(Reading* out) override {
        // Always ends the reading the library started (its Bosch API checks the sensor's status).
        bool ok = started_ && bme_.endReading();
        if (!ok) setProblem(problem::kTimeout);
        started_ = false;
        fill(out, "t", bme_.temperature, ok);
        fill(out, "rh", bme_.humidity, ok);
        fill(out, "p", bme_.pressure / 100.0f, ok);
        fill(out, "gas", bme_.gas_resistance / 1000.0f, ok && bme_.gas_resistance > 0);
    }

    uint32_t bootMs() const override { return 2; }  // Bosch: 2 ms start-up after power-on

private:
    Adafruit_BME680 bme_;
    bool ok_ = false;
    bool started_ = false;
};

// SHT-style ticks for Sensirion gas sensors: RH 0..100 % and T -45..130 °C over 0..65535.
uint16_t rhTicks(float rh) { return isnan(rh) ? 0x8000 : uint16_t(constrain(rh, 0.0f, 100.0f) * 65535.0f / 100.0f); }
uint16_t tTicks(float t) { return isnan(t) ? 0x6666 : uint16_t((constrain(t, -45.0f, 130.0f) + 45.0f) * 65535.0f / 175.0f); }

// Sensirion CRC-8 (polynomial 0x31, start 0xFF) over one 16-bit word.
uint8_t sensirionCrc(uint16_t word) {
    uint8_t crc = 0xFF;
    for (uint8_t b : {uint8_t(word >> 8), uint8_t(word & 0xFF)}) {
        crc ^= b;
        for (int i = 0; i < 8; i++) crc = (crc & 0x80) ? uint8_t((crc << 1) ^ 0x31) : uint8_t(crc << 1);
    }
    return crc;
}

// SGP30: one reading per second keeps its baseline; only offered with Always on and Modem sleep.
class Sgp30 : public Driver {
public:
    Sgp30(const DeviceConfig& cfg, const DriverContext& ctx, TwoWire* bus) : Driver(cfg, ctx), bus_(bus) {}

    bool begin(bool) override {
        ok_ = sgp_.begin(bus_) && sgp_.IAQinit();
        started_ = millis();
        measured_ = ok_;  // initialization already confirmed sensor communication
        if (!ok_) setProblem(problem::kMissing);
        return ok_;
    }

    uint32_t tickMs() const override { return 1000; }

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
        if (ok_ && !measured_) setProblem(problem::kTimeout);
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

// Light sleep: Sensirion's low-power mode, one reading every 10 s with the heater off in
// between (the VOC algorithm is specified for 1 s and 10 s). Awake: one reading per second.
uint32_t gasTickMs(const DriverContext& ctx) { return ctx.mode == SleepMode::LightSleep ? 10000 : 1000; }

class Sgp40 : public Driver {
public:
    Sgp40(const DeviceConfig& cfg, const DriverContext& ctx, TwoWire* bus)
        : Driver(cfg, ctx), bus_(bus), algorithm_(gasTickMs(ctx) / 1000.0f) {}

    bool begin(bool) override {
        started_ = millis();
        sampled_ = ok_ = sgp_.begin(bus_);
        if (!ok_) setProblem(problem::kMissing);
        return ok_;
    }

    uint32_t tickMs() const override { return gasTickMs(ctx_); }

    void tick() override {
        if (!ok_) return;
        const Ambient& a = ambient();
        uint16_t raw = measureRaw(isnan(a.rh) ? 50.0f : a.rh, isnan(a.t) ? 25.0f : a.t);
        sampled_ = raw != 0;
        if (sampled_) voc_ = algorithm_.process(raw);
        if (ctx_.mode == SleepMode::LightSleep) sgp_.heaterOff();
    }

    // The VOC index is 0 while the algorithm learns the room (about the first 45 readings).
    void read(Reading* out) override {
        bool healthy = ok_ && sampled_;
        if (ok_ && !sampled_) setProblem(problem::kTimeout);
        uint32_t learning = 60 * tickMs();
        fill(out, "voc", voc_, healthy && voc_ > 0, healthy && voc_ == 0 && millis() - started_ < learning);
    }

private:
    // sgp40_measure_raw_signal with humidity compensation. The data sheet gives 30 ms; the
    // library waits 250 ms, every tick, with the CPU awake. 0 on any error.
    uint16_t measureRaw(float rh, float t) {
        uint16_t words[2] = {rhTicks(rh), tTicks(t)};
        uint8_t address = cfg_.address ? cfg_.address : 0x59;
        bus_->beginTransmission(address);
        bus_->write(0x26);
        bus_->write(0x0F);
        for (uint16_t w : words) {
            bus_->write(uint8_t(w >> 8));
            bus_->write(uint8_t(w & 0xFF));
            bus_->write(sensirionCrc(w));
        }
        if (bus_->endTransmission() != 0) return 0;
        delay(30);
        if (bus_->requestFrom(address, uint8_t(3)) != 3) return 0;
        uint16_t raw = uint16_t(bus_->read() << 8);
        raw |= uint8_t(bus_->read());
        return uint8_t(bus_->read()) == sensirionCrc(raw) ? raw : 0;
    }

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
    Sgp41(const DeviceConfig& cfg, const DriverContext& ctx, TwoWire* bus)
        : Driver(cfg, ctx), bus_(bus), vocAlgorithm_(gasTickMs(ctx) / 1000.0f) {}

    bool begin(bool) override {
        sgp_.begin(*bus_);
        uint16_t serial[3];
        ok_ = sgp_.getSerialNumber(serial) == 0;
        sampled_ = ok_;
        started_ = millis();
        conditioning_ = 10;  // seconds of NOx conditioning after power-up, per datasheet
        if (!ok_) setProblem(problem::kMissing);
        return ok_;
    }

    uint32_t tickMs() const override { return gasTickMs(ctx_); }

    void tick() override {
        if (!ok_) return;
        const Ambient& a = ambient();
        uint16_t srawVoc = 0, srawNox = 0;
        bool lowPower = ctx_.mode == SleepMode::LightSleep;  // VOC only (catalog: NOx needs 1 s)
        if (conditioning_ > 0 && !lowPower) {
            conditioning_--;
            sampled_ = sgp_.executeConditioning(rhTicks(a.rh), tTicks(a.t), srawVoc) == 0;
            return;
        }
        sampled_ = sgp_.measureRawSignals(rhTicks(a.rh), tTicks(a.t), srawVoc, srawNox) == 0;
        if (sampled_) {
            voc_ = vocAlgorithm_.process(srawVoc);
            if (!lowPower) nox_ = noxAlgorithm_.process(srawNox);
        }
        if (lowPower) sgp_.turnHeaterOff();
    }

    // Both indices stay 0 while their algorithm learns (VOC ~45 readings, NOx ~5 min).
    void read(Reading* out) override {
        bool healthy = ok_ && sampled_;
        if (ok_ && !sampled_) setProblem(problem::kTimeout);
        fill(out, "voc", voc_, healthy && voc_ > 0, healthy && voc_ == 0 && millis() - started_ < 60 * tickMs());
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

// SCD40/SCD41. An SCD41 in a sleeping mode measures on request: wake up, one shot that is thrown
// away (Sensirion: the first after waking is less accurate), a second shot that is kept, power
// down to 0.4 µA. Its self-calibration counts shots and assumes one every 5 min, so the periods
// are scaled to the real spacing. Otherwise (SCD40, awake modes) it measures every 5 s on its own.
class Scd4x : public Driver {
public:
    Scd4x(const DeviceConfig& cfg, const DriverContext& ctx, TwoWire* bus)
        : Driver(cfg, ctx), bus_(bus),
          singleShot_(ctx.sleeps() && strcmp(cfg.text("model", "SCD41"), "SCD41") == 0) {}

    bool begin(bool cold) override {
        scd_.begin(*bus_, cfg_.address);
        // Asleep since its last round and set up at power-on: start() wakes it. A missing sensor
        // then fails its shot and says so.
        if (singleShot_ && !cold) return ok_ = true;
        // Not acknowledged (the library waits the 30 ms); stopping the measurement below proves
        // the sensor is awake.
        if (singleShot_) scd_.wakeUp();
        // In case it still runs from before a reset; the library waits the 500 ms it takes.
        uint16_t error = scd_.stopPeriodicMeasurement();
        if (error) {
            setProblem(problem::kMissing);
            return ok_ = false;
        }
        ok_ = true;
        if (singleShot_) {
            // Volatile settings: set again after every power-up.
            if (cold) {
                schedule::AscPeriods asc = schedule::scd41AscPeriods(ctx_.dueSeconds());
                scd_.setAutomaticSelfCalibrationInitialPeriod(asc.initialHours);
                scd_.setAutomaticSelfCalibrationStandardPeriod(asc.standardHours);
            }
        } else {
            ok_ = scd_.startPeriodicMeasurement() == 0;
            startedAt_ = millis();
        }
        return ok_;
    }

    uint32_t start() override {
        if (!ok_) return 0;
        if (singleShot_) {
            scd_.wakeUp();  // waits the 30 ms itself
            shot_ = 1;
            return shoot() ? kShotMs : 0;
        }
        // Periodic: the first result arrives 5 s after the start.
        waitedFrom_ = millis();
        uint32_t since = millis() - startedAt_;
        return since < 5000 ? 5000 - since : 0;
    }

    uint32_t poll() override {
        if (singleShot_ && shot_ == 1) {
            uint16_t co2 = 0;
            float t, rh;
            scd_.readMeasurement(co2, t, rh);  // the first shot after waking up: discarded
            shot_ = 2;
            return shoot() ? kShotMs : 0;
        }
        if (!singleShot_ && ok_) {
            // A new value every 5 s; the board naps between the checks.
            bool ready = false;
            if (scd_.getDataReadyStatus(ready) == 0 && ready) return 0;
            if (millis() - waitedFrom_ < 11000) return 200;
        }
        return 0;
    }

    void read(Reading* out) override {
        uint16_t co2 = 0;
        float t = NAN, rh = NAN;
        bool ok = false;
        if (ok_ && singleShot_) {
            ok = shot_ == 2 && scd_.readMeasurement(co2, t, rh) == 0;
        } else if (ok_) {
            bool ready = false;
            ok = scd_.getDataReadyStatus(ready) == 0 && ready && scd_.readMeasurement(co2, t, rh) == 0;
        }
        if (ok_ && !ok) setProblem(problem::kTimeout);
        ok = ok && co2 > 0;
        fill(out, "co2", co2, ok);
        fill(out, "t", t, ok);
        fill(out, "rh", rh, ok);
    }

    void sleep() override {
        if (singleShot_ && ok_) scd_.powerDown();
        shot_ = 0;
    }

    uint32_t bootMs() const override { return 1000; }
    // The first shot starts after the boot second and the stop command begin() sends (0.5 s).
    uint32_t powerUpMs() const override { return 1550; }

private:
    static constexpr uint32_t kShotMs = 5000;

    // measure_single_shot (0x219D). The library's call waits the 5 s awake; this returns at
    // once and the board naps meanwhile.
    bool shoot() {
        bus_->beginTransmission(cfg_.address);
        bus_->write(0x21);
        bus_->write(0x9D);
        return bus_->endTransmission() == 0;
    }

    TwoWire* bus_;
    SensirionI2cScd4x scd_;
    bool singleShot_;
    bool ok_ = false;
    uint8_t shot_ = 0;
    uint32_t startedAt_ = 0;
    uint32_t waitedFrom_ = 0;
};

class Veml7700 : public Driver {
public:
    Veml7700(const DeviceConfig& cfg, const DriverContext& ctx, TwoWire* bus) : Driver(cfg, ctx), bus_(bus) {}

    bool begin(bool) override {
        ok_ = veml_.begin(bus_);
        if (ok_) veml_.enable(false);  // shut down: 0.5 µA until the next reading
        else setProblem(problem::kMissing);
        return ok_;
    }

    // Picks gain and integration time for the current light, from moonlight to sunlight, in the
    // steps of the library's VEML_LUX_AUTO. The library waits each step out awake (up to 5 s in
    // the dark); here every wait is returned, so the board naps through them.
    uint32_t start() override {
        lux_ = -1;
        // A sensor that left the bus reads 0xFFFF, which would look like bright sunlight.
        if (!ok_ || !stillThere(bus_)) return 0;
        veml_.enable(true);  // waits the 2.5 ms start-up itself
        gain_ = 0;
        it_ = 2;  // gain 1/8, 100 ms
        corrected_ = false;
        phase_ = Phase::First;
        veml_.setGain(kGains[gain_]);
        veml_.setIntegrationTime(kTimes[it_], false);
        return 2 * kTimesMs[it_];
    }

    uint32_t poll() override {
        if (phase_ == Phase::Idle) return 0;
        uint16_t als = veml_.readALS(false);
        if (phase_ == Phase::First) {
            corrected_ = als > 100;
            phase_ = corrected_ ? Phase::Bright : Phase::Dark;
        }
        if (phase_ == Phase::Dark && als <= 100 && !(gain_ == 3 && it_ == 5)) {
            // More gain first, then a longer integration time.
            uint32_t flush = 0;
            if (gain_ < 3) {
                veml_.setGain(kGains[++gain_]);
            } else {
                flush = kTimesMs[it_];  // the running cycle of the old time ends first
                veml_.setIntegrationTime(kTimes[++it_], false);
            }
            return flush + 2 * kTimesMs[it_];
        }
        if (phase_ == Phase::Bright && als > 10000 && it_ > 0) {
            uint32_t flush = kTimesMs[it_];
            veml_.setIntegrationTime(kTimes[--it_], false);
            return flush + 2 * kTimesMs[it_];
        }
        // As the library's computeLux(): resolution from gain and time, the non-linear correction
        // above 100 counts.
        float lux = 0.0036f * (800.0f / kTimesMs[it_]) * (2.0f / kGainValues[gain_]) * als;
        if (corrected_) lux = (((6.0135e-13f * lux - 9.3924e-9f) * lux + 8.1488e-5f) * lux + 1.0023f) * lux;
        lux_ = lux;
        phase_ = Phase::Idle;
        return 0;
    }

    void read(Reading* out) override {
        bool done = phase_ == Phase::Idle && lux_ >= 0;
        if (ok_) veml_.enable(false);  // shut down: 0.5 µA until the next reading
        if (ok_ && !done && !problem()) setProblem(problem::kTimeout);
        phase_ = Phase::Idle;
        fill(out, "lux", lux_, done);
    }

private:
    enum class Phase : uint8_t { Idle, First, Dark, Bright };
    static constexpr uint8_t kGains[4] = {VEML7700_GAIN_1_8, VEML7700_GAIN_1_4, VEML7700_GAIN_1, VEML7700_GAIN_2};
    static constexpr float kGainValues[4] = {0.125f, 0.25f, 1.0f, 2.0f};
    static constexpr uint8_t kTimes[6] = {VEML7700_IT_25MS, VEML7700_IT_50MS, VEML7700_IT_100MS,
                                          VEML7700_IT_200MS, VEML7700_IT_400MS, VEML7700_IT_800MS};
    static constexpr uint32_t kTimesMs[6] = {25, 50, 100, 200, 400, 800};

    TwoWire* bus_;
    Adafruit_VEML7700 veml_;
    bool ok_ = false;
    Phase phase_ = Phase::Idle;
    uint8_t gain_ = 0;
    uint8_t it_ = 2;
    bool corrected_ = false;
    float lux_ = -1;
};

class Ina219 : public Driver {
public:
    Ina219(const DeviceConfig& cfg, const DriverContext& ctx, TwoWire* bus) : Driver(cfg, ctx), bus_(bus), ina_(cfg.address) {}

    bool begin(bool) override {
        ok_ = ina_.begin(bus_);
        const OptionValue* range = cfg_.option("range");
        if (ok_ && range && strcmp(range->text, "32V_1A") == 0) ina_.setCalibration_32V_1A();
        else if (ok_ && range && strcmp(range->text, "16V_400mA") == 0) ina_.setCalibration_16V_400mA();
        if (ok_) ina_.powerSave(true);
        else setProblem(problem::kMissing);
        return ok_;
    }

    void read(Reading* out) override {
        bool here = ok_ && stillThere(bus_);
        if (here) {
            ina_.powerSave(false);
            delay(2);  // one 12 bit conversion of shunt and bus: 2 × 532 µs
        }
        // Load voltage: bus voltage plus the drop over the shunt.
        float volts = ina_.getBusVoltage_V() + ina_.getShuntVoltage_mV() / 1000.0f;
        fill(out, "v", volts, here);
        fill(out, "i", ina_.getCurrent_mA() / 1000.0f, here);
        fill(out, "w", ina_.getPower_mW() / 1000.0f, here);
        if (here) ina_.powerSave(true);
    }

private:
    TwoWire* bus_;
    Adafruit_INA219 ina_;
    bool ok_ = false;
};

class Ms8607 : public Driver {
public:
    Ms8607(const DeviceConfig& cfg, const DriverContext& ctx, TwoWire* bus) : Driver(cfg, ctx), bus_(bus) {}

    bool begin(bool) override {
        ok_ = ms_.begin(bus_);
        if (!ok_) setProblem(problem::kMissing);
        return ok_;
    }

    void read(Reading* out) override {
        sensors_event_t t, p, rh;
        bool ok = ok_ && ms_.getEvent(&p, &t, &rh);
        if (ok_ && !ok) setProblem(problem::kTimeout);
        fill(out, "t", t.temperature, ok);
        fill(out, "rh", rh.relative_humidity, ok);
        fill(out, "p", p.pressure, ok);
    }

    uint32_t powerUpMs() const override { return 15; }

private:
    TwoWire* bus_;
    Adafruit_MS8607 ms_;
    bool ok_ = false;
};

class Bmp3xx : public Driver {
public:
    Bmp3xx(const DeviceConfig& cfg, const DriverContext& ctx, TwoWire* bus) : Driver(cfg, ctx), bus_(bus) {}

    // performReading() runs one forced measurement; 2x pressure, 1x temperature, no filter is
    // Bosch's weather setting.
    bool begin(bool) override {
        ok_ = bmp_.begin_I2C(cfg_.address, bus_);
        if (ok_) {
            bmp_.setTemperatureOversampling(BMP3_NO_OVERSAMPLING);
            bmp_.setPressureOversampling(BMP3_OVERSAMPLING_2X);
            bmp_.setIIRFilterCoeff(BMP3_IIR_FILTER_DISABLE);
        } else {
            setProblem(problem::kMissing);
        }
        return ok_;
    }

    void read(Reading* out) override {
        bool ok = ok_ && bmp_.performReading();
        if (ok_ && !ok) setProblem(problem::kTimeout);
        fill(out, "t", bmp_.temperature, ok);
        fill(out, "p", bmp_.pressure / 100.0, ok);
    }

    uint32_t bootMs() const override { return 2; }  // Bosch: 2 ms start-up after power-on

private:
    TwoWire* bus_;
    Adafruit_BMP3XX bmp_;
    bool ok_ = false;
};

// Command mode: one temperature and one pressure measurement on request, standby (0.5 µA) in
// between, instead of measuring once per second all the time.
class Dps310 : public Driver {
public:
    Dps310(const DeviceConfig& cfg, const DriverContext& ctx, TwoWire* bus) : Driver(cfg, ctx), bus_(bus) {}

    bool begin(bool) override {
        ok_ = dps_.begin_I2C(cfg_.address, bus_);
        if (ok_) {
            dps_.configurePressure(DPS310_1HZ, DPS310_8SAMPLES);
            dps_.configureTemperature(DPS310_1HZ, DPS310_2SAMPLES);
            dps_.setMode(DPS310_IDLE);
        } else {
            setProblem(problem::kMissing);
        }
        return ok_;
    }

    void read(Reading* out) override {
        bool ok = ok_ && measure(DPS310_ONE_TEMPERATURE, true) && measure(DPS310_ONE_PRESSURE, false);
        sensors_event_t t, p;
        ok = ok && dps_.getEvents(&t, &p);
        if (ok_ && !ok) setProblem(problem::kTimeout);
        fill(out, "t", t.temperature, ok);
        fill(out, "p", p.pressure, ok);
    }

    uint32_t powerUpMs() const override { return 40; }

private:
    bool measure(dps310_mode_t mode, bool temperature) {
        dps_.setMode(mode);
        for (uint32_t start = millis(); millis() - start < 200; delay(5)) {
            if (temperature ? dps_.temperatureAvailable() : dps_.pressureAvailable()) return true;
        }
        return false;
    }

    TwoWire* bus_;
    Adafruit_DPS310 dps_;
    bool ok_ = false;
};

class Lps22 : public Driver {
public:
    Lps22(const DeviceConfig& cfg, const DriverContext& ctx, TwoWire* bus) : Driver(cfg, ctx), bus_(bus) {}

    // One-shot: the sensor measures on request and powers down in between.
    bool begin(bool) override {
        ok_ = lps_.begin_I2C(cfg_.address, bus_);
        if (ok_) lps_.setDataRate(LPS22_RATE_ONE_SHOT);
        else setProblem(problem::kMissing);
        return ok_;
    }

    void read(Reading* out) override {
        sensors_event_t p{}, t{};
        bool ok = ok_ && stillThere(bus_) && lps_.getEvent(&p, &t);  // getEvent() spins on a lost sensor
        if (ok_ && !ok) setProblem(problem::kTimeout);
        fill(out, "t", t.temperature, ok);
        fill(out, "p", p.pressure, ok);
    }

    uint32_t bootMs() const override { return 5; }  // 4.5 ms boot after power-on

private:
    TwoWire* bus_;
    Adafruit_LPS22 lps_;
    bool ok_ = false;
};

class Shtc3 : public Driver {
public:
    Shtc3(const DeviceConfig& cfg, const DriverContext& ctx, TwoWire* bus) : Driver(cfg, ctx), bus_(bus) {}

    // The library wakes the sensor for each measurement and puts it back to sleep.
    bool begin(bool) override {
        ok_ = shtc_.begin(bus_);
        if (!ok_) setProblem(problem::kMissing);
        return ok_;
    }

    void read(Reading* out) override {
        sensors_event_t rh, t;
        bool ok = ok_ && shtc_.getEvent(&rh, &t);
        if (ok_ && !ok) setProblem(problem::kTimeout);
        fill(out, "t", t.temperature, ok);
        fill(out, "rh", rh.relative_humidity, ok);
    }

    uint32_t bootMs() const override { return 1; }  // 240 µs power-up

private:
    TwoWire* bus_;
    Adafruit_SHTC3 shtc_;
    bool ok_ = false;
};

class Mcp9808 : public Driver {
public:
    Mcp9808(const DeviceConfig& cfg, const DriverContext& ctx, TwoWire* bus) : Driver(cfg, ctx), bus_(bus) {}

    // 0.0625 °C resolution (250 ms conversion); shut down between readings.
    bool begin(bool) override {
        ok_ = mcp_.begin(cfg_.address, bus_);
        if (ok_) {
            mcp_.setResolution(3);
            mcp_.shutdown();
        } else {
            setProblem(problem::kMissing);
        }
        return ok_;
    }

    uint32_t start() override {
        if (!ok_) return 0;
        mcp_.shutdown_wake(false);  // wake() would also wait the 260 ms, awake
        return 260;
    }

    void read(Reading* out) override {
        float t = ok_ ? mcp_.readTempC() : NAN;
        if (ok_) mcp_.shutdown();
        if (ok_ && isnan(t)) setProblem(problem::kTimeout);
        fill(out, "t", t, !isnan(t));
    }

private:
    TwoWire* bus_;
    Adafruit_MCP9808 mcp_;
    bool ok_ = false;
};

// One-shot with 8 averages (125 ms), shut down in between: 0.25 µA instead of measuring nonstop.
class Tmp117 : public Driver {
public:
    Tmp117(const DeviceConfig& cfg, const DriverContext& ctx, TwoWire* bus) : Driver(cfg, ctx), bus_(bus) {}

    bool begin(bool) override {
        ok_ = tmp_.begin(cfg_.address, bus_);
        if (ok_) {
            tmp_.setAveragedSampleCount(TMP117_AVERAGE_8X);
            tmp_.setReadDelay(TMP117_DELAY_0_MS);
            tmp_.setMeasurementMode(TMP117_MODE_SHUTDOWN);
        } else {
            setProblem(problem::kMissing);
        }
        return ok_;
    }

    uint32_t start() override {
        ready_ = false;
        if (!ok_) return 0;
        tmp_.setMeasurementMode(TMP117_MODE_ONE_SHOT);
        return 130;
    }

    uint32_t poll() override {
        // A slow conversion: ask again shortly, at most a few times.
        ready_ = ok_ && tmp_.dataReady();
        if (ok_ && !ready_ && polls_++ < 5) return 20;
        polls_ = 0;
        return 0;
    }

    // Only a finished conversion: the result register holds the last value, or -256 °C after
    // power-up, and getEvent() reads it either way.
    void read(Reading* out) override {
        sensors_event_t t{};
        bool ok = ready_ && tmp_.getEvent(&t);
        if (ok_ && !ok) setProblem(problem::kTimeout);
        fill(out, "t", t.temperature, ok);
        ready_ = false;
    }

    uint32_t bootMs() const override { return 2; }  // 1.5 ms start-up after power-on

private:
    TwoWire* bus_;
    Adafruit_TMP117 tmp_;
    bool ok_ = false;
    bool ready_ = false;
    uint8_t polls_ = 0;
};

class Tsl2591 : public Driver {
public:
    Tsl2591(const DeviceConfig& cfg, const DriverContext& ctx, TwoWire* bus) : Driver(cfg, ctx), bus_(bus) {}

    bool begin(bool) override {
        ok_ = tsl_.begin(bus_, cfg_.address);
        if (ok_) {
            tsl_.setTiming(TSL2591_INTEGRATIONTIME_100MS);
            tsl_.disable();
        } else {
            setProblem(problem::kMissing);
        }
        return ok_;
    }

    // Starts at high gain and steps down while the sensor saturates, from shade to full sun.
    void read(Reading* out) override {
        // A sensor that left the bus reads 0, which would look like darkness.
        if (!ok_ || !stillThere(bus_)) {
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
                // Full darkness: 0 lx (the library divides by zero and returns NaN).
                lux = full == 0 ? 0.0f : tsl_.calculateLux(full, ir);
                break;
            }
        }
        if (isnan(lux)) setProblem(problem::kRange);
        fill(out, "lux", lux, !isnan(lux) && lux >= 0);
    }

private:
    TwoWire* bus_;
    Adafruit_TSL2591 tsl_{2591};
    bool ok_ = false;
};

class Ltr390 : public Driver {
public:
    Ltr390(const DeviceConfig& cfg, const DriverContext& ctx, TwoWire* bus) : Driver(cfg, ctx), bus_(bus) {}

    bool begin(bool) override {
        ok_ = ltr_.begin(bus_);
        if (ok_) {
            ltr_.setGain(LTR390_GAIN_3);
            ltr_.setResolution(LTR390_RESOLUTION_18BIT);  // 100 ms per conversion
            ltr_.enable(false);
        } else {
            setProblem(problem::kMissing);
        }
        return ok_;
    }

    void read(Reading* out) override {
        if (sends("uv")) {
            uint32_t uvs = 0;
            bool ok = sample(LTR390_MODE_UVS, uvs);
            // Datasheet: 2300 counts per UV index at gain 18 and 20 bit (400 ms); here gain 3,
            // 18 bit (100 ms): 2300 / 6 / 4.
            fill(out, "uv", uvs / (2300.0f / 24.0f), ok);
        }
        if (sends("lux")) {
            uint32_t als = 0;
            bool ok = sample(LTR390_MODE_ALS, als);
            // Datasheet: lux = 0.6 × ALS / (gain × integration time in units of 100 ms).
            fill(out, "lux", 0.6f * als / 3.0f, ok);
        }
    }

private:
    bool sample(ltr390_mode_t mode, uint32_t& value) {
        if (!ok_) return false;
        ltr_.setMode(mode);
        ltr_.enable(true);
        bool ready = false;
        for (uint32_t start = millis(); millis() - start < 400 && !(ready = ltr_.newDataAvailable());) delay(10);
        if (ready) value = mode == LTR390_MODE_UVS ? ltr_.readUVS() : ltr_.readALS();
        else setProblem(problem::kTimeout);
        ltr_.enable(false);
        return ready;
    }

    TwoWire* bus_;
    Adafruit_LTR390 ltr_;
    bool ok_ = false;
};

// Adafruit's begin() resets the SCD30 and sets a 2 s interval. Done once after power-up only: a
// sensor that kept running through the board's sleep keeps its interval and its calibration.
class Scd30Device : public Adafruit_SCD30 {
public:
    bool attach(uint8_t address, TwoWire* wire) {
        delete i2c_dev;
        i2c_dev = new Adafruit_I2CDevice(address, wire);
        return i2c_dev->begin();
    }
};

class Scd30 : public Driver {
public:
    Scd30(const DeviceConfig& cfg, const DriverContext& ctx, TwoWire* bus) : Driver(cfg, ctx), bus_(bus) {}

    // Awake: a reading every 2 s. Asleep between rounds: it measures on its own at (a little
    // less than) the spacing of its rounds, so a fresh value waits at every due round.
    bool begin(bool cold) override {
        uint16_t interval = schedule::scd30IntervalSeconds(ctx_.sleeps() ? ctx_.dueSeconds() : 0);
        ok_ = cold ? scd_.begin(cfg_.address, bus_) : scd_.attach(cfg_.address, bus_);
        // The interval is stored in the sensor: written only when it differs.
        if (ok_ && scd_.getMeasurementInterval() != interval) ok_ = scd_.setMeasurementInterval(interval);
        if (!ok_) setProblem(problem::kMissing);
        interval_ = interval;
        return ok_;
    }

    uint32_t start() override {
        // A value is due at the latest one interval after the last; wait for it when needed.
        polls_ = 0;
        return 0;
    }

    uint32_t poll() override {
        if (!ok_ || scd_.dataReady()) return 0;
        // Right after power-up the first value takes up to two intervals; awake that is 4 s.
        uint32_t limit = interval_ <= 2 ? 40 : 20;
        return polls_++ < limit ? 100 : 0;
    }

    void read(Reading* out) override {
        bool ok = ok_ && scd_.dataReady() && scd_.read() && scd_.CO2 > 0;
        if (ok_ && !ok) setProblem(problem::kTimeout);
        fill(out, "co2", scd_.CO2, ok);
        fill(out, "t", scd_.temperature, ok);
        fill(out, "rh", scd_.relative_humidity, ok);
    }

    uint32_t bootMs() const override { return 2000; }
    // After power-on its first value takes the boot time plus one 2 s interval.
    uint32_t powerUpMs() const override { return 4500; }

private:
    TwoWire* bus_;
    Scd30Device scd_;
    bool ok_ = false;
    uint16_t interval_ = 2;
    uint16_t polls_ = 0;
};

void fillParticles(Driver& d, Reading* out, const PM25_AQI_Data& data, bool ok);

// The fan of a Plantower sensor wears out after about 8000 hours and draws 60 mA. With its SET
// pin wired, it sleeps (SET low) between rounds that are more than a minute apart and runs 30 s
// before a reading, as Plantower specifies. Without SET it simply keeps running.
class FanSleep {
public:
    explicit FanSleep(int8_t setPin) : pin_(setPin) {}

    bool wired() const { return pin_ >= 0; }

    // After begin: asleep, or running when rounds are close together. Level first, then the
    // output, then the hold from the last deep sleep released: SET never floats (its pull-up on
    // the sensor would start the fan).
    void begin(bool cold, bool keepRunning) {
        if (!wired()) return;
        if (cold || !running_) {
            running_ = keepRunning;
            // Not cold with a new object: a deep sleep, through which the hold kept the fan
            // running. It has run up long ago.
            if (keepRunning) since_ = cold ? millis() : millis() - kRunUpMs;
        }
        // digitalWrite() does nothing on an ESP32 pin pinMode() has not set up yet.
#if defined(ESP8266)
        digitalWrite(pin_, running_ ? HIGH : LOW);
#else
        gpio_set_level(gpio_num_t(pin_), running_ ? 1 : 0);
#endif
        pinMode(pin_, OUTPUT);
        power::keepLevel(pin_);
        power::releaseLevel(pin_);
    }

    // Milliseconds until the counts are stable.
    uint32_t wake() {
        if (!wired()) return 0;
        if (!running_) {
            digitalWrite(pin_, HIGH);
            running_ = true;
            since_ = millis();
        }
        uint32_t ran = millis() - since_;
        return ran < kRunUpMs ? kRunUpMs - ran : 0;
    }

    void rest(const DriverContext& ctx) {
        if (!wired() || ctx.dueSeconds() <= 60) return;  // a minute apart or less: keep it running
        digitalWrite(pin_, LOW);  // held low through sleep (power::keepLevel)
        running_ = false;
    }

private:
    static constexpr uint32_t kRunUpMs = 30000;
    int8_t pin_;
    bool running_ = false;
    uint32_t since_ = 0;
};

class Pmsa003i : public Driver {
public:
    Pmsa003i(const DeviceConfig& cfg, const DriverContext& ctx, TwoWire* bus) : Driver(cfg, ctx), bus_(bus), fan_(cfg.pin) {}

    bool begin(bool cold) override {
        fan_.begin(cold, ctx_.dueSeconds() <= 60);
        ok_ = aqi_.begin_I2C(bus_);
        if (!ok_ && !fan_.wired()) setProblem(problem::kMissing);
        return ok_ || fan_.wired();  // asleep, a PMSA003I may not answer I²C until it runs
    }

    uint32_t start() override { return fan_.wake(); }

    void read(Reading* out) override {
        if (!ok_) ok_ = aqi_.begin_I2C(bus_);
        PM25_AQI_Data data{};
        bool ok = ok_ && aqi_.read(&data);
        if (!ok) setProblem(ok_ ? problem::kTimeout : problem::kMissing);
        fillParticles(*this, out, data, ok);
    }

    void sleep() override { fan_.rest(ctx_); }

    // Without SET the fan starts with the supply: 30 s until the counts are stable.
    uint32_t powerUpMs() const override { return fan_.wired() ? 0 : 30000; }

private:
    TwoWire* bus_;
    Adafruit_PM25AQI aqi_;
    FanSleep fan_;
    bool ok_ = false;
};

class Pms5003 : public Driver {
public:
    Pms5003(const DeviceConfig& cfg, const DriverContext& ctx) : Driver(cfg, ctx), fan_(cfg.pin2) {}

    // Only the sensor's TX is needed: it sends a frame every second on its own.
    bool begin(bool cold) override {
        fan_.begin(cold, ctx_.dueSeconds() <= 60);
#if defined(ESP8266)
        serial_.begin(9600, SWSERIAL_8N1, cfg_.pin, -1);
        ok_ = aqi_.begin_UART(&serial_);
#else
        Serial1.begin(9600, SERIAL_8N1, cfg_.pin, -1);
        ok_ = aqi_.begin_UART(&Serial1);
#endif
        return ok_;
    }

    uint32_t start() override { return fan_.wake(); }

    void read(Reading* out) override {
        PM25_AQI_Data data{};
        bool ok = false;
        // The library reads the oldest frame in the receive buffer, which may be from before the
        // fan ran up. Drop what is there and take the next one: the sensor sends a frame at least
        // every 2.3 s. Awake, not napping: the UART loses bytes in light sleep.
        Stream& in = stream();
        while (in.available() > 0) in.read();
        for (uint32_t start = millis(); ok_ && !ok && millis() - start < 3000; delay(50)) ok = aqi_.read(&data);
        if (!ok) setProblem(problem::kTimeout);
        fillParticles(*this, out, data, ok);
    }

    void sleep() override { fan_.rest(ctx_); }

    uint32_t powerUpMs() const override { return fan_.wired() ? 0 : 30000; }

private:
#if defined(ESP8266)
    Stream& stream() { return serial_; }
    SoftwareSerial serial_;
#else
    Stream& stream() { return Serial1; }
#endif
    Adafruit_PM25AQI aqi_;
    FanSleep fan_;
    bool ok_ = false;
};

// SEN5x: full measurement (fan on) only for due rounds. In between it rests in idle, or, when
// VOC/NOx are sent, in its gas-only mode: the indices are computed on the sensor every second,
// so they keep learning while the board sleeps.
class Sen5x : public Driver {
public:
    Sen5x(const DeviceConfig& cfg, const DriverContext& ctx, TwoWire* bus) : Driver(cfg, ctx), bus_(bus) {}

    bool begin(bool cold) override {
        sen_.begin(*bus_);
        bool ready = false;
        ok_ = sen_.readDataReady(ready) == 0;
        if (!ok_) {
            setProblem(problem::kMissing);
            return false;
        }
        if (cold) {
            // After power-up the sensor idles; keep it running when rounds are close together.
            if (keepRunning()) {
                ok_ = sen_.startMeasurement() == 0;
                full_ = ok_;  // start() must not start it again (and wait another run-up)
            } else {
                ok_ = rest() == 0;
            }
            since_ = millis();
        }
        return ok_;
    }

    uint32_t start() override {
        if (!ok_) return 0;
        if (!full_) {
            full_ = sen_.startMeasurement() == 0;
            since_ = millis();
        }
        uint32_t ran = millis() - since_;
        return ran < 10000 ? 10000 - ran : 0;  // fan run-up for stable PM values
    }

    void read(Reading* out) override {
        bool ready = false;
        for (uint32_t start = millis(); ok_ && !ready && millis() - start < 1500; delay(50)) {
            if (sen_.readDataReady(ready) != 0) break;
        }
        float pm1 = NAN, pm25 = NAN, pm4 = NAN, pm10 = NAN, rh = NAN, t = NAN, voc = NAN, nox = NAN;
        bool ok = ready && sen_.readMeasuredValues(pm1, pm25, pm4, pm10, rh, t, voc, nox) == 0;
        if (ok_ && !ok) setProblem(problem::kTimeout);
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

    void sleep() override {
        if (ok_ && !keepRunning()) rest();
    }

    uint32_t bootMs() const override { return 50; }

private:
    bool keepRunning() const { return !ctx_.sleeps() && ctx_.dueSeconds() <= 60; }

    uint16_t rest() {
        full_ = false;
        return sends("voc") || sends("nox") ? sen_.startMeasurementWithoutPm() : sen_.stopMeasurement();
    }

    TwoWire* bus_;
    SensirionI2CSen5x sen_;
    bool ok_ = false;
    bool full_ = false;
    uint32_t since_ = 0;
};

class Ina226 : public Driver {
public:
    Ina226(const DeviceConfig& cfg, const DriverContext& ctx, TwoWire* bus) : Driver(cfg, ctx), ina_(cfg.address, bus) {}

    // Triggered conversions, shut down in between.
    bool begin(bool) override {
        ok_ = ina_.begin() && ina_.isConnected();
        if (ok_) ok_ = ina_.setMaxCurrentShunt(cfg_.number("maxA", 0.8f), cfg_.number("shunt", 0.1f)) == INA226_ERR_NONE;
        if (ok_) ina_.shutDown();
        else setProblem(problem::kMissing);
        return ok_;
    }

    void read(Reading* out) override {
        bool ok = ok_ && ina_.setModeShuntBusTrigger() && ina_.waitConversionReady(50);
        if (ok_ && !ok) setProblem(problem::kTimeout);
        fill(out, "v", ina_.getBusVoltage(), ok);
        fill(out, "i", ina_.getCurrent(), ok);
        fill(out, "w", ina_.getPower(), ok);
        if (ok_) ina_.shutDown();
    }

private:
    INA226 ina_;
    bool ok_ = false;
};

class Ina260 : public Driver {
public:
    Ina260(const DeviceConfig& cfg, const DriverContext& ctx, TwoWire* bus) : Driver(cfg, ctx), bus_(bus) {}

    bool begin(bool) override {
        ok_ = ina_.begin(cfg_.address, bus_);
        if (ok_) ina_.setMode(INA260_MODE_SHUTDOWN);
        else setProblem(problem::kMissing);
        return ok_;
    }

    void read(Reading* out) override {
        bool ok = ok_ && stillThere(bus_);
        if (ok) {
            ina_.setMode(INA260_MODE_TRIGGERED);  // one conversion, then it powers down itself
            uint32_t start = millis();
            while (!(ok = ina_.conversionReady()) && millis() - start < 50) delay(2);
            if (!ok) setProblem(problem::kTimeout);
        }
        fill(out, "v", ina_.readBusVoltage() / 1000.0f, ok);
        fill(out, "i", ina_.readCurrent() / 1000.0f, ok);
        fill(out, "w", ina_.readPower() / 1000.0f, ok);
    }

private:
    TwoWire* bus_;
    Adafruit_INA260 ina_;
    bool ok_ = false;
};

class Ads1115 : public Driver {
public:
    Ads1115(const DeviceConfig& cfg, const DriverContext& ctx, TwoWire* bus) : Driver(cfg, ctx), bus_(bus) {}

    // ±4.096 V range: covers 0..3.3 V inputs (never more than the supply on any input). Single
    // shot: the converter powers down after each conversion by itself.
    bool begin(bool) override {
        ok_ = ads_.begin(cfg_.address, bus_);
        if (ok_) ads_.setGain(GAIN_ONE);
        else setProblem(problem::kMissing);
        return ok_;
    }

    void read(Reading* out) override {
        const char* inputs[] = {"a0", "a1", "a2", "a3"};
        bool here = ok_ && stillThere(bus_);
        for (uint8_t ch = 0; ch < 4; ch++) {
            if (!sends(inputs[ch])) continue;
            int16_t raw = 0;
            bool ok = here && convert(ch, raw);
            fill(out, inputs[ch], ok ? ads_.computeVolts(raw) : NAN, ok);
        }
    }

private:
    // One single-shot conversion (8 ms at 128 samples/s). readADC_SingleEnded() waits for it
    // without a limit, forever when the converter left the bus.
    bool convert(uint8_t ch, int16_t& raw) {
        static const uint16_t mux[] = {ADS1X15_REG_CONFIG_MUX_SINGLE_0, ADS1X15_REG_CONFIG_MUX_SINGLE_1,
                                       ADS1X15_REG_CONFIG_MUX_SINGLE_2, ADS1X15_REG_CONFIG_MUX_SINGLE_3};
        ads_.startADCReading(mux[ch], false);
        for (uint32_t start = millis(); millis() - start < 30; delay(1)) {
            if (ads_.conversionComplete()) {
                raw = ads_.getLastConversionResults();
                return true;
            }
        }
        setProblem(problem::kTimeout);
        return false;
    }

    TwoWire* bus_;
    Adafruit_ADS1115 ads_;
    bool ok_ = false;
};

// Distance sensors also report a fill level when "empty" and "full" distances are set: a sensor
// above a tank sees the water surface come closer as it fills.
void fillDistance(Driver& d, Reading* out, float cm, bool ok);

class Vl53l0x : public Driver {
public:
    Vl53l0x(const DeviceConfig& cfg, const DriverContext& ctx, TwoWire* bus) : Driver(cfg, ctx), bus_(bus) {}

    // Single ranging on request; the sensor returns to standby (5 µA) by itself.
    bool begin(bool) override {
        tof_.setBus(bus_);
        tof_.setTimeout(500);
        ok_ = tof_.init();
        if (!ok_) setProblem(problem::kMissing);
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
        if (ok_ && n == 0) setProblem(problem::kTimeout);
        fillDistance(*this, out, n ? sum / 10.0f / n : NAN, n > 0);
    }

    uint32_t bootMs() const override { return 2; }

private:
    TwoWire* bus_;
    VL53L0X tof_;
    bool ok_ = false;
};

class Hcsr04 : public Driver {
public:
    Hcsr04(const DeviceConfig& cfg, const DriverContext& ctx) : Driver(cfg, ctx) {}

    bool begin(bool) override {
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
            if (i < 4) delay(60);  // let the last echo die away before the next ping
        }
        if (n == 0) {
            setProblem(problem::kTimeout);
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

    bool usesAmbient() const override { return true; }
    uint32_t powerUpMs() const override { return 50; }
};

// Pulse counters: a water flow meter (YF-S201 and friends) or a tipping-bucket rain gauge. Awake,
// every pulse is counted in an interrupt. A rain gauge may also sleep: each tip wakes the board
// for a moment (addSleepTips), which a flow meter's hundreds of pulses per second cannot do.
#if !defined(ESP8266)
struct SleepTips {
    int8_t pin;
    uint16_t tips;
};
RTC_DATA_ATTR SleepTips sleepTips[4] = {{-1, 0}, {-1, 0}, {-1, 0}, {-1, 0}};
#endif

uint16_t takeSleepTips(int8_t pin) {
#if !defined(ESP8266)
    for (SleepTips& s : sleepTips) {
        if (s.pin == pin) {
            uint16_t tips = s.tips;
            s.tips = 0;
            return tips;
        }
    }
#endif
    (void)pin;
    return 0;
}

class Pulses : public Driver {
public:
    Pulses(const DeviceConfig& cfg, const DriverContext& ctx) : Driver(cfg, ctx) {}

    bool begin(bool) override {
        pinMode(cfg_.pin, INPUT_PULLUP);
        attachInterruptArg(digitalPinToInterrupt(cfg_.pin), onPulse, this, FALLING);
        since_ = millis();
        return true;
    }

    // While the board sleeps a tip wakes it instead (wake inputs in main.cpp count it); the
    // interrupt is off then, so a tip is never counted twice.
    void beforeSleep() override { detachInterrupt(digitalPinToInterrupt(cfg_.pin)); }
    void afterSleep() override { attachInterruptArg(digitalPinToInterrupt(cfg_.pin), onPulse, this, FALLING); }

    void read(Reading* out) override {
        noInterrupts();
        uint32_t count = count_;
        count_ = 0;
        interrupts();
        uint32_t now = millis();
        float minutes = (now - since_) / 60000.0f;
        since_ = now;
        if (rain()) {
            count += takeSleepTips(cfg_.pin);
            fill(out, "rain", count * cfg_.number("mm", 0.2794f), true);
        } else {
            float liters = count / cfg_.number("k", 450);
            fill(out, "vol", liters, true);
            fill(out, "flow", minutes > 0 ? liters / minutes : 0, minutes > 0);
        }
    }

private:
    bool rain() const { return strcmp(cfg_.driver, "rain") == 0; }

    // A rain gauge's reed contact bounces for a few milliseconds on every tip; a bucket cannot
    // tip twice within 50 ms. A flow meter counts every edge (up to hundreds per second).
    static void IRAM_ATTR onPulse(void* self) {
        Pulses* p = static_cast<Pulses*>(self);
        if (p->debounceUs_) {
            uint32_t now = micros();
            if (now - p->lastUs_ < p->debounceUs_) return;
            p->lastUs_ = now;
        }
        p->count_ = p->count_ + 1;
    }

    volatile uint32_t count_ = 0;
    volatile uint32_t lastUs_ = 0;
    const uint32_t debounceUs_ = rain() ? 50000 : 0;
    uint32_t since_ = 0;
};

class WifiSignal : public Driver {
public:
    WifiSignal(const DeviceConfig& cfg, const DriverContext& ctx) : Driver(cfg, ctx) {}

    bool begin(bool) override { return true; }
    bool afterConnect() const override { return true; }

    void read(Reading* out) override {
        bool ok = WiFi.status() == WL_CONNECTED;
        fill(out, "rssi", ok ? WiFi.RSSI() : NAN, ok);
    }
};

// Environmental (atmospheric) concentrations, µg/m³.
void fillParticles(Driver& d, Reading* out, const PM25_AQI_Data& data, bool ok) {
    d.fill(out, "pm1", data.pm10_env, ok);
    d.fill(out, "pm25", data.pm25_env, ok);
    d.fill(out, "pm10", data.pm100_env, ok);
}

void fillDistance(Driver& d, Reading* out, float cm, bool ok) {
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
    Analog(const DeviceConfig& cfg, const DriverContext& ctx) : Driver(cfg, ctx) {}

    bool begin(bool) override {
#if !defined(ESP8266)
        analogSetPinAttenuation(cfg_.pin, ADC_11db);
#endif
        return true;
    }

    void read(Reading* out) override {
        float mv = readMillivolts(cfg_.pin, ctx_.adcRangeMv);
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

    bool usesAmbient() const override { return strcmp(cfg_.driver, "tds") == 0; }

    // Probe boards need a moment to settle after power-up (pH and TDS about a second).
    uint32_t powerUpMs() const override {
        if (strcmp(cfg_.driver, "ph") == 0 || strcmp(cfg_.driver, "tds") == 0) return 1000;
        if (strcmp(cfg_.driver, "soil") == 0) return 100;
        return 10;
    }
};

}  // namespace

void addSleepTips(int8_t pin, uint16_t tips) {
#if !defined(ESP8266)
    for (SleepTips& s : sleepTips) {
        if (s.pin == pin) {
            s.tips += tips;
            return;
        }
    }
    for (SleepTips& s : sleepTips) {
        if (s.pin < 0) {
            s = {pin, tips};
            return;
        }
    }
#else
    (void)pin;
    (void)tips;
#endif
}

Driver* createDriver(const DeviceConfig& cfg, TwoWire* buses[kMaxI2cBuses], const DriverContext& ctx) {
    const char* id = cfg.driver;
    if (strcmp(id, "ds18b20") == 0) return new Ds18b20(cfg, ctx);
    if (strcmp(id, "dht") == 0) return new Dht(cfg, ctx);
    if (strcmp(id, "soil") == 0 || strcmp(id, "analog") == 0 || strcmp(id, "battery") == 0 || strcmp(id, "ph") == 0 ||
        strcmp(id, "tds") == 0) {
        return new Analog(cfg, ctx);
    }
    if (strcmp(id, "pms5003") == 0) return new Pms5003(cfg, ctx);
    if (strcmp(id, "hcsr04") == 0) return new Hcsr04(cfg, ctx);
    if (strcmp(id, "flow") == 0 || strcmp(id, "rain") == 0) return new Pulses(cfg, ctx);
    if (strcmp(id, "wifi") == 0) return new WifiSignal(cfg, ctx);

    if (cfg.bus < 0 || !buses[cfg.bus]) return nullptr;
    TwoWire* bus = buses[cfg.bus];
    if (strcmp(id, "sht4x") == 0) return new Sht4x(cfg, ctx, bus);
    if (strcmp(id, "sht3x") == 0) return new Sht3x(cfg, ctx, bus);
    if (strcmp(id, "aht") == 0) return new Aht(cfg, ctx, bus);
    if (strcmp(id, "htu21d") == 0) return new Htu21d(cfg, ctx, bus);
    if (strcmp(id, "bme280") == 0) return new Bme280(cfg, ctx, bus);
    if (strcmp(id, "bmp280") == 0) return new Bmp280(cfg, ctx, bus);
    if (strcmp(id, "bh1750") == 0) return new Bh1750(cfg, ctx, bus);
    if (strcmp(id, "bme680") == 0) return new Bme680(cfg, ctx, bus);
    if (strcmp(id, "sgp30") == 0) return new Sgp30(cfg, ctx, bus);
    if (strcmp(id, "sgp40") == 0) return new Sgp40(cfg, ctx, bus);
    if (strcmp(id, "sgp41") == 0) return new Sgp41(cfg, ctx, bus);
    if (strcmp(id, "scd4x") == 0) return new Scd4x(cfg, ctx, bus);
    if (strcmp(id, "veml7700") == 0) return new Veml7700(cfg, ctx, bus);
    if (strcmp(id, "ina219") == 0) return new Ina219(cfg, ctx, bus);
    if (strcmp(id, "ms8607") == 0) return new Ms8607(cfg, ctx, bus);
    if (strcmp(id, "bmp3xx") == 0) return new Bmp3xx(cfg, ctx, bus);
    if (strcmp(id, "dps310") == 0) return new Dps310(cfg, ctx, bus);
    if (strcmp(id, "lps22") == 0) return new Lps22(cfg, ctx, bus);
    if (strcmp(id, "shtc3") == 0) return new Shtc3(cfg, ctx, bus);
    if (strcmp(id, "mcp9808") == 0) return new Mcp9808(cfg, ctx, bus);
    if (strcmp(id, "tmp117") == 0) return new Tmp117(cfg, ctx, bus);
    if (strcmp(id, "tsl2591") == 0) return new Tsl2591(cfg, ctx, bus);
    if (strcmp(id, "ltr390") == 0) return new Ltr390(cfg, ctx, bus);
    if (strcmp(id, "scd30") == 0) return new Scd30(cfg, ctx, bus);
    if (strcmp(id, "pmsa003i") == 0) return new Pmsa003i(cfg, ctx, bus);
    if (strcmp(id, "sen5x") == 0) return new Sen5x(cfg, ctx, bus);
    if (strcmp(id, "ina226") == 0) return new Ina226(cfg, ctx, bus);
    if (strcmp(id, "ina260") == 0) return new Ina260(cfg, ctx, bus);
    if (strcmp(id, "ads1115") == 0) return new Ads1115(cfg, ctx, bus);
    if (strcmp(id, "vl53l0x") == 0) return new Vl53l0x(cfg, ctx, bus);
    return nullptr;
}

}  // namespace hn
