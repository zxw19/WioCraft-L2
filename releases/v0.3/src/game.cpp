#include "game.h"

#include <Arduino.h>
#include <math.h>
#include <string.h>
#include <esp32-hal-psram.h>
#include <esp_heap_caps.h>
#include <esp_system.h>

#include "audio.h"
#include "diag.h"
#include "input.h"
#include "board.h"
#include "config.h"
#include "panel.h"
#include "render.h"
#include "save.h"
#include "tex.h"
#include "world.h"

// ---------------------------------------------------------------------------
static L2Panel* g_panel = nullptr;
static uint16_t g_fb[VIEW_W * VIEW_H];    // world render target (native RGB565)
static uint16_t* g_out = nullptr;         // 320x240 panel buffer (byte swapped)

static const float EYE = 1.62f;
static const float HALF_W = 0.30f;
static const float HEIGHT = 1.80f;
static const float GRAVITY = 24.0f;
static const float JUMP_V = 7.6f;
static const float WALK = 4.2f;

static float s_px, s_py, s_pz, s_vy;
static bool s_onGround;
static float s_yaw;
static float s_pitch;        // horizon offset in pixels (+ = look up)
static int s_hp = 20;
static int s_sel = 0;
static float s_dayTime = 0.32f;
static float s_airPeak;
static float s_regen;
static float s_bob;
static float s_walkBlend;

static float s_mvx, s_mvz;
static bool s_mineDown, s_jumpTap, s_placeTap, s_craftTap, s_helpTap;
static input::Frame s_input;
static uint16_t s_count[9] = {16, 32, 16, 0, 0, 0, 0, 0, 0};
static float s_toastT;
static uint8_t s_toastKind;
static float s_coyote, s_jumpBuffer, s_handSwing;
static bool s_hasTarget, s_canPlace;
static uint8_t s_targetBlock;
static uint16_t* s_icons = nullptr;
static uint16_t* s_hands = nullptr;
struct Dust { float x, y, vx, vy, life; uint16_t color; };
static Dust s_dust[18];
static uint32_t s_dustSeed = 28761;
static const char* BLOCK_NAMES[B_COUNT] = {"AIR", "GRASS", "DIRT", "STONE", "SAND", "LOG", "LEAVES", "PLANKS", "COBBLE", "GLASS"};

// input state
static bool s_joyActive;
static float s_joyOX, s_joyOY, s_joyDX, s_joyDY;

static int s_mineX = -1, s_mineY = -1, s_mineZ = -1;
static float s_mineProg;
static float s_stepTimer;
static float s_placeCooldown;
static uint32_t s_saveTimer;
static bool s_dirty;
static float s_msSky, s_msWorld, s_msPick, s_msPresent;

static const uint8_t HOTBAR[9] = {B_GRASS, B_DIRT, B_STONE, B_COBBLE,
                                  B_SAND, B_LOG, B_PLANK, B_LEAVES, B_GLASS};
#define SEL_COUNT 9

// ---- screen layout (320x240) ----------------------------------------------
#define HB_SLOT 26
#define HB_X    43
#define HB_Y    212
#define JUMP_X  280
#define JUMP_Y  179
#define JUMP_R  24
#define HAND_X  230
#define HAND_Y  190
#define JOY_R   32

static inline bool inCircle(int x, int y, int cx, int cy, int r) {
  int dx = x - cx, dy = y - cy;
  return dx * dx + dy * dy <= r * r;
}

// ===========================================================================
// screen-space canvas (writes into g_out, byte swapped for the QSPI panel)
// ===========================================================================
static inline uint16_t bsw(uint16_t c) { return (uint16_t)((c >> 8) | (c << 8)); }

static inline void hpx(int x, int y, uint16_t c) {
  if ((unsigned)x < SCREEN_W && (unsigned)y < SCREEN_H) g_out[y * SCREEN_W + x] = bsw(c);
}
static void hrect(int x, int y, int w, int h, uint16_t c) {
  if (x < 0) { w += x; x = 0; }
  if (y < 0) { h += y; y = 0; }
  if (x + w > SCREEN_W) w = SCREEN_W - x;
  if (y + h > SCREEN_H) h = SCREEN_H - y;
  if (w <= 0 || h <= 0) return;
  uint16_t b = bsw(c);
  for (int r = y; r < y + h; r++) {
    uint16_t* p = g_out + r * SCREEN_W;
    for (int i = x; i < x + w; i++) p[i] = b;
  }
}
static void hdisc(int cx, int cy, int r, uint16_t c) {
  for (int y = -r; y <= r; y++) {
    int span = (int)sqrtf((float)(r * r - y * y));
    hrect(cx - span, cy + y, span * 2 + 1, 1, c);
  }
}
static void hring(int cx, int cy, int r, int th, uint16_t c) {
  for (int y = -r; y <= r; y++) {
    int o = (int)sqrtf((float)(r * r - y * y));
    int in_r = r - th;
    int in = (abs(y) < in_r) ? (int)sqrtf((float)(in_r * in_r - y * y)) : 0;
    if (o > in) hrect(cx - o, cy + y, o - in, 1, c);
    if (o > in) hrect(cx + in + 1, cy + y, o - in, 1, c);
  }
}

