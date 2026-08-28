// ============================================================================
//  snake.ino - classic Snake, single player
//
//  Controls: D-pad to steer, B to quit. On game over: A restart, B menu.
// ============================================================================
#include "common.h"

#define S_CELL 4
#define S_COLS (SCREEN_WIDTH / S_CELL)    // 32
#define S_ROWS (SCREEN_HEIGHT / S_CELL)   // 16
#define S_MAX  (S_COLS * S_ROWS)          // 512

enum { S_UP, S_DOWN, S_LEFT, S_RIGHT };

static uint8_t  bodyX[S_MAX], bodyY[S_MAX];
static int      len;
static int      dir, nextDir;
static uint8_t  foodX, foodY;
static int      score;
static bool     dead;
static uint32_t lastStep;
static uint16_t stepMs;

static void placeFood() {
  bool onSnake;
  do {
    onSnake = false;
    foodX = random(S_COLS);
    foodY = random(S_ROWS);
    for (int i = 0; i < len; i++)
      if (bodyX[i] == foodX && bodyY[i] == foodY) { onSnake = true; break; }
  } while (onSnake);
}

void snakeInit() {
  len = 3;
  int cx = S_COLS / 2, cy = S_ROWS / 2;
  for (int i = 0; i < len; i++) { bodyX[i] = cx - i; bodyY[i] = cy; }
  dir = nextDir = S_RIGHT;
  score = 0;
  dead = false;
  stepMs = 150;
  lastStep = millis();
  placeFood();
}

static void readDir() {
  // Prevent 180-degree reversals.
  if (btnPressed[BTN_UP]    && dir != S_DOWN)  nextDir = S_UP;
  if (btnPressed[BTN_DOWN]  && dir != S_UP)    nextDir = S_DOWN;
  if (btnPressed[BTN_LEFT]  && dir != S_RIGHT) nextDir = S_LEFT;
  if (btnPressed[BTN_RIGHT] && dir != S_LEFT)  nextDir = S_RIGHT;
}

static void step() {
  dir = nextDir;
  int hx = bodyX[0], hy = bodyY[0];
  switch (dir) {
    case S_UP:    hy--; break;
    case S_DOWN:  hy++; break;
    case S_LEFT:  hx--; break;
    case S_RIGHT: hx++; break;
  }

  // Walls
  if (hx < 0 || hx >= S_COLS || hy < 0 || hy >= S_ROWS) { dead = true; return; }
  // Self (skip tail cell, which will move away unless we grow)
  for (int i = 0; i < len - 1; i++)
    if (bodyX[i] == hx && bodyY[i] == hy) { dead = true; return; }

  bool grow = (hx == foodX && hy == foodY);
  int newLen = grow ? len + 1 : len;
  if (newLen > S_MAX) newLen = S_MAX;

  for (int i = newLen - 1; i > 0; i--) { bodyX[i] = bodyX[i - 1]; bodyY[i] = bodyY[i - 1]; }
  bodyX[0] = hx; bodyY[0] = hy;
  len = newLen;

  if (grow) {
    score++;
    if (stepMs > 60) stepMs -= 4;    // speed up a little each apple
    placeFood();
  }
}

static void render() {
  display.clearDisplay();
  // Food
  display.fillRect(foodX * S_CELL, foodY * S_CELL, S_CELL, S_CELL, SSD1306_WHITE);
  // Snake (hollow head so it reads clearly)
  for (int i = 0; i < len; i++) {
    int x = bodyX[i] * S_CELL, y = bodyY[i] * S_CELL;
    if (i == 0) display.drawRect(x, y, S_CELL, S_CELL, SSD1306_WHITE);
    else        display.fillRect(x, y, S_CELL, S_CELL, SSD1306_WHITE);
  }
  display.display();
}

static void renderGameOver() {
  display.clearDisplay();
  uiCenter("GAME OVER", 12, 1);
  char buf[20];
  snprintf(buf, sizeof(buf), "Score: %d", score);
  uiCenter(buf, 28, 1);
  uiCenter("A:retry B:menu", 50, 1);
  display.display();
}

void snakeUpdate() {
  if (dead) {
    if (btnPressed[BTN_A]) { snakeInit(); return; }
    if (btnPressed[BTN_B]) { setState(ST_MENU); return; }
    renderGameOver();
    return;
  }

  if (btnPressed[BTN_B]) { setState(ST_MENU); return; }

  readDir();
  uint32_t now = millis();
  if (now - lastStep >= stepMs) {
    lastStep = now;
    step();
  }
  render();
}
