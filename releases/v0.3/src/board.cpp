#include "board.h"
#include <Wire.h>
#include "config.h"

namespace board {

static uint8_t out_shadow[2] = {0, 0};
static uint8_t cfg_shadow[2] = {0xFF, 0xFF};
static bool exp_ok = false;
static uint8_t wake_baseline = 0;

static const uint8_t TCA_OUT0 = 0x02, TCA_OUT1 = 0x03;
static const uint8_t TCA_CFG0 = 0x06, TCA_CFG1 = 0x07;
static const uint8_t TCA_IN0 = 0x00;

static bool expWrite(uint8_t reg, uint8_t v) {
  Wire.beginTransmission(TCA9535_ADDR);
  Wire.write(reg);
  Wire.write(v);
  return Wire.endTransmission() == 0;
}

static void expOut(uint8_t pin, bool level) {
  uint8_t port = pin >> 3, bit = pin & 7;
  if (level) out_shadow[port] |= (1 << bit);
  else out_shadow[port] &= ~(1 << bit);
  expWrite(port ? TCA_OUT1 : TCA_OUT0, out_shadow[port]);
  cfg_shadow[port] &= ~(1 << bit);
  expWrite(port ? TCA_CFG1 : TCA_CFG0, cfg_shadow[port]);
}

static void expInput(uint8_t pin) {
  uint8_t port = pin >> 3, bit = pin & 7;
  cfg_shadow[port] |= (1 << bit);
  expWrite(port ? TCA_CFG1 : TCA_CFG0, cfg_shadow[port]);
}

static void expLevel(uint8_t pin, bool level) {
  uint8_t port = pin >> 3, bit = pin & 7;
  if (level) out_shadow[port] |= (1 << bit);
  else out_shadow[port] &= ~(1 << bit);
  expWrite(port ? TCA_OUT1 : TCA_OUT0, out_shadow[port]);
}

static int expRead() {
  Wire.beginTransmission(TCA9535_ADDR);
  Wire.write(TCA_IN0);
  if (Wire.endTransmission() != 0) return -1;
  if (Wire.requestFrom((uint8_t)TCA9535_ADDR, (uint8_t)1) != 1) return -1;
  return Wire.read();
}

bool begin() {
  Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL);
  Wire.setClock(400000);
  delay(5);

  Wire.beginTransmission(TCA9535_ADDR);
  if (Wire.endTransmission() != 0) return false;
  exp_ok = true;

  expInput(EXP_WAKE_BTN);
  expInput(EXP_I2C_IRQ);
  expInput(EXP_SD_DETECT);

  expOut(EXP_OTG_EN, LOW);
  delay(10);
  expOut(EXP_PA_EN, LOW);
  delay(10);
  expOut(EXP_TF_EN, HIGH);
  delay(10);
  expOut(EXP_BAT_ADC_EN, HIGH);
  delay(10);
  expOut(EXP_GNSS_EN, LOW);     // game does not need GNSS
  expOut(EXP_GNSS_RST, LOW);
  expOut(EXP_USER_LED, HIGH);   // active low -> off
  delay(10);
  expOut(EXP_GROVE_EN, LOW);    // grove rail off
  delay(10);

  expOut(EXP_LCD_EN, HIGH);
  delay(50);
  expOut(EXP_LCD_RST, HIGH);
  delay(5);
  expLevel(EXP_LCD_RST, LOW);
  delay(10);
  expLevel(EXP_LCD_RST, HIGH);
  delay(500);                   // NV3031B settle
  expOut(EXP_LCD_CS, HIGH);
  delay(10);

  // GT911: INT low during reset selects I2C address 0x5D
  expOut(EXP_TP_RST, LOW);
  expOut(EXP_TP_INT, LOW);
  delay(10);
  expLevel(EXP_TP_RST, HIGH);
  delay(60);

  int in = expRead();
  wake_baseline = (in >= 0) ? (uint8_t)(in & 1) : 0;

  pinMode(PIN_USER_BTN, INPUT_PULLUP);
  return true;
}

void setAmp(bool on) {
  if (exp_ok) expLevel(EXP_PA_EN, on);
}

void setLed(bool on) {
  if (exp_ok) expLevel(EXP_USER_LED, !on);
}

bool sdCardPresent() {
  if (!exp_ok) return false;
  int in = expRead();
  if (in < 0) return false;
  return (in & (1 << EXP_SD_DETECT)) == 0;   // active low
}

bool wakePressed() {
  if (!exp_ok) return false;
  int in = expRead();
  if (in < 0) return false;
  uint8_t v = (uint8_t)(in & 1);
  return v != wake_baseline;
}

// ADS1115 @0x48, AIN0, x2 divider
uint16_t batteryMilliVolts() {
  if (!exp_ok) return 0;
  if (!expWrite(EXP_BAT_ADC_EN, HIGH)) {}
  Wire.beginTransmission(0x48);
  Wire.write((uint8_t)0x01);            // config
  Wire.write((uint8_t)0xC3);            // OS=1, MUX=AIN0-GND, PGA=+-4.096V
  Wire.write((uint8_t)0x83);            // single shot, 128 SPS, comp off
  if (Wire.endTransmission() != 0) return 0;
  delay(2);
  Wire.beginTransmission(0x48);
  Wire.write((uint8_t)0x00);
  if (Wire.endTransmission() != 0) return 0;
  if (Wire.requestFrom((uint8_t)0x48, (uint8_t)2) != 2) return 0;
  int16_t raw = (int16_t)((Wire.read() << 8) | Wire.read());
  if (raw < 0) raw = 0;
  // 125 uV/LSB, x2 divider
  return (uint16_t)((int32_t)raw * 250 / 1000);
}

}  // namespace board
