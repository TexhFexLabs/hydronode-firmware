#include "Status.h"

#include <Arduino.h>
#include <stdarg.h>

namespace hn::status {

void begin() {
    Serial.begin(115200);
#if ARDUINO_USB_CDC_ON_BOOT
    // Serial is the native USB port here. Boards with a USB-UART bridge are
    // wired to UART0 instead, so every line goes to both.
    Serial0.begin(115200);
#if ARDUINO_USB_MODE
    Serial.setTxTimeoutMs(0);  // never block when no host is attached
#endif
#endif
}

void line(const char* fmt, ...) {
    char buf[160];
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

void flush() {
    Serial.flush();
#if ARDUINO_USB_CDC_ON_BOOT
    Serial0.flush();
#endif
}

}  // namespace hn::status
