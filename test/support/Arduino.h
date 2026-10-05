#pragma once
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstdlib>
using std::min;
using std::max;
struct Restarted {};
struct FakeEsp {
    [[noreturn]] void restart() { throw Restarted{}; }
    unsigned getFreeHeap() { return 100000; }
};
inline FakeEsp ESP;
inline uint32_t fakeMillis = 0;
uint32_t millis();
void delay(uint32_t ms);
#define HIGH 1
#define LOW 0
inline int digitalRead(uint8_t) { return HIGH; }
#define INPUT 1
#define INPUT_PULLUP 2
#define INPUT_PULLDOWN 3
#define RTC_DATA_ATTR
inline void pinMode(uint8_t, uint8_t) {}
