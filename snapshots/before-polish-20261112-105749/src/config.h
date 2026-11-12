#pragma once
#include <stdint.h>

// ---- view ---------------------------------------------------------------
// 160x120 internal render, integer 2x upscale to the 320x240 panel.
#define VIEW_W 160
#define VIEW_H 120
#define SCALE  2
#define SCREEN_W 320
#define SCREEN_H 240

// ---- world --------------------------------------------------------------
#define CHUNK_W 16
#define WORLD_CHUNKS 7            // 7x7 chunks => 112x112 blocks
#define WORLD_X (WORLD_CHUNKS * CHUNK_W)
#define WORLD_Z (WORLD_CHUNKS * CHUNK_W)
#define WORLD_H 64
#define SEA_LEVEL 30

#define MAX_RAY_DIST 46.0f
#define REACH        5.0f

// ---- board pins ---------------------------------------------------------
#define PIN_I2C_SDA 47
#define PIN_I2C_SCL 48
#define PIN_USER_BTN 0            // BOOT / user button
#define PIN_SD_CLK 2
#define PIN_SD_CMD 3
#define PIN_SD_D0  1

#define TCA9535_ADDR 0x21

// TCA9535 pin map (0..7 = port0, 8..15 = port1)
#define EXP_WAKE_BTN   0
#define EXP_I2C_IRQ    1
#define EXP_SD_DETECT  2
#define EXP_TP_INT     3
#define EXP_LCD_CS     4
#define EXP_LCD_EN     5
#define EXP_LCD_RST    6
#define EXP_GROVE_EN   7
#define EXP_TP_RST     8
#define EXP_GNSS_RST   9
#define EXP_USER_LED  10
#define EXP_OTG_EN    11
#define EXP_PA_EN     12
#define EXP_GNSS_EN   13
#define EXP_TF_EN     14
#define EXP_BAT_ADC_EN 15

// ---- audio pins ---------------------------------------------------------
#define PIN_I2S_MCLK 10
#define PIN_I2S_BCK  11
#define PIN_I2S_WS   12
#define PIN_I2S_DOUT 16

// Set to 1 if colours come out with red/blue swapped on your panel revision.
#ifndef L2_SWAP_RGB
#define L2_SWAP_RGB 0
#endif

#define RGB565(r, g, b) ((uint16_t)((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3)))
