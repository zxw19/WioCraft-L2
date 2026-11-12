#pragma once
#include "FS.h"
#define CARD_NONE 0
struct MockSD {
  void setPins(int,int,int) {}
  bool begin(const char*,bool) { return true; }
  int cardType() { return 1; }
  bool exists(const char* path);
  bool mkdir(const char*) { return true; }
  bool remove(const char* path);
  bool rename(const char* from,const char* to);
  File open(const char* path,const char* mode);
};
extern MockSD SD_MMC;
struct SerialStub { void println(const char*) {} };
extern SerialStub Serial;
