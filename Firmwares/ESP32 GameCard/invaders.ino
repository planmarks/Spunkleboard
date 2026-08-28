// ============================================================================
//  invaders.ino - Space Invaders (single player)
//  Controls: LEFT/RIGHT move, A shoot, B quit.
// ============================================================================
#include "common.h"

#define IN_ROWS 3
#define IN_COLS 5
#define IN_AW   8
#define IN_AH   6
#define IN_DX   16
#define IN_DY   10
#define IN_SHIPY 58

static bool     al[IN_ROWS][IN_COLS];
static int      alX, alY, alDir, alCount;
static uint32_t alStep;
static int      shipX;
static int      pbX, pbY; static bool pbOn;
static int      ebX, ebY; static bool ebOn;
static int      inScore, inLives;
static bool     inOver, inWon;
static uint32_t inLast;

void invadersInit() {
  for (int r = 0; r < IN_ROWS; r++)
    for (int c = 0; c < IN_COLS; c++) al[r][c] = true;
  alCount = IN_ROWS * IN_COLS;
  alX = 8; alY = 12; alDir = 1;
  shipX = SCREEN_WIDTH / 2 - 5;
  pbOn = ebOn = false;
  inScore = 0; inLives = 3;
  inOver = inWon = false;
  alStep = inLast = millis();
}

static int alienX(int c) { return alX + c * IN_DX; }
static int alienY(int r) { return alY + r * IN_DY; }

static void stepFormation() {
  // Would moving hit an edge?
  int minX = 999, maxX = -999;
  for (int r = 0; r < IN_ROWS; r++)
    for (int c = 0; c < IN_COLS; c++)
      if (al[r][c]) {
        if (alienX(c) < minX) minX = alienX(c);
        if (alienX(c) + IN_AW > maxX) maxX = alienX(c) + IN_AW;
      }
  if (maxX + alDir * 4 > SCREEN_WIDTH - 1 || minX + alDir * 4 < 1) {
    alDir = -alDir;
    alY += IN_DY / 2;
  } else {
    alX += alDir * 4;
  }
  // Reached the ship?
  for (int r = 0; r < IN_ROWS; r++)
    for (int c = 0; c < IN_COLS; c++)
      if (al[r][c] && alienY(r) + IN_AH >= IN_SHIPY) inOver = true;
}

static void enemyFire() {
  if (ebOn) return;
  int cols[IN_COLS], n = 0;
  for (int c = 0; c < IN_COLS; c++)
    for (int r = 0; r < IN_ROWS; r++)
      if (al[r][c]) { cols[n++] = c; break; }
  if (!n) return;
  int c = cols[random(n)];
  int lr = -1;
  for (int r = 0; r < IN_ROWS; r++) if (al[r][c]) lr = r;
  ebX = alienX(c) + IN_AW / 2;
  ebY = alienY(lr) + IN_AH;
  ebOn = true;
}

static void inRender() {
  display.clearDisplay();
  for (int r = 0; r < IN_ROWS; r++)
    for (int c = 0; c < IN_COLS; c++)
      if (al[r][c]) display.fillRect(alienX(c), alienY(r), IN_AW, IN_AH, SSD1306_WHITE);
  display.fillRect(shipX, IN_SHIPY, 10, 4, SSD1306_WHITE);
  display.fillRect(shipX + 4, IN_SHIPY - 2, 2, 2, SSD1306_WHITE);   // turret
  if (pbOn) display.fillRect(pbX, pbY, 1, 3, SSD1306_WHITE);
  if (ebOn) display.fillRect(ebX, ebY, 1, 3, SSD1306_WHITE);
  char buf[16]; snprintf(buf, sizeof(buf), "%d L%d", inScore, inLives);
  display.setCursor(2, 0); display.print(buf);
  display.display();
}

void invadersUpdate() {
  if (inOver) {
    if (btnPressed[BTN_A]) { invadersInit(); return; }
    if (btnPressed[BTN_B]) { setState(ST_MENU); return; }
    display.clearDisplay();
    uiCenter(inWon ? "YOU WIN!" : "GAME OVER", 16);
    char buf[20]; snprintf(buf, sizeof(buf), "Score %d", inScore);
    uiCenter(buf, 32);
    uiCenter("A:retry B:menu", 50);
    display.display();
    return;
  }
  if (btnPressed[BTN_B]) { setState(ST_MENU); return; }

  // Fire on the button edge every loop, BEFORE the frame gate, or the
  // one-frame press is cleared by pollButtons before we ever see it.
  if (btnPressed[BTN_A] && !pbOn) { pbOn = true; pbX = shipX + 5; pbY = IN_SHIPY - 3; }

  uint32_t now = millis();
  if (now - inLast < 33) return;     // ~30 fps
  inLast = now;

  if (btnDown[BTN_LEFT])  shipX -= 3;
  if (btnDown[BTN_RIGHT]) shipX += 3;
  if (shipX < 0) shipX = 0;
  if (shipX > SCREEN_WIDTH - 10) shipX = SCREEN_WIDTH - 10;

  // Player bullet
  if (pbOn) {
    pbY -= 4;
    if (pbY < 0) pbOn = false;
    for (int r = 0; r < IN_ROWS && pbOn; r++)
      for (int c = 0; c < IN_COLS; c++) {
        if (!al[r][c]) continue;
        if (pbX >= alienX(c) && pbX <= alienX(c) + IN_AW &&
            pbY >= alienY(r) && pbY <= alienY(r) + IN_AH) {
          al[r][c] = false; alCount--; inScore += 10; pbOn = false;
          if (alCount == 0) { inOver = true; inWon = true; }
          break;
        }
      }
  }

  // Enemy bullet
  if (ebOn) {
    ebY += 3;
    if (ebY > SCREEN_HEIGHT) ebOn = false;
    else if (ebX >= shipX && ebX <= shipX + 10 && ebY >= IN_SHIPY) {
      ebOn = false;
      if (--inLives <= 0) inOver = true;
    }
  }
  if (random(100) < 3) enemyFire();

  // Formation movement (speeds up as aliens die)
  uint16_t iv = 120 + alCount * 25;
  if (now - alStep >= iv) { alStep = now; stepFormation(); }

  inRender();
}
