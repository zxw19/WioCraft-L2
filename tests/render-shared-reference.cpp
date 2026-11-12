#include "render.h"
#include <math.h>
#include <string.h>
#include "config.h"
#include "tex.h"
#include "world.h"

#define PLANE 0.75f
#define PROJ  ((VIEW_W * 0.5f) / PLANE)

static inline uint16_t shade(uint16_t c, int f) {   // f: 0..256
  int r = (c >> 11) & 0x1F, g = (c >> 5) & 0x3F, b = c & 0x1F;
  r = (r * f) >> 8; g = (g * f) >> 8; b = (b * f) >> 8;
  if (r > 31) r = 31;
  if (g > 63) g = 63;
  if (b > 31) b = 31;
  return (uint16_t)((r << 11) | (g << 5) | b);
}
static inline uint16_t mixc(uint16_t a, uint16_t b, int t) {   // t: 0..256
  if (t <= 0) return a;
  if (t >= 256) return b;
  int ar = (a >> 11) & 0x1F, ag = (a >> 5) & 0x3F, ab = a & 0x1F;
  int br = (b >> 11) & 0x1F, bg = (b >> 5) & 0x3F, bb = b & 0x1F;
  int r = ar + (((br - ar) * t) >> 8);
  int g = ag + (((bg - ag) * t) >> 8);
  int bl = ab + (((bb - ab) * t) >> 8);
  return (uint16_t)((r << 11) | (g << 5) | bl);
}

