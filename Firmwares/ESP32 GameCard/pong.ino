// ============================================================================
//  pong.ino - Pong
//    * pongSingle* : left paddle = player, right paddle = CPU
//    * pongMp*     : 2 players over ESP-NOW.
//                    HOST is authoritative (left paddle + ball physics).
//                    CLIENT sends its paddle position, renders host's state.
//
//  Integer math only (ESP32-C3 has no hardware FPU).
//  Controls: UP/DOWN move your paddle, B quit. Game over: A rematch / B menu.
// ============================================================================
#include "common.h"

#define PB_W       2
#define PB_H       14
#define PB_LX      3
#define PB_RX      (SCREEN_WIDTH - 3 - PB_W)   // 123
#define BALL       3
#define FRAME_MS   33                          // ~30 fps
#define PADDLE_SPD 3
#define WIN_SCORE  5

static int      lpY, rpY;          // paddle top-left Y
static int      bx, by, bvx, bvy;  // ball position / velocity
static int      scoreL, scoreR;
static uint32_t pLast;
static bool     pOver;
static bool     leftWon;

// multiplayer-specific
static bool     mpHost;
static int      myPaddle;          // client's local paddle Y
static bool     gotFirst;          // client received a first state?
static bool     mpDisconnected;

// ---------------------------------------------------------------------------
static void serveBall(int dir) {   // dir: -1 to left, +1 to right
  bx = SCREEN_WIDTH / 2 - BALL / 2;
  by = SCREEN_HEIGHT / 2 - BALL / 2;
  bvx = 2 * dir;
  bvy = (random(2) ? 1 : -1) * (1 + random(2));
}

static void resetMatch() {
  lpY = rpY = (SCREEN_HEIGHT - PB_H) / 2;
  myPaddle = lpY;
  scoreL = scoreR = 0;
  pOver = false;
  gotFirst = false;
  mpDisconnected = false;
  serveBall(random(2) ? 1 : -1);
  pLast = millis();
}

// Advance ball + handle wall / paddle collisions + scoring.
// Uses the current lpY / rpY as the two paddles.
static void ballPhysics() {
  bx += bvx;
  by += bvy;

  if (by <= 0)                 { by = 0; bvy = -bvy; }
  if (by >= SCREEN_HEIGHT - BALL) { by = SCREEN_HEIGHT - BALL; bvy = -bvy; }

  // Left paddle
  if (bvx < 0 && bx <= PB_LX + PB_W && bx >= PB_LX - 3 &&
      by + BALL >= lpY && by <= lpY + PB_H) {
    bx = PB_LX + PB_W;
    bvx = -bvx;
    int rel = (by + BALL / 2) - (lpY + PB_H / 2);
    bvy += rel / 4;
    if (bvy > 3) bvy = 3; if (bvy < -3) bvy = -3;
  }
  // Right paddle
  if (bvx > 0 && bx + BALL >= PB_RX && bx + BALL <= PB_RX + PB_W + 3 &&
      by + BALL >= rpY && by <= rpY + PB_H) {
    bx = PB_RX - BALL;
    bvx = -bvx;
    int rel = (by + BALL / 2) - (rpY + PB_H / 2);
    bvy += rel / 4;
    if (bvy > 3) bvy = 3; if (bvy < -3) bvy = -3;
  }

  if (bx < -BALL)              { scoreR++; serveBall(-1); }
  if (bx > SCREEN_WIDTH)       { scoreL++; serveBall(1); }

  if (scoreL >= WIN_SCORE) { pOver = true; leftWon = true; }
  if (scoreR >= WIN_SCORE) { pOver = true; leftWon = false; }
}

static int clampPaddle(int y) {
  if (y < 0) return 0;
  if (y > SCREEN_HEIGHT - PB_H) return SCREEN_HEIGHT - PB_H;
  return y;
}

// ---------------------------------------------------------------------------
//  Rendering (shared by all modes)
// ---------------------------------------------------------------------------
static void renderField(int leftY, int rightY, int ballX, int ballY,
                        int sL, int sR) {
  display.clearDisplay();
  // Center dashed line
  for (int y = 0; y < SCREEN_HEIGHT; y += 6)
    display.drawFastVLine(SCREEN_WIDTH / 2, y, 3, SSD1306_WHITE);
  // Scores
  char buf[8];
  snprintf(buf, sizeof(buf), "%d", sL);
  display.setTextSize(1); display.setCursor(SCREEN_WIDTH / 2 - 14, 2); display.print(buf);
  snprintf(buf, sizeof(buf), "%d", sR);
  display.setCursor(SCREEN_WIDTH / 2 + 9, 2); display.print(buf);
  // Paddles + ball
  display.fillRect(PB_LX, leftY,  PB_W, PB_H, SSD1306_WHITE);
  display.fillRect(PB_RX, rightY, PB_W, PB_H, SSD1306_WHITE);
  display.fillRect(ballX, ballY, BALL, BALL, SSD1306_WHITE);
}

