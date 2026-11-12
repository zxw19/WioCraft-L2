#include <Arduino.h>
#include "panel.h"
#include "game.h"

static L2Panel g_l2panel;

void setup() { gameInit(&g_l2panel); }
void loop() { gameLoop(); }
