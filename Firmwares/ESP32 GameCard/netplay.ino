// ============================================================================
//  netplay.ino - ESP-NOW lobby + transport
//
//  Model:
//    * Every unit runs the same firmware and filters traffic by NET_MAGIC.
//    * A host broadcasts NET_ADV ("lobby open") a few times per second.
//    * Browsers collect adverts into a lobby list.
//    * A browser sends NET_JOIN to a chosen host; the host replies NET_ACCEPT.
//    * Once connected, host and client exchange NET_STATE / NET_INPUT.
//
//  All devices stay on the default WiFi channel (1) since we never join an AP,
//  so no channel negotiation is needed.
// ============================================================================
#include "common.h"
#include "esp_wifi.h"

// ---- Definitions of the globals declared in common.h -----------------------
LobbyInfo lobbies[MAX_LOBBIES];
int       lobbyCount = 0;

bool             isHost = false;
uint8_t          peerMac[6] = { 0 };
volatile bool    netConnected     = false;
volatile bool    netJustConnected = false;
volatile bool    netPeerLost      = false;
volatile uint32_t netLastRecv     = 0;

volatile int16_t gBallX = 0, gBallY = 0, gLeftP = 0, gRightP = 0,
                 gScoreL = 0, gScoreR = 0;
volatile bool    gStateNew = false;
volatile int16_t gClientPaddle = 0;
volatile bool    gClientInputNew = false;

// ---- Local state -----------------------------------------------------------
static const uint8_t BCAST[6] = { 0xFF,0xFF,0xFF,0xFF,0xFF,0xFF };
static bool     hosting  = false;
static bool     browsing = false;
static uint8_t  hostGame = 0;
static uint32_t lastAdv  = 0;

// ---- Helpers ---------------------------------------------------------------
static bool addPeer(const uint8_t* mac) {
  if (esp_now_is_peer_exist(mac)) return true;
  esp_now_peer_info_t peer = {};
  memcpy(peer.peer_addr, mac, 6);
  peer.channel = 0;        // use current channel
  peer.encrypt = false;
  return esp_now_add_peer(&peer) == ESP_OK;
}

void netFill(NetMessage* m, uint8_t type, uint8_t game) {
  memset(m, 0, sizeof(NetMessage));
  m->magic = NET_MAGIC;
  m->type  = type;
  m->game  = game;
  strncpy(m->name, deviceName, NAME_MAX - 1);
}

void netBroadcast(NetMessage* m)  { esp_now_send(BCAST, (uint8_t*)m, sizeof(NetMessage)); }
void netSendToPeer(NetMessage* m) { esp_now_send(peerMac, (uint8_t*)m, sizeof(NetMessage)); }

static void addOrUpdateLobby(const uint8_t* mac, const NetMessage* m) {
  for (int i = 0; i < lobbyCount; i++) {
    if (memcmp(lobbies[i].mac, mac, 6) == 0) {
      lobbies[i].lastSeen = millis();
      strncpy(lobbies[i].name, m->name, NAME_MAX - 1);
      return;
    }
  }
  if (lobbyCount < MAX_LOBBIES) {
    memcpy(lobbies[lobbyCount].mac, mac, 6);
    strncpy(lobbies[lobbyCount].name, m->name, NAME_MAX - 1);
    lobbies[lobbyCount].name[NAME_MAX - 1] = '\0';
    lobbies[lobbyCount].lastSeen = millis();
    lobbyCount++;
  }
}

