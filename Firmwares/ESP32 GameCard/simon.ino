// ============================================================================
//  simon.ino - Simon memory game (single player)
//  Watch the flashing pads, then repeat the sequence with the D-pad. B quits.
//  Pad = direction: matches BTN_UP/DOWN/LEFT/RIGHT (0..3).
// ============================================================================
#include "common.h"

#define SIM_MAX 64

enum { SIM_SHOW, SIM_INPUT, SIM_NEXT, SIM_FAIL };

static uint8_t  seq[SIM_MAX];
static int      seqLen, showIdx, inputIdx, simState;
static bool     flashing;
static int      flashDir;
static uint32_t phaseT, fbUntil;
static int      fbDir;

// Pad rectangles: {x, y, w, h} indexed by direction 0..3.
static const int PADS[4][4] = {
  { 46, 2,  36, 18 },   // UP
  { 46, 44, 36, 18 },   // DOWN
  { 4,  22, 40, 20 },   // LEFT
  { 84, 22, 40, 20 },   // RIGHT
};

static void addStep() { if (seqLen < SIM_MAX) seq[seqLen++] = random(4); }

static void startShow() {
  simState = SIM_SHOW; showIdx = 0; flashing = false; phaseT = millis();
}

void simonInit() {
  seqLen = 0;
  addStep();
  fbUntil = 0; fbDir = -1;
  startShow();
}

static void drawPads(int hi) {
  display.clearDisplay();
  for (int d = 0; d < 4; d++) {
    const int* p = PADS[d];
    if (d == hi) display.fillRect(p[0], p[1], p[2], p[3], SSD1306_WHITE);
    else         display.drawRect(p[0], p[1], p[2], p[3], SSD1306_WHITE);
  }
  char buf[12]; snprintf(buf, sizeof(buf), "%d", seqLen);
  display.setCursor(60, 27); display.print(buf);
  display.display();
}

void simonUpdate() {
  if (simState == SIM_FAIL) {
    if (btnPressed[BTN_A]) { simonInit(); return; }
    if (btnPressed[BTN_B]) { setState(ST_MENU); return; }
    display.clearDisplay();
    uiCenter("WRONG!", 14, 2);
    char buf[20]; snprintf(buf, sizeof(buf), "Reached %d", seqLen);
    uiCenter(buf, 36);
    uiCenter("A:retry B:menu", 52);
    display.display();
    return;
  }
  if (btnPressed[BTN_B]) { setState(ST_MENU); return; }

  uint32_t now = millis();

  if (simState == SIM_SHOW) {
    if (!flashing) {
      if (now - phaseT >= 220) { flashDir = seq[showIdx]; flashing = true; phaseT = now; }
    } else {
      if (now - phaseT >= 400) {
        flashing = false; phaseT = now; showIdx++;
        if (showIdx >= seqLen) { simState = SIM_INPUT; inputIdx = 0; }
      }
    }
    drawPads(flashing ? flashDir : -1);
    return;
  }

  if (simState == SIM_NEXT) {          // brief pause before replaying, grown
    if (now - phaseT >= 600) { addStep(); startShow(); }
    drawPads(-1);
    return;
  }

  // SIM_INPUT
  int pressed = -1;
  if (btnPressed[BTN_UP])    pressed = 0;
  if (btnPressed[BTN_DOWN])  pressed = 1;
  if (btnPressed[BTN_LEFT])  pressed = 2;
  if (btnPressed[BTN_RIGHT]) pressed = 3;

  if (pressed >= 0) {
    if (pressed == seq[inputIdx]) {
      fbDir = pressed; fbUntil = now + 150;
      inputIdx++;
      if (inputIdx >= seqLen) { simState = SIM_NEXT; phaseT = now; }
    } else {
      simState = SIM_FAIL;
      return;
    }
  }
  drawPads(now < fbUntil ? fbDir : -1);
}
