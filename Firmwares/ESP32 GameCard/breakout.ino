// ============================================================================
//  breakout.ino - Breakout (single player)
//  Controls: LEFT/RIGHT move paddle, B quit.
// ============================================================================
#include "common.h"

#define BK_COLS 8
#define BK_ROWS 3
#define BK_BW   15
#define BK_BH   4
#define BK_TOP  9
#define BK_ROWH 6
#define BK_PADW 22
#define BK_PADY 60
#define BK_BALL 2

static bool     brick[BK_ROWS][BK_COLS];
static int      padX;
static int      bkX, bkY, bkVX, bkVY;
static int      lives, bkScore, remaining;
static bool     bkOver, bkWon;
static uint32_t bkLast;

static void serve() {
  padX = (SCREEN_WIDTH - BK_PADW) / 2;
  bkX = SCREEN_WIDTH / 2;
  bkY = BK_PADY - 3;
  bkVX = (random(2) ? 1 : -1);
  bkVY = -2;
}

void breakoutInit() {
  for (int r = 0; r < BK_ROWS; r++)
    for (int c = 0; c < BK_COLS; c++) brick[r][c] = true;
  remaining = BK_ROWS * BK_COLS;
  lives = 3; bkScore = 0;
  bkOver = false; bkWon = false;
  serve();
  bkLast = millis();
}

static void bkRender() {
  display.clearDisplay();
  for (int r = 0; r < BK_ROWS; r++)
    for (int c = 0; c < BK_COLS; c++)
      if (brick[r][c])
        display.fillRect(c * 16, BK_TOP + r * BK_ROWH, BK_BW, BK_BH, SSD1306_WHITE);
  display.fillRect(padX, BK_PADY, BK_PADW, 2, SSD1306_WHITE);
  display.fillRect(bkX, bkY, BK_BALL, BK_BALL, SSD1306_WHITE);
  char buf[16]; snprintf(buf, sizeof(buf), "%d  L%d", bkScore, lives);
  display.setCursor(2, 0); display.print(buf);
  display.display();
}

void breakoutUpdate() {
  if (bkOver) {
    if (btnPressed[BTN_A]) { breakoutInit(); return; }
    if (btnPressed[BTN_B]) { setState(ST_MENU); return; }
    display.clearDisplay();
    uiCenter(bkWon ? "YOU WIN!" : "GAME OVER", 16);
    char buf[20]; snprintf(buf, sizeof(buf), "Score %d", bkScore);
    uiCenter(buf, 32);
    uiCenter("A:retry B:menu", 50);
    display.display();
    return;
  }
  if (btnPressed[BTN_B]) { setState(ST_MENU); return; }

  uint32_t now = millis();
  if (now - bkLast < 25) return;      // ~40 fps
  bkLast = now;

  if (btnDown[BTN_LEFT])  padX -= 4;
  if (btnDown[BTN_RIGHT]) padX += 4;
  if (padX < 0) padX = 0;
  if (padX > SCREEN_WIDTH - BK_PADW) padX = SCREEN_WIDTH - BK_PADW;

  bkX += bkVX; bkY += bkVY;
  if (bkX <= 0)                      { bkX = 0; bkVX = -bkVX; }
  if (bkX >= SCREEN_WIDTH - BK_BALL) { bkX = SCREEN_WIDTH - BK_BALL; bkVX = -bkVX; }
  if (bkY <= 0)                      { bkY = 0; bkVY = -bkVY; }

  // Paddle
  if (bkVY > 0 && bkY + BK_BALL >= BK_PADY && bkY < BK_PADY + 2 &&
      bkX + BK_BALL >= padX && bkX <= padX + BK_PADW) {
    bkY = BK_PADY - BK_BALL;
    bkVY = -bkVY;
    int rel = (bkX + BK_BALL / 2) - (padX + BK_PADW / 2);
    bkVX = rel / 6;
    if (bkVX == 0) bkVX = (random(2) ? 1 : -1);
    if (bkVX > 2) bkVX = 2; if (bkVX < -2) bkVX = -2;
  }

  // Bricks (one hit per frame)
  for (int r = 0; r < BK_ROWS && !bkWon; r++)
    for (int c = 0; c < BK_COLS; c++) {
      if (!brick[r][c]) continue;
      int brx = c * 16, bry = BK_TOP + r * BK_ROWH;
      if (bkX + BK_BALL > brx && bkX < brx + BK_BW &&
          bkY + BK_BALL > bry && bkY < bry + BK_BH) {
        brick[r][c] = false;
        remaining--; bkScore += 10;
        bkVY = -bkVY;
        if (remaining == 0) { bkOver = true; bkWon = true; }
        goto done;
      }
    }
  done:

  // Lost the ball
  if (bkY > SCREEN_HEIGHT) {
    lives--;
    if (lives <= 0) bkOver = true;
    else serve();
  }
  bkRender();
}
