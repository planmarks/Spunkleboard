// ============================================================================
//  settings.ino - device name, stored in NVS via Preferences
//
//  Name editor controls:
//    LEFT / RIGHT : move cursor
//    UP / DOWN    : change character at cursor
//    A            : save & exit
//    B            : cancel & exit
// ============================================================================
#include "common.h"

static Preferences prefs;

// Charset the editor cycles through.
static const char* CHARSET = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789- ";
static const int   CHARSET_LEN = 38;

static char editBuf[NAME_MAX];
static int  cursor = 0;
static int  editLen = 0;

void loadSettings() {
  prefs.begin("gamecard", true);
  String n = prefs.getString("name", "");
  prefs.end();

  if (n.length() == 0) {
    uint8_t mac[6];
    WiFi.macAddress(mac);
    snprintf(deviceName, NAME_MAX, "GC-%02X%02X", mac[4], mac[5]);
  } else {
    strncpy(deviceName, n.c_str(), NAME_MAX - 1);
    deviceName[NAME_MAX - 1] = '\0';
  }
}

void saveDeviceName(const char* name) {
  prefs.begin("gamecard", false);
  prefs.putString("name", name);
  prefs.end();
  strncpy(deviceName, name, NAME_MAX - 1);
  deviceName[NAME_MAX - 1] = '\0';
}

static int charIndex(char c) {
  for (int i = 0; i < CHARSET_LEN; i++)
    if (CHARSET[i] == toupper(c)) return i;
  return CHARSET_LEN - 1;  // space
}

void settingsInit() {
  // Copy current name into a fixed-width edit buffer, space-padded.
  memset(editBuf, ' ', NAME_MAX - 1);
  editBuf[NAME_MAX - 1] = '\0';
  int len = strlen(deviceName);
  if (len > NAME_MAX - 1) len = NAME_MAX - 1;
  memcpy(editBuf, deviceName, len);
  editLen = NAME_MAX - 1;
  cursor  = 0;
}

static void commitName() {
  // Trim trailing spaces before saving.
  char out[NAME_MAX];
  strncpy(out, editBuf, NAME_MAX - 1);
  out[NAME_MAX - 1] = '\0';
  for (int i = (int)strlen(out) - 1; i >= 0 && out[i] == ' '; i--) out[i] = '\0';
  if (out[0] == '\0') strcpy(out, "GC");
  saveDeviceName(out);
}

void settingsUpdate() {
  if (btnPressed[BTN_B]) { setState(ST_MENU); return; }
  if (btnPressed[BTN_A]) { commitName(); setState(ST_MENU); return; }

  if (btnHeldRepeat(BTN_LEFT, 350, 130))  cursor = (cursor + editLen - 1) % editLen;
  if (btnHeldRepeat(BTN_RIGHT, 350, 130)) cursor = (cursor + 1) % editLen;

  if (btnHeldRepeat(BTN_UP, 350, 130)) {
    int idx = (charIndex(editBuf[cursor]) + 1) % CHARSET_LEN;
    editBuf[cursor] = CHARSET[idx];
  }
  if (btnHeldRepeat(BTN_DOWN, 350, 130)) {
    int idx = (charIndex(editBuf[cursor]) + CHARSET_LEN - 1) % CHARSET_LEN;
    editBuf[cursor] = CHARSET[idx];
  }

  // Render
  display.clearDisplay();
  uiTitle("SETTINGS");
  display.setCursor(0, 16);
  display.print(" Device name:");

  // The name, size 2, with an underline under the active character.
  int startX = 6, y = 30;
  display.setTextSize(2);
  for (int i = 0; i < editLen; i++) {
    int x = startX + i * 12;
    display.setCursor(x, y);
    display.print(editBuf[i]);
    if (i == cursor) display.drawFastHLine(x, y + 17, 11, SSD1306_WHITE);
  }
  display.setTextSize(1);

  display.setCursor(0, 56);
  display.print("A:save B:cancel");
  display.display();
}
