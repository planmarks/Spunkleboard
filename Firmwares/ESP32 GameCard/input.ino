// ============================================================================
//  input.ino - button polling, debounce, edge detection, auto-repeat
// ============================================================================
#include "common.h"

#define DEBOUNCE_MS 15

static bool     rawPrev[NUM_BTNS]   = { false };
static bool     stable[NUM_BTNS]    = { false };
static uint32_t lastChange[NUM_BTNS] = { 0 };
static uint32_t heldSince[NUM_BTNS]  = { 0 };
static uint32_t lastRepeat[NUM_BTNS] = { 0 };

// Poll all buttons once per frame. Fills btnDown[] (held) and btnPressed[]
// (rising edge, true for exactly one frame).
void pollButtons() {
  uint32_t now = millis();
  for (int i = 0; i < NUM_BTNS; i++) {
    bool raw = (digitalRead(BTN_PINS[i]) == LOW);   // active-low
    if (raw != rawPrev[i]) { rawPrev[i] = raw; lastChange[i] = now; }

    btnPressed[i] = false;
    if (now - lastChange[i] >= DEBOUNCE_MS && raw != stable[i]) {
      stable[i] = raw;
      if (raw) {                       // just pressed
        btnPressed[i] = true;
        heldSince[i]  = now;
        lastRepeat[i] = now;
      }
    }
    btnDown[i] = stable[i];
  }
}

// Returns true on the initial press and then repeatedly while held:
// once after firstMs, then every repeatMs. Handy for menu scrolling.
bool btnHeldRepeat(uint8_t b, uint16_t firstMs, uint16_t repeatMs) {
  if (btnPressed[b]) return true;
  if (!btnDown[b])   return false;
  uint32_t now = millis();
  if (now - heldSince[b] < firstMs) return false;
  if (now - lastRepeat[b] >= repeatMs) { lastRepeat[b] = now; return true; }
  return false;
}
