// ============================================================================
//  Gamecard - a pocket game console for the XIAO ESP32-C3
//
//  Hardware:
//    - Seeed XIAO ESP32-C3
//    - 0.96" SSD1306 OLED (I2C)  SDA=D4(GPIO6)  SCL=D5(GPIO7)
//    - 6 tactile buttons to GND (internal pull-ups)
//    - 3V LiPo
//
//  Files in this sketch:
//    gamecard.ino  - globals, setup/loop, main menu, UI helpers
//    input.ino     - button polling / debounce
//    settings.ino  - device name (persisted in NVS)
//    netplay.ino   - ESP-NOW lobby + transport
//    snake.ino     - Snake (single player)
//    pong.ino      - Pong (single player + 2-player over ESP-NOW)
// ============================================================================

#include "common.h"

// ---- Global object / state definitions (declared extern in common.h) -------
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

// Button GPIOs (XIAO ESP32-C3), order: UP, DOWN, LEFT, RIGHT, A(OK), B(BACK).
//   UP=D0(2)  DOWN=D1(3)  LEFT=D8(8)  RIGHT=D9(9)  OK=D2(4)  BACK=D3(5)
// NOTE: D0/D8/D9 (GPIO2/8/9) are ESP32-C3 strapping pins. GPIO9 is the BOOT
// pin, so don't hold RIGHT (and ideally not UP/LEFT) while powering on/resetting
// or the chip may drop into download mode instead of running the sketch.
const uint8_t BTN_PINS[NUM_BTNS] = { 2, 3, 8, 9, 4, 5 };
bool btnDown[NUM_BTNS]    = { false };
bool btnPressed[NUM_BTNS] = { false };

AppState appState = ST_MENU;
char deviceName[NAME_MAX] = "GC";

// ---- Generic single-player game runner -------------------------------------
// Single-player games register an update fn here and run under ST_PLAY.
static void (*gGameUpdate)() = nullptr;
void startGame(void (*init)(), void (*update)()) {
  gGameUpdate = update;
  if (init) init();
  appState = ST_PLAY;
}

// ---- Pong sub-menu ---------------------------------------------------------
static const char* PONG_ITEMS[] = { "1 Player", "Host Game", "Join Game", "Back" };
static const int    PONG_COUNT   = 4;
static int pongIndex = 0;

// ===========================================================================
//  UI helpers
// ===========================================================================
void uiTitle(const char* t) {
  display.fillRect(0, 0, SCREEN_WIDTH, 11, SSD1306_WHITE);
  display.setTextColor(SSD1306_BLACK);
  display.setTextSize(1);
  display.setCursor(2, 2);
  display.print(t);
  display.setTextColor(SSD1306_WHITE);
}

void uiCenter(const char* t, int y, uint8_t size) {
  display.setTextSize(size);
  int w = strlen(t) * 6 * size;
  display.setCursor((SCREEN_WIDTH - w) / 2, y);
  display.print(t);
  display.setTextSize(1);
}

// ===========================================================================
//  Menu rendering / logic
// ===========================================================================
static void drawList(const char* title, const char* const* items,
                     int count, int sel) {
  display.clearDisplay();
  uiTitle(title);
  int top = 14;
  for (int i = 0; i < count; i++) {
    int y = top + i * 12;
    if (i == sel) {
      display.fillRect(0, y - 1, SCREEN_WIDTH, 11, SSD1306_WHITE);
      display.setTextColor(SSD1306_BLACK);
    } else {
      display.setTextColor(SSD1306_WHITE);
    }
    display.setCursor(6, y);
    display.print(items[i]);
  }
  display.setTextColor(SSD1306_WHITE);
  display.display();
}

// ---- Game registry ---------------------------------------------------------
// Each entry knows how to launch itself. Single-player games call startGame();
// games with their own sub-menu / lobby (Pong) jump to a dedicated state.
static void L_snake()    { startGame(snakeInit,    snakeUpdate);    }
static void L_tetris()   { startGame(tetrisInit,   tetrisUpdate);   }
static void L_2048()     { startGame(g2048Init,    g2048Update);    }
static void L_runner()   { startGame(runnerInit,   runnerUpdate);   }
static void L_breakout() { startGame(breakoutInit, breakoutUpdate); }
static void L_invaders() { startGame(invadersInit, invadersUpdate); }
static void L_mines()    { startGame(minesInit,    minesUpdate);    }
static void L_simon()    { startGame(simonInit,    simonUpdate);    }
static void L_pong()     { setState(ST_PONG_MODE); }
static void L_tron()     { setState(ST_TRON_MODE); }
static void L_bship()    { mpMenuConfig("BATTLESHIP", GAME_BSHIP, ST_BSHIP_PLAY, 2, true);  setState(ST_MP_MODE); }
static void L_uno()      { mpMenuConfig("UNO",        GAME_UNO,   ST_UNO_PLAY,   2, false); setState(ST_MP_MODE); }
static void L_settings() { setState(ST_SETTINGS);  }