// texturing a parallelogram: P(u,v) = P0 + u*U + v*V, u,v in [0,1]
static void texPara(float px0, float py0, float ux, float uy, float vx, float vy,
                    int shadeF, const uint16_t* tex) {
  float det = ux * vy - uy * vx;
  if (fabsf(det) < 1e-5f) return;
  float xs[4] = {px0, px0 + ux, px0 + vx, px0 + ux + vx};
  float ys[4] = {py0, py0 + uy, py0 + vy, py0 + uy + vy};
  float minx = xs[0], maxx = xs[0], miny = ys[0], maxy = ys[0];
  for (int i = 1; i < 4; i++) {
    if (xs[i] < minx) minx = xs[i];
    if (xs[i] > maxx) maxx = xs[i];
    if (ys[i] < miny) miny = ys[i];
    if (ys[i] > maxy) maxy = ys[i];
  }
  int x0 = (int)floorf(minx), x1 = (int)ceilf(maxx);
  int y0 = (int)floorf(miny), y1 = (int)ceilf(maxy);
  if (x0 < 0) x0 = 0;
  if (y0 < 0) y0 = 0;
  if (x1 > SCREEN_W) x1 = SCREEN_W;
  if (y1 > SCREEN_H) y1 = SCREEN_H;

  float rx = 1.0f / det;
  for (int py = y0; py < y1; py++) {
    float fy = py + 0.5f - py0;
    for (int px = x0; px < x1; px++) {
      float fx = px + 0.5f - px0;
      float u = (fx * vy - fy * vx) * rx;
      float v = (ux * fy - uy * fx) * rx;
      if (u < 0.0f || u >= 1.0f || v < 0.0f || v >= 1.0f) continue;
      int tu = (int)(u * 16.0f); if (tu > 15) tu = 15;
      int tv = (int)(v * 16.0f); if (tv > 15) tv = 15;
      uint16_t c = tex[tv * 16 + tu];
      int rr = (c >> 11) & 0x1F, gg = (c >> 5) & 0x3F, bb = c & 0x1F;
      rr = rr * shadeF >> 8; gg = gg * shadeF >> 8; bb = bb * shadeF >> 8;
      if (rr > 31) rr = 31;
      if (gg > 63) gg = 63;
      if (bb > 31) bb = 31;
      hpx(px, py, (uint16_t)((rr << 11) | (gg << 5) | bb));
    }
  }
}

// isometric block icon
static void drawCube(int cx, int cy, int s, uint8_t block, int mul) {
  if (block >= B_COUNT) return;
  const BlockDef& bd = BLOCKS[block];
  int a = s / 2;
  int bb = s * 28 / 100;
  int hgt = s * 54 / 100;
  int oy = cy - (hgt + 2 * bb) / 2 + bb;

  texPara((float)(cx - a), (float)oy, (float)a, (float)-bb, (float)a, (float)bb,
          256 * mul >> 8, g_tex[bd.top]);
  texPara((float)(cx - a), (float)oy, (float)a, (float)bb, 0.0f, (float)hgt,
          200 * mul >> 8, g_tex[bd.side]);
  texPara((float)cx, (float)(oy + bb), (float)a, (float)-bb, 0.0f, (float)hgt,
          148 * mul >> 8, g_tex[bd.side]);
}

