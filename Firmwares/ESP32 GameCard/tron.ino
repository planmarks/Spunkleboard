// ============================================================================
//  tron.ino - Tron light-cycles (2-4 players over ESP-NOW)
//
//  Host is authoritative: it simulates every cycle and broadcasts head
//  positions each tick; clients render and send their steering input.
//  Trails are rebuilt on each device by marking received head cells.
//
//  Controls: D-pad to steer (no reversing), B quit. Host presses A to start.
// ============================================================================
#include "common.h"

#define TCOLS 64
#define TROWS 32
#define TCELL 2
#define TRON_TICK 90        // ms per movement step

static const int TDX[4] = { 0, 0, -1, 1 };   // dir 0=up 1=down 2=left 3=right
static const int TDY[4] = { -1, 1, 0, 0 };
static const int TOPP[4] = { 1, 0, 3, 2 };
static const int TSX[4] = { 5, 58, 32, 32 }; // start cells per slot
static const int TSY[4] = { 16, 16, 4, 27 };
static const int TSD[4] = { 3, 2, 1, 0 };    // start directions

static uint8_t  tgrid[TROWS][TCOLS];
static int      thx[4], thy[4], tdir[4];
static bool     talive[4];
static int      tPlayers;
static bool     tGameOver;
static int      tWinner;                     // slot 0..3, 4 = draw, -1 playing
static uint32_t tTick, tInputSent;
static int      tReqDir;
static int      tMode, tBrowseSel;

static inline int16_t packHead(int x, int y) { return (int16_t)((x << 8) | (y & 0xFF)); }
static inline int upX(int16_t v) { return (v >> 8) & 0xFF; }
static inline int upY(int16_t v) { return v & 0xFF; }

// ===========================================================================
//  Tron sub-menu (Host / Join / Back)
// ===========================================================================
void tronModeUpdate() {
  static const char* items[] = { "Host Game", "Join Game", "Back" };
  if (btnHeldRepeat(BTN_UP, 350, 140))   tMode = (tMode + 2) % 3;
  if (btnHeldRepeat(BTN_DOWN, 350, 140)) tMode = (tMode + 1) % 3;
  if (btnPressed[BTN_B]) { setState(ST_MENU); return; }
  if (btnPressed[BTN_A]) {
    if (tMode == 0) setState(ST_TRON_HOST);
    else if (tMode == 1) setState(ST_TRON_BROWSE);
    else setState(ST_MENU);
    return;
  }
  display.clearDisplay();
  uiTitle("TRON");
  for (int i = 0; i < 3; i++) {
    int y = 16 + i * 13;
    if (i == tMode) { display.fillRect(0, y - 1, SCREEN_WIDTH, 11, SSD1306_WHITE);
                      display.setTextColor(SSD1306_BLACK); }
    else display.setTextColor(SSD1306_WHITE);
    display.setCursor(6, y); display.print(items[i]);
  }
  display.setTextColor(SSD1306_WHITE);
  display.display();
}

// ===========================================================================
//  Host lobby
// ===========================================================================
void tronHostUpdate() {
  if (btnPressed[BTN_B]) { mpLeave(); setState(ST_TRON_MODE); return; }

  if (btnPressed[BTN_A] && mpNumClients >= 1) {
    mpHostBegin();
    setState(ST_TRON_PLAY);
    return;
  }

  display.clearDisplay();
  uiTitle("HOST TRON");
  display.setCursor(0, 14);
  display.print(deviceName); display.print(" (you)");
  for (int i = 0; i < mpNumClients; i++) {
    display.setCursor(0, 24 + i * 9);
    display.print("+ "); display.print(mpClientNames[i]);
  }
  display.setCursor(0, 56);
  if (mpNumClients >= 1) display.print("A:start  B:cancel");
  else                   display.print("Waiting...  B:back");
  display.display();
}

