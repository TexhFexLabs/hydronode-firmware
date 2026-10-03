#pragma once
#include <Arduino.h>
#include <cstdint>
#include <functional>
class Ticker {
public:
    inline static std::function<void()> callback;
    inline static uint32_t deadline = 0;
    void detach() { callback = nullptr; }
    void once_ms(uint32_t ms, std::function<void()> fn) { deadline = millis() + ms; callback = fn; }
};
