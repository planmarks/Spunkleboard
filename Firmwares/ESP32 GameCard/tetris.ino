// ============================================================================
//  tetris.ino - Tetris (single player)
//  Controls: LEFT/RIGHT move, A rotate, DOWN soft drop, UP hard drop, B quit.
// ============================================================================
#include "common.h"

#define TB_W    10
#define TB_H    20
#define TB_CELL 3
#define TB_X0   3
#define TB_Y0   3

// 7 tetrominoes x 4 rotations, each a 4x4 bitmap (bit 0x8000 = top-left).
static const uint16_t PIECES[7][4] = {
  { 0x0F00, 0x2222, 0x00F0, 0x4444 },  // I
  { 0x44C0, 0x8E00, 0x6440, 0x0E20 },  // J
  { 0x4460, 0x0E80, 0xC440, 0x2E00 },  // L
  { 0xCC00, 0xCC00, 0xCC00, 0xCC00 },  // O
  { 0x06C0, 0x8C40, 0x6C00, 0x4620 },  // S
  { 0x0E40, 0x4C40, 0x4E00, 0x4640 },  // T
  { 0x0C60, 0x4C80, 0xC600, 0x2640 },  // Z
};

static uint8_t  tb[TB_H][TB_W];
static int      tType, tRot, tX, tY, tNext;
static int      tScore, tLines;
static bool     tOver;
static uint32_t tLast;
static uint16_t tInterval;

static inline bool cell(uint16_t m, int r, int c) {
  return m & (0x8000 >> (r * 4 + c));
}

static bool tCollide(int type, int rot, int x, int y) {
  uint16_t m = PIECES[type][rot];
  for (int r = 0; r < 4; r++)
    for (int c = 0; c < 4; c++) {
      if (!cell(m, r, c)) continue;
      int bx = x + c, by = y + r;
      if (bx < 0 || bx >= TB_W || by >= TB_H) return true;
      if (by >= 0 && tb[by][bx]) return true;
    }
  return false;
}

static void tSpawn() {
  tType = tNext; tNext = random(7);
  tRot = 0; tX = 3; tY = 0;
  if (tCollide(tType, tRot, tX, tY)) tOver = true;
}

static void tLock() {
  uint16_t m = PIECES[tType][tRot];
  for (int r = 0; r < 4; r++)
    for (int c = 0; c < 4; c++)
      if (cell(m, r, c) && tY + r >= 0) tb[tY + r][tX + c] = 1;

  int cleared = 0;
  for (int r = TB_H - 1; r >= 0; ) {
    bool full = true;
    for (int c = 0; c < TB_W; c++) if (!tb[r][c]) { full = false; break; }
    if (full) {
      for (int rr = r; rr > 0; rr--) memcpy(tb[rr], tb[rr - 1], TB_W);
      memset(tb[0], 0, TB_W);
      cleared++;
    } else r--;
  }
  if (cleared) {
    tLines += cleared;
    static const int pts[5] = { 0, 100, 300, 500, 800 };
    tScore += pts[cleared];
    if (tInterval > 130) tInterval -= cleared * 12;
  }
  tSpawn();
}

void tetrisInit() {
  memset(tb, 0, sizeof(tb));
  tScore = 0; tLines = 0; tOver = false;
  tInterval = 600;
  tNext = random(7);
  tSpawn();
  tLast = millis();
}

static void tRender() {
  display.clearDisplay();
  // Well border
  display.drawRect(TB_X0 - 1, TB_Y0 - 1, TB_W * TB_CELL + 2, TB_H * TB_CELL + 2,
                   SSD1306_WHITE);
  // Settled blocks
  for (int r = 0; r < TB_H; r++)
    for (int c = 0; c < TB_W; c++)
      if (tb[r][c])
        display.fillRect(TB_X0 + c * TB_CELL, TB_Y0 + r * TB_CELL,
                         TB_CELL, TB_CELL, SSD1306_WHITE);
  // Falling piece
  uint16_t m = PIECES[tType][tRot];
  for (int r = 0; r < 4; r++)
    for (int c = 0; c < 4; c++)
      if (cell(m, r, c) && tY + r >= 0)
        display.fillRect(TB_X0 + (tX + c) * TB_CELL, TB_Y0 + (tY + r) * TB_CELL,
                         TB_CELL, TB_CELL, SSD1306_WHITE);
  // Side panel
  int px = TB_X0 + TB_W * TB_CELL + 6;
  display.setCursor(px, 4);  display.print("NEXT");
  uint16_t nm = PIECES[tNext][0];
  for (int r = 0; r < 4; r++)
    for (int c = 0; c < 4; c++)
      if (cell(nm, r, c))
        display.fillRect(px + c * 4, 14 + r * 4, 4, 4, SSD1306_WHITE);
  display.setCursor(px, 36); display.print("LN");
  display.setCursor(px, 46); display.print(tLines);
  display.display();
}

void tetrisUpdate() {
  if (tOver) {
    if (btnPressed[BTN_A]) { tetrisInit(); return; }
    if (btnPressed[BTN_B]) { setState(ST_MENU); return; }
    display.clearDisplay();
    uiCenter("GAME OVER", 14);
    char buf[20]; snprintf(buf, sizeof(buf), "Lines: %d", tLines);
    uiCenter(buf, 30);
    uiCenter("A:retry B:menu", 50);
    display.display();
    return;
  }
  if (btnPressed[BTN_B]) { setState(ST_MENU); return; }

  if (btnHeldRepeat(BTN_LEFT, 220, 90)  && !tCollide(tType, tRot, tX - 1, tY)) tX--;
  if (btnHeldRepeat(BTN_RIGHT, 220, 90) && !tCollide(tType, tRot, tX + 1, tY)) tX++;

  if (btnPressed[BTN_A]) {                     // rotate w/ simple wall kick
    int nr = (tRot + 1) & 3;
    if      (!tCollide(tType, nr, tX,     tY)) tRot = nr;
    else if (!tCollide(tType, nr, tX - 1, tY)) { tX--; tRot = nr; }
    else if (!tCollide(tType, nr, tX + 1, tY)) { tX++; tRot = nr; }
  }

  if (btnPressed[BTN_UP]) {                     // hard drop
    while (!tCollide(tType, tRot, tX, tY + 1)) tY++;
    tLock();
    tRender();
    return;
  }

  uint16_t iv = btnDown[BTN_DOWN] ? 55 : tInterval;
  if (millis() - tLast >= iv) {
    tLast = millis();
    if (!tCollide(tType, tRot, tX, tY + 1)) tY++;
    else tLock();
  }
  tRender();
}