// ===========================================================================
//  Join / browse lobby
// ===========================================================================
void tronBrowseUpdate() {
  if (mpJustStarted) { mpJustStarted = false; setState(ST_TRON_PLAY); return; }
  if (mpAborted)     { mpLeave(); setState(ST_TRON_MODE); return; }
  if (btnPressed[BTN_B]) { mpLeave(); setState(ST_TRON_MODE); return; }

  display.clearDisplay();
  uiTitle("JOIN TRON");

  if (mpMySlot > 0) {                        // already accepted, waiting
    uiCenter("Joined!", 26);
    uiCenter("Waiting for host", 40);
    display.display();
    return;
  }

  if (lobbyCount == 0) {
    uiCenter("Searching...", 30);
  } else {
    if (btnHeldRepeat(BTN_UP, 350, 160))   tBrowseSel = (tBrowseSel + lobbyCount - 1) % lobbyCount;
    if (btnHeldRepeat(BTN_DOWN, 350, 160)) tBrowseSel = (tBrowseSel + 1) % lobbyCount;
    if (tBrowseSel >= lobbyCount) tBrowseSel = lobbyCount - 1;
    if (btnPressed[BTN_A]) mpJoinLobby(lobbies[tBrowseSel].mac);

    for (int i = 0; i < lobbyCount && i < 4; i++) {
      int y = 14 + i * 11;
      if (i == tBrowseSel) { display.fillRect(0, y - 1, SCREEN_WIDTH, 11, SSD1306_WHITE);
                             display.setTextColor(SSD1306_BLACK); }
      else display.setTextColor(SSD1306_WHITE);
      display.setCursor(4, y); display.print(lobbies[i].name);
    }
    display.setTextColor(SSD1306_WHITE);
  }
  display.display();
}

// ===========================================================================
//  Gameplay
// ===========================================================================
void tronPlayInit() {
  memset(tgrid, 0, sizeof(tgrid));
  tPlayers = mpNumPlayers;
  if (tPlayers < 2) tPlayers = 2;
  if (tPlayers > 4) tPlayers = 4;
  for (int s = 0; s < 4; s++) {
    talive[s] = (s < tPlayers);
    thx[s] = TSX[s]; thy[s] = TSY[s]; tdir[s] = TSD[s];
    if (s < tPlayers) {
      tgrid[thy[s]][thx[s]] = 1;
      mpInput[s] = TSD[s];                   // default: keep going straight
    }
  }
  tGameOver = false; tWinner = -1;
  tReqDir = -1; tInputSent = 0;
  tTick = millis();
}

static void tHostStep() {
  // Apply desired directions (reject reversals).
  for (int s = 0; s < tPlayers; s++) {
    if (!talive[s]) continue;
    int want = mpInput[s];
    if (want >= 0 && want < 4 && want != TOPP[tdir[s]]) tdir[s] = want;
  }
  // Proposed new heads.
  int nx[4], ny[4]; bool die[4] = { false, false, false, false };
  for (int s = 0; s < tPlayers; s++) {
    if (!talive[s]) continue;
    nx[s] = thx[s] + TDX[tdir[s]];
    ny[s] = thy[s] + TDY[tdir[s]];
    if (nx[s] < 0 || nx[s] >= TCOLS || ny[s] < 0 || ny[s] >= TROWS) die[s] = true;
  }
  // Head-on collisions.
  for (int i = 0; i < tPlayers; i++)
    for (int j = i + 1; j < tPlayers; j++)
      if (talive[i] && talive[j] && !die[i] && !die[j] &&
          nx[i] == nx[j] && ny[i] == ny[j]) { die[i] = die[j] = true; }
  // Trail collisions.
  for (int s = 0; s < tPlayers; s++)
    if (talive[s] && !die[s] && tgrid[ny[s]][nx[s]]) die[s] = true;
  // Commit.
  for (int s = 0; s < tPlayers; s++) {
    if (!talive[s]) continue;
    if (die[s]) talive[s] = false;
    else { thx[s] = nx[s]; thy[s] = ny[s]; tgrid[ny[s]][nx[s]] = 1; }
  }
  // Winner?
  int cnt = 0, last = -1;
  for (int s = 0; s < tPlayers; s++) if (talive[s]) { cnt++; last = s; }
  if (cnt <= 1) { tGameOver = true; tWinner = (cnt == 1) ? last : 4; }
}