namespace render {
uint32_t statCells = 0, statBlocks = 0, statPixels = 0, statOutlines = 0;

// ---- lighting / atmosphere -----------------------------------------------
static inline int dayK(float daylight) {           // 0 = deep night .. 256 = noon
  int k = (int)((daylight - 0.10f) * 256.0f / 0.90f);
  if (k < 0) k = 0;
  if (k > 256) k = 256;
  return k;
}
static inline uint16_t skyZenith(int k)  { return mixc(RGB565(10, 14, 34),   RGB565(112, 174, 250), k); }
static inline uint16_t skyHorizon(int k) { return mixc(RGB565(24, 30, 56),   RGB565(176, 200, 238), k); }

static const float INV_PROJ = 1.0f / PROJ;

// Light changes slowly over the eight-minute day. Cache the five combinations
// used by top/X/Z/buried faces rather than splitting RGB565 for every pixel.
// A four-level step is visually smaller than one RGB565 component step in most
// texels and refreshes the cache only a few times per second while time moves.
static uint16_t s_lit[5][T_COUNT][256];
static int s_cachedLight = -1;
static void updateLightCache(int lightF) {
  int q = (lightF + 2) & ~3;
  if (q > 256) q = 256;
  if (q == s_cachedLight) return;
  s_cachedLight = q;
  const int x = 206 * q >> 8, z = 156 * q >> 8;
  const int factors[5] = {q, x, z, x * 214 >> 8, z * 214 >> 8};
  for (int face = 0; face < 5; face++)
    for (int tex = 0; tex < T_COUNT; tex++)
      for (int p = 0; p < 256; p++)
        s_lit[face][tex][p] = shade(g_tex[tex][p], factors[face]);
}

// ---- sky ------------------------------------------------------------------
static inline uint32_t chash(int x, int z) {
  uint32_t h = (uint32_t)x * 374761393u + (uint32_t)z * 668265263u + 0x9E3779B9u;
  h ^= h >> 13; h *= 1274126177u; h ^= h >> 16;
  return h;
}

static void sunBody(uint16_t* fb, int cx, int cy, int r, int k, uint16_t core) {
  for (int y = -r; y <= r; y++) {
    int py = cy + y;
    if (py < 0 || py >= VIEW_H) continue;
    uint16_t* p = fb + py * VIEW_W;
    for (int x = -r; x <= r; x++) {
      int pxx = cx + x;
      if (pxx < 0 || pxx >= VIEW_W) continue;
      int d2 = x * x + y * y;
      if (d2 <= (r - 2) * (r - 2)) {
        p[pxx] = core;
      } else if (d2 <= r * r) {
        p[pxx] = mixc(p[pxx], core, 110 + k / 4);
      } else {
        p[pxx] = mixc(p[pxx], core, 60);
      }
    }
  }
}

void fillSky(uint16_t* fb, const Camera& cam, float daylight, float timeOfDay) {
  const int k = dayK(daylight);
  const uint16_t zen = skyZenith(k);
  const uint16_t hor = skyHorizon(k);

  int horizon = cam.horizon;
  if (horizon < 1) horizon = 1;
  if (horizon > VIEW_H) horizon = VIEW_H;

  for (int y = 0; y < VIEW_H; y++) {
    int t = (y < horizon) ? (y * 256 / horizon) : 256;
    uint16_t c = mixc(zen, hor, t);
    if (y > horizon) c = mixc(c, RGB565(26, 30, 26), 110);   // looking into a pit
    uint16_t* p = fb + y * VIEW_W;
    for (int x = 0; x < VIEW_W; x++) p[x] = c;
  }

  const float fx = cosf(cam.yaw), fz = sinf(cam.yaw);
  const float rx = -sinf(cam.yaw), rz = cosf(cam.yaw);

  // cam yaw is always 0 here, keep the trig anyway for correctness
  (void)fx; (void)fz; (void)rx; (void)rz;

  // ---- sun / moon ----
  {
    const float az = 0.85f;
    float ang = (timeOfDay - 0.25f) * 6.2831853f;
    float ca = cosf(ang), sa = sinf(ang);
    float dx = ca * cosf(az), dz = ca * sinf(az), dy = sa;

    for (int m = 0; m < 2; m++) {
      float sx = m ? -dx : dx, sy = m ? -dy : dy, sz = m ? -dz : dz;
      if (m == 0 && sy < -0.08f) continue;
      if (m == 1 && (k > 150 || sy < -0.08f)) continue;
      float depth = sx * fx + sz * fz;
      if (depth < 0.15f) continue;
      float lat = sx * rx + sz * rz;
      int px = (int)(VIEW_W * 0.5f + lat / depth * PROJ);
      int py = horizon - (int)(sy / depth * PROJ);
      if (py < -30 || py > VIEW_H + 30) continue;
      if (m == 0)
        sunBody(fb, px, py, 9, k, RGB565(255, 246, 200));
      else
        sunBody(fb, px, py, 7, k, RGB565(226, 232, 244));
    }
  }

  // ---- cloud deck ----
  {
    const float cloudY = 88.0f;
    float dyc = cloudY - cam.y;
    if (dyc > 4.0f && k > 30) {
      static float cdx[VIEW_W], cdz[VIEW_W];
      for (int col = 0; col < VIEW_W; col++) {
        float sxf = ((float)col - VIEW_W * 0.5f) / (VIEW_W * 0.5f);
        float dx2 = fx + rx * sxf * PLANE;
        float dz2 = fz + rz * sxf * PLANE;
        float len2 = sqrtf(dx2 * dx2 + dz2 * dz2);
        if (len2 < 1e-6f) len2 = 1e-6f;
        cdx[col] = dx2 / len2;
        cdz[col] = dz2 / len2;
      }
      const uint16_t cloudCol = mixc(RGB565(120, 130, 150), RGB565(250, 252, 255), k);
      const int top = horizon > 44 ? horizon - 44 : 0;
      for (int rr = top; rr < horizon; rr += 2) {
        int denom = horizon - rr;
        if (denom <= 0) continue;
        float dd = dyc * PROJ / (float)denom;
        if (dd > 300.0f) continue;
        int fade = 210 - (int)(dd * 150.0f / 300.0f);
        if (fade < 40) continue;
        uint16_t* p0 = fb + rr * VIEW_W;
        uint16_t* p1 = (rr + 1 < VIEW_H) ? (p0 + VIEW_W) : p0;
        for (int col = 0; col < VIEW_W; col += 2) {
          float wx = cam.x + cdx[col] * dd;
          float wz = cam.z + cdz[col] * dd;
          int cx = (int)floorf(wx * 0.0833f);     // 12 blocks per cell
          int cz = (int)floorf(wz * 0.0833f);
          if ((chash(cx, cz) & 255) < 150) continue;
          uint16_t v = mixc(p0[col], cloudCol, fade);
          p0[col] = v; p0[col + 1] = v;
          p1[col] = v; p1[col + 1] = v;
        }
      }
    }
  }
}

// ---- terrain --------------------------------------------------------------
// All screen rows in one column share an X/Z ray. Traverse those horizontal
// cells once, copy each vertical world column to internal RAM, and retain an
// independent Y DDA for every unresolved pixel. This is the same nearest-hit
// 3-D traversal as a ray per pixel, without repeated PSRAM voxel fetches.
void world(const Camera& cam, uint16_t* fb, float daylight) {
  statCells = statBlocks = statPixels = 0;
  const int k = dayK(daylight);
  const int lightF = 118 + (k * 138 >> 8);
  updateLightCache(lightF);
  const float fx = cosf(cam.yaw), fz = sinf(cam.yaw);
  const float rx = -fz, rz = fx;
  const int horizon = cam.horizon;
  const uint16_t fogCol = skyHorizon(k);
  const int startX = (int)floorf(cam.x), startY = (int)floorf(cam.y), startZ = (int)floorf(cam.z);
  if (startX < 0 || startX >= WORLD_X || startZ < 0 || startZ >= WORLD_Z ||
      startY < 0 || startY >= WORLD_H) return;
  struct RowRay { float dy, tyd, ty; int iy, stepY; };
  static RowRay initialRows[VIEW_H];
  RowRay rows[VIEW_H];
  uint8_t active[VIEW_H];
  uint8_t blocks[WORLD_H];
  for (int row = 0; row < VIEW_H; ++row) {
    RowRay& r = initialRows[row];
    r.dy = ((float)horizon - ((float)row + 0.5f)) * INV_PROJ;
    r.stepY = r.dy >= 0 ? 1 : -1;
    const float ay = fabsf(r.dy);
    r.tyd = ay > 1e-7f ? 1.0f / ay : 1e9f;
    r.ty = ay > 1e-7f ? (r.dy >= 0 ? startY + 1 - cam.y : cam.y - startY) * r.tyd : 1e9f;
    r.iy = startY;
  }
  for (int col = 0; col < VIEW_W; ++col) {
    const float sx = (((float)col + 0.5f) - VIEW_W * 0.5f) * INV_PROJ;
    const float dx = fx + rx * sx, dz = fz + rz * sx;
    const int stepX = dx >= 0 ? 1 : -1, stepZ = dz >= 0 ? 1 : -1;
    const float ax = fabsf(dx), az = fabsf(dz);
    const float txd = ax > 1e-7f ? 1.0f / ax : 1e9f;
    const float tzd = az > 1e-7f ? 1.0f / az : 1e9f;
    float tx = ax > 1e-7f ? (dx >= 0 ? startX + 1 - cam.x : cam.x - startX) * txd : 1e9f;
    float tz = az > 1e-7f ? (dz >= 0 ? startZ + 1 - cam.z : cam.z - startZ) * tzd : 1e9f;
    memcpy(rows, initialRows, sizeof(rows));
    for (int row = 0; row < VIEW_H; ++row) active[row] = (uint8_t)row;
    int count = VIEW_H, ix = startX, iz = startZ;
    int entryAxis = -1, entrySign = 0;
    float t0 = 0.0f;
    for (int guard = 0; guard < 160 && count && t0 <= MAX_RAY_DIST; ++guard) {
      if (ix < 0 || ix >= WORLD_X || iz < 0 || iz >= WORLD_Z) break;
      statCells++;
      world::column(ix, iz, blocks, WORLD_H);
      const bool exitX = tx <= tz;
      float t1 = exitX ? tx : tz;
      const bool atLimit = t1 > MAX_RAY_DIST;
      if (atLimit) t1 = MAX_RAY_DIST;
      for (int a = 0; a < count; ) {
        const int row = active[a];
        RowRay& r = rows[row];
        float enter = t0;
        int faceAxis = entryAxis, faceSign = entrySign;
        uint8_t block = B_AIR;
        bool hit = false, finished = false;
        for (;;) {
          if ((unsigned)r.iy >= WORLD_H) { finished = true; break; }
          block = blocks[r.iy];
          statBlocks++;
          if (block != B_AIR && block < B_COUNT && BLOCKS[block].solid) {
            hit = faceAxis >= 0;
            finished = true;
            break;
          }
          // Preserve the full DDA tie order X, Y, Z at voxel corners.
          if (r.ty > t1 || (exitX && !atLimit && r.ty == t1)) break;
          enter = r.ty;
          r.ty += r.tyd;
          r.iy += r.stepY;
          faceAxis = 1; faceSign = -r.stepY;
        }
        if (hit) {
          const float hx = cam.x + dx * enter;
          const float hy = cam.y + r.dy * enter;
          const float hz = cam.z + dz * enter;
          float u, v;
          int face;
          const BlockDef& bd = BLOCKS[block];
          if (faceAxis == 1) {
            u = hx - floorf(hx); v = hz - floorf(hz);
            if (faceSign < 0) { u = 1.0f - u; v = 1.0f - v; }
            face = faceSign > 0 ? 0 : 3;
          } else if (faceAxis == 0) {
            u = hz - floorf(hz); v = 1.0f - (hy - floorf(hy));
            if (faceSign < 0) u = 1.0f - u;
            face = 1;
          } else {
            u = hx - floorf(hx); v = 1.0f - (hy - floorf(hy));
            if (faceSign > 0) u = 1.0f - u;
            face = 2;
          }
          int tu = (int)(u * 16.0f); if (tu < 0) tu = 0; if (tu > 15) tu = 15;
          int tv = (int)(v * 16.0f); if (tv < 0) tv = 0; if (tv > 15) tv = 15;
          const int texture = faceAxis == 1 ? (faceSign < 0 ? bd.bottom : bd.top) : bd.side;
          uint16_t color = s_lit[face][texture][tv * 16 + tu];
          int fog = (int)((enter - 14.0f) * 6.0f);
          if (fog > 202) fog = 202;
          if (fog > 0) color = mixc(color, fogCol, fog);
          fb[row * VIEW_W + col] = color;
          statPixels++;
        }
        if (finished) active[a] = active[--count];
        else ++a;
      }
      if (atLimit) break;
      t0 = t1;
      if (exitX) { ix += stepX; tx += txd; entryAxis = 0; entrySign = -stepX; }
      else { iz += stepZ; tz += tzd; entryAxis = 2; entrySign = -stepZ; }
    }
  }
}

// ---- picking -------------------------------------------------------------
bool pick(const Camera& cam, float maxDist, int& bx, int& by, int& bz,
          int& nx, int& ny, int& nz) {
  float dx = cosf(cam.yaw);
  float dy = ((float)cam.horizon - VIEW_H * 0.5f) / PROJ;
  float dz = sinf(cam.yaw);
  float len = sqrtf(dx * dx + dy * dy + dz * dz);
  dx /= len; dy /= len; dz /= len;

  float px = cam.x, py = cam.y, pz = cam.z;
  int ix = (int)floorf(px), iy = (int)floorf(py), iz = (int)floorf(pz);
  int sx = (dx > 0) ? 1 : -1, sy2 = (dy > 0) ? 1 : -1, sz = (dz > 0) ? 1 : -1;
  float adx = fabsf(dx), ady = fabsf(dy), adz = fabsf(dz);
  float tdx = (adx > 1e-6f) ? 1.0f / adx : 1e9f;
  float tdy = (ady > 1e-6f) ? 1.0f / ady : 1e9f;
  float tdz = (adz > 1e-6f) ? 1.0f / adz : 1e9f;
  float tmx = (adx > 1e-6f) ? ((dx > 0 ? (ix + 1 - px) : (px - ix)) / adx) : 1e9f;
  float tmy = (ady > 1e-6f) ? ((dy > 0 ? (iy + 1 - py) : (py - iy)) / ady) : 1e9f;
  float tmz = (adz > 1e-6f) ? ((dz > 0 ? (iz + 1 - pz) : (pz - iz)) / adz) : 1e9f;
  if (tmx < 0) tmx = 0;
  if (tmy < 0) tmy = 0;
  if (tmz < 0) tmz = 0;

  nx = ny = nz = 0;
  float t = 0;
  while (t <= maxDist) {
    if (world::solid(ix, iy, iz)) {
      bx = ix; by = iy; bz = iz;
      return true;
    }
    if (tmx < tmy && tmx < tmz) {
      ix += sx; t = tmx; tmx += tdx; nx = -sx; ny = 0; nz = 0;
    } else if (tmy < tmz) {
      iy += sy2; t = tmy; tmy += tdy; nx = 0; ny = -sy2; nz = 0;
    } else {
      iz += sz; t = tmz; tmz += tdz; nx = 0; ny = 0; nz = -sz;
    }
    if (iy < 0 || iy >= WORLD_H) break;
  }
  return false;
}

// ---- selection outline ---------------------------------------------------
static bool project(const Camera& cam, float wx, float wy, float wz, int& sx, int& sy) {
  float dx = wx - cam.x, dy = wy - cam.y, dz = wz - cam.z;
  float fx = cosf(cam.yaw), fz = sinf(cam.yaw);
  float rx = -sinf(cam.yaw), rz = cosf(cam.yaw);
  float depth = dx * fx + dz * fz;
  float lat = dx * rx + dz * rz;
  if (depth < 0.25f) return false;
  sx = (int)(VIEW_W * 0.5f + lat / depth * PROJ);
  sy = cam.horizon - (int)(dy / depth * PROJ);
  return true;
}

static void line(uint16_t* fb, int x0, int y0, int x1, int y1, uint16_t c) {
  int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
  int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
  int err = dx + dy;
  for (int guard = 0; guard < 400; guard++) {
    if (x0 >= 0 && x0 < VIEW_W && y0 >= 0 && y0 < VIEW_H) fb[y0 * VIEW_W + x0] = c;
    if (x0 == x1 && y0 == y1) break;
    int e2 = 2 * err;
    if (e2 >= dy) { err += dy; x0 += sx; }
    if (e2 <= dx) { err += dx; y0 += sy; }
  }
}

void outline(uint16_t* fb, const Camera& cam, int bx, int by, int bz, uint16_t col) {
  float x0 = (float)bx, y0 = (float)by, z0 = (float)bz;
  float x1 = x0 + 1, y1 = y0 + 1, z1 = z0 + 1;
  int px[8], py[8];
  bool ok[8];
  const float cx[8] = {x0, x1, x1, x0, x0, x1, x1, x0};
  const float cy[8] = {y0, y0, y0, y0, y1, y1, y1, y1};
  const float cz[8] = {z0, z0, z1, z1, z0, z0, z1, z1};
  for (int i = 0; i < 8; i++) ok[i] = project(cam, cx[i], cy[i], cz[i], px[i], py[i]);
  static const uint8_t E[12][2] = {{0,1},{1,2},{2,3},{3,0},{4,5},{5,6},{6,7},{7,4},{0,4},{1,5},{2,6},{3,7}};
  for (int i = 0; i < 12; i++) {
    int a = E[i][0], b = E[i][1];
    if (!ok[a] || !ok[b]) continue;
    line(fb, px[a], py[a], px[b], py[b], col);
  }
}

}  // namespace render
