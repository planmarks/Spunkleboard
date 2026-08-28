// ============================================================================
//  runner.ino - endless jumper (single player, one button)
//  Controls: A or UP to jump, B quit.
// ============================================================================
#include "common.h"

#define GROUND_Y  54
#define PLYR      9
#define PLYR_X    12
#define MAX_OBS   4

static int      rY, rVel;
static int      obX[MAX_OBS], obW[MAX_OBS], obH[MAX_OBS];
static bool     obOn[MAX_OBS];
static int      rSpeed;
static long     rScore;
static bool     rOver;
static uint32_t rLast, rNextSpawn;

static int groundTop() { return GROUND_Y - PLYR; }

void runnerInit() {
  rY = groundTop(); rVel = 0;
  for (int i = 0; i < MAX_OBS; i++) obOn[i] = false;
  rSpeed = 3;
  rScore = 0;
  rOver = false;
  rLast = millis();
  rNextSpawn = rLast + 600;
}

static void spawnObstacle() {
  for (int i = 0; i < MAX_OBS; i++) {
    if (!obOn[i]) {
      obOn[i] = true;
      obX[i] = SCREEN_WIDTH + 2;
      obW[i] = 4 + random(4);        // 4..7 wide
      obH[i] = 8 + random(9);        // 8..16 tall
      return;
    }
  }
}

static void rRender() {
  display.clearDisplay();
  display.drawFastHLine(0, GROUND_Y, SCREEN_WIDTH, SSD1306_WHITE);
  display.fillRect(PLYR_X, rY, PLYR, PLYR, SSD1306_WHITE);
  for (int i = 0; i < MAX_OBS; i++)
    if (obOn[i])
      display.fillRect(obX[i], GROUND_Y - obH[i], obW[i], obH[i], SSD1306_WHITE);
  char buf[12]; snprintf(buf, sizeof(buf), "%ld", rScore / 10);
  display.setCursor(2, 2); display.print(buf);
  display.display();
}

void runnerUpdate() {
  if (rOver) {
    if (btnPressed[BTN_A]) { runnerInit(); return; }
    if (btnPressed[BTN_B]) { setState(ST_MENU); return; }
    display.clearDisplay();
    uiCenter("GAME OVER", 16);
    char buf[20]; snprintf(buf, sizeof(buf), "Score %ld", rScore / 10);
    uiCenter(buf, 32);
    uiCenter("A:retry B:menu", 50);
    display.display();
    return;
  }
  if (btnPressed[BTN_B]) { setState(ST_MENU); return; }

  // Jump only from the ground.
  if ((btnPressed[BTN_A] || btnPressed[BTN_UP]) && rY >= groundTop())
    rVel = -7;

  uint32_t now = millis();
  if (now - rLast < 33) return;      // ~30 fps
  rLast = now;

  // Physics
  rVel += 1;                         // gravity
  rY += rVel;
  if (rY >= groundTop()) { rY = groundTop(); rVel = 0; }

  // Obstacles
  for (int i = 0; i < MAX_OBS; i++) {
    if (!obOn[i]) continue;
    obX[i] -= rSpeed;
    if (obX[i] + obW[i] < 0) obOn[i] = false;
    // AABB collision
    if (PLYR_X < obX[i] + obW[i] && PLYR_X + PLYR > obX[i] &&
        rY + PLYR > GROUND_Y - obH[i] && rY < GROUND_Y)
      rOver = true;
  }

  if (now >= rNextSpawn) {
    spawnObstacle();
    rNextSpawn = now + 500 + random(900);
  }

  rScore++;
  if ((rScore % 400) == 0 && rSpeed < 7) rSpeed++;   // speed up over time
  rRender();
}