static void tHostBroadcast() {
  int16_t p[6];
  uint16_t mask = 0;
  for (int s = 0; s < 4; s++) {
    p[s] = (s < tPlayers) ? packHead(thx[s], thy[s]) : 0;
    if (s < tPlayers && talive[s]) mask |= (1 << s);
  }
  p[4] = mask;
  p[5] = tGameOver ? tWinner : -1;
  mpBroadcastState(p);
}

static void tClientApply() {
  uint16_t mask = mpS[4];
  for (int s = 0; s < tPlayers; s++) {
    int x = upX(mpS[s]), y = upY(mpS[s]);
    talive[s] = (mask >> s) & 1;
    thx[s] = x; thy[s] = y;
    if (x >= 0 && x < TCOLS && y >= 0 && y < TROWS) tgrid[y][x] = 1;
  }
  if (mpS[5] >= 0) { tGameOver = true; tWinner = mpS[5]; }
}

static void tRenderGrid() {
  display.clearDisplay();
  for (int y = 0; y < TROWS; y++)
    for (int x = 0; x < TCOLS; x++)
      if (tgrid[y][x]) display.fillRect(x * TCELL, y * TCELL, TCELL, TCELL, SSD1306_WHITE);
  int ms = mpMySlot;                         // ring around your own head
  if (ms < tPlayers && talive[ms])
    display.drawRect(thx[ms] * TCELL - 1, thy[ms] * TCELL - 1, TCELL + 2, TCELL + 2, SSD1306_WHITE);
}

static void tDrawOver() {
  display.fillRect(14, 18, 100, 30, SSD1306_BLACK);
  display.drawRect(14, 18, 100, 30, SSD1306_WHITE);
  const char* msg;
  if (tWinner == 4)            msg = "DRAW";
  else if (tWinner == mpMySlot) msg = "YOU WIN!";
  else                          msg = "YOU LOSE";
  uiCenter(msg, 24);
  uiCenter("A/B: menu", 36);
}

static void tQuit() { mpLeave(); setState(ST_MENU); }

void tronPlayUpdate() {
  uint32_t now = millis();

  // Client lost the host?
  if (!mpIsHost && (mpAborted || now - mpLastRecv > 3000)) {
    if (btnPressed[BTN_A] || btnPressed[BTN_B]) { tQuit(); return; }
    display.clearDisplay();
    uiCenter("Host left", 24);
    uiCenter("A/B: menu", 40);
    display.display();
    return;
  }

  if (tGameOver) {
    if (btnPressed[BTN_A] || btnPressed[BTN_B]) { tQuit(); return; }
  } else if (btnPressed[BTN_B]) { tQuit(); return; }

  if (mpIsHost) {
    // Local steering feeds the same input slot the clients use.
    if (btnPressed[BTN_UP])    mpInput[0] = 0;
    if (btnPressed[BTN_DOWN])  mpInput[0] = 1;
    if (btnPressed[BTN_LEFT])  mpInput[0] = 2;
    if (btnPressed[BTN_RIGHT]) mpInput[0] = 3;
    if (now - tTick >= TRON_TICK) {
      tTick = now;
      if (!tGameOver) tHostStep();
      tHostBroadcast();
    }
  } else {
    int req = -1;
    if (btnPressed[BTN_UP])    req = 0;
    if (btnPressed[BTN_DOWN])  req = 1;
    if (btnPressed[BTN_LEFT])  req = 2;
    if (btnPressed[BTN_RIGHT]) req = 3;
    if (req >= 0) { tReqDir = req; mpSendInput(req); tInputSent = now; }
    else if (tReqDir >= 0 && now - tInputSent >= 150) { mpSendInput(tReqDir); tInputSent = now; }
    if (mpStateNew) { mpStateNew = false; tClientApply(); }
  }

  tRenderGrid();
  if (tGameOver) tDrawOver();
  display.display();
}
