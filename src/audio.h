#pragma once
#include <stdint.h>

enum : uint8_t { SFX_STEP = 0, SFX_DIG, SFX_BREAK, SFX_PLACE, SFX_HURT, SFX_UI, SFX_COUNT };

namespace audio {
bool init();
void play(uint8_t id, float gain);
void setVolume(uint8_t v);
bool ready();
// Call from the game loop; all amplifier I2C access stays on that thread.
void service();
// True while a sound is queued or mixed, for deferring SD writes.
bool busy();
}
