// ============================================================================
//  bship.ino - Battleship (2 players over ESP-NOW)
//
//  Each unit owns a private 8x8 board (the hidden-info payoff of two screens).
//  Peer-to-peer: shooter sends a SHOT, the defender computes hit/miss/sunk on
//  its own board and replies RESULT. Host only coordinates who fires first.
//
//  Placement: D-pad reshuffles a random valid layout, A = ready.
//  Battle: D-pad moves the radar cursor, A fires. B quits.
// ============================================================================
#include "common.h"

enum { BS_PLACE, BS_WAIT, BS_MYTURN, BS_THEIRTURN, BS_WAITRES, BS_OVER };
enum { BSK_READY = 1, BSK_BEGIN, BSK_SHOT, BSK_RESULT };

#define BS_N 8
#define BS_TOTAL 17                       // 5+4+3+3+2

static const int BS_LEN[5] = { 5, 4, 3, 3, 2 };

static uint8_t  bsShips[BS_N][BS_N];      // 0 empty, else ship index+1
static bool     bsHit[BS_N][BS_N];        // incoming shots on my board
static uint8_t  bsRadar[BS_N][BS_N];      // my shots: 0 none, 1 miss, 2 hit
static int      bsShipHits[5];
static int      bsMyHits;                 // total hits taken (I lose at 17)
static int      bsPhase;
static int      bsCurX, bsCurY;
static bool     bsHostReady, bsFoeReady, bsBegan;
static bool     bsWin;
static int      bsFoe;                    // slot to message (1 host / 0 client)
static int      bsFirst;                  // host: who fires first (for BEGIN resend)
static int      bsLastX, bsLastY, bsLastRes, bsLastAll;   // idempotent shot reply
static uint32_t bsFireTime, bsReadyTime;  // resend timers

static void bsSend(int kind, int a, int b, int c, int d) {
  NetMessage m; netFill(&m, NET_MMSG, GAME_BSHIP);
  m.p[0] = kind; m.p[1] = a; m.p[2] = b; m.p[3] = c; m.p[4] = d;
  mpTx(bsFoe, &m);
}

static bool bsFits(int k, int x, int y, bool horiz) {
  int L = BS_LEN[k];
  for (int i = 0; i < L; i++) {
    int cx = x + (horiz ? i : 0), cy = y + (horiz ? 0 : i);
    if (cx >= BS_N || cy >= BS_N) return false;
    if (bsShips[cy][cx]) return false;
  }
  return true;
}

static void bsShuffle() {
  memset(bsShips, 0, sizeof(bsShips));
  for (int k = 0; k < 5; k++) {
    int L = BS_LEN[k];
    for (;;) {
      bool horiz = random(2);
      int x = horiz ? random(BS_N - L + 1) : random(BS_N);
      int y = horiz ? random(BS_N) : random(BS_N - L + 1);
      if (!bsFits(k, x, y, horiz)) continue;
      for (int i = 0; i < L; i++)
        bsShips[y + (horiz ? 0 : i)][x + (horiz ? i : 0)] = k + 1;
      break;
    }
  }
}

void bshipInit() {
  memset(bsHit, 0, sizeof(bsHit));
  memset(bsRadar, 0, sizeof(bsRadar));
  memset(bsShipHits, 0, sizeof(bsShipHits));
  bsMyHits = 0;
  bsShuffle();
  bsPhase = BS_PLACE;
  bsCurX = bsCurY = 0;
  bsHostReady = bsFoeReady = bsBegan = false;
  bsWin = false;
  bsFoe = mpIsHost ? 1 : 0;
  bsFirst = 0;
  bsLastX = bsLastY = -1;
  bsFireTime = bsReadyTime = 0;
}

// Defender resolves an incoming shot on its own board. Idempotent: a resent
// SHOT for the same cell replays the stored reply instead of recomputing.
static void bsResolveShot(int x, int y) {
  if (x == bsLastX && y == bsLastY) {      // duplicate/resend
    bsSend(BSK_RESULT, x, y, bsLastRes, bsLastAll);
    return;
  }
  int result = 0;
  if (x < 0 || x >= BS_N || y < 0 || y >= BS_N) { bsSend(BSK_RESULT, x, y, 0, 0); return; }
  if (bsShips[y][x] && !bsHit[y][x]) {
    bsHit[y][x] = true;
    int k = bsShips[y][x] - 1;
    bsShipHits[k]++; bsMyHits++;
    result = (bsShipHits[k] == BS_LEN[k]) ? 2 : 1;
  } else {
    bsHit[y][x] = true;                   // record miss too
    result = 0;
  }
  int allSunk = (bsMyHits >= BS_TOTAL) ? 1 : 0;
  bsLastX = x; bsLastY = y; bsLastRes = result; bsLastAll = allSunk;
  bsSend(BSK_RESULT, x, y, result, allSunk);
  if (allSunk) { bsWin = false; bsPhase = BS_OVER; }
  else         bsPhase = BS_MYTURN;
}

static void bsPoll() {
  int from; NetMessage m;
  while (mpPoll(&from, &m)) {
    switch (m.p[0]) {
      case BSK_READY:
        bsFoeReady = true;
        if (bsBegan) bsSend(BSK_BEGIN, bsFirst, 0, 0, 0);   // client missed BEGIN
        break;
      case BSK_BEGIN:                     // client learns who fires first
        bsPhase = (m.p[1] == mpMySlot) ? BS_MYTURN : BS_THEIRTURN;
        break;
      case BSK_SHOT: bsResolveShot(m.p[1], m.p[2]); break;
      case BSK_RESULT: {
        int x = m.p[1], y = m.p[2];
        if (x >= 0 && x < BS_N && y >= 0 && y < BS_N)
          bsRadar[y][x] = (m.p[3] == 0) ? 1 : 2;
        if (m.p[4]) { bsWin = true; bsPhase = BS_OVER; }
        else        bsPhase = BS_THEIRTURN;
        break;
      }
    }
  }
}

