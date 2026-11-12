#include "input.h"
#include "world.h"
#include "render.h"
#include "tex.h"
#include "config.h"
#include <stddef.h>

static unsigned char pool[2*1024*1024];
static size_t nextAlloc;
extern "C" void* ps_malloc(size_t n) {
  size_t p=(nextAlloc+15)&~15;
  if(p+n>sizeof(pool)) return nullptr;
  nextAlloc=p+n;
  return pool+p;
}
extern "C" void* memcpy(void* d,const void* s,size_t n) {
  auto a=(unsigned char*)d;auto b=(const unsigned char*)s;
  for(size_t i=0;i<n;++i)a[i]=b[i];return d;
}
extern "C" void* memset(void* d,int c,size_t n) {
  auto a=(unsigned char*)d;for(size_t i=0;i<n;++i)a[i]=(unsigned char)c;return d;
}
static input::Frame frame;
static uint16_t fb[VIEW_W*VIEW_H];
extern "C" {
int init() { texInit(); return world::init(20261112u); }
void clearRegion(int x,int y,int z,int size) {
  for(int a=x;a<x+size;++a)for(int b=y;b<y+size;++b)for(int c=z;c<z+size;++c)
    world::set(a,b,c,B_AIR);
  world::clearEdits();
}
int setBlock(int x,int y,int z,int b) { return world::set(x,y,z,b); }
int getBlock(int x,int y,int z) { return world::get(x,y,z); }
int countEdits() { return world::editCount(); }
int heightAt(int x,int z) { return world::height(x,z); }
void resetInput() { input::reset(); }
void touch(int x0,int y0,int id0,int x1,int y1,int id1,int x2,int y2,int id2,int n,float dt) {
  input::Point p[3]={{x0,y0,id0},{x1,y1,id1},{x2,y2,id2}};
  frame=input::update(p,n,dt);
}
float inputValue(int i) {
  switch(i) {
    case 0:return frame.forward;case 1:return frame.side;
    case 2:return frame.yawDelta;case 3:return frame.pitchDelta;
    case 4:return frame.joy;case 5:return frame.jump;case 6:return frame.mine;
    case 7:return frame.place;case 8:return frame.slot;case 9:return frame.touching;
    case 10:return frame.craft;default:return frame.help;
  }
}
int draw(float x,float y,float z,float yaw,int horizon) {
  Camera cam={x,y,z,yaw,horizon};
  render::fillSky(fb,cam,1.0f,0.5f);
  render::world(cam,fb,1.0f);
  return (int)fb;
}
int drawSky(float x,float y,float z,float yaw,int horizon) {
  Camera cam={x,y,z,yaw,horizon};
  render::fillSky(fb,cam,1.0f,0.5f);
  return (int)fb;
}
int pick(float x,float y,float z,float yaw,int horizon) {
  Camera cam={x,y,z,yaw,horizon};int bx,by,bz,nx,ny,nz;
  if(!render::pick(cam,REACH,bx,by,bz,nx,ny,nz))return -1;
  return (bx<<16)|(by<<8)|bz;
}
}