struct GameDef { const char* name; void (*launch)(); };
static const GameDef GAMES[] = {
  { "Snake",       L_snake    },
  { "Tetris",      L_tetris   },
  { "2048",        L_2048     },
  { "Runner",      L_runner   },
  { "Breakout",    L_breakout },
  { "Invaders",    L_invaders },
  { "Minesweeper", L_mines    },
  { "Simon",       L_simon    },
  { "Pong",        L_pong     },
  { "Tron",        L_tron     },
  { "Battleship",  L_bship    },
  { "Uno",         L_uno      },
  { "Settings",    L_settings },
};
static const int GAME_COUNT = sizeof(GAMES) / sizeof(GAMES[0]);

#define MENU_VISIBLE 4
static int menuSel = 0;
static int menuTop = 0;

static void menuUpdate() {
  if (btnHeldRepeat(BTN_UP, 350, 140))
    menuSel = (menuSel + GAME_COUNT - 1) % GAME_COUNT;
  if (btnHeldRepeat(BTN_DOWN, 350, 140))
    menuSel = (menuSel + 1) % GAME_COUNT;

  // Keep the selection inside the visible window.
  if (menuSel < menuTop)                    menuTop = menuSel;
  if (menuSel >= menuTop + MENU_VISIBLE)     menuTop = menuSel - MENU_VISIBLE + 1;

  if (btnPressed[BTN_A]) { GAMES[menuSel].launch(); return; }

  display.clearDisplay();
  uiTitle("GAMECARD");
  for (int i = 0; i < MENU_VISIBLE; i++) {
    int idx = menuTop + i;
    if (idx >= GAME_COUNT) break;
    int y = 14 + i * 12;
    if (idx == menuSel) {
      display.fillRect(0, y - 1, SCREEN_WIDTH, 11, SSD1306_WHITE);
      display.setTextColor(SSD1306_BLACK);
    } else display.setTextColor(SSD1306_WHITE);
    display.setCursor(6, y);
    display.print(GAMES[idx].name);
  }
  display.setTextColor(SSD1306_WHITE);
  // Scroll indicators
  if (menuTop > 0)                            display.fillTriangle(123, 15, 119, 20, 127, 20, SSD1306_WHITE);
  if (menuTop + MENU_VISIBLE < GAME_COUNT)    display.fillTriangle(123, 62, 119, 57, 127, 57, SSD1306_WHITE);
  display.display();
}

static void pongModeUpdate() {
  if (btnHeldRepeat(BTN_UP, 350, 140))
    pongIndex = (pongIndex + PONG_COUNT - 1) % PONG_COUNT;
  if (btnHeldRepeat(BTN_DOWN, 350, 140))
    pongIndex = (pongIndex + 1) % PONG_COUNT;

  if (btnPressed[BTN_B]) { setState(ST_MENU); return; }

  if (btnPressed[BTN_A]) {
    switch (pongIndex) {
      case 0: setState(ST_PONG_SINGLE); return;
      case 1: setState(ST_LOBBY_HOST);  return;
      case 2: setState(ST_LOBBY_BROWSE);return;
      case 3: setState(ST_MENU);        return;
    }
  }
  drawList("PONG", PONG_ITEMS, PONG_COUNT, pongIndex);
}

// ===========================================================================
//  Lobby screens (networking logic lives in netplay.ino)
// ===========================================================================
static void lobbyHostUpdate() {
  // Connection completed inside the ESP-NOW callback?
  if (netJustConnected) {
    netJustConnected = false;
    setState(ST_PONG_MP);
    return;
  }
  if (btnPressed[BTN_B]) { netReset(); setState(ST_PONG_MODE); return; }

  display.clearDisplay();
  uiTitle("HOST LOBBY");
  display.setCursor(0, 18);
  display.print(" Name: ");
  display.print(deviceName);
  uiCenter("Waiting for", 34);
  uiCenter("a player...", 44);
  display.setCursor(0, 56);
  display.print(" B: cancel");
  display.display();
}

