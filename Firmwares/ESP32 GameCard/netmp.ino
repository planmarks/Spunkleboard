// ============================================================================
//  netmp.ino - multiplayer transport (up to 4 players)
//
//  1 host + up to 3 clients. Discovery reuses NET_ADV + the lobbies[] list.
//    Client -> host : NET_MJOIN (unicast), NET_MINPUT (unicast)
//    Host -> client : NET_MACCEPT (unicast), NET_MSTART (unicast to each)
//    Host -> all    : NET_MSTATE (broadcast; clients filter by host MAC)
//
//  The host is authoritative. State is small enough to broadcast each tick.
// ============================================================================
#include "common.h"

// ---- Definitions of globals declared in common.h ---------------------------
bool              mpIsHost = false;
int               mpMySlot = 0;
int               mpNumPlayers = 0;
int               mpNumClients = 0;
volatile bool     mpStarted = false;
volatile bool     mpJustStarted = false;
volatile bool     mpAborted = false;
volatile uint32_t mpLastRecv = 0;

bool     mpBrowsing = false;
uint8_t  mpBrowseGame = 0;

volatile bool    mpStateNew = false;
volatile int16_t mpS[6] = { 0 };
volatile int16_t mpInput[MP_MAXPLAYERS] = { 0 };

char mpClientNames[MP_MAXCLIENTS][NAME_MAX];   // host: joined client names

// ---- Local state -----------------------------------------------------------
static bool     mpHosting = false;
static uint8_t  mpGame = 0;
static uint8_t  mpClients[MP_MAXCLIENTS][6];
static uint8_t  mpHostMac[6];
static bool     mpJoined = false;
static uint32_t mpLastAdv = 0;

static const uint8_t MP_BCAST[6] = { 0xFF,0xFF,0xFF,0xFF,0xFF,0xFF };

// Received app-message queue (single producer: WiFi task; consumer: loop).
#define MP_QLEN 8
static NetMessage    mpQ[MP_QLEN];
static int           mpQFrom[MP_QLEN];
static volatile int  mpQHead = 0, mpQTail = 0;

static bool mpAddPeer(const uint8_t* mac) {
  if (esp_now_is_peer_exist(mac)) return true;
  esp_now_peer_info_t p = {};
  memcpy(p.peer_addr, mac, 6);
  p.channel = 0; p.encrypt = false;
  return esp_now_add_peer(&p) == ESP_OK;
}

static int mpFindClient(const uint8_t* mac) {
  for (int i = 0; i < mpNumClients; i++)
    if (memcmp(mpClients[i], mac, 6) == 0) return i;
  return -1;
}

// ---- Session control -------------------------------------------------------
void mpReset() {
  // Remove any peers we added (leave the broadcast peer intact).
  for (int i = 0; i < mpNumClients; i++)
    if (esp_now_is_peer_exist(mpClients[i])) esp_now_del_peer(mpClients[i]);
  if (mpJoined && esp_now_is_peer_exist(mpHostMac)) esp_now_del_peer(mpHostMac);

  mpHosting = mpBrowsing = false;
  mpJoined = false;
  mpGame = mpBrowseGame = 0;
  mpNumClients = 0;
  mpNumPlayers = 0;
  mpMySlot = 0;
  mpIsHost = false;
  mpStarted = mpJustStarted = mpAborted = false;
  mpStateNew = false;
  mpQHead = mpQTail = 0;
  lobbyCount = 0;
}

void mpHostStart(uint8_t game) {
  mpReset();
  mpHosting = true;
  mpIsHost = true;
  mpGame = game;
  mpMySlot = 0;
  mpLastAdv = 0;
}

void mpBrowseStart(uint8_t game) {
  mpReset();
  mpBrowsing = true;
  mpBrowseGame = game;
  mpGame = game;                 // so NET_MMSG filtering works on the client
}

// Notify peers we're leaving, then tear down.
void mpLeave() {
  NetMessage m; netFill(&m, NET_LEAVE, mpGame);
  if (mpIsHost)
    for (int i = 0; i < mpNumClients; i++)
      esp_now_send(mpClients[i], (uint8_t*)&m, sizeof(m));
  else if (mpJoined)
    esp_now_send(mpHostMac, (uint8_t*)&m, sizeof(m));
  mpReset();
}

void mpJoinLobby(const uint8_t* mac) {
  memcpy(mpHostMac, mac, 6);
  mpAddPeer(mpHostMac);
  NetMessage m; netFill(&m, NET_MJOIN, mpBrowseGame);
  esp_now_send(mpHostMac, (uint8_t*)&m, sizeof(m));
}

void mpHostBegin() {
  mpNumPlayers = mpNumClients + 1;
  mpStarted = true;
  NetMessage m; netFill(&m, NET_MSTART, mpGame);
  m.p[0] = mpNumPlayers;
  for (int i = 0; i < mpNumClients; i++)
    esp_now_send(mpClients[i], (uint8_t*)&m, sizeof(m));   // reliable unicast
}

// ---- Gameplay transport ----------------------------------------------------
void mpBroadcastState(const int16_t p[6]) {
  NetMessage m; netFill(&m, NET_MSTATE, mpGame);
  for (int i = 0; i < 6; i++) m.p[i] = p[i];
  netBroadcast(&m);                        // reaches all clients
}

