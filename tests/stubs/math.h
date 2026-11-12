#pragma once
extern "C" {
float sqrtf(float);
float sinf(float);
float cosf(float);
float floorf(float);
float ceilf(float);
float fabsf(float);
float powf(float,float);
}
static inline int abs(int v) { return v < 0 ? -v : v; }