static int browseSel = 0;
static void lobbyBrowseUpdate() {
  if (netJustConnected) {              // host accepted us
    netJustConnected = false;
    setState(ST_PONG_MP);
    return;
  }
  if (btnPressed[BTN_B]) { netReset(); setState(ST_PONG_MODE); return; }

  if (lobbyCount > 0) {
    if (btnHeldRepeat(BTN_UP, 350, 160))
      browseSel = (browseSel + lobbyCount - 1) % lobbyCount;
    if (btnHeldRepeat(BTN_DOWN, 350, 160))
      browseSel = (browseSel + 1) % lobbyCount;
    if (browseSel >= lobbyCount) browseSel = lobbyCount - 1;

    if (btnPressed[BTN_A]) {          // send a join request
      NetMessage m; netFill(&m, NET_JOIN, GAME_PONG);
      memcpy(peerMac, lobbies[browseSel].mac, 6);
      esp_now_peer_info_t peer = {};
      memcpy(peer.peer_addr, peerMac, 6);
      peer.channel = 0; peer.encrypt = false;
      if (!esp_now_is_peer_exist(peerMac)) esp_now_add_peer(&peer);
      netSendToPeer(&m);
    }
  }

  display.clearDisplay();
  uiTitle("JOIN LOBBY");
  if (lobbyCount == 0) {
    uiCenter("Searching...", 30);
  } else {
    int top = 14;
    for (int i = 0; i < lobbyCount && i < 4; i++) {
      int y = top + i * 11;
      if (i == browseSel) {
        display.fillRect(0, y - 1, SCREEN_WIDTH, 11, SSD1306_WHITE);
        display.setTextColor(SSD1306_BLACK);
      } else display.setTextColor(SSD1306_WHITE);
      display.setCursor(4, y);
      display.print(lobbies[i].name);
    }
    display.setTextColor(SSD1306_WHITE);
  }
  display.display();
}

// ===========================================================================
//  State transitions
// ===========================================================================
void setState(AppState s) {
  appState = s;
  switch (s) {
    case ST_MENU:                                          break;
    case ST_PLAY:                                          break;  // via startGame()
    case ST_PONG_MODE:   pongIndex = 0;                    break;
    case ST_SETTINGS:    settingsInit();                   break;
    case ST_PONG_SINGLE: pongSingleInit();                 break;
    case ST_LOBBY_HOST:  netReset(); netStartHost(GAME_PONG);   break;
    case ST_LOBBY_BROWSE:netReset(); browseSel = 0;
                         netStartBrowse(GAME_PONG);         break;
    case ST_PONG_MP:     pongMpInit(isHost);               break;
    case ST_TRON_MODE:                                     break;
    case ST_TRON_HOST:   mpHostStart(GAME_TRON);           break;
    case ST_TRON_BROWSE: mpBrowseStart(GAME_TRON);         break;
    case ST_TRON_PLAY:   tronPlayInit();                   break;
    case ST_MP_MODE:                                       break;
    case ST_MP_HOST:                                       break;  // started in mpModeUpdate
    case ST_MP_BROWSE:                                     break;  // started in mpModeUpdate
    case ST_BSHIP_PLAY:  bshipInit();                      break;
    case ST_UNO_PLAY:    unoInit();                        break;
  }
}

// ===========================================================================
//  Arduino entry points
// ===========================================================================
void setup() {
  Serial.begin(115200);

  // Buttons
  for (int i = 0; i < NUM_BTNS; i++) pinMode(BTN_PINS[i], INPUT_PULLUP);

  // Display (SDA=GPIO6, SCL=GPIO7 on XIAO ESP32-C3)
  Wire.begin(6, 7);
  if (!display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR)) {
    Serial.println("SSD1306 not found");
    for (;;) delay(1000);
  }
  display.setRotation(2);   // OLED is mounted upside-down on the PCB
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  uiCenter("GAMECARD", 24, 2);
  display.display();
  delay(800);

  netInit();        // starts WiFi/ESP-NOW; needed before loadSettings (MAC)
  loadSettings();   // device name from NVS (or default from MAC)

  setState(ST_MENU);
}

void loop() {
  pollButtons();
  netTick();

  switch (appState) {
    case ST_MENU:         menuUpdate();        break;
    case ST_SETTINGS:     settingsUpdate();    break;
    case ST_PLAY:         if (gGameUpdate) gGameUpdate(); break;
    case ST_PONG_MODE:    pongModeUpdate();    break;
    case ST_PONG_SINGLE:  pongSingleUpdate();  break;
    case ST_LOBBY_HOST:   lobbyHostUpdate();   break;
    case ST_LOBBY_BROWSE: lobbyBrowseUpdate(); break;
    case ST_PONG_MP:      pongMpUpdate();      break;
    case ST_TRON_MODE:    tronModeUpdate();    break;
    case ST_TRON_HOST:    tronHostUpdate();    break;
    case ST_TRON_BROWSE:  tronBrowseUpdate();  break;
    case ST_TRON_PLAY:    tronPlayUpdate();    break;
    case ST_MP_MODE:      mpModeUpdate();      break;
    case ST_MP_HOST:      mpHostLobbyUpdate(); break;
    case ST_MP_BROWSE:    mpBrowseLobbyUpdate(); break;
    case ST_BSHIP_PLAY:   bshipUpdate();       break;
    case ST_UNO_PLAY:     unoUpdate();         break;
  }
  delay(2);
}
