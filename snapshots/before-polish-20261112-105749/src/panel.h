#pragma once

#define LGFX_USE_V1
#include <LovyanGFX.hpp>
#include <Wire.h>
#include "config.h"

// LP5814 quad LED driver used as the backlight (I2C 0x2C)
class L2Backlight : public lgfx::v1::ILight {
  static constexpr uint8_t R_DEVCFG = 0x00, R_MAXCUR = 0x01, R_ENABLE = 0x02;
  static constexpr uint8_t R_DIMMODE = 0x04, R_ENGINE = 0x05, R_UPDATE = 0x0F;
  static constexpr uint8_t R_LED0_DC = 0x14, R_LED0_PWM = 0x18;
  uint8_t _brightness = 200;

  void wr(uint8_t reg, uint8_t v) {
    Wire.beginTransmission(0x2C);
    Wire.write(reg);
    Wire.write(v);
    Wire.endTransmission();
  }

 public:
  bool init(uint8_t brightness) override {
    Wire.beginTransmission(0x2C);
    if (Wire.endTransmission() != 0) return false;
    wr(R_DEVCFG, 0x01);
    wr(R_MAXCUR, 0x01);
    wr(R_ENABLE, 0x00);
    wr(R_DIMMODE, 0x4E);
    wr(R_ENGINE, 0xF0);
    for (uint8_t i = 0; i < 4; i++) wr(R_LED0_DC + i, 200);
    wr(R_ENABLE, 0x0F);
    wr(R_UPDATE, 0x55);
    delay(5);
    setBrightness(brightness);
    return true;
  }

  void setBrightness(uint8_t b) override {
    for (uint8_t i = 0; i < 4; i++) wr(R_LED0_PWM + i, b);
    _brightness = b;
  }
  uint8_t getBrightness() const { return _brightness; }
};

class L2Panel : public lgfx::LGFX_Device {
  lgfx::Panel_NV3031B _panel;
  lgfx::Bus_SPI _bus;
  lgfx::Touch_GT911 _touch;
  L2Backlight _light;

 public:
  bool init_impl(bool use_reset, bool use_clear) override {
    _light.init(_light.getBrightness());
    bool ok = LGFX_Device::init_impl(use_reset, use_clear);
    // GT911 probing can leave the I2C peripheral busy; cycle it.
    Wire.end();
    Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL);
    Wire.setClock(400000);
    return ok;
  }

  L2Panel() {
    {
      auto c = _bus.config();
      c.spi_host = SPI3_HOST;
      c.spi_mode = 3;
      c.freq_write = 75000000;
      c.freq_read = 16000000;
      c.pin_sclk = 42;
      c.pin_io0 = 41;
      c.pin_io1 = 40;
      c.pin_io2 = 39;
      c.pin_io3 = 38;
      _bus.config(c);
      _panel.setBus(&_bus);
    }
    {
      auto c = _panel.config();
      c.pin_cs = 46;
      c.pin_rst = -1;
      c.pin_busy = -1;
      c.panel_width = 240;
      c.panel_height = 320;
      c.memory_width = 240;
      c.memory_height = 320;
      c.offset_x = 0;
      c.offset_y = 0;
      c.offset_rotation = 1;
      c.invert = true;
      c.rgb_order = true;
      c.dlen_16bit = false;
      c.bus_shared = false;
      _panel.config(c);
    }
    {
      auto c = _touch.config();
      c.pin_cs = -1;
      c.x_min = 0;
      c.x_max = 239;
      c.y_min = 0;
      c.y_max = 319;
      c.pin_int = -1;
      c.offset_rotation = 2;
      c.i2c_port = 0;
      c.i2c_addr = 0x5D;
      c.pin_sda = PIN_I2C_SDA;
      c.pin_scl = PIN_I2C_SCL;
      c.bus_shared = false;
      c.freq = 400000;
      _touch.config(c);
      _panel.setTouch(&_touch);
    }
    _panel.setLight(&_light);
    setPanel(&_panel);
  }
};
