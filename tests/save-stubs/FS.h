#pragma once
#include <stddef.h>
#include <stdint.h>
#define FILE_READ "r"
#define FILE_WRITE "w"
struct File {
  int slot=-1;
  size_t pos=0;
  explicit operator bool() const { return slot>=0; }
  size_t read(uint8_t* dst,size_t n);
  size_t write(const uint8_t* src,size_t n);
  bool seek(size_t p);
  size_t size() const;
  void flush() {}
  void close() { slot=-1; }
};
