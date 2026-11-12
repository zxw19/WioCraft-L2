#pragma once
#include <stdint.h>

// RTC breadcrumbs survive most software/panic/watchdog resets. They do not
// replace a panic backtrace, but identify which subsystem was last executing
// when USB CDC disconnected before the host could read the panic message.
namespace diag {
enum Stage : uint32_t { BOOT, TOUCH, UPDATE, AUDIO, SKY, WORLD, PRESENT, SAVE, IDLE };
void begin();
void mark(Stage stage);
void tick();
void status();
}
