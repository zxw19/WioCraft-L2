#include "tex.h"
#include <math.h>
#include "config.h"

uint16_t g_tex[T_COUNT][256];

static inline uint32_t h32(uint32_t x) {
  x ^= x >> 16; x *= 0x7feb352du; x ^= x >> 15; x *= 0x846ca68bu; x ^= x >> 16;
  return x;
}
static inline int nz(int x, int y, uint32_t s) {
  return (int)(h32((uint32_t)(x & 15) * 1973u + (uint32_t)(y & 15) * 9277u + s) & 0xFF);
}
static inline int cl(int v) { return v < 0 ? 0 : (v > 255 ? 255 : v); }
static inline uint16_t C(int r, int g, int b) { return RGB565(cl(r), cl(g), cl(b)); }

// Blocky 4x4 "cluster" noise, the way vanilla textures read at 16px.
static inline int cluster(int x, int y, uint32_t s, int cell) {
  int bx = (x / cell) * cell, by = (y / cell) * cell;
  return nz(bx, by, s);
}

void texInit() {
  for (int y = 0; y < 16; y++) {
    for (int x = 0; x < 16; x++) {
      const int i = y * 16 + x;

      // ---------------- grass top: mid green with tufts ----------------
      {
        int n = nz(x, y, 3);
        int base = 112 + (n % 34);
        if (nz(x, y, 7) > 224) base += 22;          // bright tuft
        if (nz(x, y, 11) < 26) base -= 26;          // dark gap
        g_tex[T_GRASS_TOP][i] = C(base - 46, base + 36, base - 68);
      }

      // ---------------- grass side: dirt + irregular green lip ----------------
      {
        int dirt = 122 + (nz(x, y, 21) % 34);
        if (nz(x, y, 23) > 232) dirt -= 22;
        int lip = 3 + (nz(x, 0, 31) % 3);            // 3..5 rows of green
        if (y < lip) {
          int g = 112 + (nz(x, y, 9) % 30);
          g_tex[T_GRASS_SIDE][i] = C(g - 46, g + 34, g - 66);
        } else if (y == lip) {
          g_tex[T_GRASS_SIDE][i] = C(dirt - 40, dirt + 6, dirt - 58);   // shadow line
        } else {
          g_tex[T_GRASS_SIDE][i] = C(dirt, dirt * 3 / 4, dirt * 11 / 20);
        }
      }

      // ---------------- dirt ----------------
      {
        int d = 120 + (nz(x, y, 41) % 32);
        if (nz(x, y, 43) > 240) d -= 30;
        g_tex[T_DIRT][i] = C(d, d * 3 / 4, d * 11 / 20);
      }

      // ---------------- stone: blotches ----------------
      {
        int s = 112 + (cluster(x, y, 51, 4) % 26) + (nz(x, y, 57) % 14);
        if (nz(x, y, 59) > 236) s -= 24;
        g_tex[T_STONE][i] = C(s, s, s + 3);
      }

      // ---------------- cobblestone: stones + dark mortar ----------------
      {
        int cx2 = (x + ((y / 5) % 2) * 3) / 5;
        int cy2 = y / 5;
        int idn = nz(cx2, cy2, 61);
        int edgeX = ((x + ((y / 5) % 2) * 3) % 5 == 0);
        int edgeY = (y % 5 == 0);
        if (edgeX || edgeY) {
          g_tex[T_COBBLE][i] = C(74, 74, 76);
        } else {
          int v = 108 + (idn % 34) + (nz(x, y, 63) % 12);
          g_tex[T_COBBLE][i] = C(v, v, v + 3);
        }
      }

      // ---------------- sand ----------------
      {
        int s = 214 + (nz(x, y, 71) % 22);
        if (nz(x, y, 73) > 238) s -= 26;
        g_tex[T_SAND][i] = C(s, s - 10, s - 52);
      }

      // ---------------- log side: vertical bark ----------------
      {
        int stripe = nz(x, 0, 81) % 3;
        int b = 92 + stripe * 9 + (nz(x, y, 83) % 12);
        if (stripe == 0) b -= 14;
        g_tex[T_LOG_SIDE][i] = C(b, b * 13 / 20, b * 9 / 20);
      }

      // ---------------- log top: growth rings ----------------
      {
        int dx = x - 8, dy = y - 8;
        int r = (int)(sqrtf((float)(dx * dx + dy * dy)) + 0.5f);
        int v = 156 + ((r & 2) ? 20 : 0) + (nz(x, y, 85) % 10);
        if (r >= 7) v -= 30;                          // bark rim
        g_tex[T_LOG_TOP][i] = C(v, v * 19 / 25, v * 11 / 25);
      }

      // ---------------- leaves: clustered foliage with dark holes ----------------
      {
        int n = nz(x, y, 91);
        if (n > 226) {
          g_tex[T_LEAVES][i] = C(26, 58, 20);          // deep gap
        } else {
          int v = n % 26;
          g_tex[T_LEAVES][i] = C(40 + v, 96 + v * 2, 30 + v);
          if (nz(x, y, 93) > 214) g_tex[T_LEAVES][i] = C(86, 152, 52);  // highlight leaf
        }
      }

      // ---------------- planks: four boards ----------------
      {
        int board = y / 4;
        int v = 158 + (nz(x, board, 95) % 20) + (nz(x, y, 97) % 8);
        if ((y % 4) == 3) v -= 58;                    // seam
        if ((x + board * 5) % 16 == 0) v -= 26;       // board end joint
        g_tex[T_PLANK][i] = C(v, v * 4 / 5, v * 12 / 25);
      }

      // ---------------- glass ----------------
      {
        bool edge = (x == 0 || y == 0 || x == 15 || y == 15);
        bool inner = (x == 1 || y == 1 || x == 14 || y == 14);
        if (edge)       g_tex[T_GLASS][i] = C(226, 240, 244);
        else if (inner) g_tex[T_GLASS][i] = C(186, 210, 220);
        else if ((x >= 3 && x <= 5 && y >= 3 && y <= 12) || (y >= 3 && y <= 4 && x >= 3 && x <= 10))
          g_tex[T_GLASS][i] = C(212, 232, 240);       // highlight streak
        else            g_tex[T_GLASS][i] = C(160, 190, 205);
      }
    }
  }
}
