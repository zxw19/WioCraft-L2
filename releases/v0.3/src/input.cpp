#include "input.h"
#include <math.h>

namespace input {
struct Finger {
  bool used = false, seen = false, fresh = false, mining = false;
  int id = 0, x = 0, y = 0, startX = 0, startY = 0, misses = 0;
  float age = 0, missing = 0, moved = 0;
  Zone zone = NONE;
};
static Finger fingers[5];
static bool circle(int x, int y, int cx, int cy, int r) {
  return (x-cx)*(x-cx)+(y-cy)*(y-cy) <= r*r;
}
Zone zoneAt(int x, int y) {
  if (x < 0 || x >= 320 || y < 0 || y >= 240) return NONE;
  if (y >= 208) return x >= 39 && x < 281 ? BAR : NONE;
  if (y < 24 && x >= 282) return HELP;
  if (y < 24 && x >= 224) return CRAFT;
  if (circle(x,y,290,77,22)) return DIG;
  if (circle(x,y,290,127,22)) return PLACE;
  if (circle(x,y,280,179,24)) return JUMP;
  if (x < 116 && y >= 108 && y < 204) return JOY;
  return LOOK;
}
void reset() { for (auto& f : fingers) f = Finger(); }
Frame update(const Point* points, int count, float dt) {
  Frame out;
  for (auto& f : fingers) { f.seen = f.fresh = false; }
  for (int i=0; i<count; ++i) {
    const Point& p = points[i];
    Finger* found = nullptr;
    for (auto& f : fingers) if (f.used && f.id == p.id) { found=&f; break; }
    if (!found) {
      if (zoneAt(p.x,p.y) == NONE) continue;
      for (auto& f : fingers) if (!f.used) {
        f = Finger(); f.used=true; f.fresh=true; f.id=p.id;
        f.x=f.startX=p.x; f.y=f.startY=p.y; f.zone=zoneAt(p.x,p.y);
        found=&f; break;
      }
    }
    if (!found || found->seen) continue;
    Finger& f = *found;
    int dx=p.x-f.x, dy=p.y-f.y;
    int sx=p.x-f.startX, sy=p.y-f.startY;
    float distance=sqrtf((float)(sx*sx+sy*sy));
    if (distance > f.moved) f.moved=distance;
    if (!f.fresh && f.zone == LOOK) {
      // Ignore a corrupt sample, retain smooth small movements without drift.
      if (dx >= -65 && dx <= 65 && dy >= -65 && dy <= 65) {
        if (dx > 1 || dx < -1) out.yawDelta += dx * 0.0034f;
        if (dy > 1 || dy < -1) out.pitchDelta -= dy * 0.26f;
      }
    }
    f.x=p.x; f.y=p.y; f.seen=true; f.misses=0; f.missing=0;
  }
  for (auto& f : fingers) {
    if (!f.used) continue;
    if (!f.seen) {
      ++f.misses; f.missing += dt;
      if (f.misses >= 2 && f.missing >= 0.055f) {
        // A view tap only looks. Placement has its own unambiguous button.
        f = Finger(); continue;
      }
    }
    f.age += dt;
    out.touching=true;
    switch (f.zone) {
      case JOY: {
        float dx=f.x-56.0f, dy=f.y-168.0f;
        float len=sqrtf(dx*dx+dy*dy);
        if (len > 32) { dx*=32/len; dy*=32/len; len=32; }
        out.joy=true; out.joyX=dx; out.joyY=dy;
        if (len > 6) {
          float amount=(len-6)/26.0f;
          out.side=dx/len*amount; out.forward=-dy/len*amount;
        }
        break;
      }
      case LOOK:
        if (!f.mining && f.age >= 0.22f && f.moved < 9) f.mining=true;
        if (f.moved >= 12) f.mining=false;
        out.mine |= f.mining;
        break;
      case BAR:
        if (f.x >= 39 && f.x < 281) {
          int slot=(f.x-43)/26;
          out.slot=slot < 0 ? 0 : slot > 8 ? 8 : slot;
        }
        break;
      case JUMP: out.jump |= f.fresh; out.jumpHeld=true; break;
      case DIG: out.mine=true; out.digHeld=true; break;
      case PLACE: out.place |= f.fresh; out.placeHeld=true; break;
      case CRAFT: out.craft |= f.fresh; break;
      case HELP: out.help |= f.fresh; break;
      default: break;
    }
  }
  return out;
}
}
