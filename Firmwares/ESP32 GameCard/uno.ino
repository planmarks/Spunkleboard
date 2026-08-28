// ============================================================================
//  uno.ino - Uno (2-4 players over ESP-NOW)
//
//  Host is the authoritative dealer: it holds the deck, discard and every hand,
//  validates each move, and sends each player only their own hand (private) plus
//  a broadcast public state. Clients are thin: render + send actions.
//
//  Card byte = (color<<4)|value.  color 0..3 = R/G/B/Y, 4 = wild.
//  value 0..9 numbers, 10 Skip, 11 Reverse, 12 Draw2, 13 Wild, 14 WildDraw4.
//
//  Controls (on your turn): LEFT/RIGHT pick a card or the DRAW slot, A confirm.
//  Playing a wild -> choose a colour with the D-pad. B quits.
// ============================================================================
#include "common.h"

enum { U_STATE = 1, U_HAND, U_PLAY, U_DRAW };

// ---- Host authoritative state ----------------------------------------------
static uint8_t  unDeck[108], unDiscard[108];
static int      unDeckN, unDiscardN;
static uint8_t  unHands[4][40];
static int      unHandN[4];
static int      unDir, unCurPlayer, unWinner, unPlayers;
static uint8_t  unTop, unColor, unValue;
static uint32_t unHeartbeat;

// ---- Client mirror ---------------------------------------------------------
static uint8_t  unMyHand[40];
static int      unMyHandN;
static uint8_t  unPubTop, unPubColor;
static int      unPubCur, unPubDir, unPubWinner, unPubCounts[4];
static bool     unHaveState;

// ---- UI --------------------------------------------------------------------
static int      unSel;
static bool     unPick;
static uint8_t  unPend;

// ===========================================================================
//  Host deck / rules
// ===========================================================================
static void unShuffle(uint8_t* a, int n) {
  for (int i = n - 1; i > 0; i--) { int j = random(i + 1); uint8_t t = a[i]; a[i] = a[j]; a[j] = t; }
}

static void unBuildDeck() {
  unDeckN = 0;
  for (int c = 0; c < 4; c++) {
    unDeck[unDeckN++] = (c << 4) | 0;
    for (int v = 1; v <= 9; v++) { unDeck[unDeckN++] = (c << 4) | v; unDeck[unDeckN++] = (c << 4) | v; }
    for (int t = 0; t < 2; t++) {
      unDeck[unDeckN++] = (c << 4) | 10;
      unDeck[unDeckN++] = (c << 4) | 11;
      unDeck[unDeckN++] = (c << 4) | 12;
    }
  }
  for (int w = 0; w < 4; w++) { unDeck[unDeckN++] = (4 << 4) | 13; unDeck[unDeckN++] = (4 << 4) | 14; }
  unShuffle(unDeck, unDeckN);
}

static uint8_t unDraw() {
  if (unDeckN == 0) {                       // recycle the discard pile
    for (int i = 0; i < unDiscardN; i++) unDeck[unDeckN++] = unDiscard[i];
    unDiscardN = 0;
    unShuffle(unDeck, unDeckN);
  }
  if (unDeckN == 0) return 0xFF;
  return unDeck[--unDeckN];
}

static void unGive(int slot, int n) {
  for (int i = 0; i < n; i++) {
    uint8_t c = unDraw();
    if (c != 0xFF && unHandN[slot] < 40) unHands[slot][unHandN[slot]++] = c;
  }
}

static bool unPlayable(uint8_t card) {
  int col = card >> 4, val = card & 0x0F;
  return col == 4 || col == unColor || val == unValue;
}

static void unAdvance(int steps) {
  unCurPlayer = ((unCurPlayer + unDir * steps) % unPlayers + unPlayers) % unPlayers;
}

static int unNext() { return ((unCurPlayer + unDir) % unPlayers + unPlayers) % unPlayers; }

static void unApplyPlay(int slot, uint8_t card, int chosen) {
  if (slot != unCurPlayer || unWinner >= 0) return;
  int idx = -1;
  for (int i = 0; i < unHandN[slot]; i++) if (unHands[slot][i] == card) { idx = i; break; }
  if (idx < 0 || !unPlayable(card)) return;

  unDiscard[unDiscardN++] = unTop;
  for (int i = idx; i < unHandN[slot] - 1; i++) unHands[slot][i] = unHands[slot][i + 1];
  unHandN[slot]--;

  int col = card >> 4, val = card & 0x0F;
  unTop = card;
  if (col == 4) { if (chosen < 0 || chosen > 3) chosen = random(4); unColor = chosen; }
  else          unColor = col;
  unValue = val;

  if (unHandN[slot] == 0) { unWinner = slot; return; }

  if (val <= 9)       unAdvance(1);
  else if (val == 10) unAdvance(2);                          // skip
  else if (val == 11) { if (unPlayers == 2) unAdvance(2);    // reverse
                        else { unDir = -unDir; unAdvance(1); } }
  else if (val == 12) { unGive(unNext(), 2); unAdvance(2); } // draw two
  else if (val == 13) unAdvance(1);                          // wild
  else                { unGive(unNext(), 4); unAdvance(2); } // wild draw four
}

