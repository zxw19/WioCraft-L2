#pragma once
#include <stdint.h>

namespace input {
enum Zone : uint8_t { NONE, JOY, LOOK, BAR, JUMP, DIG, PLACE, CRAFT, HELP };
struct Point { int x, y, id; };
struct Frame {
  float forward = 0, side = 0, yawDelta = 0, pitchDelta = 0;
  float joyX = 0, joyY = 0;
  bool joy = false, jump = false, mine = false, place = false;
  bool digHeld = false, placeHeld = false, jumpHeld = false;
  bool craft = false, help = false, touching = false;
  int slot = -1;
};
// ID tracking locks the original region until a debounced release.
Frame update(const Point* points, int count, float dt);
void reset();
Zone zoneAt(int x, int y);
}
