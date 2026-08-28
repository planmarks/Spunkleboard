// ============================================================================
//  mines.ino - Minesweeper (single player)
//  Controls: D-pad move cursor, tap A reveal, HOLD A to flag, B quit.
// ============================================================================
#include "common.h"

#define MR      5
#define MC      10
#define MCELL   10
#define M_X0    14
#define M_Y0    13
#define M_MINES 8

static bool     mine[MR][MC], shown[MR][MC], flag[MR][MC];
static uint8_t  around[MR][MC];
static int      curR, curC, shownCount;
static bool     mnOver, mnWon, placed;
static bool     aWasDown; static uint32_t aDownAt;

void minesInit() {
  memset(mine, 0, sizeof(mine));
  memset(shown, 0, sizeof(shown));
  memset(flag, 0, sizeof(flag));
  memset(around, 0, sizeof(around));
  curR = curC = 0; shownCount = 0;
  mnOver = mnWon = placed = false;
  aWasDown = false;
}

static void placeMines(int sr, int sc) {
  int n = 0;
  while (n < M_MINES) {
    int r = random(MR), c = random(MC);
    if (mine[r][c] || (r == sr && c == sc)) continue;
    mine[r][c] = true; n++;
  }
  for (int r = 0; r < MR; r++)
    for (int c = 0; c < MC; c++) {
      int cnt = 0;
      for (int dr = -1; dr <= 1; dr++)
        for (int dc = -1; dc <= 1; dc++) {
          int nr = r + dr, nc = c + dc;
          if (nr >= 0 && nr < MR && nc >= 0 && nc < MC && mine[nr][nc]) cnt++;
        }
      around[r][c] = cnt;
    }
  placed = true;
}

static void floodReveal(int r0, int c0) {
  static uint8_t sr[MR * MC * 8], sc[MR * MC * 8];
  int sp = 0;
  sr[sp] = r0; sc[sp] = c0; sp++;
  while (sp) {
    sp--; int r = sr[sp], c = sc[sp];
    if (r < 0 || r >= MR || c < 0 || c >= MC) continue;
    if (shown[r][c] || flag[r][c]) continue;
    shown[r][c] = true; shownCount++;
    if (around[r][c] == 0) {
      for (int dr = -1; dr <= 1; dr++)
        for (int dc = -1; dc <= 1; dc++)
          if (sp < (int)sizeof(sr)) { sr[sp] = r + dr; sc[sp] = c + dc; sp++; }
    }
  }
}

static void revealCursor() {
  if (shown[curR][curC] || flag[curR][curC]) return;
  if (!placed) placeMines(curR, curC);
  if (mine[curR][curC]) { mnOver = true; mnWon = false; return; }
  floodReveal(curR, curC);
  if (shownCount == MR * MC - M_MINES) { mnOver = true; mnWon = true; }
}

static void mnRender() {
  display.clearDisplay();
  uiTitle("MINESWEEPER");
  for (int r = 0; r < MR; r++)
    for (int c = 0; c < MC; c++) {
      int x = M_X0 + c * MCELL, y = M_Y0 + r * MCELL;
      if (shown[r][c]) {
        if (mine[r][c]) display.fillRect(x + 2, y + 2, 6, 6, SSD1306_WHITE);
        else if (around[r][c]) {
          char d = '0' + around[r][c];
          display.setCursor(x + 3, y + 2); display.write(d);
        }
      } else {
        display.drawRect(x, y, MCELL, MCELL, SSD1306_WHITE);
        if (flag[r][c]) { display.setCursor(x + 3, y + 2); display.write('F'); }
      }
    }
  // Cursor
  int cx = M_X0 + curC * MCELL, cy = M_Y0 + curR * MCELL;
  display.drawRect(cx, cy, MCELL, MCELL, SSD1306_WHITE);
  display.drawRect(cx + 1, cy + 1, MCELL - 2, MCELL - 2, SSD1306_WHITE);
  display.display();
}

void minesUpdate() {
  if (mnOver) {
    if (btnPressed[BTN_A]) { minesInit(); return; }
    if (btnPressed[BTN_B]) { setState(ST_MENU); return; }
    display.clearDisplay();
    uiCenter(mnWon ? "CLEARED!" : "BOOM!", 20, 2);
    uiCenter("A:retry B:menu", 50);
    display.display();
    return;
  }
  if (btnPressed[BTN_B]) { setState(ST_MENU); return; }

  if (btnHeldRepeat(BTN_LEFT, 300, 130))  curC = (curC + MC - 1) % MC;
  if (btnHeldRepeat(BTN_RIGHT, 300, 130)) curC = (curC + 1) % MC;
  if (btnHeldRepeat(BTN_UP, 300, 130))    curR = (curR + MR - 1) % MR;
  if (btnHeldRepeat(BTN_DOWN, 300, 130))  curR = (curR + 1) % MR;

  // Tap A = reveal, hold A >= 350ms = toggle flag (acted on release).
  if (btnPressed[BTN_A]) { aWasDown = true; aDownAt = millis(); }
  if (aWasDown && !btnDown[BTN_A]) {
    aWasDown = false;
    if (millis() - aDownAt >= 350) {
      if (!shown[curR][curC]) flag[curR][curC] = !flag[curR][curC];
    } else {
      revealCursor();
    }
  }
  mnRender();
}
