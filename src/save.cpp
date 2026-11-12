#include "save.h"
#include <SD_MMC.h>
#include <FS.h>
#include <esp32-hal-psram.h>
#include <stdlib.h>
#include <string.h>
#include "board.h"
#include "config.h"
#include "world.h"

namespace save {

static bool s_ready = false;
static const char* DIR = "/wicraft";
static const char* FILE_PATH = "/wicraft/save.bin";
static const char* BACKUP_PATH = "/wicraft/save.bak";
static const char* TEMP_PATH = "/wicraft/save.tmp";
static const uint32_t OLD_MAGIC = 0x57433130u;   // WC10, before inventory
static const uint32_t MAGIC = 0x57433131u;       // WC11

struct OldState {
  float x, y, z, yaw, pitchOff, dayTime;
  int32_t hp, sel;
};
struct OldHeader {
  uint32_t magic, seed;
  OldState st;
  uint16_t count;
};

struct Header {
  uint32_t magic;
  uint32_t seed;
  SaveState st;
  uint16_t count;
};

bool begin() {
  if (!board::sdCardPresent()) return false;
  SD_MMC.setPins(PIN_SD_CLK, PIN_SD_CMD, PIN_SD_D0);
  if (!SD_MMC.begin("/sdcard", true)) return false;   // 1-bit mode
  if (SD_MMC.cardType() == CARD_NONE) return false;
  if (!SD_MMC.exists(DIR) && !SD_MMC.mkdir(DIR)) return false;
  s_ready = true;
  return true;
}

bool ready() { return s_ready; }

// Bit inspection remains valid with the project's -ffast-math build flag,
// which permits the compiler to assume that floating point values are finite.
static bool finiteFloat(const float& v) {
  uint32_t bits;
  memcpy(&bits, &v, sizeof(bits));
  return (bits & 0x7F800000u) != 0x7F800000u;
}

static bool validState(const SaveState& s) {
  if (!finiteFloat(s.x) || !finiteFloat(s.y) || !finiteFloat(s.z) ||
      !finiteFloat(s.yaw) || !finiteFloat(s.pitchOff) || !finiteFloat(s.dayTime))
    return false;
  if (s.x < 0.0f || s.x >= (float)WORLD_X || s.z < 0.0f || s.z >= (float)WORLD_Z)
    return false;
  if (s.y < -2.0f || s.y > (float)WORLD_H + 2.0f) return false;
  if (s.pitchOff < -90.0f || s.pitchOff > 90.0f) return false;
  if (s.dayTime < 0.0f || s.dayTime >= 1.0f) return false;
  if (s.hp < 1 || s.hp > 20 || s.sel < 0 || s.sel >= 9) return false;
  for (int i = 0; i < 9; ++i) if (s.inventory[i] > 999) return false;
  return true;
}

static bool readSnapshot(const char* path, SaveState* state) {
  File f = SD_MMC.open(path, FILE_READ);
  if (!f) return false;
  Header h = {};
  uint32_t magic = 0;
  if (f.read((uint8_t*)&magic, sizeof(magic)) != sizeof(magic) || !f.seek(0)) {
    f.close(); return false;
  }
  size_t headerSize;
  if (magic == OLD_MAGIC) {
    OldHeader old;
    if (f.read((uint8_t*)&old, sizeof(old)) != sizeof(old)) { f.close(); return false; }
    h.magic = MAGIC; h.seed = old.seed; h.count = old.count;
    memcpy(&h.st, &old.st, sizeof(old.st));
    headerSize = sizeof(old);
  } else if (magic == MAGIC) {
    if (f.read((uint8_t*)&h, sizeof(h)) != sizeof(h)) { f.close(); return false; }
    headerSize = sizeof(h);
  } else { f.close(); return false; }
  // Reject truncated or implausibly large files before touching the world.
  // MAX_EDITS in world.cpp is 8192. Keep the on-disk WC10 layout compatible.
  if (h.count > 8192 || !validState(h.st)) { f.close(); return false; }
  const uint64_t expected = (uint64_t)headerSize +
                            (uint64_t)h.count * sizeof(Edit);
  if ((uint64_t)f.size() < expected) { f.close(); return false; }

  // Stage every edit before applying any.  A bad block id near the end of a
  // file must not leave a half-loaded world or poison a recovered backup.
  Edit* edits = nullptr;
  if (state && h.count) {
    edits = (Edit*)ps_malloc((size_t)h.count * sizeof(Edit));
    if (!edits) { f.close(); return false; }
  }
  for (uint16_t i = 0; i < h.count; i++) {
    Edit e;
    if (f.read((uint8_t*)&e, sizeof(e)) != sizeof(e) ||
        e.x < 0 || e.x >= WORLD_X || e.y < 0 || e.y >= WORLD_H ||
        e.z < 0 || e.z >= WORLD_Z || e.b >= B_COUNT) {
      f.close();
      free(edits);
      return false;
    }
    if (edits) edits[i] = e;
  }
  f.close();
  if (state) {
    world::clearEdits();
    for (uint16_t i = 0; i < h.count; i++) world::applyEdit(edits[i], true);
    *state = h.st;
  }
  free(edits);
  return true;
}

bool load(SaveState& s) {
  if (!s_ready) return false;
  if (readSnapshot(FILE_PATH, &s)) return true;
  if (readSnapshot(BACKUP_PATH, &s)) {
    Serial.println("[save] recovered previous snapshot");
    return true;
  }
  if (readSnapshot(TEMP_PATH, &s)) {
    Serial.println("[save] recovered pending snapshot");
    return true;
  }
  return false;
}

bool store(const SaveState& s) {
  if (!s_ready || !validState(s)) return false;
  Header h;
  memset(&h, 0, sizeof(h));
  h.magic = MAGIC;
  h.seed = 0;
  h.st = s;
  h.count = (uint16_t)world::editCount();

  // Explicitly truncate a stale temp left by an interrupted write.
  if (SD_MMC.exists(TEMP_PATH) && !SD_MMC.remove(TEMP_PATH)) return false;
  File f = SD_MMC.open(TEMP_PATH, FILE_WRITE);
  if (!f) return false;
  bool ok = f.write((const uint8_t*)&h, sizeof(h)) == sizeof(h);
  for (int i = 0; ok && i < world::editCount(); i++) {
    const Edit* e = world::editAt(i);
    if (!e) { ok = false; break; }
    ok = f.write((const uint8_t*)e, sizeof(Edit)) == sizeof(Edit);
  }
  f.flush();
  f.close();
  if (!ok || !readSnapshot(TEMP_PATH, nullptr)) {
    SD_MMC.remove(TEMP_PATH);
    return false;
  }
  // Keep the previous complete snapshot until a later successful write.
  // Either the main file or its backup survives an interrupted commit.
  bool movedOld = false;
  if (SD_MMC.exists(FILE_PATH)) {
    if (readSnapshot(FILE_PATH, nullptr)) {
      if (SD_MMC.exists(BACKUP_PATH) && !SD_MMC.remove(BACKUP_PATH)) return false;
      if (!SD_MMC.rename(FILE_PATH, BACKUP_PATH)) return false;
      movedOld = true;
    } else {
      // A recovery may have used save.bak; do not overwrite it with a corrupt
      // primary file on the next attempt.
      if (!SD_MMC.remove(FILE_PATH)) return false;
    }
  }
  if (SD_MMC.rename(TEMP_PATH, FILE_PATH)) return true;
  if (movedOld) SD_MMC.rename(BACKUP_PATH, FILE_PATH);
  return false;
}

}  // namespace save
