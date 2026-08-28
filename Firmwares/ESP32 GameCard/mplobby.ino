// ============================================================================
//  mplobby.ino - generic multiplayer lobby (host / join) for turn-based games
//  Configured per game via mpMenuConfig(); drives ST_MP_MODE/HOST/BROWSE.
// ============================================================================
#include "common.h"

static const char* gTitle = "GAME";
static uint8_t   gGame    = 0;
static AppState  gPlay    = ST_MENU;
static int       gMin     = 2;
static bool      gAuto    = false;
static int       gModeIdx = 0, gBrowseIdx = 0;

void mpMenuConfig(const char* title, uint8_t game, AppState playState,
                  int minPlayers, bool autoStart) {
  gTitle = title; gGame = game; gPlay = playState;
  gMin = minPlayers; gAuto = autoStart;
  gModeIdx = 0; gBrowseIdx = 0;
}

// ---- Host / Join / Back ----------------------------------------------------
void mpModeUpdate() {
  static const char* items[] = { "Host Game", "Join Game", "Back" };
  if (btnHeldRepeat(BTN_UP, 350, 140))   gModeIdx = (gModeIdx + 2) % 3;
  if (btnHeldRepeat(BTN_DOWN, 350, 140)) gModeIdx = (gModeIdx + 1) % 3;
  if (btnPressed[BTN_B]) { setState(ST_MENU); return; }
  if (btnPressed[BTN_A]) {
    if (gModeIdx == 0)      { mpHostStart(gGame);   setState(ST_MP_HOST); }
    else if (gModeIdx == 1) { mpBrowseStart(gGame); setState(ST_MP_BROWSE); }
    else setState(ST_MENU);
    return;
  }
  display.clearDisplay();
  uiTitle(gTitle);
  for (int i = 0; i < 3; i++) {
    int y = 16 + i * 13;
    if (i == gModeIdx) { display.fillRect(0, y - 1, SCREEN_WIDTH, 11, SSD1306_WHITE);
                         display.setTextColor(SSD1306_BLACK); }
    else display.setTextColor(SSD1306_WHITE);
    display.setCursor(6, y); display.print(items[i]);
  }
  display.setTextColor(SSD1306_WHITE);
  display.display();
}

// ---- Host lobby ------------------------------------------------------------
void mpHostLobbyUpdate() {
  if (btnPressed[BTN_B]) { mpLeave(); setState(ST_MP_MODE); return; }

  bool canStart = mpNumClients >= (gMin - 1);
  if (canStart && (gAuto || btnPressed[BTN_A])) {
    mpHostBegin();
    setState(gPlay);
    return;
  }

  display.clearDisplay();
  uiTitle(gTitle);
  display.setCursor(0, 14);
  display.print(deviceName); display.print(" (you)");
  for (int i = 0; i < mpNumClients; i++) {
    display.setCursor(0, 24 + i * 9);
    display.print("+ "); display.print(mpClientNames[i]);
  }
  display.setCursor(0, 56);
  if (canStart) display.print("A:start  B:cancel");
  else          display.print("Waiting...  B:back");
  display.display();
}

// ---- Join / browse ---------------------------------------------------------
void mpBrowseLobbyUpdate() {
  if (mpJustStarted) { mpJustStarted = false; setState(gPlay); return; }
  if (mpAborted)     { mpLeave(); setState(ST_MP_MODE); return; }
  if (btnPressed[BTN_B]) { mpLeave(); setState(ST_MP_MODE); return; }

  display.clearDisplay();
  uiTitle(gTitle);

  if (mpMySlot > 0) {                        // accepted, waiting for host start
    uiCenter("Joined!", 26);
    uiCenter("Waiting for host", 40);
    display.display();
    return;
  }

  if (lobbyCount == 0) {
    uiCenter("Searching...", 30);
  } else {
    if (btnHeldRepeat(BTN_UP, 350, 160))   gBrowseIdx = (gBrowseIdx + lobbyCount - 1) % lobbyCount;
    if (btnHeldRepeat(BTN_DOWN, 350, 160)) gBrowseIdx = (gBrowseIdx + 1) % lobbyCount;
    if (gBrowseIdx >= lobbyCount) gBrowseIdx = lobbyCount - 1;
    if (btnPressed[BTN_A]) mpJoinLobby(lobbies[gBrowseIdx].mac);

    for (int i = 0; i < lobbyCount && i < 4; i++) {
      int y = 14 + i * 11;
      if (i == gBrowseIdx) { display.fillRect(0, y - 1, SCREEN_WIDTH, 11, SSD1306_WHITE);
                             display.setTextColor(SSD1306_BLACK); }
      else display.setTextColor(SSD1306_WHITE);
      display.setCursor(4, y); display.print(lobbies[i].name);
    }
    display.setTextColor(SSD1306_WHITE);
  }
  display.display();
}