// Small cached sprites keep floating point texture mapping out of the HUD path.
static void cacheIcons() {
  s_icons = (uint16_t*)ps_malloc(9 * 26 * 26 * sizeof(uint16_t));
  s_hands = (uint16_t*)ps_malloc(9 * 40 * 46 * sizeof(uint16_t));
  if (!s_icons || !s_hands) {
    free(s_icons); free(s_hands); s_icons=nullptr; s_hands=nullptr; return;
  }
  for (int i=0; i<9; ++i) {
    hrect(0,0,40,46,0);
    drawCube(13,13,18,HOTBAR[i],256);
    for (int y=0;y<26;++y) memcpy(s_icons+i*26*26+y*26,g_out+y*SCREEN_W,26*2);
    hrect(0,0,40,46,0);
    drawCube(20,23,32,HOTBAR[i],256);
    for (int y=0;y<46;++y) memcpy(s_hands+i*40*46+y*40,g_out+y*SCREEN_W,40*2);
  }
}
static void blitIcon(const uint16_t* pixels, int w, int h, int x, int y) {
  for (int r=0;r<h;++r) {
    if ((unsigned)(y+r)>=SCREEN_H) continue;
    for (int c=0;c<w;++c) {
      if ((unsigned)(x+c)>=SCREEN_W) continue;
      uint16_t p=pixels[r*w+c];
      if (p) g_out[(y+r)*SCREEN_W+x+c]=p;
    }
  }
}
static const uint8_t FONT[36][5] = {
  {0x3e,0x51,0x49,0x45,0x3e},{0,0x42,0x7f,0x40,0},{0x42,0x61,0x51,0x49,0x46},
  {0x21,0x41,0x45,0x4b,0x31},{0x18,0x14,0x12,0x7f,0x10},{0x27,0x45,0x45,0x45,0x39},
  {0x3c,0x4a,0x49,0x49,0x30},{1,0x71,9,5,3},{0x36,0x49,0x49,0x49,0x36},{6,0x49,0x49,0x29,0x1e},
  {0x7e,0x11,0x11,0x11,0x7e},{0x7f,0x49,0x49,0x49,0x36},{0x3e,0x41,0x41,0x41,0x22},
  {0x7f,0x41,0x41,0x22,0x1c},{0x7f,0x49,0x49,0x49,0x41},{0x7f,9,9,9,1},
  {0x3e,0x41,0x49,0x49,0x7a},{0x7f,8,8,8,0x7f},{0,0x41,0x7f,0x41,0},
  {0x20,0x40,0x41,0x3f,1},{0x7f,8,0x14,0x22,0x41},{0x7f,0x40,0x40,0x40,0x40},
  {0x7f,2,0x0c,2,0x7f},{0x7f,4,8,0x10,0x7f},{0x3e,0x41,0x41,0x41,0x3e},
  {0x7f,9,9,9,6},{0x3e,0x41,0x51,0x21,0x5e},{0x7f,9,0x19,0x29,0x46},
  {0x46,0x49,0x49,0x49,0x31},{1,1,0x7f,1,1},{0x3f,0x40,0x40,0x40,0x3f},
  {0x1f,0x20,0x40,0x20,0x1f},{0x3f,0x40,0x38,0x40,0x3f},{0x63,0x14,8,0x14,0x63},
  {7,8,0x70,8,7},{0x61,0x51,0x49,0x45,0x43}
};
static void drawText(int x,int y,const char* text,uint16_t color) {
  for (; *text; ++text,x+=6) {
    unsigned char c=*text;
    const uint8_t* glyph=nullptr;
    if (c>='0'&&c<='9') glyph=FONT[c-'0'];
    else if(c>='A'&&c<='Z') glyph=FONT[c-'A'+10];
    if(glyph) for(int col=0;col<5;++col) for(int row=0;row<7;++row)
      if(glyph[col]&(1<<row)) hpx(x+col,y+row,color);
    if(c=='+') {hrect(x,y+3,5,1,color);hrect(x+2,y+1,1,5,color);}
    if(c=='-') hrect(x,y+3,5,1,color);
    if(c=='>') {hpx(x+1,y+1,color);hpx(x+2,y+2,color);hpx(x+3,y+3,color);hpx(x+2,y+4,color);hpx(x+1,y+5,color);}
    if(c=='?') {hrect(x+1,y,3,1,color);hpx(x+4,y+1,color);hpx(x+3,y+2,color);hpx(x+2,y+3,color);hpx(x+2,y+6,color);}
  }
}
static void label(int x,int y,const char* text,uint16_t color=RGB565(244,244,236)) {
  hrect(x-3,y-2,(int)strlen(text)*6+5,11,RGB565(22,27,30));
  drawText(x,y,text,color);
}
static void spawnDust(uint8_t block,int count) {
  s_handSwing=0.22f;
  for(int i=0;i<count;++i) {
    int slot=i;
    for(int j=0;j<18;++j) if(s_dust[j].life<=0) {slot=j;break;}
    s_dustSeed=s_dustSeed*1664525u+1013904223u;
    unsigned n=s_dustSeed;
    Dust& d=s_dust[slot];
    d.x=160+(int)(n&15)-7; d.y=120+(int)((n>>4)&15)-7;
    d.vx=((int)((n>>8)&127)-63)*0.8f; d.vy=-15-(int)((n>>16)&63);
    d.life=0.35f+(n&7)*0.025f;
    d.color=g_tex[BLOCKS[block].side][(n>>12)&255];
  }
}

// 9x7 heart silhouette
static const char* HEART_MASK[7] = {
  ".XXX.XXX.",
  "XXXXXXXXX",
  "XXXXXXXXX",
  ".XXXXXXX.",
  "..XXXXX..",
  "...XXX...",
  "....X....",
};
static void drawHeart(int x, int y, int kind) {
  const uint16_t outline = RGB565(24, 8, 8);
  const uint16_t full = RGB565(226, 42, 42);
  const uint16_t dark = RGB565(78, 20, 20);
  const uint16_t empty = RGB565(46, 16, 16);

  for (int pass = 0; pass < 2; pass++) {
    int ox = pass ? 0 : -1, oy = pass ? 0 : 1;
    for (int r = 0; r < 7; r++) {
      const char* row = HEART_MASK[r];
      for (int c = 0; c < 9; c++) {
        if (row[c] != 'X') continue;
        if (!pass) {
          hpx(x + c + ox, y + r, outline);
          hpx(x + c, y + r + oy, outline);
          continue;
        }
        uint16_t col;
        if (kind == 0) col = empty;
        else if (kind == 1) col = (c <= 4) ? full : dark;
        else col = full;
        hpx(x + c, y + r, col);
      }
    }
  }
  if (kind == 2) {
    hpx(x + 2, y + 1, RGB565(255, 168, 160));
    hpx(x + 1, y + 2, RGB565(255, 168, 160));
  }
}

// ---- physics --------------------------------------------------------------
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

