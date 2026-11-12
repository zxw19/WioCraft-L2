#include "diag.h"
#include <Arduino.h>
#include <esp_attr.h>
#include <esp_system.h>
#include <esp_rom_sys.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

namespace diag {
struct Breadcrumb {
  uint32_t magic;
  uint32_t magicInv;
  uint32_t boots;
  uint32_t stage;
  uint32_t uptime;
  uint32_t frames;
};
static const uint32_t MAGIC = 0x57434432u;
RTC_NOINIT_ATTR static volatile Breadcrumb s_rtc;
static Breadcrumb s_previous;
static esp_reset_reason_t s_reason;
static uint32_t s_nextReport;

static const char* stageName(uint32_t stage) {
  static const char* names[] = {"boot","touch","update","audio","sky","world","present","save","idle"};
  return stage <= IDLE ? names[stage] : "unknown";
}
static const char* reasonName(esp_reset_reason_t reason) {
  switch (reason) {
    case ESP_RST_POWERON: return "poweron";
    case ESP_RST_EXT: return "external";
    case ESP_RST_SW: return "software";
    case ESP_RST_PANIC: return "panic";
    case ESP_RST_INT_WDT: return "interrupt-watchdog";
    case ESP_RST_TASK_WDT: return "task-watchdog";
    case ESP_RST_WDT: return "watchdog";
    case ESP_RST_DEEPSLEEP: return "deep-sleep";
    case ESP_RST_BROWNOUT: return "brownout";
    case ESP_RST_SDIO: return "sdio";
    default: return "unknown";
  }
}
void begin() {
  s_reason = esp_reset_reason();
  bool valid = s_reason != ESP_RST_POWERON && s_rtc.magic == MAGIC &&
               s_rtc.magicInv == ~MAGIC && s_rtc.stage <= IDLE;
  if (valid) {
    s_previous = {s_rtc.magic,s_rtc.magicInv,s_rtc.boots,s_rtc.stage,s_rtc.uptime,s_rtc.frames};
  } else {
    s_previous = {};
  }
  s_rtc.magic = 0;
  s_rtc.magicInv = ~MAGIC;
  s_rtc.boots = valid ? s_previous.boots + 1 : 1;
  s_rtc.stage = BOOT;
  s_rtc.uptime = millis();
  s_rtc.frames = 0;
  s_rtc.magic = MAGIC;
  s_nextReport = millis() + 5000;
  status();
}
void mark(Stage stage) {
  s_rtc.stage = stage;
  s_rtc.uptime = millis();
}
void status() {
  Serial.printf("[health] reset=%s(%d) raw=%d/%d boot=%u previous=%s at=%ums frame=%u now=%s heap=%u stackMin=%u\n",
                reasonName(s_reason),(int)s_reason,
                (int)esp_rom_get_reset_reason(0),(int)esp_rom_get_reset_reason(1),(unsigned)s_rtc.boots,
                s_previous.magic == MAGIC ? stageName(s_previous.stage) : "none",
                (unsigned)s_previous.uptime,(unsigned)s_previous.frames,
                stageName(s_rtc.stage),(unsigned)esp_get_free_heap_size(),
                (unsigned)uxTaskGetStackHighWaterMark(nullptr));
}
void tick() {
  ++s_rtc.frames;
  s_rtc.uptime = millis();
  if (Serial && (int32_t)(millis() - s_nextReport) >= 0) {
    status();
    s_nextReport = millis() + 30000;
  }
}
}
