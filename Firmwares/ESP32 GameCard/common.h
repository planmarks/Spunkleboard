// ============================================================================
//  Gamecard - shared declarations
//  XIAO ESP32-C3 pocket game console
// ============================================================================
#pragma once

#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <WiFi.h>
#include <esp_now.h>
#include <Preferences.h>

// ---------------------------------------------------------------------------
//  Display  (SSD1306 128x64 over I2C)
// ---------------------------------------------------------------------------
#define SCREEN_WIDTH   128
#define SCREEN_HEIGHT  64
#define OLED_ADDR      0x3C     // most 0.96" modules; some are 0x3D
#define OLED_RESET     -1
extern Adafruit_SSD1306 display;

// ---------------------------------------------------------------------------
//  Buttons
//  Wiring: each button connects its GPIO to GND. INPUT_PULLUP -> pressed = LOW.
//  GPIO map is for the Seeed XIAO ESP32-C3 (silkscreen Dx label in comments).
// ---------------------------------------------------------------------------
#define BTN_UP     0
#define BTN_DOWN   1
#define BTN_LEFT   2
#define BTN_RIGHT  3
#define BTN_A      4
#define BTN_B      5
#define NUM_BTNS   6

extern const uint8_t BTN_PINS[NUM_BTNS];
extern bool btnDown[NUM_BTNS];      // held this frame
extern bool btnPressed[NUM_BTNS];   // rising edge (just pressed this frame)

void pollButtons();
bool btnHeldRepeat(uint8_t b, uint16_t firstMs, uint16_t repeatMs);

// ---------------------------------------------------------------------------
//  Application state machine
// ---------------------------------------------------------------------------
enum AppState {
  ST_MENU,          // main game list
  ST_SETTINGS,      // device settings (rename)
  ST_PLAY,          // generic single-player game (see gGameUpdate)
  ST_PONG_MODE,     // pong sub-menu (1P / host / join)
  ST_PONG_SINGLE,   // pong vs CPU
  ST_LOBBY_HOST,    // advertising a lobby, waiting for a player
  ST_LOBBY_BROWSE,  // scanning for lobbies to join
  ST_PONG_MP,       // pong 2-player over ESP-NOW
  ST_TRON_MODE,     // tron sub-menu (host / join)
  ST_TRON_HOST,     // tron host lobby (up to 4 players)
  ST_TRON_BROWSE,   // tron: scanning for lobbies
  ST_TRON_PLAY,     // tron match
  ST_MP_MODE,       // generic multiplayer sub-menu (host / join)
  ST_MP_HOST,       // generic multiplayer host lobby
  ST_MP_BROWSE,     // generic multiplayer join/browse
  ST_BSHIP_PLAY,    // battleship match
  ST_UNO_PLAY       // uno match
};
extern AppState appState;
void setState(AppState s);

// ---------------------------------------------------------------------------
//  Device settings (persisted in NVS)
// ---------------------------------------------------------------------------
#define NAME_MAX 11                 // max chars incl. null (10 editable chars)
extern char deviceName[NAME_MAX];
void loadSettings();
void saveDeviceName(const char* name);
void settingsInit();
void settingsUpdate();

// ---------------------------------------------------------------------------
//  Games
//  Single-player games follow the init()/update() pair convention and run
//  under ST_PLAY. They return to the menu by calling setState(ST_MENU).
// ---------------------------------------------------------------------------
void snakeInit();     void snakeUpdate();
void tetrisInit();    void tetrisUpdate();
void g2048Init();     void g2048Update();
void runnerInit();    void runnerUpdate();
void breakoutInit();  void breakoutUpdate();
void invadersInit();  void invadersUpdate();
void minesInit();     void minesUpdate();
void simonInit();     void simonUpdate();

// Pong (single + multiplayer) keeps its own sub-menu / lobby states.
void pongSingleInit();
void pongSingleUpdate();
void pongMpInit(bool asHost);
void pongMpUpdate();

// Registered by gamecard.ino; games call it to launch (used internally).
void startGame(void (*init)(), void (*update)());

// ---------------------------------------------------------------------------
//  Networking (ESP-NOW)
// ---------------------------------------------------------------------------
#define NET_MAGIC 0xC3              // filters foreign ESP-NOW traffic
#define GAME_PONG 1                 // game id advertised in a lobby
#define GAME_TRON 2                 // up-to-4-player game id
#define GAME_BSHIP 3                // battleship (2 players)
#define GAME_UNO  4                 // uno (2-4 players)

enum NetMsgType {
  NET_ADV = 1,   // host -> broadcast : "lobby open"
  NET_JOIN,      // client -> host    : "let me in"        (Pong, 2P)
  NET_ACCEPT,    // host -> client    : "you're in"        (Pong, 2P)
  NET_STATE,     // host -> client    : authoritative state (Pong, 2P)
  NET_INPUT,     // client -> host    : paddle position     (Pong, 2P)
  NET_LEAVE,     // either way        : "I'm quitting"
  // --- multiplayer layer (up to 4 players) ---
  NET_MJOIN,     // client -> host    : join multi lobby
  NET_MACCEPT,   // host -> client    : accepted, here's your slot (p[0])
  NET_MSTART,    // host -> clients   : match begins (p[0] = player count)
  NET_MSTATE,    // host -> clients   : authoritative game state (p[0..5])
  NET_MINPUT,    // client -> host    : input (p[0]=slot, p[1]=value)
  NET_MMSG       // any -> any        : generic turn-based app message (p[]+d[])
};

