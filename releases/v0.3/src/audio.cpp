#include "audio.h"
#include <Arduino.h>
#include <Wire.h>
#include <driver/i2s.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <atomic>
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
static std::atomic<bool> s_ready{false};
static bool s_ampOn = false;
static std::atomic<bool> s_mixing{false};
static std::atomic<bool> s_ampEnabled{false};
static std::atomic<uint32_t> s_ampStarted{0};
static uint32_t s_lastVoice = 0;   // main thread only
static std::atomic<uint8_t> s_volume{65};
static QueueHandle_t s_commands = nullptr;

struct PlayCommand { uint8_t id; float gain; };

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
                phase += (130 - 50 * t) / SR * 6.2831853f;
                v = (lp * 0.7f + sinf(phase) * 0.3f) * env * 0.9f; break;
        case 2: env = powf(1 - t, 2.2f); lp += (nz - lp) * 0.6f; v = lp * env; break;
        case 3: env = powf(1 - t, 3.0f); phase += 420.0f / SR * 6.2831853f;
                v = sinf(phase) * env * 0.7f + nz * env * 0.15f; break;
        case 4: env = (1 - t); phase += (300 - 180 * t) / SR * 6.283f;
                v = (sinf(phase) > 0 ? 0.5f : -0.5f) * env * 0.6f; break;
        default: env = (1 - t); phase += 880.0f / SR * 6.2831853f;
                 v = sinf(phase) * env * 0.5f; break;
      }
      // A short attack removes the initial full-scale click and current spike.
      float attack = k < 88 ? k / 88.0f : 1.0f;
      int s = (int)(v * attack * 22000.0f);
      if (s > 32000) s = 32000;
      if (s < -32000) s = -32000;
      s_sfx[i].data[k] = (int16_t)s;
    }
  }
}

static void mixerTask(void*) {
  // Exactly 128 stereo frames; the previous 256-frame buffer was half filled.
  static int16_t buf[128 * 2];
  unsigned failures = 0;
  for (;;) {
    PlayCommand cmd;
    while (xQueueReceive(s_commands, &cmd, 0) == pdTRUE) {
      int slot = -1;
      for (int i = 0; i < MAX_VOICES; i++) {
        if (!s_voices[i].active) { slot = i; break; }
      }
      // Quiet footsteps do not cut off a more important in-flight sound.
      if (slot < 0 && cmd.id == SFX_STEP) continue;
      if (slot < 0) {
        slot = 0;
        for (int i = 1; i < MAX_VOICES; i++)
          if (s_voices[i].pos > s_voices[slot].pos) slot = i;
      }
      s_voices[slot] = {(int)cmd.id, 0, cmd.gain, true};
    }
    bool any = false;
    for (int i = 0; i < MAX_VOICES; i++) if (s_voices[i].active) any = true;
    s_mixing.store(any);
    if (!any) {
      vTaskDelay(pdMS_TO_TICKS(3));
      continue;
    }
    // Only the game loop enables the amplifier, so Wire is never used here.
    if (!s_ampEnabled.load() || millis() - s_ampStarted.load() < 5) {
      vTaskDelay(pdMS_TO_TICKS(2));
      continue;
    }

    const int volume = s_volume.load();
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
      int scaled = acc * volume / 100;
      if (scaled > 20000) scaled = 20000;
      if (scaled < -20000) scaled = -20000;
      buf[f * 2] = (int16_t)scaled;
      buf[f * 2 + 1] = (int16_t)scaled;
    }
    size_t written = 0;
    esp_err_t err = i2s_write(I2S_NUM_0, buf, sizeof(buf), &written, pdMS_TO_TICKS(50));
    if (err != ESP_OK || written != sizeof(buf)) {
      if (++failures >= 3) {
        // A failed peripheral must not hold the game or power rail hostage.
        s_ready.store(false);
        s_mixing.store(false);
        xQueueReset(s_commands);
        Serial.printf("[audio] disabled after I2S error %d (%u/%u bytes)\n",
                      (int)err, (unsigned)written, (unsigned)sizeof(buf));
        vTaskDelete(nullptr);
      }
    } else {
      failures = 0;
    }
  }
}

namespace audio {

bool init() {
  board::setAmp(false);
  if (heap_caps_get_free_size(MALLOC_CAP_DMA) < 50000) return false;
  buildSfx();

  s_pins.addI2C(PinFunction::CODEC, Wire);
  s_pins.addI2S(PinFunction::CODEC, PIN_I2S_MCLK, PIN_I2S_BCK, PIN_I2S_WS, PIN_I2S_DOUT, -1);

  CodecConfig cfg;
  cfg.input_device = ADC_INPUT_NONE;
  cfg.output_device = DAC_OUTPUT_ALL;
  cfg.i2s.bits = BIT_LENGTH_16BITS;
  cfg.i2s.rate = RATE_22K;
  if (!s_codec.begin(cfg)) return false;
  s_codec.setVolume(60);

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
  if (err != ESP_OK) return false;

  i2s_pin_config_t pc = {};
  pc.mck_io_num = PIN_I2S_MCLK;
  pc.bck_io_num = PIN_I2S_BCK;
  pc.ws_io_num = PIN_I2S_WS;
  pc.data_out_num = PIN_I2S_DOUT;
  pc.data_in_num = I2S_PIN_NO_CHANGE;
  if (i2s_set_pin(I2S_NUM_0, &pc) != ESP_OK || i2s_start(I2S_NUM_0) != ESP_OK) {
    i2s_driver_uninstall(I2S_NUM_0);
    return false;
  }
  i2s_zero_dma_buffer(I2S_NUM_0);

  for (int i = 0; i < MAX_VOICES; i++) s_voices[i].active = false;
  s_commands = xQueueCreate(8, sizeof(PlayCommand));
  if (!s_commands) { i2s_driver_uninstall(I2S_NUM_0); return false; }
  if (xTaskCreatePinnedToCore(mixerTask, "mixer", 4096, nullptr, 2, nullptr, 0) != pdPASS) {
    vQueueDelete(s_commands);
    s_commands = nullptr;
    i2s_driver_uninstall(I2S_NUM_0);
    return false;
  }
  s_ready.store(true);
  return true;
}

bool ready() { return s_ready.load(); }
void setVolume(uint8_t v) { s_volume.store(v > 100 ? 100 : v); }

bool busy() {
  return s_ready.load() && (s_mixing.load() ||
                           (s_commands && uxQueueMessagesWaiting(s_commands) != 0));
}

void service() {
  uint32_t now = millis();
  // The game loop may call service immediately after play().  Keep the amp
  // enabled briefly after a voice drains so a queued command cannot be lost.
  bool active = busy();
  if (active) s_lastVoice = now;
  bool desired = s_ready.load() && (active || (s_ampOn && now - s_lastVoice < 240));
  if (desired == s_ampOn) return;
  if (desired) {
    board::setAmp(true);
    s_ampStarted.store(now);
    s_ampOn = true;
    s_ampEnabled.store(true);
  } else {
    s_ampEnabled.store(false);
    board::setAmp(false);
    s_ampOn = false;
  }
}

void play(uint8_t id, float gain) {
  if (!s_ready.load() || !s_commands || id >= SFX_COUNT || !isfinite(gain)) return;
  if (gain <= 0) return;
  if (gain > 0.65f) gain = 0.65f;
  PlayCommand cmd = {id, gain};
  // Non-blocking submission: the mixer exclusively owns every Voice.
  xQueueSend(s_commands, &cmd, 0);
}

}  // namespace audio