static void renderOver(const char* who) {
  uiCenter("GAME OVER", 20, 1);
  uiCenter(who, 34, 1);
  uiCenter("A:rematch B:menu", 52, 1);
  display.display();
}

// ===========================================================================
//  Single player
// ===========================================================================
void pongSingleInit() { resetMatch(); }

void pongSingleUpdate() {
  if (pOver) {
    if (btnPressed[BTN_A]) { resetMatch(); return; }
    if (btnPressed[BTN_B]) { setState(ST_PONG_MODE); return; }
    display.clearDisplay();
    renderOver(leftWon ? "You win!" : "CPU wins");
    return;
  }
  if (btnPressed[BTN_B]) { setState(ST_PONG_MODE); return; }

  uint32_t now = millis();
  if (now - pLast < FRAME_MS) return;
  pLast = now;

  // Player paddle
  if (btnDown[BTN_UP])   lpY -= PADDLE_SPD;
  if (btnDown[BTN_DOWN]) lpY += PADDLE_SPD;
  lpY = clampPaddle(lpY);

  // CPU: track the ball with a capped speed (a little lazy, so it's beatable)
  int target = by + BALL / 2 - PB_H / 2;
  if (rpY < target) rpY += min(2, target - rpY);
  if (rpY > target) rpY -= min(2, rpY - target);
  rpY = clampPaddle(rpY);

  ballPhysics();
  renderField(lpY, rpY, bx, by, scoreL, scoreR);
  display.display();
}

// ===========================================================================
//  Multiplayer
// ===========================================================================
void pongMpInit(bool asHost) {
  mpHost = asHost;
  resetMatch();
}

static bool mpLinkDown() {
  if (netPeerLost) return true;
  if (millis() - netLastRecv > 2500) return true;
  return false;
}

static void mpQuit() {
  netReset();
  setState(ST_PONG_MODE);
}

void pongMpUpdate() {
  // Disconnect handling
  if (mpDisconnected) {
    if (btnPressed[BTN_A] || btnPressed[BTN_B]) { mpQuit(); return; }
    display.clearDisplay();
    uiCenter("Player left", 24, 1);
    uiCenter("A/B: back", 44, 1);
    display.display();
    return;
  }
  if (mpLinkDown()) { mpDisconnected = true; return; }

  if (pOver) {
    if (btnPressed[BTN_B]) { mpQuit(); return; }
    // (rematch would need a re-sync handshake; keep it simple: B to exit)
    display.clearDisplay();
    bool iWon = mpHost ? leftWon : !leftWon;
    renderOver(iWon ? "You win!" : "You lose");
    // keep pumping state so both sides agree the match ended
    if (mpHost) {
      NetMessage m; netFill(&m, NET_STATE, GAME_PONG);
      m.p[0]=bx; m.p[1]=by; m.p[2]=lpY; m.p[3]=rpY; m.p[4]=scoreL; m.p[5]=scoreR;
      netSendToPeer(&m);
    }
    return;
  }

  if (btnPressed[BTN_B]) { mpQuit(); return; }

  uint32_t now = millis();
  if (now - pLast < FRAME_MS) return;
  pLast = now;

  if (mpHost) {
    // Host: own paddle from buttons, opponent from last received input.
    if (btnDown[BTN_UP])   lpY -= PADDLE_SPD;
    if (btnDown[BTN_DOWN]) lpY += PADDLE_SPD;
    lpY = clampPaddle(lpY);
    rpY = clampPaddle(gClientPaddle);

    ballPhysics();

    NetMessage m; netFill(&m, NET_STATE, GAME_PONG);
    m.p[0]=bx; m.p[1]=by; m.p[2]=lpY; m.p[3]=rpY; m.p[4]=scoreL; m.p[5]=scoreR;
    netSendToPeer(&m);

    renderField(lpY, rpY, bx, by, scoreL, scoreR);
    display.display();

  } else {
    // Client: move local paddle, send it, render host's authoritative state.
    if (btnDown[BTN_UP])   myPaddle -= PADDLE_SPD;
    if (btnDown[BTN_DOWN]) myPaddle += PADDLE_SPD;
    myPaddle = clampPaddle(myPaddle);

    NetMessage m; netFill(&m, NET_INPUT, GAME_PONG);
    m.p[0] = myPaddle;
    netSendToPeer(&m);

    if (gStateNew) { gotFirst = true; gStateNew = false;
                     scoreL = gScoreL; scoreR = gScoreR;
                     if (scoreL >= WIN_SCORE) { pOver = true; leftWon = true; }
                     if (scoreR >= WIN_SCORE) { pOver = true; leftWon = false; } }

    if (!gotFirst) {
      display.clearDisplay();
      uiCenter("Connecting...", 28, 1);
      display.display();
      return;
    }
    renderField(gLeftP, gRightP, gBallX, gBallY, gScoreL, gScoreR);
    display.display();
  }
}