typedef struct __attribute__((packed)) {
  uint8_t  magic;
  uint8_t  type;
  uint8_t  game;
  uint8_t  _pad;
  char     name[NAME_MAX];
  int16_t  p[6];        // generic payload (meaning depends on type)
  uint8_t  d[40];       // extra bytes for turn-based games (e.g. Uno hands)
} NetMessage;

// Discovered lobbies
#define MAX_LOBBIES 6
typedef struct {
  uint8_t  mac[6];
  char     name[NAME_MAX];
  uint32_t lastSeen;
} LobbyInfo;
extern LobbyInfo lobbies[MAX_LOBBIES];
extern int       lobbyCount;

// Connection state (touched from the ESP-NOW receive callback -> volatile)
extern bool             isHost;
extern uint8_t          peerMac[6];
extern volatile bool    netConnected;
extern volatile bool    netJustConnected;
extern volatile bool    netPeerLost;
extern volatile uint32_t netLastRecv;

// Live gameplay values exchanged during a match
extern volatile int16_t gBallX, gBallY, gLeftP, gRightP, gScoreL, gScoreR;
extern volatile bool    gStateNew;
extern volatile int16_t gClientPaddle;
extern volatile bool    gClientInputNew;

void netInit();
void netReset();                    // drop connection, keep radio alive
void netStartHost(uint8_t game);
void netStartBrowse(uint8_t game);
void netTick();                     // call every loop
void netBroadcast(NetMessage* m);
void netSendToPeer(NetMessage* m);
void netFill(NetMessage* m, uint8_t type, uint8_t game);

// ---------------------------------------------------------------------------
//  Multiplayer layer (up to 4 players: 1 host + up to 3 clients)
//  Reuses NET_ADV discovery + the lobbies[] list; own join/state/input path.
// ---------------------------------------------------------------------------
#define MP_MAXCLIENTS 3
#define MP_MAXPLAYERS 4

extern bool              mpIsHost;
extern int               mpMySlot;         // 0 = host, 1..3 = clients
extern int               mpNumPlayers;
extern int               mpNumClients;     // host: how many joined
extern volatile bool     mpStarted;
extern volatile bool     mpJustStarted;    // consume to enter the match
extern volatile bool     mpAborted;        // host left / link lost
extern volatile uint32_t mpLastRecv;

// Discovery target (read by the receive callback in netplay.ino)
extern bool     mpBrowsing;
extern uint8_t  mpBrowseGame;

// Latest received game state (clients read this)
extern volatile bool    mpStateNew;
extern volatile int16_t mpS[6];
// Host reads latest input value per player slot
extern volatile int16_t mpInput[MP_MAXPLAYERS];
extern char mpClientNames[MP_MAXCLIENTS][NAME_MAX];

void mpReset();
void mpLeave();
void mpHostStart(uint8_t game);
void mpBrowseStart(uint8_t game);
void mpTick();                          // called from netTick()
void mpJoinLobby(const uint8_t* mac);
void mpHostBegin();                     // host: start the match
void mpBroadcastState(const int16_t p[6]);
void mpSendInput(int16_t value);        // client -> host
void mpRecv(const uint8_t* mac, const NetMessage* m);  // dispatched by onRecv

// Generic turn-based messaging (Battleship, Uno).
// target: -1 = host broadcast to all clients, 0 = to host, 1..3 = to that client.
// Games fill m with netFill(&m, NET_MMSG, game), set p[]/d[], then mpTx.
void mpTx(int target, NetMessage* m);
bool mpPoll(int* fromSlot, NetMessage* out);   // dequeue a received app message

// Tron (up to 4 players) - screens live in tron.ino
void tronModeUpdate();
void tronHostUpdate();
void tronBrowseUpdate();
void tronPlayInit();
void tronPlayUpdate();

// ---------------------------------------------------------------------------
//  Generic multiplayer lobby (mplobby.ino) - used by Battleship & Uno
// ---------------------------------------------------------------------------
void mpMenuConfig(const char* title, uint8_t game, AppState playState,
                  int minPlayers, bool autoStart);
void mpModeUpdate();
void mpHostLobbyUpdate();
void mpBrowseLobbyUpdate();

// Battleship (2 players) - bship.ino
void bshipInit();
void bshipUpdate();

// Uno (2-4 players) - uno.ino
void unoInit();
void unoUpdate();

// ---------------------------------------------------------------------------
//  Small UI helpers (defined in gamecard.ino)
// ---------------------------------------------------------------------------
void uiTitle(const char* t);
void uiCenter(const char* t, int y, uint8_t size = 1);