// ---- input ----------------------------------------------------------------
static void readInput(float dt) {
  s_mvx = s_mvz = 0;
  s_mineDown = false;
  s_jumpTap = false;
  s_placeTap = false;
  s_craftTap = false;
  s_helpTap = false;

  lgfx::touch_point_t tp[5];
  uint8_t n = g_panel->getTouch(tp, 5);
  input::Point pts[5];
  for (uint8_t i = 0; i < n; ++i) {
    pts[i].x = tp[i].x; pts[i].y = tp[i].y; pts[i].id = tp[i].id;
  }
  s_input = input::update(pts, n, dt);
  if (s_input.slot >= 0 && s_input.slot < SEL_COUNT && s_input.slot != s_sel) {
    s_sel = s_input.slot;
    s_dirty = true;
    audio::play(SFX_UI, 0.28f);
  }
  s_yaw += s_input.yawDelta;
  if (s_yaw > 3.141593f) s_yaw-=6.283185f;
  if (s_yaw < -3.141593f) s_yaw+=6.283185f;
  s_pitch += s_input.pitchDelta;
  // Keep the low drag gain while allowing level ground inside the 5m reach.
  if (s_pitch > 68.0f) s_pitch = 68.0f;
  if (s_pitch < -76.0f) s_pitch = -76.0f;

  s_joyActive = s_input.joy;
  s_joyOX = 56.0f; s_joyOY = 168.0f;
  s_joyDX = s_input.joyX; s_joyDY = s_input.joyY;
  if (s_input.forward != 0 || s_input.side != 0) {
    float fwd = s_input.forward, side = s_input.side;
    s_mvx = cosf(s_yaw) * fwd - sinf(s_yaw) * side;
    s_mvz = sinf(s_yaw) * fwd + cosf(s_yaw) * side;
  }
  s_mineDown = s_input.mine;
  s_jumpTap = s_input.jump;
  s_placeTap = s_input.place;
  s_craftTap = s_input.craft;
  s_helpTap = s_input.help;
  if (s_toastT > 0) s_toastT -= dt;
}

// ---- interaction ----------------------------------------------------------
static void updateInteraction(float dt) {
  Camera cam;
  cam.x = s_px; cam.y = s_py + EYE; cam.z = s_pz;
  cam.yaw = s_yaw;
  cam.horizon = VIEW_H / 2 + (int)s_pitch;

  int bx, by, bz, nx, ny, nz;
  bool hit = render::pick(cam, REACH, bx, by, bz, nx, ny, nz);
  s_hasTarget=hit;
  s_targetBlock=hit ? world::get(bx,by,bz) : B_AIR;
  s_canPlace=false;
  if(hit) {
    int tx=bx+nx,ty=by+ny,tz=bz+nz;
    bool overlaps=tx+1>s_px-HALF_W && tx<s_px+HALF_W && ty+1>s_py && ty<s_py+HEIGHT && tz+1>s_pz-HALF_W && tz<s_pz+HALF_W;
    s_canPlace=s_count[s_sel]>0 && !overlaps && tx>=0 && tx<WORLD_X && tz>=0 && tz<WORLD_Z && ty>=0 && ty<WORLD_H && world::get(tx,ty,tz)==B_AIR;
  }

  if (hit && by > 0 && s_mineDown) {
    if (bx != s_mineX || by != s_mineY || bz != s_mineZ) {
      s_mineX = bx; s_mineY = by; s_mineZ = bz; s_mineProg = 0;
    }
    uint8_t b = world::get(bx, by, bz);
    float need = BLOCKS[b].hardness10 / 20.0f;
    if (need < 0.12f) need = 0.12f;
    s_mineProg += dt;
    if (s_mineProg >= need) {
      if (world::set(bx, by, bz, B_AIR)) {
        // Resources go directly into the matching visible slot.
        for (int i = 0; i < SEL_COUNT; ++i) if (HOTBAR[i] == b) {
          if (s_count[i] < 999) ++s_count[i];
          s_toastKind = b; s_toastT = 1.4f;
          break;
        }
        spawnDust(b,14);
        audio::play(SFX_BREAK, 0.58f);
        s_dirty = true;
      }
      s_mineProg = 0;
      s_mineX = -1;
    } else if (((int)(s_mineProg * 8)) != ((int)((s_mineProg - dt) * 8))) {
      spawnDust(b,2);
      audio::play(SFX_DIG, 0.35f);
    }
  } else {
    s_mineProg = 0;
    s_mineX = -1;
  }

  if (s_placeCooldown > 0) s_placeCooldown -= dt;
  if (s_craftTap) {
    // One log makes four planks, matching the basic building resource loop.
    if (s_count[5] > 0 && s_count[6] <= 995) {
      --s_count[5]; s_count[6] += 4; s_sel = 6;
      s_toastKind = B_PLANK; s_toastT = 1.6f; s_dirty = true;
      audio::play(SFX_UI, 0.4f);
    } else { s_toastKind = 255; s_toastT = 1.6f; }
  }
  if (s_helpTap) { s_toastKind = 254; s_toastT = 7.0f; }
  if (s_placeTap && s_count[s_sel] == 0) {
    s_toastKind = 253; s_toastT = 1.6f;
  }
  if (s_placeTap && !s_mineDown && s_count[s_sel] > 0 && hit && s_placeCooldown <= 0) {
    int tx = bx + nx, ty = by + ny, tz = bz + nz;
    if (world::get(tx, ty, tz) == B_AIR) {
      bool overlapPlayer = (tx + 1 > s_px - HALF_W && tx < s_px + HALF_W &&
                            ty + 1 > s_py && ty < s_py + HEIGHT &&
                            tz + 1 > s_pz - HALF_W && tz < s_pz + HALF_W);
      if (!overlapPlayer) {
        if (world::set(tx, ty, tz, HOTBAR[s_sel])) {
          --s_count[s_sel];
          s_handSwing = 0.22f;
          audio::play(SFX_PLACE, 0.50f);
          s_placeCooldown = 0.16f;
          s_dirty = true;
        }
      }
    }
  }
}

