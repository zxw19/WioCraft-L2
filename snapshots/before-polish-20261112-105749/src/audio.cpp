#include "audio.h"
#include <Arduino.h>
#include <Wire.h>
#include <driver/i2s.h>
#include <esp_heap_caps.h>
#include <esp32-hal-psram.h>
#include <math.h>
#include <string.h>
#include "AudioBoard.h"
#include "board.h"
#include "config.h"

#define SR 22050
#define MAX_VOICES 4

static DriverPins s_pins;
static AudioBoard s_codec(AudioDriverES8311, s_pins);
static bool s_ready = false;
static bool s_ampOn = false;
static uint32_t s_lastVoice = 0;
static uint8_t s_volume = 80;

struct Sample { int16_t* data; int len; };
static Sample s_sfx[SFX_COUNT];

struct Voice { int id; int pos; float gain; bool active; };
static Voice s_voices[MAX_VOICES];

static inline uint32_t lcg(uint32_t& s) {
  s = s * 1664525u + 1013904223u;
  return s;
}

static int16_t* allocSamples(int frames) {
  return (int16_t*)ps_malloc(sizeof(int16_t) * frames);
}

static void buildSfx() {
  uint32_t seed = 12345;
  struct Spec { int frames; int kind; };
  const Spec spec[SFX_COUNT] = {
      {SR * 60 / 1000, 0},    // step: filtered noise burst
      {SR * 90 / 1000, 1},    // dig: noise + thump
      {SR * 200 / 1000, 2},   // break: noise decay
      {SR * 70 / 1000, 3},    // place: click
      {SR * 260 / 1000, 4},   // hurt: descending tone
      {SR * 45 / 1000, 5},    // ui: blip
  };
  for (int i = 0; i < SFX_COUNT; i++) {
    int n = spec[i].frames;
    s_sfx[i].data = allocSamples(n);
    s_sfx[i].len = n;
    if (!s_sfx[i].data) continue;
    float lp = 0;
    float phase = 0;
    for (int k = 0; k < n; k++) {
      float t = (float)k / (float)n;
      float env = 0, v = 0;
      float nz = ((int)(lcg(seed) & 0xFFFF) - 32768) / 32768.0f;
      switch (spec[i].kind) {
        case 0: env = (1 - t) * (1 - t); lp += (nz - lp) * 0.25f; v = lp * env * 0.8f; break;
        case 1: env = (1 - t) * (1 - t); lp += (nz - lp) * 0.45f;
                v = (lp * 0.7f + sinf(t * 6.283f * (90 - 40 * t)) * 0.3f) * env * 0.9f; break;
        case 2: env = powf(1 - t, 2.2f); lp += (nz - lp) * 0.6f; v = lp * env; break;
        case 3: env = powf(1 - t, 3.0f); v = sinf(t * 6.283f * 420) * env * 0.7f + nz * env * 0.15f; break;
        case 4: env = (1 - t); phase += (300 - 180 * t) / SR * 6.283f;
                v = (sinf(phase) > 0 ? 0.5f : -0.5f) * env * 0.6f; break;
        default: env = (1 - t); v = sinf(t * 6.283f * 880) * env * 0.5f; break;
      }
      int s = (int)(v * 26000.0f);
      if (s > 32000) s = 32000;
      if (s < -32000) s = -32000;
      s_sfx[i].data[k] = (int16_t)s;
    }
  }
}

static void mixerTask(void*) {
  static int16_t buf[256 * 2];
  for (;;) {
    bool any = false;
    for (int i = 0; i < MAX_VOICES; i++) if (s_voices[i].active) any = true;

    if (!any) {
      if (s_ampOn && (millis() - s_lastVoice) > 120) {
        board::setAmp(false);
        s_ampOn = false;
      }
      vTaskDelay(pdMS_TO_TICKS(8));
      continue;
    }
    if (!s_ampOn) {
      board::setAmp(true);
      s_ampOn = true;
      vTaskDelay(pdMS_TO_TICKS(5));
    }

    for (int f = 0; f < 128; f++) {
      int acc = 0;
      for (int v = 0; v < MAX_VOICES; v++) {
        Voice& vo = s_voices[v];
        if (!vo.active) continue;
        const Sample& s = s_sfx[vo.id];
        if (vo.pos >= s.len || !s.data) { vo.active = false; continue; }
        acc += (int)(s.data[vo.pos] * vo.gain);
        vo.pos++;
      }
      int scaled = acc * s_volume / 100;
      if (scaled > 32000) scaled = 32000;
      if (scaled < -32000) scaled = -32000;
      buf[f * 2] = (int16_t)scaled;
      buf[f * 2 + 1] = (int16_t)scaled;
    }
    size_t written = 0;
    i2s_write(I2S_NUM_0, buf, sizeof(buf), &written, portMAX_DELAY);
    s_lastVoice = millis();
  }
}

namespace audio {

bool init() {
  if (heap_caps_get_free_size(MALLOC_CAP_DMA) < 50000) return false;
  buildSfx();

  s_pins.addI2C(PinFunction::CODEC, Wire);
  s_pins.addI2S(PinFunction::CODEC, PIN_I2S_MCLK, PIN_I2S_BCK, PIN_I2S_WS, PIN_I2S_DOUT, -1);

  CodecConfig cfg;
  cfg.input_device = ADC_INPUT_NONE;
  cfg.output_device = DAC_OUTPUT_ALL;
  cfg.i2s.bits = BIT_LENGTH_16BITS;
  cfg.i2s.rate = RATE_44K;
  if (!s_codec.begin(cfg)) return false;
  s_codec.setVolume(70);

  i2s_config_t ic = {};
  ic.mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX);
  ic.sample_rate = SR;
  ic.bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT;
  ic.channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT;
  ic.communication_format = I2S_COMM_FORMAT_STAND_I2S;
  ic.intr_alloc_flags = 0;
  ic.dma_buf_count = 3;
  ic.dma_buf_len = 256;
  ic.tx_desc_auto_clear = true;
  esp_err_t err = i2s_driver_install(I2S_NUM_0, &ic, 0, NULL);
  if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return false;

  i2s_pin_config_t pc = {};
  pc.mck_io_num = PIN_I2S_MCLK;
  pc.bck_io_num = PIN_I2S_BCK;
  pc.ws_io_num = PIN_I2S_WS;
  pc.data_out_num = PIN_I2S_DOUT;
  pc.data_in_num = I2S_PIN_NO_CHANGE;
  i2s_set_pin(I2S_NUM_0, &pc);
  i2s_start(I2S_NUM_0);

  for (int i = 0; i < MAX_VOICES; i++) s_voices[i].active = false;
  xTaskCreatePinnedToCore(mixerTask, "mixer", 4096, nullptr, 3, nullptr, 1);
  s_ready = true;
  return true;
}

bool ready() { return s_ready; }
void setVolume(uint8_t v) { s_volume = v > 100 ? 100 : v; }

void play(uint8_t id, float gain) {
  if (!s_ready || id >= SFX_COUNT) return;
  for (int i = 0; i < MAX_VOICES; i++) {
    if (!s_voices[i].active) {
      s_voices[i].id = id;
      s_voices[i].pos = 0;
      s_voices[i].gain = gain;
      s_voices[i].active = true;
      return;
    }
  }
  // steal the oldest-ish slot
  s_voices[0].id = id;
  s_voices[0].pos = 0;
  s_voices[0].gain = gain;
  s_voices[0].active = true;
}

}  // namespace audio
