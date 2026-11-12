#include <SD_MMC.h>
#include "world.h"
#include "save.h"
#include <string.h>

static unsigned char data[3][70000];
static size_t lengths[3];
static bool exists_[3];
static int failRename,failWrites;
static unsigned char arena[128000];
static size_t arenaNext;
extern "C" void* ps_malloc(size_t n) {
  size_t p=(arenaNext+7)&~size_t(7); if(p+n>sizeof(arena))return nullptr;
  arenaNext=p+n;return arena+p;
}
extern "C" void free(void*) {}
extern "C" void* memcpy(void* d,const void* s,size_t n) {
  auto a=(unsigned char*)d;auto b=(const unsigned char*)s;
  for(size_t i=0;i<n;++i)a[i]=b[i];return d;
}
extern "C" void* memset(void* d,int c,size_t n) {
  auto a=(unsigned char*)d;for(size_t i=0;i<n;++i)a[i]=(unsigned char)c;return d;
}
static bool same(const char* a,const char* b) {
  while(*a && *b && *a==*b){++a;++b;}return *a==*b;
}
static int pathSlot(const char* path) {
  if(same(path,"/wicraft/save.bin"))return 0;
  if(same(path,"/wicraft/save.bak"))return 1;
  if(same(path,"/wicraft/save.tmp"))return 2;
  return -1;
}
size_t File::read(uint8_t* dst,size_t n) {
  if(slot<0)return 0;if(n>lengths[slot]-pos)n=lengths[slot]-pos;
  memcpy(dst,data[slot]+pos,n);pos+=n;return n;
}
size_t File::write(const uint8_t* src,size_t n) {
  if(slot<0)return 0;if(failWrites && n>0)--n;
  if(pos+n>70000)return 0;memcpy(data[slot]+pos,src,n);pos+=n;
  if(pos>lengths[slot])lengths[slot]=pos;return n;
}
bool File::seek(size_t p) { if(slot<0||p>lengths[slot])return false;pos=p;return true; }
size_t File::size() const { return slot<0?0:lengths[slot]; }
bool MockSD::exists(const char* path) { int n=pathSlot(path);return n<0?same(path,"/wicraft"):exists_[n]; }
bool MockSD::remove(const char* path) { int n=pathSlot(path);if(n<0||!exists_[n])return false;exists_[n]=false;lengths[n]=0;return true; }
bool MockSD::rename(const char* from,const char* to) {
  int a=pathSlot(from),b=pathSlot(to);if(a<0||b<0||!exists_[a]||exists_[b])return false;
  if(failRename==1 && a==2 && b==0)return false;
  if(failRename==2 && a==0 && b==1)return false;
  memcpy(data[b],data[a],lengths[a]);lengths[b]=lengths[a];exists_[b]=true;exists_[a]=false;lengths[a]=0;return true;
}
File MockSD::open(const char* path,const char* mode) {
  int n=pathSlot(path);if(n<0)return {};
  if(*mode=='w'){exists_[n]=true;lengths[n]=0;}else if(!exists_[n])return {};
  File f;f.slot=n;return f;
}
MockSD SD_MMC;SerialStub Serial;
namespace board { bool sdCardPresent() { return true; } }
static Edit edits[8192];static int editN,applied,cleared;
namespace world {
int editCount(){return editN;}
const Edit* editAt(int i){return i>=0&&i<editN?&edits[i]:nullptr;}
void clearEdits(){editN=0;++cleared;}
void applyEdit(const Edit& e,bool log){++applied;if(log)edits[editN++]=e;}
}
#ifndef SAVE_SOURCE
#define SAVE_SOURCE "../src/save.cpp"
#endif
#include SAVE_SOURCE
static SaveState testState,loaded;
extern "C" {
void reset() {
  memset(lengths,0,sizeof(lengths));memset(exists_,0,sizeof(exists_));
  failRename=failWrites=0;arenaNext=0;editN=applied=cleared=0;
  testState={};testState.x=56.5f;testState.y=34;testState.z=56.5f;
  testState.dayTime=.32f;testState.hp=20;testState.sel=5;
  for(int i=0;i<9;++i)testState.inventory[i]=i*3;
  loaded={};save::begin();
}
int statePtr(){return (int)&testState;}
int loadedPtr(){return (int)&loaded;}
int filePtr(int slot){return (int)data[slot];}
int fileSize(int slot){return (int)lengths[slot];}
int fileExists(int slot){return exists_[slot];}
void setFileSize(int slot,int n){lengths[slot]=n;exists_[slot]=true;}
void failures(int rename,int writes){failRename=rename;failWrites=writes;}
void setEdits(int n){editN=n;for(int i=0;i<n;++i)edits[i]={(int16_t)(i%112),20,40,(uint8_t)(i%9+1)};}
int store(){arenaNext=0;return save::store(testState);}
int load(){arenaNext=0;return save::load(loaded);}
int applications(){return applied;}
int clearCount(){return cleared;}
int loadedEditCount(){return editN;}
int stateSize(){return sizeof(SaveState);}
int headerSize(){return sizeof(save::Header);}
int oldHeaderSize(){return sizeof(save::OldHeader);}
int countOffset(){return offsetof(save::Header,count);}
void oldFixture(int slot,int n) {
  save::OldHeader h={};h.magic=save::OLD_MAGIC;memcpy(&h.st,&testState,sizeof(h.st));h.count=n;
  memcpy(data[slot],&h,sizeof(h));
  for(int i=0;i<n;++i){Edit e={(int16_t)i,20,40,B_LOG};memcpy(data[slot]+sizeof(h)+i*sizeof(Edit),&e,sizeof(e));}
  lengths[slot]=sizeof(h)+n*sizeof(Edit);exists_[slot]=true;
}
}
