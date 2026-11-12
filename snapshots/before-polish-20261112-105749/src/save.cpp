#include "save.h"
#include <SD_MMC.h>
#include <FS.h>
#include <string.h>
#include "board.h"
#include "config.h"
#include "world.h"

namespace save {

static bool s_ready = false;
static const char* DIR = "/wicraft";
static const char* FILE_PATH = "/wicraft/save.bin";
static const uint32_t MAGIC = 0x57433130u;   // "WC10"

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
  if (!SD_MMC.exists(DIR)) SD_MMC.mkdir(DIR);
  s_ready = true;
  return true;
}

bool ready() { return s_ready; }

bool load(SaveState& s) {
  if (!s_ready) return false;
  File f = SD_MMC.open(FILE_PATH, FILE_READ);
  if (!f) return false;
  Header h;
  size_t n = f.read((uint8_t*)&h, sizeof(h));
  if (n != sizeof(h) || h.magic != MAGIC) { f.close(); return false; }
  if (h.count > 8000) { f.close(); return false; }
  s = h.st;
  world::clearEdits();
  for (uint16_t i = 0; i < h.count; i++) {
    Edit e;
    if (f.read((uint8_t*)&e, sizeof(e)) != sizeof(e)) break;
    world::applyEdit(e, true);
  }
  f.close();
  return true;
}

void store(const SaveState& s) {
  if (!s_ready) return;
  Header h;
  memset(&h, 0, sizeof(h));
  h.magic = MAGIC;
  h.seed = 0;
  h.st = s;
  h.count = (uint16_t)world::editCount();

  File f = SD_MMC.open("/wicraft/save.tmp", FILE_WRITE);
  if (!f) return;
  bool ok = f.write((const uint8_t*)&h, sizeof(h)) == sizeof(h);
  for (int i = 0; ok && i < world::editCount(); i++) {
    const Edit* e = world::editAt(i);
    if (!e) break;
    ok = f.write((const uint8_t*)e, sizeof(Edit)) == sizeof(Edit);
  }
  f.flush();
  f.close();
  if (!ok) {
    SD_MMC.remove("/wicraft/save.tmp");
    return;
  }
  SD_MMC.remove(FILE_PATH);
  SD_MMC.rename("/wicraft/save.tmp", FILE_PATH);   // atomic-ish commit
}

}  // namespace save
