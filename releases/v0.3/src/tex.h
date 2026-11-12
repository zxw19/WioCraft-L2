#pragma once
#include <stdint.h>

enum : uint8_t {
  T_GRASS_TOP = 0, T_GRASS_SIDE, T_DIRT, T_STONE, T_SAND,
  T_LOG_SIDE, T_LOG_TOP, T_LEAVES, T_PLANK, T_COBBLE, T_GLASS, T_COUNT
};

extern uint16_t g_tex[T_COUNT][256];

void texInit();
