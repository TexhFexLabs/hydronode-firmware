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