// ---- Receive callback ------------------------------------------------------
//  Signature differs between Arduino-ESP32 core 3.x and 2.x.
#if ESP_ARDUINO_VERSION_MAJOR >= 3
static void onRecv(const esp_now_recv_info_t* info, const uint8_t* data, int len)
#else
static void onRecv(const uint8_t* mac, const uint8_t* data, int len)
#endif
{
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  const uint8_t* mac = info->src_addr;
#endif
  if (len != sizeof(NetMessage)) return;
  NetMessage m;
  memcpy(&m, data, sizeof(m));
  if (m.magic != NET_MAGIC) return;

  // Route the multiplayer-layer messages to netmp.ino.
  if (m.type >= NET_MJOIN) { mpRecv(mac, &m); return; }

  switch (m.type) {
    case NET_ADV:
      if (browsing && m.game == hostGame) addOrUpdateLobby(mac, &m);
      if (mpBrowsing && m.game == mpBrowseGame) addOrUpdateLobby(mac, &m);
      break;

    case NET_JOIN:
      if (hosting && !netConnected && m.game == hostGame) {
        memcpy(peerMac, mac, 6);
        addPeer(peerMac);
        NetMessage r; netFill(&r, NET_ACCEPT, hostGame);
        netSendToPeer(&r);
        isHost = true;
        netConnected = true;
        netJustConnected = true;
        netLastRecv = millis();
      }
      break;

    case NET_ACCEPT:
      if (browsing && !netConnected) {
        memcpy(peerMac, mac, 6);
        addPeer(peerMac);
        isHost = false;
        netConnected = true;
        netJustConnected = true;
        netLastRecv = millis();
      }
      break;

    case NET_STATE:               // client consumes host's authoritative state
      gBallX  = m.p[0]; gBallY = m.p[1];
      gLeftP  = m.p[2]; gRightP = m.p[3];
      gScoreL = m.p[4]; gScoreR = m.p[5];
      gStateNew   = true;
      netLastRecv = millis();
      break;

    case NET_INPUT:               // host consumes client's paddle position
      gClientPaddle   = m.p[0];
      gClientInputNew = true;
      netLastRecv     = millis();
      break;

    case NET_LEAVE:
      netPeerLost = true;              // Pong path
      mpRecv(mac, &m);                 // multi path (sets mpAborted if joined)
      break;
  }
}

// ---- Public API ------------------------------------------------------------
void netInit() {
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  WiFi.setSleep(false);              // disable modem sleep: reliable ESP-NOW RX
  esp_wifi_set_max_tx_power(84);     // 84 * 0.25 dBm = 21 dBm (maximum)
  if (esp_now_init() != ESP_OK) {
    Serial.println("ESP-NOW init failed");
    return;
  }
  esp_now_register_recv_cb(onRecv);
  addPeer(BCAST);
}

// Tear the connection down but leave the radio + broadcast peer alive.
void netReset() {
  if (netConnected || memcmp(peerMac, "\0\0\0\0\0\0", 6) != 0) {
    NetMessage m; netFill(&m, NET_LEAVE, hostGame);
    if (esp_now_is_peer_exist(peerMac)) netSendToPeer(&m);
    if (esp_now_is_peer_exist(peerMac)) esp_now_del_peer(peerMac);
  }
  memset(peerMac, 0, 6);
  hosting = browsing = false;
  hostGame = 0;
  netConnected = netJustConnected = netPeerLost = false;
  gStateNew = gClientInputNew = false;
  lobbyCount = 0;
}

void netStartHost(uint8_t game) {
  hosting = true; browsing = false; hostGame = game;
  lastAdv = 0;
}

void netStartBrowse(uint8_t game) {
  browsing = true; hosting = false; hostGame = game;
  lobbyCount = 0;
}

void netTick() {
  uint32_t now = millis();

  if (hosting && !netConnected && now - lastAdv >= 400) {
    lastAdv = now;
    NetMessage m; netFill(&m, NET_ADV, hostGame);
    netBroadcast(&m);
  }

  if (browsing && !netConnected) {          // prune lobbies we stopped hearing
    for (int i = 0; i < lobbyCount; ) {
      if (now - lobbies[i].lastSeen > 2500) {
        for (int j = i; j < lobbyCount - 1; j++) lobbies[j] = lobbies[j + 1];
        lobbyCount--;
      } else i++;
    }
  }

  mpTick();                                 // multiplayer-layer advertising/prune
}
