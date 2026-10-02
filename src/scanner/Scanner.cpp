// I²C and 1-Wire scanner, flashed from the wiring step of the device builder.
//
// Waits for commands on the serial port (115200 baud, one per line) and answers with "HN:" lines,
// so the browser can scan any pin pair without flashing again:
//
//   I2C <sda> <scl>   →  HN:I2C FOUND 0x76 … HN:I2C DONE <count>
//                         HN:I2C ERR SDA_LOW / SCL_LOW when a line is held low (no pull-up, short)
//   OW <pin>          →  HN:OW FOUND 28FF641E8716045C … HN:OW DONE <count>
//                         HN:OW ERR NO_PULLUP when the line stays low
//   HELLO             →  HN:SCAN READY …
//
// Until the first command it repeats READY every second, so a monitor that attaches late still
// sees it. Nothing here touches WiFi or the config block.

#include <Arduino.h>
#include <OneWireNg_CurrentPlatform.h>
#include <Wire.h>
#include <stdarg.h>

namespace {

constexpr int kMaxPin = 48;
bool heard = false;
uint32_t lastReady = 0;
char line[48];
size_t lineLen = 0;
bool wireStarted = false;

void out(const char* fmt, ...) {
    char buf[96];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    Serial.print("HN:");
    Serial.println(buf);
#if ARDUINO_USB_CDC_ON_BOOT
    Serial0.print("HN:");
    Serial0.println(buf);
#endif
}

void ready() { out("SCAN READY fw=%s family=%s", HN_FW_VERSION, HN_FAMILY); }

bool validPin(int pin) { return pin >= 0 && pin <= kMaxPin; }

// A line that reads low with the internal pull-up on is shorted or held by a device: every
// transfer would fail, so say so instead of reporting "nothing found".
bool lineLow(int pin) {
    pinMode(pin, INPUT_PULLUP);
    delay(2);
    return digitalRead(pin) == LOW;
}

void scanI2c(int sda, int scl) {
    if (!validPin(sda) || !validPin(scl) || sda == scl) {
        out("I2C ERR BAD_PINS");
        return;
    }
    if (wireStarted) {
#if !defined(ESP8266)
        Wire.end();
#endif
        wireStarted = false;
    }
    bool sdaLow = lineLow(sda);
    bool sclLow = lineLow(scl);
    if (sdaLow || sclLow) {
        out("I2C ERR %s", sdaLow ? "SDA_LOW" : "SCL_LOW");
        return;
    }
#if defined(ESP8266)
    Wire.begin(sda, scl);
    Wire.setClock(100000);
#else
    Wire.begin(sda, scl, 100000);
#endif
    wireStarted = true;
    int count = 0;
    for (uint8_t addr = 0x08; addr <= 0x77; addr++) {
        Wire.beginTransmission(addr);
        if (Wire.endTransmission() == 0) {
            out("I2C FOUND 0x%02x", addr);
            count++;
        }
        delay(2);
    }
    out("I2C DONE %d", count);
}

void scanOneWire(int pin) {
    if (!validPin(pin)) {
        out("OW ERR BAD_PINS");
        return;
    }
    if (lineLow(pin)) {
        out("OW ERR NO_PULLUP");
        return;
    }
    OneWireNg_CurrentPlatform ow(pin, true);  // internal pull-up as a fallback, 4.7 kΩ is better
    OneWireNg::Id id;
    int count = 0;
    ow.searchReset();
    while (ow.search(id) == OneWireNg::EC_MORE && count < 32) {
        char hex[17];
        for (int i = 0; i < 8; i++) snprintf(hex + 2 * i, 3, "%02X", id[i]);
        out("OW FOUND %s", hex);
        count++;
    }
    out("OW DONE %d", count);
}

void handle(char* cmd) {
    heard = true;
    int a = -1, b = -1;
    if (sscanf(cmd, "I2C %d %d", &a, &b) == 2) scanI2c(a, b);
    else if (sscanf(cmd, "OW %d", &a) == 1) scanOneWire(a);
    else if (strncmp(cmd, "HELLO", 5) == 0) ready();
    else out("ERR UNKNOWN");
}

void readCommands(Stream& in) {
    while (in.available()) {
        char c = char(in.read());
        if (c == '\r') continue;
        if (c == '\n') {
            line[lineLen] = 0;
            if (lineLen) handle(line);
            lineLen = 0;
        } else if (lineLen < sizeof(line) - 1) {
            line[lineLen++] = c;
        }
    }
}

}  // namespace

void setup() {
    Serial.begin(115200);
#if ARDUINO_USB_CDC_ON_BOOT
    Serial0.begin(115200);
#endif
    delay(100);
    ready();
}

void loop() {
    readCommands(Serial);
#if ARDUINO_USB_CDC_ON_BOOT
    readCommands(Serial0);
#endif
    if (!heard && millis() - lastReady > 1000) {
        lastReady = millis();
        ready();
    }
    delay(5);
}