static void unApplyDraw(int slot) {
  if (slot != unCurPlayer || unWinner >= 0) return;
  unGive(slot, 1);
  unAdvance(1);
}

static void unHostBroadcast() {
  for (int s = 1; s < unPlayers; s++) {              // private hands
    NetMessage m; netFill(&m, NET_MMSG, GAME_UNO);
    int n = unHandN[s]; if (n > 39) n = 39;          // d[] holds count + 39 cards
    m.p[0] = U_HAND;
    m.d[0] = n;
    for (int i = 0; i < n; i++) m.d[1 + i] = unHands[s][i];
    mpTx(s, &m);
  }
  NetMessage m; netFill(&m, NET_MMSG, GAME_UNO);     // public state
  m.p[0] = U_STATE; m.p[1] = unTop; m.p[2] = unColor;
  m.p[3] = unCurPlayer; m.p[4] = unDir; m.p[5] = unWinner;
  for (int s = 0; s < 4; s++) m.d[s] = (s < unPlayers) ? unHandN[s] : 0;
  mpTx(-1, &m);
}

// ===========================================================================
//  Init
// ===========================================================================
void unoInit() {
  unSel = 0; unPick = false; unHaveState = false;
  unMyHandN = 0;
  unPlayers = mpNumPlayers; if (unPlayers < 2) unPlayers = 2; if (unPlayers > 4) unPlayers = 4;

  if (!mpIsHost) return;                              // client waits for state

  unBuildDeck(); unDiscardN = 0;
  for (int s = 0; s < unPlayers; s++) { unHandN[s] = 0; unGive(s, 7); }
  uint8_t c;
  do { c = unDraw(); if ((c & 0x0F) > 9) unDiscard[unDiscardN++] = c; } while ((c & 0x0F) > 9);
  unTop = c; unColor = c >> 4; unValue = c & 0x0F;
  unCurPlayer = 0; unDir = 1; unWinner = -1;
  unHeartbeat = millis();
  unHostBroadcast();
}

// ===========================================================================
//  Rendering
// ===========================================================================
static void unValStr(uint8_t card, char* o) {
  int v = card & 0x0F;
  if (v <= 9) { o[0] = '0' + v; o[1] = 0; }
  else if (v == 10) strcpy(o, "Sk");
  else if (v == 11) strcpy(o, "Rv");
  else if (v == 12) strcpy(o, "+2");
  else if (v == 13) strcpy(o, "Wi");
  else              strcpy(o, "+4");
}

static void unCardBox(int x, uint8_t card, bool sel) {
  display.drawRect(x, 40, 20, 20, SSD1306_WHITE);
  if (sel) display.drawRect(x - 1, 39, 22, 22, SSD1306_WHITE);
  display.setCursor(x + 7, 43); display.write("RGBYW"[card >> 4]);
  char v[3]; unValStr(card, v);
  display.setCursor(x + 4, 52); display.print(v);
}

static void unRender(uint8_t vTop, uint8_t vColor, int vCur, int vWinner,
                     const int* counts, const uint8_t* hand, int handN, int mySlot) {
  display.clearDisplay();
  char buf[24], vs[3];
  unValStr(vTop, vs);
  snprintf(buf, sizeof(buf), "Top %c%s", "RGBYW"[vColor], vs);
  uiTitle(buf);

  // counts row
  display.setCursor(0, 13);
  for (int s = 0; s < unPlayers; s++) {
    display.print(s == vCur ? '>' : ' ');
    display.print('P'); display.print(s + 1); display.print(':'); display.print(counts[s]);
    display.print(' ');
  }

  if (vWinner >= 0) {
    uiCenter(vWinner == mySlot ? "YOU WIN!" : "You lose", 30, 1);
    uiCenter("A/B: menu", 44);
    display.display();
    return;
  }

  if (unPick) {
    uiCenter("Pick colour:", 24);
    display.setCursor(0, 36); display.print(" U:Grn D:Yel");
    display.setCursor(0, 46); display.print(" L:Red R:Blu");
    display.display();
    return;
  }

  if (vCur != mySlot) {
    char w[16]; snprintf(w, sizeof(w), "P%d's turn", vCur + 1);
    uiCenter(w, 30);
    display.display();
    return;
  }

  // Your hand: cards + a DRAW slot, windowed around the cursor.
  int total = handN + 1;
  int win = unSel - 2; if (win < 0) win = 0;
  if (win > total - 5) win = total - 5; if (win < 0) win = 0;
  for (int i = 0; i < 5; i++) {
    int idx = win + i;
    if (idx >= total) break;
    int x = 4 + i * 24;
    if (idx == handN) {                    // DRAW slot
      display.drawRect(x, 40, 20, 20, SSD1306_WHITE);
      if (idx == unSel) display.drawRect(x - 1, 39, 22, 22, SSD1306_WHITE);
      display.setCursor(x + 3, 47); display.print("DR");
    } else {
      unCardBox(x, hand[idx], idx == unSel);
    }
  }
  display.display();
}