// ---- update ---------------------------------------------------------------
static void update(float dt) {
  if(s_handSwing>0) s_handSwing-=dt;
  for(auto& p:s_dust) if(p.life>0) {
    p.life-=dt;p.x+=p.vx*dt;p.y+=p.vy*dt;p.vy+=160*dt;
  }
  float len = sqrtf(s_mvx * s_mvx + s_mvz * s_mvz);
  if (len > 1e-4f) {
    s_dirty = true;
    if (len > 1.0f) { s_mvx /= len; s_mvz /= len; len = 1.0f; }
    s_stepTimer += dt;
    float interval = 0.42f / (len > 0.5f ? 1.0f : 0.8f);
    if (s_onGround && s_stepTimer >= interval) {
      s_stepTimer = 0;
      audio::play(SFX_STEP, 0.32f);
    }
    s_bob += dt * 9.0f;
    s_walkBlend += dt * 4.0f;
    if (s_walkBlend > 1.0f) s_walkBlend = 1.0f;
  } else {
    s_stepTimer = 0.30f;
    s_walkBlend -= dt * 4.0f;
    if (s_walkBlend < 0.0f) s_walkBlend = 0.0f;
  }

  if (s_onGround) s_coyote=0.09f;
  else s_coyote-=dt;
  if (s_jumpTap) s_jumpBuffer=0.16f;
  else s_jumpBuffer-=dt;
  if (s_jumpBuffer > 0 && s_coyote > 0) {
    s_jumpBuffer = 0; s_coyote = 0;
    s_vy = JUMP_V;
    s_onGround = false;
    audio::play(SFX_STEP, 0.4f);
  }

  // Holding jump gives the familiar forgiving edge behavior without making
  // the button repeat while grounded after a tap.
  if (s_input.jumpHeld && s_onGround && s_jumpBuffer <= 0.0f) {
    s_vy = JUMP_V;
    s_onGround = false;
    audio::play(SFX_STEP, 0.32f);
  }

  bool wasGround = s_onGround;
  applyVertical(dt);

  if (!s_onGround) {
    if (s_py > s_airPeak) s_airPeak = s_py;
  } else {
    if (!wasGround && s_airPeak - s_py > 3.4f) {
      int dmg = (int)((s_airPeak - s_py - 3.0f) * 1.6f);
      if (dmg > 0) {
        s_hp -= dmg;
        s_regen = 0;
        audio::play(SFX_HURT, 0.8f);
      }
    }
    s_airPeak = s_py;
  }

  if (s_hp < 20) {
    s_regen += dt;
    if (s_regen > 9.0f) { s_regen = 0; s_hp++; }
  }
  if (s_hp <= 0) {
    int sx = WORLD_X / 2, sz = WORLD_Z / 2;
    s_px = (float)sx + 0.5f;
    s_pz = (float)sz + 0.5f;
    s_py = (float)world::height(sx, sz);
    s_vy = 0;
    s_hp = 20;
    s_airPeak = s_py;
  }

  updateInteraction(dt);

  s_dayTime += dt / 480.0f;
  if (s_dayTime >= 1.0f) s_dayTime -= 1.0f;

  if (s_py < -2.0f) {
    s_py = (float)world::height((int)s_px, (int)s_pz) + 0.5f;
    s_vy = 0;
  }
}

static float daylight(float t) {
  float s = sinf((t - 0.25f) * 6.2831853f);
  float d = 0.10f + 0.90f * (s * 0.5f + 0.5f);
  if (d < 0.10f) d = 0.10f;
  if (d > 1.0f) d = 1.0f;
  return d;
}

// ---- hud ------------------------------------------------------------------
static void hudHotbar() {
  int x0 = HB_X, y0 = HB_Y;
  hrect(x0 - 4, y0 - 4, SEL_COUNT * HB_SLOT + 8, HB_SLOT + 8, RGB565(26, 26, 28));
  hrect(x0 - 4, y0 - 4, SEL_COUNT * HB_SLOT + 8, 1, RGB565(104, 104, 108));
  for (int i = 0; i < SEL_COUNT; i++) {
    int x = x0 + i * HB_SLOT;
    hrect(x, y0, HB_SLOT - 1, HB_SLOT, RGB565(60, 60, 64));
    hrect(x, y0, HB_SLOT - 1, 1, RGB565(40, 40, 44));
    hrect(x, y0 + HB_SLOT - 1, HB_SLOT - 1, 1, RGB565(92, 92, 96));
    if(s_icons) blitIcon(s_icons+i*26*26,26,26,x,y0);
    else drawCube(x+13,y0+13,18,HOTBAR[i],256);
    char amount[5]; snprintf(amount,sizeof(amount),"%u",(unsigned)s_count[i]);
    int tx=x+HB_SLOT-2-(int)strlen(amount)*6;
    hrect(tx-1,y0+18,(int)strlen(amount)*6+1,8,RGB565(31,32,34));
    drawText(tx,y0+18,amount,s_count[i]?RGB565(255,255,255):RGB565(140,144,144));
    if (i == s_sel) {
      hrect(x - 2, y0 - 2, HB_SLOT + 3, 2, RGB565(255, 255, 255));
      hrect(x - 2, y0 + HB_SLOT, HB_SLOT + 3, 2, RGB565(255, 255, 255));
      hrect(x - 2, y0 - 2, 2, HB_SLOT + 4, RGB565(255, 255, 255));
      hrect(x + HB_SLOT - 1, y0 - 2, 3, HB_SLOT + 4, RGB565(255, 255, 255));
    }
  }
}

