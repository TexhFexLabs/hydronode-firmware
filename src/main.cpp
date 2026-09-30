#include <Arduino.h>

// Skeleton. The real firmware (config partition, drivers, sleep) follows.
void setup() {
    Serial.begin(115200);
    Serial.printf("HN:BOOT fw=%s family=%s\n", HN_FW_VERSION, HN_FAMILY);
}

void loop() {
    delay(1000);
}