// ===========================================================================
//  Update
// ===========================================================================
static void unCommitPlay(uint8_t card, int color) {
  if (mpIsHost) { unApplyPlay(0, card, color); unHostBroadcast(); }
  else { NetMessage m; netFill(&m, NET_MMSG, GAME_UNO);
         m.p[0] = U_PLAY; m.p[1] = card; m.p[2] = color; mpTx(0, &m); }
}

static void unDoDraw() {
  if (mpIsHost) { unApplyDraw(0); unHostBroadcast(); }
  else { NetMessage m; netFill(&m, NET_MMSG, GAME_UNO);
         m.p[0] = U_DRAW; mpTx(0, &m); }
}

static void unQuit() { mpLeave(); setState(ST_MENU); }

void unoUpdate() {
  if (!mpIsHost && mpAborted) {
    if (btnPressed[BTN_A] || btnPressed[BTN_B]) { unQuit(); return; }
    display.clearDisplay(); uiCenter("Host left", 24); uiCenter("A/B: menu", 40);
    display.display(); return;
  }

  // Networking
  int from; NetMessage m;
  if (mpIsHost) {
    bool changed = false;
    while (mpPoll(&from, &m)) {
      if (m.p[0] == U_PLAY)      { unApplyPlay(from, (uint8_t)m.p[1], m.p[2]); changed = true; }
      else if (m.p[0] == U_DRAW) { unApplyDraw(from); changed = true; }
    }
    if (changed) unHostBroadcast();
    if (millis() - unHeartbeat > 700) { unHeartbeat = millis(); unHostBroadcast(); }
  } else {
    while (mpPoll(&from, &m)) {
      if (m.p[0] == U_STATE) {
        unPubTop = m.p[1]; unPubColor = m.p[2]; unPubCur = m.p[3];
        unPubDir = m.p[4]; unPubWinner = m.p[5];
        for (int s = 0; s < 4; s++) unPubCounts[s] = m.d[s];
        unHaveState = true;
      } else if (m.p[0] == U_HAND) {
        unMyHandN = m.d[0]; if (unMyHandN > 39) unMyHandN = 39;
        for (int i = 0; i < unMyHandN; i++) unMyHand[i] = m.d[1 + i];
      }
    }
    if (!unHaveState) {
      display.clearDisplay(); uiCenter("Dealing...", 28); display.display();
      return;
    }
  }

  // Build the local view.
  int mySlot = mpIsHost ? 0 : mpMySlot;
  uint8_t vTop, vColor; int vCur, vWinner; int counts[4];
  const uint8_t* hand; int handN;
  if (mpIsHost) {
    vTop = unTop; vColor = unColor; vCur = unCurPlayer; vWinner = unWinner;
    for (int s = 0; s < 4; s++) counts[s] = unHandN[s];
    hand = unHands[0]; handN = unHandN[0];
  } else {
    vTop = unPubTop; vColor = unPubColor; vCur = unPubCur; vWinner = unPubWinner;
    for (int s = 0; s < 4; s++) counts[s] = unPubCounts[s];
    hand = unMyHand; handN = unMyHandN;
  }
  int vValue = vTop & 0x0F;

  // Quit / game over
  if (vWinner >= 0) {
    if (btnPressed[BTN_A] || btnPressed[BTN_B]) { unQuit(); return; }
    unRender(vTop, vColor, vCur, vWinner, counts, hand, handN, mySlot);
    return;
  }
  if (btnPressed[BTN_B]) { unQuit(); return; }

  // Input on your turn
  if (vCur == mySlot) {
    if (unPick) {
      int col = -1;
      if (btnPressed[BTN_LEFT])  col = 0;   // Red
      if (btnPressed[BTN_UP])    col = 1;   // Green
      if (btnPressed[BTN_RIGHT]) col = 2;   // Blue
      if (btnPressed[BTN_DOWN])  col = 3;   // Yellow
      if (col >= 0) { unCommitPlay(unPend, col); unPick = false; }
    } else {
      int total = handN + 1;                // cards + DRAW
      if (unSel >= total) unSel = total - 1;
      if (btnHeldRepeat(BTN_LEFT, 300, 120))  unSel = (unSel + total - 1) % total;
      if (btnHeldRepeat(BTN_RIGHT, 300, 120)) unSel = (unSel + 1) % total;
      if (btnPressed[BTN_A]) {
        if (unSel == handN) unDoDraw();
        else {
          uint8_t card = hand[unSel];
          int col = card >> 4, val = card & 0x0F;
          bool ok = (col == 4 || col == vColor || val == vValue);
          if (ok) {
            if (col == 4) { unPick = true; unPend = card; }
            else unCommitPlay(card, -1);
          }
        }
      }
    }
  } else unSel = 0;

  unRender(vTop, vColor, vCur, vWinner, counts, hand, handN, mySlot);
}