static void bsDrawGrid(int ox, int oy, uint8_t which) {
  // which 0 = radar (my shots), 1 = fleet (my board)
  for (int y = 0; y < BS_N; y++)
    for (int x = 0; x < BS_N; x++) {
      int px = ox + x * 6, py = oy + y * 6;
      display.drawRect(px, py, 6, 6, SSD1306_WHITE);
      if (which == 0) {
        if (bsRadar[y][x] == 1) display.drawPixel(px + 2, py + 2, SSD1306_WHITE);
        if (bsRadar[y][x] == 2) display.fillRect(px + 1, py + 1, 4, 4, SSD1306_WHITE);
      } else {
        if (bsShips[y][x]) display.fillRect(px + 1, py + 1, 4, 4, SSD1306_WHITE);
        if (bsHit[y][x] && bsShips[y][x]) { display.drawLine(px, py, px + 5, py + 5, SSD1306_BLACK);
                                            display.drawLine(px + 5, py, px, py + 5, SSD1306_BLACK); }
        if (bsHit[y][x] && !bsShips[y][x]) display.drawPixel(px + 2, py + 2, SSD1306_WHITE);
      }
    }
}

static void bsRender() {
  display.clearDisplay();

  if (bsPhase == BS_PLACE) {
    display.setCursor(0, 0); display.print("Place fleet");
    bsDrawGrid(40, 10, 1);
    display.setCursor(0, 58); display.print("dpad:shuffle A:ready");
    display.display();
    return;
  }
  if (bsPhase == BS_WAIT) {
    uiCenter("Ready!", 20); uiCenter("Waiting...", 36);
    display.display();
    return;
  }
  if (bsPhase == BS_OVER) {
    uiCenter(bsWin ? "YOU WIN!" : "YOU LOSE", 20, 2);
    uiCenter("A/B: menu", 48);
    display.display();
    return;
  }

  // Battle view: radar (left) + fleet (right)
  const char* st = (bsPhase == BS_MYTURN) ? "YOUR TURN" :
                   (bsPhase == BS_WAITRES) ? "firing..." : "FOE TURN";
  display.setCursor(2, 0); display.print(st);
  bsDrawGrid(2, 12, 0);
  bsDrawGrid(70, 12, 1);
  if (bsPhase == BS_MYTURN) {              // cursor on radar
    int px = 2 + bsCurX * 6, py = 12 + bsCurY * 6;
    display.drawRect(px - 1, py - 1, 8, 8, SSD1306_WHITE);
  }
  display.display();
}

void bshipUpdate() {
  if (mpAborted) {
    if (btnPressed[BTN_A] || btnPressed[BTN_B]) { mpLeave(); setState(ST_MENU); return; }
    display.clearDisplay(); uiCenter("Opponent left", 24); uiCenter("A/B: menu", 40);
    display.display(); return;
  }

  bsPoll();

  // Host decides the first shooter once both are ready.
  if (mpIsHost && bsPhase == BS_WAIT && bsHostReady && bsFoeReady && !bsBegan) {
    bsFirst = random(2);                   // slot 0 (host) or 1 (client)
    bsSend(BSK_BEGIN, bsFirst, 0, 0, 0);
    bsPhase = (bsFirst == 0) ? BS_MYTURN : BS_THEIRTURN;
    bsBegan = true;
  }

  // Resends so a single lost packet can't deadlock the turn exchange.
  uint32_t now = millis();
  if (!mpIsHost && bsPhase == BS_WAIT && now - bsReadyTime > 800) {
    bsReadyTime = now; bsSend(BSK_READY, 0, 0, 0, 0);       // until BEGIN arrives
  }
  if (bsPhase == BS_WAITRES && now - bsFireTime > 1500) {
    bsFireTime = now; bsSend(BSK_SHOT, bsCurX, bsCurY, 0, 0);
  }

  if (bsPhase == BS_OVER) {
    if (btnPressed[BTN_A] || btnPressed[BTN_B]) { mpLeave(); setState(ST_MENU); return; }
    bsRender();
    return;
  }
  if (btnPressed[BTN_B]) { mpLeave(); setState(ST_MENU); return; }

  if (bsPhase == BS_PLACE) {
    if (btnPressed[BTN_UP] || btnPressed[BTN_DOWN] ||
        btnPressed[BTN_LEFT] || btnPressed[BTN_RIGHT]) bsShuffle();
    if (btnPressed[BTN_A]) {
      bsPhase = BS_WAIT;
      bsReadyTime = now;
      if (mpIsHost) bsHostReady = true;
      else          bsSend(BSK_READY, 0, 0, 0, 0);
    }
  } else if (bsPhase == BS_MYTURN) {
    if (btnPressed[BTN_LEFT])  bsCurX = (bsCurX + BS_N - 1) % BS_N;
    if (btnPressed[BTN_RIGHT]) bsCurX = (bsCurX + 1) % BS_N;
    if (btnPressed[BTN_UP])    bsCurY = (bsCurY + BS_N - 1) % BS_N;
    if (btnPressed[BTN_DOWN])  bsCurY = (bsCurY + 1) % BS_N;
    if (btnPressed[BTN_A] && bsRadar[bsCurY][bsCurX] == 0) {
      bsSend(BSK_SHOT, bsCurX, bsCurY, 0, 0);
      bsFireTime = now;
      bsPhase = BS_WAITRES;
    }
  }

  bsRender();
}
