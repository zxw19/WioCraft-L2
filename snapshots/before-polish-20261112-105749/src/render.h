#pragma once
#include <stdint.h>

struct Camera {
  float x, y, z;
  float yaw;      // radians, 0 = +X
  int horizon;    // screen row of the horizon
};

namespace render {
extern uint32_t statCells, statBlocks, statPixels, statOutlines;
void fillSky(uint16_t* fb, const Camera& cam, float daylight, float timeOfDay);
void world(const Camera& cam, uint16_t* fb, float daylight);
void outline(uint16_t* fb, const Camera& cam, int bx, int by, int bz, uint16_t col);
bool pick(const Camera& cam, float maxDist,
          int& bx, int& by, int& bz, int& nx, int& ny, int& nz);
}
