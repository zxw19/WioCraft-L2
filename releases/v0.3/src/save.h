#pragma once
#include <stdint.h>

struct SaveState {
  float x, y, z, yaw, pitchOff;
  float dayTime;
  int32_t hp, sel;
  uint16_t inventory[9];
};

namespace save {
bool begin();
bool ready();
bool load(SaveState& s);
// Writes a complete snapshot and returns true only after the final rename.
// A failed write leaves the previous save intact.
bool store(const SaveState& s);
}
