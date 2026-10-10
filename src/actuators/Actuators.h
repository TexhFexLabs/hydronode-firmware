#pragma once

// Outputs (relay, LED, switched output) and buttons.
//
// Outputs are switched by HydroNode commands named after the device ("relay1"): BOOL switches
// on or off, UINT32 switches on for that many milliseconds, "<name>_level" (UINT32, 0..100) dims
// an LED. Commands arrive with the response to a sent value, so they take effect within one
// round. Buttons report a press right away (value 1) and can toggle an output on the board.

#include "config/Config.h"
#include "power/Power.h"

class HydroNode;

namespace hn::act {

// True for the driver ids handled here instead of by a sensor driver.
bool isActuator(const char* driver);

// Sets up every output in its start state and every button pin. Call once after boot.
// `safe`: the battery is in Recovery or Standby, every output starts off (the remembered state
// stays stored for when the battery recovered).
void begin(const Config& cfg, bool safe = false);

// Battery low: every output off, timed pulses ended, held off through sleep. The remembered
// state is not touched, so the outputs come back as they were after the next normal boot.
void safeOff();

// The battery recovered: every output back to its remembered state.
void resume();

// Registers the command callbacks.
void attach(HydroNode& hydro);

// Ends timed pulses and reads the buttons. Call at least every 10 ms while awake.
void service();

// After an ESP8266 light sleep, during which millis() stood still: moves the timed switch-offs
// and the button debounce on by the time slept and switches off what is due now.
void advance(uint32_t sleptMs);

// Something needs service() soon: a button to watch or a pulse still running.
bool busy();

// Milliseconds until the next timed switch-off, 0 when nothing runs.
uint32_t pendingMs();

// A button press waiting to be sent; returns its measurement type or nullptr.
const char* takePress();

// Buttons as pins that wake the board (light and deep sleep), at most `cap`.
uint8_t wakeInputs(power::WakeInput* out, uint8_t cap);

// A button on `pin` woke the board: handle the press (toggle, queue) once. False for no button.
bool wakePress(int8_t pin);

// A press is waiting to be sent.
bool hasPresses();

}  // namespace hn::act