static void hudHearts() {
  for (int i = 0; i < 10; i++) {
    int hp = s_hp - i * 2;
    int kind = hp >= 2 ? 2 : (hp == 1 ? 1 : 0);
    drawHeart(6 + i * 11, 5, kind);
  }
}

static void hudJoystick() {
  int cx, cy;
  if (s_joyActive) { cx = (int)s_joyOX; cy = (int)s_joyOY; }
  else { cx = 56; cy = 168; }
  hdisc(cx, cy, JOY_R, RGB565(30, 32, 30));
  hring(cx, cy, JOY_R, 2, RGB565(150, 152, 150));
  int kx = cx + (int)s_joyDX, ky = cy + (int)s_joyDY;
  hdisc(kx, ky, 14, RGB565(196, 200, 196));
  hdisc(kx - 2, ky - 2, 8, RGB565(236, 238, 236));
}

static void hudJump() {
  hdisc(JUMP_X, JUMP_Y, JUMP_R, RGB565(46, 64, 108));
  hring(JUMP_X, JUMP_Y, JUMP_R, 2, s_input.jumpHeld ? RGB565(245, 245, 245) : RGB565(140, 170, 230));
  for (int i = 0; i < 9; i++) hrect(JUMP_X - i, JUMP_Y + i - 5, i * 2 + 1, 1, RGB565(228, 236, 250));
  hrect(JUMP_X - 3, JUMP_Y + 4, 7, 7, RGB565(228, 236, 250));
}

static void hudCrosshair() {
  int cx = SCREEN_W / 2, cy = SCREEN_H / 2;
  uint16_t c = RGB565(255, 255, 255);
  uint16_t o = RGB565(20, 20, 20);
  hrect(cx - 7, cy, 5, 1, o); hrect(cx + 3, cy, 5, 1, o);
  hrect(cx, cy - 7, 1, 5, o); hrect(cx, cy + 3, 1, 5, o);
  hrect(cx - 6, cy, 4, 1, c); hrect(cx + 3, cy, 4, 1, c);
  hrect(cx, cy - 6, 1, 4, c); hrect(cx, cy + 3, 1, 4, c);
  hpx(cx, cy, c);
}

static void hudMining() {
  if (s_mineProg <= 0) return;
  uint8_t b = world::get(s_mineX, s_mineY, s_mineZ);
  float need = BLOCKS[b].hardness10 / 20.0f;
  if (need < 0.12f) need = 0.12f;
  float f = s_mineProg / need;
  if (f > 1.0f) f = 1.0f;
  int w = (int)(f * 58.0f);
  int cx = SCREEN_W / 2;
  hrect(cx - 30, SCREEN_H / 2 + 15, 60, 6, RGB565(18, 18, 18));
  hrect(cx - 29, SCREEN_H / 2 + 16, w, 4, RGB565(236, 206, 72));
}

static void hudHand() {
  int bob = (int)(sinf(s_bob) * 2.5f * s_walkBlend);
  float swingT=s_handSwing>0?s_handSwing:0;
  int swing=(int)(sinf(swingT/0.22f*3.14159f)*8);
  if(s_count[s_sel]>0) {
    if(s_hands) blitIcon(s_hands+s_sel*40*46,40,46,HAND_X-20-swing,HAND_Y-23+bob+swing);
    else drawCube(HAND_X-swing,HAND_Y+bob+swing,32,HOTBAR[s_sel],256);
  } else { // Empty hand is visibly different from a held building block.
    hrect(HAND_X-6-swing,HAND_Y-8+bob,15,23,RGB565(184,132,87));
    hrect(HAND_X-8-swing,HAND_Y-12+bob,18,12,RGB565(222,172,120));
  }
}

static void drawHud() {
  hudHearts();
  hudHotbar();
  hudHand();
  hudJoystick();
  hudJump();
  hudCrosshair();
  hudMining();
  // Separate action zones make looking, mining and placement predictable.
  hdisc(290,77,21,s_input.digHeld?RGB565(150,99,46):RGB565(64,57,43));
  hring(290,77,21,2,RGB565(218,187,124)); drawText(282,74,"DIG",RGB565(255,244,214));
  hdisc(290,127,21,s_input.placeHeld?RGB565(66,124,80):RGB565(38,64,47));
  hring(290,127,21,2,s_canPlace?RGB565(173,237,155):RGB565(89,118,91));
  drawText(276,124,"PLACE",s_canPlace?RGB565(232,255,225):RGB565(140,164,140));
  label(230,8,"CRAFT",s_count[5]?RGB565(255,222,132):RGB565(156,160,160));
  label(298,8,"?");
  if(s_hasTarget) label(130,147,BLOCK_NAMES[s_targetBlock],RGB565(222,226,208));
  for(auto& p:s_dust) if(p.life>0) hrect((int)p.x,(int)p.y,3,3,p.color);
  if(s_toastT>0) {
    if(s_toastKind==254) {
      label(12,28,"MOVE LEFT  DRAG VIEW");
      label(12,40,"HOLD DIG  TAP PLACE");
      label(12,52,"MINE TO COLLECT BLOCKS");
      label(12,64,"CRAFT 1 LOG > 4 PLANKS");
    } else if(s_toastKind==255) label(70,30,"NEED 1 LOG TO CRAFT");
    else if(s_toastKind==253) label(68,30,"EMPTY - MINE TO COLLECT");
    else { char message[24]; snprintf(message,sizeof(message),"+ %s",BLOCK_NAMES[s_toastKind]); label(110,30,message,RGB565(226,246,164)); }
  }
  label(123,195,BLOCK_NAMES[HOTBAR[s_sel]],RGB565(240,230,180));
}

