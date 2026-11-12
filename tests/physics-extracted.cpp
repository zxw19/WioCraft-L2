
#include <math.h>
static const float HALF_W=.30f, HEIGHT=1.80f, GRAVITY=24.0f, JUMP_V=7.6f, WALK=4.2f;
#define WORLD_X 16
#define WORLD_Z 16
static float s_px,s_py,s_pz,s_vy,s_mvx,s_mvz;
static bool s_onGround;
static int holeDepth=1, lowCeiling=0;
namespace world {
bool solid(int x,int y,int z) {
  if(y<1)return true;
  if(lowCeiling && y==3)return true;
  bool pit=x==1 && z==1;
  return !pit && y<1+holeDepth;
}
}
static bool collides(float x, float y, float z) {
  int x0 = (int)floorf(x - HALF_W), x1 = (int)floorf(x + HALF_W);
  int y0 = (int)floorf(y), y1 = (int)floorf(y + HEIGHT - 0.01f);
  int z0 = (int)floorf(z - HALF_W), z1 = (int)floorf(z + HALF_W);
  if (x0 < 0 || x1 >= WORLD_X || z0 < 0 || z1 >= WORLD_Z || y0 < 0) return true;
  for (int yy = y0; yy <= y1; yy++)
    for (int zz = z0; zz <= z1; zz++)
      for (int xx = x0; xx <= x1; xx++)
        if (world::solid(xx, yy, zz)) return true;
  return false;
}

static bool stepHorizontal(float dx, float dz) {
  float nx = s_px + dx, nz = s_pz + dz;
  if (!collides(nx, s_py, nz)) { s_px = nx; s_pz = nz; return true; }
  if (s_onGround && !collides(nx, s_py + 0.55f, nz) && !collides(s_px, s_py + 0.55f, s_pz)) {
    s_py += 0.55f; s_px = nx; s_pz = nz; return true;
  }
  return false;
}

static void moveHorizontal(float dt) {
  float dx = s_mvx * WALK * dt;
  float dz = s_mvz * WALK * dt;
  float distance = fabsf(dx) + fabsf(dz);
  int steps = (int)ceilf(distance / 0.12f);
  if (steps < 1) steps = 1;
  float sx = dx / steps, sz = dz / steps;
  for (int i = 0; i < steps; ++i) {
    // Separate axes keeps a diagonal corner from tunnelling through a block.
    stepHorizontal(sx, 0);
    stepHorizontal(0, sz);
  }
}

static void applyVertical(float dt) {
  // Integrate at a fixed small step. At the old 100 ms frame cap, applying
  // gravity once before displacement reduced a 7.6 m/s jump to about 0.84 m,
  // so a player in a one-block hole could never clear its rim.
  float left = dt;
  while (left > 0.0001f) {
    float h = left > (1.0f / 120.0f) ? (1.0f / 120.0f) : left;
    left -= h;
    moveHorizontal(h);
    s_onGround = false;
    s_vy -= GRAVITY * h;
    if (s_vy < -30.0f) s_vy = -30.0f;
    float delta = s_vy * h;
    int steps = (int)ceilf(fabsf(delta) / 0.10f);
    if (steps < 1) steps = 1;
    float step = delta / steps;
    for (int i = 0; i < steps; ++i) {
      float ny = s_py + step;
      if (!collides(s_px, ny, s_pz)) { s_py = ny; continue; }
      float lo = s_py, hi = ny;
      for (int j = 0; j < 7; ++j) {
        float mid = (lo + hi) * 0.5f;
        if (collides(s_px, mid, s_pz)) hi = mid; else lo = mid;
      }
      s_py = lo;
      s_onGround = step <= 0;
      s_vy = 0;
      break;
    }
  }
}


extern "C" {
void reset(int depth,int ceiling) {
  holeDepth=depth;lowCeiling=ceiling;
  s_px=1.5f;s_py=1.0f;s_pz=1.5f;s_vy=0;s_onGround=true;s_mvx=s_mvz=0;
}
void step(float dt,int jump,float move) {
  s_mvx=move;
  if(jump && s_onGround) {s_vy=JUMP_V;s_onGround=false;}
  applyVertical(dt);
}
float value(int i) {return i==0?s_px:i==1?s_py:i==2?s_vy:(float)s_onGround;}
}