void mpSendInput(int16_t value) {
  if (!mpJoined) return;
  NetMessage m; netFill(&m, NET_MINPUT, mpGame);
  m.p[0] = mpMySlot;
  m.p[1] = value;
  esp_now_send(mpHostMac, (uint8_t*)&m, sizeof(m));
}

// Generic addressed send. Caller fills m (netFill + payload) with NET_MMSG.
//   target -1 = broadcast to all clients, 0 = to host, 1..3 = that client slot.
void mpTx(int target, NetMessage* m) {
  if (target < 0)              netBroadcast(m);
  else if (target == 0)        esp_now_send(mpHostMac, (uint8_t*)m, sizeof(*m));
  else if (target <= mpNumClients)
                               esp_now_send(mpClients[target - 1], (uint8_t*)m, sizeof(*m));
}

bool mpPoll(int* fromSlot, NetMessage* out) {
  if (mpQHead == mpQTail) return false;
  *out = mpQ[mpQTail];
  if (fromSlot) *fromSlot = mpQFrom[mpQTail];
  mpQTail = (mpQTail + 1) % MP_QLEN;
  return true;
}

// ---- Periodic work (called from netTick) -----------------------------------
void mpTick() {
  uint32_t now = millis();
  if (mpHosting && !mpStarted && mpNumClients < MP_MAXCLIENTS &&
      now - mpLastAdv >= 400) {
    mpLastAdv = now;
    NetMessage m; netFill(&m, NET_ADV, mpGame);
    netBroadcast(&m);
  }
  if (mpBrowsing) {                        // prune lobbies we stopped hearing
    for (int i = 0; i < lobbyCount; ) {
      if (now - lobbies[i].lastSeen > 2500) {
        for (int j = i; j < lobbyCount - 1; j++) lobbies[j] = lobbies[j + 1];
        lobbyCount--;
      } else i++;
    }
  }
}

// ---- Receive (dispatched from onRecv in netplay.ino) -----------------------
void mpRecv(const uint8_t* mac, const NetMessage* m) {
  switch (m->type) {

    case NET_MJOIN:
      if (mpHosting && !mpStarted && m->game == mpGame) {
        int idx = mpFindClient(mac);
        if (idx < 0 && mpNumClients < MP_MAXCLIENTS) {
          idx = mpNumClients++;
          memcpy(mpClients[idx], mac, 6);
          strncpy(mpClientNames[idx], m->name, NAME_MAX - 1);
          mpClientNames[idx][NAME_MAX - 1] = '\0';
          mpAddPeer(mac);
        }
        if (idx >= 0) {                    // (re)confirm the slot
          NetMessage r; netFill(&r, NET_MACCEPT, mpGame);
          r.p[0] = idx + 1;                // slot 0 is the host
          esp_now_send(mac, (uint8_t*)&r, sizeof(r));
        }
        mpLastRecv = millis();
      }
      break;

    case NET_MACCEPT:
      if (mpBrowsing && !mpJoined && m->game == mpBrowseGame) {
        mpMySlot = m->p[0];
        mpJoined = true;
        mpLastRecv = millis();
      }
      break;

    case NET_MSTART:
      if (mpJoined && memcmp(mac, mpHostMac, 6) == 0) {
        mpNumPlayers = m->p[0];
        if (!mpStarted) { mpStarted = true; mpJustStarted = true; }
        mpLastRecv = millis();
      }
      break;

    case NET_MSTATE:
      if (mpJoined && memcmp(mac, mpHostMac, 6) == 0) {
        for (int i = 0; i < 6; i++) mpS[i] = m->p[i];
        mpStateNew = true;
        mpLastRecv = millis();
        if (!mpStarted) {                    // fallback if NET_MSTART was lost
          mpStarted = true; mpJustStarted = true;
          if (mpNumPlayers < 2) {
            int cnt = 0; uint16_t mask = (uint16_t)m->p[4];
            for (int b = 0; b < MP_MAXPLAYERS; b++) if (mask & (1 << b)) cnt++;
            mpNumPlayers = (cnt < 2) ? 2 : cnt;
          }
        }
      }
      break;

    case NET_MINPUT:
      if (mpHosting) {
        int slot = m->p[0];
        if (slot >= 1 && slot < MP_MAXPLAYERS) mpInput[slot] = m->p[1];
        mpLastRecv = millis();
      }
      break;

    case NET_MMSG:
      if (m->game == mpGame) {
        bool ok = mpIsHost ? (mpFindClient(mac) >= 0)
                           : (mpJoined && memcmp(mac, mpHostMac, 6) == 0);
        if (ok) {
          int from = mpIsHost ? (mpFindClient(mac) + 1) : 0;
          int nh = (mpQHead + 1) % MP_QLEN;
          if (nh != mpQTail) {           // drop if full (shouldn't happen)
            mpQ[mpQHead] = *m;
            mpQFrom[mpQHead] = from;
            mpQHead = nh;
          }
          mpLastRecv = millis();
          // Safety net: enter the match even if NET_MSTART was missed.
          if (!mpIsHost && mpJoined && !mpStarted) { mpStarted = true; mpJustStarted = true; }
        }
      }
      break;

    case NET_LEAVE:
      if (mpJoined && memcmp(mac, mpHostMac, 6) == 0) mpAborted = true;
      break;
  }
}
