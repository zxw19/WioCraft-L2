#include "world.h"
#include <math.h>
#include <string.h>
#include <esp_heap_caps.h>
#include <esp32-hal-psram.h>
#include "config.h"
#include "tex.h"

const BlockDef BLOCKS[B_COUNT] = {
  /* air    */ {0, 0, 0, 0, 0},
  /* grass  */ {T_GRASS_TOP, T_GRASS_SIDE, T_DIRT, 1, 6},
  /* dirt   */ {T_DIRT, T_DIRT, T_DIRT, 1, 5},
  /* stone  */ {T_STONE, T_STONE, T_STONE, 1, 30},
  /* sand   */ {T_SAND, T_SAND, T_SAND, 1, 5},
  /* log    */ {T_LOG_TOP, T_LOG_SIDE, T_LOG_TOP, 1, 20},
  /* leaves */ {T_LEAVES, T_LEAVES, T_LEAVES, 1, 2},
  /* plank  */ {T_PLANK, T_PLANK, T_PLANK, 1, 20},
  /* cobble */ {T_COBBLE, T_COBBLE, T_COBBLE, 1, 30},
  /* glass  */ {T_GLASS, T_GLASS, T_GLASS, 1, 3},
};

namespace world {

static uint8_t* s_blocks = nullptr;      // PSRAM, y-major: (y*WORLD_Z + z)*WORLD_X + x
static uint8_t* s_height = nullptr;      // PSRAM
static uint32_t s_seed = 1;

static Edit* s_edits = nullptr;
static int s_editCount = 0;
static const int MAX_EDITS = 8192;

static inline uint32_t h32(uint32_t x) {
  x ^= x >> 16; x *= 0x7feb352du; x ^= x >> 15; x *= 0x846ca68bu; x ^= x >> 16;
  return x;
}
static inline float hashf(int x, int z, uint32_t s) {
  return (float)(h32((uint32_t)x * 374761393u + (uint32_t)z * 668265263u + s) & 0xFFFF) / 65535.0f;
}
static float vnoise(float x, float z, uint32_t s) {
  int xi = (int)floorf(x), zi = (int)floorf(z);
  float fx = x - xi, fz = z - zi;
  fx = fx * fx * (3 - 2 * fx);
  fz = fz * fz * (3 - 2 * fz);
  float a = hashf(xi, zi, s), b = hashf(xi + 1, zi, s);
  float c = hashf(xi, zi + 1, s), d = hashf(xi + 1, zi + 1, s);
  return (a * (1 - fx) + b * fx) * (1 - fz) + (c * (1 - fx) + d * fx) * fz;
}
static float fbm(float x, float z, uint32_t s, int oct) {
  float sum = 0, amp = 1, norm = 0, f = 1;
  for (int i = 0; i < oct; i++) {
    sum += vnoise(x * f, z * f, s + i * 131) * amp;
    norm += amp;
    amp *= 0.5f;
    f *= 2.0f;
  }
  return sum / norm;
}

// y-contiguous: a whole vertical column lives in ~2 cache lines instead of 56
static inline int idx(int x, int y, int z) { return (x * WORLD_Z + z) * WORLD_H + y; }
static inline bool inWorld(int x, int y, int z) {
  return x >= 0 && x < WORLD_X && z >= 0 && z < WORLD_Z && y >= 0 && y < WORLD_H;
}

static void recalcColumn(int x, int z) {
  int h = 0;
  for (int y = WORLD_H - 1; y >= 0; y--) {
    if (s_blocks[idx(x, y, z)] != B_AIR) { h = y + 1; break; }
  }
  s_height[z * WORLD_X + x] = (uint8_t)h;
}

static void put(int x, int y, int z, uint8_t b) {
  if (!inWorld(x, y, z)) return;
  s_blocks[idx(x, y, z)] = b;
}

static void genTree(int x, int z, int ground) {
  uint32_t hv = h32((uint32_t)x * 7919u + (uint32_t)z * 104729u + s_seed);
  int trunk = 4 + (int)(hv % 3);
  int top = ground + trunk;
  for (int y = ground; y < top && y < WORLD_H; y++) put(x, y, z, B_LOG);
  for (int dy = -2; dy <= 1; dy++) {
    int r = (dy >= 1) ? 1 : 2;
    for (int dz = -r; dz <= r; dz++) {
      for (int dx = -r; dx <= r; dx++) {
        if (abs(dx) == r && abs(dz) == r && r == 2 && (hv & 1)) continue;
        int yy = top + dy;
        if (yy < 0 || yy >= WORLD_H) continue;
        if (s_blocks[idx((x + dx + WORLD_X) % WORLD_X, yy, (z + dz + WORLD_Z) % WORLD_Z)] == B_AIR) {
          put((x + dx + WORLD_X) % WORLD_X, yy, (z + dz + WORLD_Z) % WORLD_Z, B_LEAVES);
        }
      }
    }
  }
}

bool init(uint32_t seed) {
  s_seed = seed ? seed : 1;
  s_blocks = (uint8_t*)ps_malloc((size_t)WORLD_X * WORLD_Z * WORLD_H);
  s_height = (uint8_t*)ps_malloc((size_t)WORLD_X * WORLD_Z);
  s_edits = (Edit*)ps_malloc(sizeof(Edit) * MAX_EDITS);
  if (!s_blocks || !s_height || !s_edits) return false;
  memset(s_blocks, 0, (size_t)WORLD_X * WORLD_Z * WORLD_H);
  memset(s_height, 0, (size_t)WORLD_X * WORLD_Z);
  s_editCount = 0;

  for (int z = 0; z < WORLD_Z; z++) {
    for (int x = 0; x < WORLD_X; x++) {
      float base = fbm(x * 0.018f, z * 0.018f, s_seed, 4);
      float ridge = fbm(x * 0.006f + 11.3f, z * 0.006f - 4.7f, s_seed + 977, 2);
      int h = 20 + (int)(base * 12.0f) + (int)(ridge * ridge * 16.0f);
      if (h < 6) h = 6;
      if (h > 40) h = 40;

      float bio = fbm(x * 0.022f + 31.0f, z * 0.022f + 17.0f, s_seed + 5000, 2);
      uint8_t topBlock = (bio < 0.30f) ? B_SAND : B_GRASS;

      for (int y = 0; y <= h; y++) {
        uint8_t b;
        if (y == h) b = topBlock;
        else if (y > h - 3) b = (topBlock == B_SAND) ? B_SAND : B_DIRT;
        else b = B_STONE;
        put(x, y, z, b);
      }
      recalcColumn(x, z);
    }
  }

  // trees
  for (int z = 2; z < WORLD_Z - 2; z++) {
    for (int x = 2; x < WORLD_X - 2; x++) {
      int h = s_height[z * WORLD_X + x];
      if (h < SEA_LEVEL + 1 || h > WORLD_H - 12) continue;
      if (get(x, h - 1, z) != B_GRASS) continue;
      uint32_t hv = h32((uint32_t)x * 3571u + (uint32_t)z * 6151u + s_seed + 12345u);
      float bio = fbm(x * 0.022f + 31.0f, z * 0.022f + 17.0f, s_seed + 5000, 2);
      int chance = (bio > 0.55f) ? 30 : 8;
      if ((int)(hv % 1000) < chance) genTree(x, z, h);
    }
  }

  for (int z = 0; z < WORLD_Z; z++)
    for (int x = 0; x < WORLD_X; x++) recalcColumn(x, z);

  return true;
}

uint8_t get(int x, int y, int z) {
  if (!inWorld(x, y, z)) return B_AIR;
  return s_blocks[idx(x, y, z)];
}

bool solid(int x, int y, int z) {
  if (!inWorld(x, y, z)) return false;
  uint8_t b = s_blocks[idx(x, y, z)];
  return b != B_AIR && BLOCKS[b].solid;
}

void set(int x, int y, int z, uint8_t b) {
  if (!inWorld(x, y, z)) return;
  uint8_t old = s_blocks[idx(x, y, z)];
  if (old == b) return;
  s_blocks[idx(x, y, z)] = b;
  recalcColumn(x, z);
  if (s_editCount < MAX_EDITS) {
    s_edits[s_editCount].x = (int16_t)x;
    s_edits[s_editCount].y = (int16_t)y;
    s_edits[s_editCount].z = (int16_t)z;
    s_edits[s_editCount].b = b;
    s_editCount++;
  }
}

void column(int x, int z, uint8_t* dst, int len) {
  if (x < 0 || x >= WORLD_X || z < 0 || z >= WORLD_Z || len <= 0) return;
  if (len > WORLD_H) len = WORLD_H;
  memcpy(dst, s_blocks + (x * WORLD_Z + z) * WORLD_H, len);
}

int height(int x, int z) {
  if (x < 0 || x >= WORLD_X || z < 0 || z >= WORLD_Z) return 0;
  return s_height[z * WORLD_X + x];
}

int editCount() { return s_editCount; }
const Edit* editAt(int i) { return (i >= 0 && i < s_editCount) ? &s_edits[i] : nullptr; }
void clearEdits() { s_editCount = 0; }

void applyEdit(const Edit& e, bool log) {
  if (!inWorld(e.x, e.y, e.z)) return;
  s_blocks[idx(e.x, e.y, e.z)] = e.b;
  recalcColumn(e.x, e.z);
  if (log && s_editCount < MAX_EDITS) s_edits[s_editCount++] = e;
}

}  // namespace world
