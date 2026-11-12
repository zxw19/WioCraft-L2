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

static inline int rowAtS(float wy, float scale, const Camera& c) {
  return c.horizon - (int)((wy - c.y) * scale);
}

// Xtensa has no hardware float divide (~180 cycles each). Every per-pixel
// divide in the inner loops is replaced by a table/multiply.
static const float INV_PROJ = 1.0f / PROJ;
static float s_invN[513];
static bool s_invNReady = false;
static void initInvN() {
  if (s_invNReady) return;
  for (int i = 0; i < 513; i++) {
    int n = i - 256;
    s_invN[i] = (n == 0) ? 0.0f : 1.0f / (float)n;
  }
  s_invNReady = true;
}
static inline float invN(int n) {
  if (n < -256) n = -256;
  if (n > 256) n = 256;
  return s_invN[n + 256];
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
void world(const Camera& cam, uint16_t* fb, float daylight) {
  initInvN();
  statCells = statBlocks = statPixels = 0;
  const int k = dayK(daylight);
  const int lightF = 118 + (k * 138 >> 8);           // 118..256

  const float fx = cosf(cam.yaw), fz = sinf(cam.yaw);
  const float rx = -sinf(cam.yaw), rz = cosf(cam.yaw);

  const int horizon = cam.horizon;
  const uint16_t fogCol = skyHorizon(k);

  int paintedFromBottom;
  static const int MASK_BYTES = (VIEW_H + 7) / 8;
  uint8_t mask[MASK_BYTES];

  for (int col = 0; col < VIEW_W; col++) {
    memset(mask, 0, MASK_BYTES);
    paintedFromBottom = VIEW_H;

    float sxf = ((float)col - VIEW_W * 0.5f) / (VIEW_W * 0.5f);
    float dx = fx + rx * sxf * PLANE;
    float dz = fz + rz * sxf * PLANE;
    float len = sqrtf(dx * dx + dz * dz);
    if (len < 1e-6f) continue;
    dx /= len; dz /= len;

    int cx = (int)floorf(cam.x), cz = (int)floorf(cam.z);
    int stepX = (dx > 0) ? 1 : -1;
    int stepZ = (dz > 0) ? 1 : -1;
    float adx = fabsf(dx), adz = fabsf(dz);
    float tDeltaX = (adx > 1e-6f) ? 1.0f / adx : 1e9f;
    float tDeltaZ = (adz > 1e-6f) ? 1.0f / adz : 1e9f;
    float tMaxX = (adx > 1e-6f) ? ((dx > 0 ? (cx + 1 - cam.x) : (cam.x - cx)) / adx) : 1e9f;
    float tMaxZ = (adz > 1e-6f) ? ((dz > 0 ? (cz + 1 - cam.z) : (cam.z - cz)) / adz) : 1e9f;
    if (tMaxX < 0) tMaxX = 0;
    if (tMaxZ < 0) tMaxZ = 0;

    float t = 0.0f;
    while (t < MAX_RAY_DIST) {
      int axis;
      if (tMaxX < tMaxZ) {
        cx += stepX; t = tMaxX; tMaxX += tDeltaX; axis = 0;
      } else {
        cz += stepZ; t = tMaxZ; tMaxZ += tDeltaZ; axis = 1;
      }
      if (cx < 0 || cx >= WORLD_X || cz < 0 || cz >= WORLD_Z) break;
      if (t <= 0.0001f) continue;

      statCells++;
      int h = world::height(cx, cz);
      if (h <= 0) continue;

      float tExit = (tMaxX < tMaxZ) ? tMaxX : tMaxZ;
      if (tExit > MAX_RAY_DIST) tExit = MAX_RAY_DIST;

      float ex = cam.x + dx * t, ez = cam.z + dz * t;
      float u = (axis == 0) ? (ez - floorf(ez)) : (ex - floorf(ex));
      int tu = (int)(u * 16.0f) & 15;

      const float sc = PROJ / t;
      int cellTop = rowAtS((float)h, sc, cam);
      if (cellTop >= VIEW_H) continue;          // below viewport: keep marching
      if (cellTop >= paintedFromBottom) break;  // fully hidden

      uint8_t colbuf[WORLD_H];
      world::column(cx, cz, colbuf, h);

      // X faces face the light a little more than Z faces
      int faceShade = (axis == 0) ? (206 * lightF >> 8) : (156 * lightF >> 8);

      int fogT = (int)((t - 14.0f) * 6.0f);
      if (fogT < 0) fogT = 0;
      if (fogT > 202) fogT = 202;

      for (int y = h - 1; y >= 0; y--) {
        statBlocks++;
        uint8_t b = colbuf[y];
        if (b == B_AIR) continue;

        int r0 = rowAtS((float)(y + 1), sc, cam);
        int r1 = rowAtS((float)y, sc, cam);
        if (r0 >= paintedFromBottom) break;
        if (r0 < 0) r0 = 0;
        if (r1 > VIEW_H) r1 = VIEW_H;
        if (r1 <= r0) r1 = r0 + 1;
        if (r1 <= 0 || r0 >= VIEW_H) continue;

        const BlockDef& bd = BLOCKS[b];
        const uint16_t* tex = g_tex[bd.side];
        // blocks buried under the surface are darker (cheap ambient occlusion)
        int shadeF = faceShade;
        if (y < h - 2) shadeF = shadeF * 214 >> 8;
        for (int r = r0; r < r1; r++) {
          if (mask[r >> 3] & (1 << (r & 7))) continue;
          mask[r >> 3] |= (1 << (r & 7));
          statPixels++;
          float wy = cam.y - (float)(r - horizon) * t * INV_PROJ;
          int tv = (int)((wy - (float)y) * 16.0f) & 15;
          uint16_t c = shade(tex[tv * 16 + tu], shadeF);
          c = mixc(c, fogCol, fogT);
          fb[r * VIEW_W + col] = c;
        }

        if (y == h - 1 && cam.y > (float)(y + 1)) {
          int rn = rowAtS((float)(y + 1), sc, cam);
          int rf = rowAtS((float)(y + 1), PROJ / tExit, cam);
          if (rf < rn) { int tmp = rn; rn = rf; rf = tmp; }
          if (rf < 0) rf = 0;
          if (rn > VIEW_H) rn = VIEW_H;
          const uint16_t* tt = g_tex[bd.top];
          if (rn <= rf) rn = rf + 1;
          const float ddBase = (cam.y - (float)(y + 1)) * PROJ;
          for (int r = rf; r < rn; r++) {
            if (r == horizon) continue;
            if (mask[r >> 3] & (1 << (r & 7))) continue;
            float dd = ddBase * invN(r - horizon);
            if (dd < 0.2f) continue;
            float px = cam.x + dx * dd, pz = cam.z + dz * dd;
            int uu = (int)((px - floorf(px)) * 16.0f) & 15;
            int vv = (int)((pz - floorf(pz)) * 16.0f) & 15;
            mask[r >> 3] |= (1 << (r & 7));
            uint16_t c = shade(tt[vv * 16 + uu], lightF);
            int ft = (int)((dd - 14.0f) * 6.0f);
            if (ft < 0) ft = 0;
            if (ft > 202) ft = 202;
            fb[r * VIEW_W + col] = mixc(c, fogCol, ft);
          }
        }
      }

      int p = VIEW_H;
      while (p > 0) {
        int r = p - 1;
        if (mask[r >> 3] & (1 << (r & 7))) p = r;
        else break;
      }
      paintedFromBottom = p;
      if (paintedFromBottom <= horizon) break;
      if (paintedFromBottom <= 0) break;
    }
  }
}

// ---- picking -------------------------------------------------------------
bool pick(const Camera& cam, float maxDist, int& bx, int& by, int& bz,
          int& nx, int& ny, int& nz) {
  float dx = cosf(cam.yaw);
  float dy = -((float)cam.horizon - VIEW_H * 0.5f) / PROJ;
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
