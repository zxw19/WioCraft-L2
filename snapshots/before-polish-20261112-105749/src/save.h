#pragma once
#include <stdint.h>

struct SaveState {
  float x, y, z, yaw, pitchOff;
  float dayTime;
  int32_t hp, sel;
};

namespace save {
bool begin();
bool ready();
bool load(SaveState& s);
void store(const SaveState& s);
}
