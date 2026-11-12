#pragma once
#include <stdint.h>

enum : uint8_t {
  B_AIR = 0, B_GRASS, B_DIRT, B_STONE, B_SAND,
  B_LOG, B_LEAVES, B_PLANK, B_COBBLE, B_GLASS, B_COUNT
};

struct BlockDef {
  uint8_t top, side, bottom;
  uint8_t solid;
  uint8_t hardness10;   // 1/10 s units
};

extern const BlockDef BLOCKS[B_COUNT];

struct Edit { int16_t x, y, z; uint8_t b; };

namespace world {
bool init(uint32_t seed);
uint8_t get(int x, int y, int z);
void set(int x, int y, int z, uint8_t b);
int height(int x, int z);          // topmost solid + 1
void column(int x, int z, uint8_t* dst, int len);  // block ids, bottom-up
bool solid(int x, int y, int z);
int editCount();
const Edit* editAt(int i);
void applyEdit(const Edit& e, bool log);
void clearEdits();
}