// ---- present --------------------------------------------------------------
static void present() {
  if (!g_out) return;
  g_panel->waitDMA();
  for (int y = 0; y < VIEW_H; y++) {
    const uint16_t* s = g_fb + y * VIEW_W;
    uint32_t* d0 = (uint32_t*)(g_out + (y * 2) * SCREEN_W);
    uint32_t* d1 = (uint32_t*)(g_out + (y * 2 + 1) * SCREEN_W);
    for (int x = 0; x < VIEW_W; x++) {
      uint16_t c = s[x];
      uint16_t b = (uint16_t)((c >> 8) | (c << 8));
      uint32_t w = ((uint32_t)b << 16) | b;
      d0[x] = w;
      d1[x] = w;
    }
  }
  drawHud();
  g_panel->pushImageDMA(0, 0, SCREEN_W, SCREEN_H, (const uint16_t*)g_out);
}

// ---- public ---------------------------------------------------------------
void gameInit(L2Panel* panel) {
  g_panel = panel;
  Serial.begin(115200);
  for (int i = 0; i < 40 && !Serial; i++) delay(50);
  delay(200);
  Serial.println();
  Serial.printf("[boot] WioCraft v0.3 starting | reset_reason=%d\n", (int)esp_reset_reason());
  diag::begin();
  Serial.flush();

  Serial.println("[boot] board rails");
  Serial.flush();
  board::begin();

  Serial.println("[boot] display init");
  Serial.flush();
  panel->init();
  Serial.printf("[boot] display %dx%d\n", panel->width(), panel->height());
  Serial.flush();
  int bestRot = 0;
  for (int r = 0; r < 4; r++) {
    panel->setRotation(r);
    int w = panel->width(), h = panel->height();
    Serial.printf("[boot] rot %d -> %dx%d\n", r, w, h);
    if (w == SCREEN_W && h == SCREEN_H) { bestRot = r; break; }
  }
  panel->setRotation(bestRot);
  panel->setColorDepth(16);
  panel->setBrightness(200);
  Serial.printf("[boot] display %dx%d rot %d\n", panel->width(), panel->height(), bestRot);
  Serial.flush();

  const size_t OUT_BYTES = (size_t)SCREEN_W * SCREEN_H * 2;
  size_t big = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
  if (big >= OUT_BYTES + 32768)
    g_out = (uint16_t*)heap_caps_malloc(OUT_BYTES, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
  bool inPsram = false;
  if (!g_out) { g_out = (uint16_t*)ps_malloc(OUT_BYTES); inPsram = true; }
  if (!g_out) {
    Serial.println("[boot] FATAL: out buffer alloc failed");
    while (true) delay(100);
  }
  memset(g_out, 0, OUT_BYTES);
  Serial.printf("[boot] out buffer %u bytes in %s\n", (unsigned)OUT_BYTES, inPsram ? "PSRAM" : "internal DMA RAM");
  Serial.flush();

  // POST: red -> green -> blue through the real push path
  {
    const uint16_t tst[3] = {RGB565(210, 30, 30), RGB565(30, 190, 60), RGB565(40, 70, 220)};
    for (int i = 0; i < 3; i++) {
      uint16_t b = bsw(tst[i]);
      for (int k2 = 0; k2 < SCREEN_W * SCREEN_H; k2++) g_out[k2] = b;
      panel->pushImageDMA(0, 0, SCREEN_W, SCREEN_H, (const uint16_t*)g_out);
      delay(160);
    }
  }

  Serial.println("[boot] textures");
  Serial.flush();
  texInit();
  cacheIcons();
  if (!world::init(20261112u)) {
    Serial.println("world alloc failed");
    while (true) delay(100);
  }

  Serial.println("[boot] world gen");
  Serial.flush();
  int sx = WORLD_X / 2, sz = WORLD_Z / 2;
  s_px = (float)sx + 0.5f;
  s_pz = (float)sz + 0.5f;
  s_py = (float)world::height(sx, sz);
  s_yaw = 0.0f;
  s_pitch = 0.0f;
  s_vy = 0;
  s_airPeak = s_py;

  Serial.println("[boot] audio");
  Serial.flush();
  audio::init();
  Serial.printf("[boot] audio %s\r\n", audio::ready() ? "ok" : "off");
  Serial.println("[boot] sd");
  Serial.flush();
  save::begin();
  Serial.printf("[boot] sd %s\r\n", save::ready() ? "ok" : "off");
  SaveState st;
  if (save::load(st)) {
    s_px = st.x; s_py = st.y; s_pz = st.z;
    s_yaw = st.yaw; s_pitch = st.pitchOff;
    s_dayTime = st.dayTime;
    s_hp = st.hp;
    s_sel = st.sel;
    memcpy(s_count, st.inventory, sizeof(s_count));
    if (s_pitch > 68) s_pitch = 68;
    if (s_pitch < -76) s_pitch = -76;
    if (s_sel < 0 || s_sel >= SEL_COUNT) s_sel = 0;
    if (s_hp < 1) s_hp = 20;
    s_airPeak = s_py;
  }
  s_saveTimer = millis();
  input::reset();
  s_toastKind = 254; s_toastT = 7.0f;

  Serial.printf("WioCraft up: %d x %d x %d world, cpu %u MHz\n", WORLD_X, WORLD_H, WORLD_Z,
                (unsigned)getCpuFrequencyMhz());
}

// USB CDC diagnostics only.  No reset line manipulation is required to fetch
// an exact copy of the framebuffer or inspect interaction state.
static void diagnostics() {
  if (!Serial.available()) return;
  int command = Serial.read();
  if (command == 'I') {
    diag::status();
    Serial.printf("[state] uptime=%lu reset=%d heap=%u minheap=%u pos=%.2f,%.2f,%.2f yaw=%.3f pitch=%.2f hp=%d slot=%d target=%d mine=%.2f counts=",
                  (unsigned long)millis(),(int)esp_reset_reason(),ESP.getFreeHeap(),ESP.getMinFreeHeap(),
                  s_px,s_py,s_pz,s_yaw,s_pitch,s_hp,s_sel,s_targetBlock,s_mineProg);
    for (int i=0;i<9;++i) Serial.printf("%u%s",(unsigned)s_count[i],i==8?"\n":",");
  } else if (command == 'P') {
    g_panel->waitDMA();
    Serial.println("[frame] 320 240 RGB565-RLE-HEX");
    int pos=0;
    while(pos<SCREEN_W*SCREEN_H) {
      uint16_t c=g_out[pos];
      int run=1;
      while(pos+run<SCREEN_W*SCREEN_H && g_out[pos+run]==c && run<65535) ++run;
      Serial.printf("%04x%04x",run,bsw(c));
      pos+=run;
    }
    Serial.println("\n[/frame]");
  }
}

void gameLoop() {
  static uint32_t last = 0;
  static uint32_t fpsTimer = 0;
  static uint32_t frames = 0;
  static float fpsAvg = 0;

  uint32_t now = millis();
  float dt = last ? (now - last) / 1000.0f : 0.016f;
  last = now;
  if (dt > 0.10f) dt = 0.10f;

  diag::mark(diag::TOUCH);
  readInput(dt);
  diag::mark(diag::UPDATE);
  update(dt);
  diag::mark(diag::AUDIO);
  audio::service();

  float dl = daylight(s_dayTime);
  Camera cam;
  cam.x = s_px; cam.y = s_py + EYE; cam.z = s_pz;
  cam.yaw = s_yaw;
  cam.horizon = VIEW_H / 2 + (int)s_pitch;

  uint32_t t0 = micros();
  diag::mark(diag::SKY);
  render::fillSky(g_fb, cam, dl, s_dayTime);
  uint32_t t1 = micros();
  diag::mark(diag::WORLD);
  render::world(cam, g_fb, dl);
  uint32_t t2 = micros();

  int bx, by, bz, nx, ny, nz;
  if (render::pick(cam, REACH, bx, by, bz, nx, ny, nz)) {
    uint16_t oc = RGB565(18, 18, 18);
    if (s_mineProg > 0) {
      uint8_t b = world::get(s_mineX, s_mineY, s_mineZ);
      float need = BLOCKS[b].hardness10 / 20.0f;
      if (need < 0.12f) need = 0.12f;
      int f = (int)(s_mineProg / need * 256.0f);
      if (f > 256) f = 256;
      int rr = 18 + (f * 220 >> 8);
      int gg = 18 + (f * 90 >> 8);
      oc = RGB565(rr, gg, 18);
    }
    render::outline(g_fb, cam, bx, by, bz, oc);
  }

  uint32_t t3 = micros();
  diag::mark(diag::PRESENT);
  present();
  uint32_t t4 = micros();
  s_msSky     = s_msSky     * 0.8f + (t1 - t0) * 0.2f;
  s_msWorld   = s_msWorld   * 0.8f + (t2 - t1) * 0.2f;
  s_msPick    = s_msPick    * 0.8f + (t3 - t2) * 0.2f;
  s_msPresent = s_msPresent * 0.8f + (t4 - t3) * 0.2f;

  frames++;
  if (now - fpsTimer >= 2000) {
    fpsAvg = frames * 1000.0f / (float)(now - fpsTimer);
    Serial.printf("fps %.1f | sky %.1f world %.1f pres %.1f ms | cells %u blk %u px %u\n",
                  fpsAvg, s_msSky / 1000.0f, s_msWorld / 1000.0f, s_msPresent / 1000.0f,
                  (unsigned)render::statCells, (unsigned)render::statBlocks,
                  (unsigned)render::statPixels);
    fpsTimer = now;
    frames = 0;
  }

  if (save::ready() && s_dirty && !s_input.touching && !audio::busy() && now - s_saveTimer > 30000) {
    s_saveTimer = now;
    SaveState st = {};
    st.x=s_px; st.y=s_py; st.z=s_pz; st.yaw=s_yaw; st.pitchOff=s_pitch;
    st.dayTime=s_dayTime; st.hp=s_hp; st.sel=s_sel;
    memcpy(st.inventory, s_count, sizeof(s_count));
    diag::mark(diag::SAVE);
    if (save::store(st)) { s_dirty = false; Serial.println("[save] committed"); }
    else Serial.println("[save] failed; retry pending");
  }
  diagnostics();
  diag::mark(diag::IDLE);
  diag::tick();
}
