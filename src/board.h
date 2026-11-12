#pragma once
#include <Arduino.h>

namespace board {
// Runs the L2 power-up sequence (TCA9535 rails, LCD reset, touch reset).
// Must run before display init.
bool begin();
void setAmp(bool on);      // speaker amplifier rail (expander P12)
void setLed(bool on);
bool sdCardPresent();
bool wakePressed();
uint16_t batteryMilliVolts();
}  // namespace board
