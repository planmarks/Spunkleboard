// ============================================================================
//  g2048.ino - 2048 (single player)
//  Controls: D-pad to slide tiles, B quit. A restarts on game over.
// ============================================================================
#include "common.h"

#define G_X0   4
#define G_Y0   13
#define G_CW   30
#define G_CH   12

static int  g[4][4];
static long gScore;
static bool gOver;

static void gAddTile() {
  int empty[16][2], n = 0;
  for (int r = 0; r < 4; r++)
    for (int c = 0; c < 4; c++)
      if (g[r][c] == 0) { empty[n][0] = r; empty[n][1] = c; n++; }
  if (!n) return;
  int k = random(n);
  g[empty[k][0]][empty[k][1]] = (random(10) == 0) ? 4 : 2;
}

void g2048Init() {
  memset(g, 0, sizeof(g));
  gScore = 0;
  gOver = false;
  gAddTile();
  gAddTile();
}

// Slide+merge one line of 4 to the "front" (index 0). Returns true if changed.
static bool slideLine(int* v) {
  int tmp[4], n = 0;
  for (int i = 0; i < 4; i++) if (v[i]) tmp[n++] = v[i];
  int out[4] = { 0, 0, 0, 0 }, m = 0;
  for (int i = 0; i < n; i++) {
    if (i + 1 < n && tmp[i] == tmp[i + 1]) {
      out[m] = tmp[i] * 2; gScore += out[m]; m++; i++;
    } else out[m++] = tmp[i];
  }
  bool moved = false;
  for (int i = 0; i < 4; i++) { if (v[i] != out[i]) moved = true; v[i] = out[i]; }
  return moved;
}

// dir: 0 up, 1 down, 2 left, 3 right
static bool gMove(int dir) {
  bool moved = false;
  for (int i = 0; i < 4; i++) {
    int line[4];
    for (int j = 0; j < 4; j++) {
      switch (dir) {
        case 0: line[j] = g[j][i];     break;   // up:   column top->bottom
        case 1: line[j] = g[3 - j][i]; break;   // down
        case 2: line[j] = g[i][j];     break;   // left: row left->right
        case 3: line[j] = g[i][3 - j]; break;   // right
      }
    }
    if (slideLine(line)) moved = true;
    for (int j = 0; j < 4; j++) {
      switch (dir) {
        case 0: g[j][i]     = line[j]; break;
        case 1: g[3 - j][i] = line[j]; break;
        case 2: g[i][j]     = line[j]; break;
        case 3: g[i][3 - j] = line[j]; break;
      }
    }
  }
  return moved;
}

static bool gHasMoves() {
  for (int r = 0; r < 4; r++)
    for (int c = 0; c < 4; c++) {
      if (g[r][c] == 0) return true;
      if (c < 3 && g[r][c] == g[r][c + 1]) return true;
      if (r < 3 && g[r][c] == g[r + 1][c]) return true;
    }
  return false;
}

static void gRender() {
  display.clearDisplay();
  char buf[16];
  snprintf(buf, sizeof(buf), "2048  %ld", gScore);
  uiTitle(buf);
  for (int r = 0; r < 4; r++)
    for (int c = 0; c < 4; c++) {
      int x = G_X0 + c * G_CW, y = G_Y0 + r * G_CH;
      display.drawRect(x, y, G_CW, G_CH, SSD1306_WHITE);
      if (g[r][c]) {
        snprintf(buf, sizeof(buf), "%d", g[r][c]);
        int w = strlen(buf) * 6;
        display.setCursor(x + (G_CW - w) / 2, y + 3);
        display.print(buf);
      }
    }
  display.display();
}

void g2048Update() {
  if (gOver) {
    if (btnPressed[BTN_A]) { g2048Init(); return; }
    if (btnPressed[BTN_B]) { setState(ST_MENU); return; }
    display.clearDisplay();
    uiCenter("GAME OVER", 16);
    char buf[20]; snprintf(buf, sizeof(buf), "Score %ld", gScore);
    uiCenter(buf, 32);
    uiCenter("A:retry B:menu", 50);
    display.display();
    return;
  }
  if (btnPressed[BTN_B]) { setState(ST_MENU); return; }

  bool moved = false;
  if (btnPressed[BTN_UP])    moved = gMove(0);
  if (btnPressed[BTN_DOWN])  moved = gMove(1);
  if (btnPressed[BTN_LEFT])  moved = gMove(2);
  if (btnPressed[BTN_RIGHT]) moved = gMove(3);

  if (moved) {
    gAddTile();
    if (!gHasMoves()) gOver = true;
  }
  gRender();
}
