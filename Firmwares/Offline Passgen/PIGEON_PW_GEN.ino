/*
 * ============================================================================
 *  PIGEON  -  Private Independent Generator & Encrypted Offline Notebook
 * ============================================================================
 *  Offline secure password generator + encrypted vault.
 *
 *  Target board : Seeed Studio XIAO ESP32-C3
 *  Display      : 0.96" OLED, SSD1306, 128x64, I2C @ 0x3C
 *  Inputs       : 6 momentary push buttons (active-low, internal pull-ups)
 *
 *  Button wiring:
 *      D0 -> UP       D1 -> DOWN     D2 -> OK
 *      D3 -> BACK     D8 -> LEFT     D9 -> RIGHT
 *
 *  I2C wiring (XIAO ESP32-C3 default):
 *      D4 (GPIO6) -> OLED SDA
 *      D5 (GPIO7) -> OLED SCL
 *      3V3        -> OLED VCC
 *      GND        -> OLED GND
 *
 *  Security model
 *  --------------
 *  - A master PIN gates the device. The PIN never leaves RAM.
 *  - A 32-byte AES key is derived from the PIN with PBKDF2-HMAC-SHA256
 *    (per-device random 16-byte salt, high iteration count).
 *  - The stored verifier is SHA-256(AES key) -- the key itself is never
 *    written to flash, so a flash dump cannot reveal it without the PIN.
 *  - Each vault entry is encrypted with AES-256-CBC using a fresh random IV.
 *  - Randomness comes from the ESP32-C3 hardware RNG (esp_random()).
 *  - The device is fully offline: WiFi and Bluetooth are never started.
 *
 *  Libraries required (install via Arduino Library Manager):
 *      - Adafruit GFX Library
 *      - Adafruit SSD1306
 *  (mbedTLS and Preferences ship with the ESP32 Arduino core.)
 * ============================================================================
 */

#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <Preferences.h>

#include "esp_random.h"
#include "mbedtls/aes.h"
#include "mbedtls/md.h"
#include "mbedtls/sha256.h"
#include "mbedtls/pkcs5.h"

// ---------------------------------------------------------------------------
//  Display
// ---------------------------------------------------------------------------
#define SCREEN_WIDTH  128
#define SCREEN_HEIGHT 64
#define OLED_RESET    -1
#define OLED_ADDR     0x3C
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

// ---------------------------------------------------------------------------
//  Buttons
// ---------------------------------------------------------------------------
#define BTN_UP    D0
#define BTN_DOWN  D1
#define BTN_OK    D2
#define BTN_BACK  D3
#define BTN_LEFT  D8
#define BTN_RIGHT D9

enum Event : int8_t {
  EV_NONE = -1,
  EV_UP   = 0,
  EV_DOWN,
  EV_OK,
  EV_BACK,
  EV_LEFT,
  EV_RIGHT,
  EV_OK_LONG,     // OK held past LONG_MS
  EV_BACK_LONG    // BACK held past LONG_MS
};

const uint8_t kBtnPins[6] = { BTN_UP, BTN_DOWN, BTN_OK, BTN_BACK, BTN_LEFT, BTN_RIGHT };
static bool     btnState[6]    = { false };
static uint32_t btnRepeat[6]   = { 0 };
static uint32_t btnDownTime[6] = { 0 };
static bool     btnLongFired[6]= { false };

// Auto-repeat timing (ms) for directional keys held down.
static const uint32_t REPEAT_DELAY = 420;
static const uint32_t REPEAT_RATE  = 110;
// Hold time (ms) that turns an OK/BACK tap into a long-press.
static const uint32_t LONG_MS      = 550;

// ---------------------------------------------------------------------------
//  Crypto / storage parameters
// ---------------------------------------------------------------------------
static const uint32_t PBKDF2_ITERS = 20000;
static const uint8_t  SALT_LEN     = 16;
static const uint8_t  IV_LEN       = 16;
static const uint8_t  KEY_LEN      = 32;   // AES-256
static const uint8_t  VERIF_LEN    = 32;   // SHA-256

// Fixed-size plaintext record. Total MUST be a multiple of 16 (AES block).
#define LBL_LEN  24
#define USR_LEN  32
#define PWD_LEN  64
struct Entry {
  char label[LBL_LEN];
  char user[USR_LEN];
  char pass[PWD_LEN];
  char pad[8];        // -> 24+32+64+8 = 128 bytes
};
static const uint16_t ENTRY_PLAIN = sizeof(Entry);       // 128
static const uint16_t ENTRY_BLOB  = IV_LEN + ENTRY_PLAIN; // 144

static uint8_t g_aesKey[KEY_LEN];   // live only while unlocked
static bool    g_unlocked = false;

// ---------------------------------------------------------------------------
//  Generator configuration (non-secret, persisted in NVS)
// ---------------------------------------------------------------------------
struct GenCfg {
  uint8_t length;
  bool    useLower;
  bool    useUpper;
  bool    useDigits;
  bool    useSymbols;
};
static GenCfg g_cfg = { 20, true, true, true, true };

static const char* CS_LOWER   = "abcdefghijklmnopqrstuvwxyz";
static const char* CS_UPPER   = "ABCDEFGHIJKLMNOPQRSTUVWXYZ";
static const char* CS_DIGITS  = "0123456789";
static const char* CS_SYMBOLS = "!@#$%^&*()-_=+[]{};:,.?/";

// ===========================================================================
//  Low level helpers
// ===========================================================================
static void fillRandom(uint8_t* buf, size_t n) {
  size_t i = 0;
  while (i < n) {
    uint32_t r = esp_random();
    size_t c = (n - i < 4) ? (n - i) : 4;
    memcpy(buf + i, &r, c);
    i += c;
  }
}

// Uniform integer in [0, range) using rejection sampling (no modulo bias).
static uint32_t randBelow(uint32_t range) {
  if (range == 0) return 0;
  uint32_t limit = UINT32_MAX - (UINT32_MAX % range);
  uint32_t r;
  do { r = esp_random(); } while (r >= limit);
  return r % range;
}

static void pbkdf2(const char* pw, const uint8_t* salt, size_t slen,
                   uint32_t iters, uint8_t* out, size_t olen) {
  // mbedTLS 3.x (ESP32 core 3.x): context-free API.
  mbedtls_pkcs5_pbkdf2_hmac_ext(MBEDTLS_MD_SHA256,
                                (const uint8_t*)pw, strlen(pw),
                                salt, slen, iters, olen, out);
}

static void sha256(const uint8_t* in, size_t len, uint8_t* out32) {
  mbedtls_sha256(in, len, out32, 0 /* not SHA-224 */);
}

static bool constTimeEqual(const uint8_t* a, const uint8_t* b, size_t n) {
  uint8_t diff = 0;
  for (size_t i = 0; i < n; i++) diff |= (a[i] ^ b[i]);
  return diff == 0;
}

static void encryptEntry(const Entry& e, uint8_t* out /* ENTRY_BLOB */) {
  uint8_t iv[IV_LEN];
  fillRandom(iv, IV_LEN);
  memcpy(out, iv, IV_LEN);

  uint8_t ivWork[IV_LEN];
  memcpy(ivWork, iv, IV_LEN);   // CBC mutates the IV buffer

  mbedtls_aes_context aes;
  mbedtls_aes_init(&aes);
  mbedtls_aes_setkey_enc(&aes, g_aesKey, 256);
  mbedtls_aes_crypt_cbc(&aes, MBEDTLS_AES_ENCRYPT, ENTRY_PLAIN,
                        ivWork, (const uint8_t*)&e, out + IV_LEN);
  mbedtls_aes_free(&aes);
}

static void decryptEntry(const uint8_t* blob /* ENTRY_BLOB */, Entry& e) {
  uint8_t ivWork[IV_LEN];
  memcpy(ivWork, blob, IV_LEN);

  mbedtls_aes_context aes;
  mbedtls_aes_init(&aes);
  mbedtls_aes_setkey_dec(&aes, g_aesKey, 256);
  mbedtls_aes_crypt_cbc(&aes, MBEDTLS_AES_DECRYPT, ENTRY_PLAIN,
                        ivWork, blob + IV_LEN, (uint8_t*)&e);
  mbedtls_aes_free(&aes);
}

// ===========================================================================
//  Vault (NVS namespace "vault")
// ===========================================================================
static uint32_t vaultCount() {
  Preferences p;
  p.begin("vault", true);
  uint32_t n = p.getUInt("cnt", 0);
  p.end();
  return n;
}

static bool vaultLoad(uint32_t idx, Entry& e) {
  char key[8];
  snprintf(key, sizeof(key), "e%lu", (unsigned long)idx);
  uint8_t blob[ENTRY_BLOB];
  Preferences p;
  p.begin("vault", true);
  size_t got = p.getBytes(key, blob, ENTRY_BLOB);
  p.end();
  if (got != ENTRY_BLOB) return false;
  decryptEntry(blob, e);
  return true;
}

static bool vaultStoreRaw(uint32_t idx, const Entry& e) {
  char key[8];
  snprintf(key, sizeof(key), "e%lu", (unsigned long)idx);
  uint8_t blob[ENTRY_BLOB];
  encryptEntry(e, blob);
  Preferences p;
  p.begin("vault", false);
  size_t wrote = p.putBytes(key, blob, ENTRY_BLOB);
  p.end();
  return wrote == ENTRY_BLOB;
}

static bool vaultAdd(const Entry& e) {
  uint32_t n = vaultCount();
  if (!vaultStoreRaw(n, e)) return false;
  Preferences p;
  p.begin("vault", false);
  p.putUInt("cnt", n + 1);
  p.end();
  return true;
}

static void vaultDelete(uint32_t idx) {
  uint32_t n = vaultCount();
  if (idx >= n) return;
  // Compact: shift each later entry down by one slot.
  for (uint32_t i = idx; i + 1 < n; i++) {
    Entry tmp;
    if (vaultLoad(i + 1, tmp)) vaultStoreRaw(i, tmp);
  }
  // Remove the now-duplicate last slot and update the count.
  char key[8];
  snprintf(key, sizeof(key), "e%lu", (unsigned long)(n - 1));
  Preferences p;
  p.begin("vault", false);
  p.remove(key);
  p.putUInt("cnt", n - 1);
  p.end();
}

// ===========================================================================
//  Auth (NVS namespace "auth")
// ===========================================================================
static bool authExists() {
  Preferences p;
  p.begin("auth", true);
  bool has = p.isKey("verif");
  p.end();
  return has;
}

// Derive key from PIN + stored salt, load it into g_aesKey.
static void deriveKeyFromPin(const String& pin) {
  uint8_t salt[SALT_LEN];
  Preferences p;
  p.begin("auth", true);
  p.getBytes("salt", salt, SALT_LEN);
  p.end();
  pbkdf2(pin.c_str(), salt, SALT_LEN, PBKDF2_ITERS, g_aesKey, KEY_LEN);
}

static void authCreate(const String& pin) {
  uint8_t salt[SALT_LEN];
  fillRandom(salt, SALT_LEN);
  pbkdf2(pin.c_str(), salt, SALT_LEN, PBKDF2_ITERS, g_aesKey, KEY_LEN);

  uint8_t verif[VERIF_LEN];
  sha256(g_aesKey, KEY_LEN, verif);

  Preferences p;
  p.begin("auth", false);
  p.putBytes("salt", salt, SALT_LEN);
  p.putBytes("verif", verif, VERIF_LEN);
  p.end();
}

static bool authVerify(const String& pin) {
  deriveKeyFromPin(pin);
  uint8_t verif[VERIF_LEN];
  sha256(g_aesKey, KEY_LEN, verif);

  uint8_t stored[VERIF_LEN];
  Preferences p;
  p.begin("auth", true);
  size_t got = p.getBytes("verif", stored, VERIF_LEN);
  p.end();
  if (got != VERIF_LEN) return false;

  bool ok = constTimeEqual(verif, stored, VERIF_LEN);
  if (!ok) memset(g_aesKey, 0, KEY_LEN);   // wipe wrong key
  return ok;
}

static void factoryReset() {
  Preferences p;
  p.begin("auth", false);  p.clear(); p.end();
  p.begin("vault", false); p.clear(); p.end();
  p.begin("cfg", false);   p.clear(); p.end();
  memset(g_aesKey, 0, KEY_LEN);
  g_unlocked = false;
}

// ===========================================================================
//  Config persistence (NVS namespace "cfg")
// ===========================================================================
static void cfgLoad() {
  Preferences p;
  p.begin("cfg", true);
  g_cfg.length     = p.getUChar("len", 20);
  g_cfg.useLower   = p.getBool("lo", true);
  g_cfg.useUpper   = p.getBool("up", true);
  g_cfg.useDigits  = p.getBool("di", true);
  g_cfg.useSymbols = p.getBool("sy", true);
  p.end();
  if (g_cfg.length < 8)  g_cfg.length = 8;
  if (g_cfg.length > PWD_LEN - 1) g_cfg.length = PWD_LEN - 1;
}

static void cfgSave() {
  Preferences p;
  p.begin("cfg", false);
  p.putUChar("len", g_cfg.length);
  p.putBool("lo", g_cfg.useLower);
  p.putBool("up", g_cfg.useUpper);
  p.putBool("di", g_cfg.useDigits);
  p.putBool("sy", g_cfg.useSymbols);
  p.end();
}

// ===========================================================================
//  Password generation
// ===========================================================================
// Returns false if no character set is enabled.
static bool generatePassword(const GenCfg& cfg, char* out, uint8_t maxLen) {
  char pool[128];
  uint16_t pn = 0;
  if (cfg.useLower)   { strcpy(pool + pn, CS_LOWER);   pn += strlen(CS_LOWER); }
  if (cfg.useUpper)   { strcpy(pool + pn, CS_UPPER);   pn += strlen(CS_UPPER); }
  if (cfg.useDigits)  { strcpy(pool + pn, CS_DIGITS);  pn += strlen(CS_DIGITS); }
  if (cfg.useSymbols) { strcpy(pool + pn, CS_SYMBOLS); pn += strlen(CS_SYMBOLS); }
  if (pn == 0) return false;

  uint8_t len = cfg.length;
  if (len > maxLen - 1) len = maxLen - 1;

  for (uint8_t i = 0; i < len; i++) {
    out[i] = pool[randBelow(pn)];
  }
  out[len] = '\0';
  return true;
}

// ===========================================================================
//  Input: buttons
// ===========================================================================
static int8_t readEvent() {
  int8_t ev = EV_NONE;
  uint32_t now = millis();
  for (int i = 0; i < 6; i++) {
    bool pressed = (digitalRead(kBtnPins[i]) == LOW);
    bool dir = (i == EV_UP || i == EV_DOWN || i == EV_LEFT || i == EV_RIGHT);
    if (pressed && !btnState[i]) {
      btnState[i]     = true;
      btnDownTime[i]  = now;
      btnLongFired[i] = false;
      btnRepeat[i]    = now + REPEAT_DELAY;
      if (dir) ev = i;              // directional: act on press
      // OK/BACK: wait for release to tell a tap from a long-press
    } else if (pressed && btnState[i]) {
      if (dir) {
        if (now >= btnRepeat[i]) { btnRepeat[i] = now + REPEAT_RATE; ev = i; }
      } else if (!btnLongFired[i] && (now - btnDownTime[i] >= LONG_MS)) {
        btnLongFired[i] = true;     // fire the long event once, while still held
        ev = (i == EV_OK) ? EV_OK_LONG : EV_BACK_LONG;
      }
    } else if (!pressed && btnState[i]) {
      btnState[i] = false;          // release
      if (!dir && !btnLongFired[i]) ev = i;   // short tap emitted on release
    }
  }
  delay(6);   // crude debounce
  return ev;
}

// Block until any button is pressed and released.
static void waitAnyKey() {
  while (readEvent() == EV_NONE) {}
}

// ===========================================================================
//  Display helpers
// ===========================================================================
static void header(const char* title) {
  display.fillRect(0, 0, SCREEN_WIDTH, 11, SSD1306_WHITE);
  display.setTextColor(SSD1306_BLACK);
  display.setCursor(2, 2);
  display.print(title);
  display.setTextColor(SSD1306_WHITE);
}

static void toast(const char* line1, const char* line2 = nullptr) {
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 22);
  display.setTextSize(1);
  display.println(line1);
  if (line2) { display.setCursor(0, 34); display.println(line2); }
  display.display();
}

// Scrollable list menu. Returns selected index, or -1 on BACK.
static int menu(const char* title, const char* const* items, int count) {
  int sel = 0, top = 0;
  const int rows = 5;   // visible rows below the header
  while (true) {
    display.clearDisplay();
    header(title);
    display.setTextColor(SSD1306_WHITE);
    for (int r = 0; r < rows && (top + r) < count; r++) {
      int idx = top + r;
      int y = 14 + r * 10;
      if (idx == sel) {
        display.fillRect(0, y - 1, SCREEN_WIDTH, 10, SSD1306_WHITE);
        display.setTextColor(SSD1306_BLACK);
      } else {
        display.setTextColor(SSD1306_WHITE);
      }
      display.setCursor(2, y);
      display.print(items[idx]);
    }
    // scroll indicators
    display.setTextColor(SSD1306_WHITE);
    if (top > 0)              display.drawChar(122, 14, 0x18, SSD1306_WHITE, 0, 1);
    if (top + rows < count)   display.drawChar(122, 54, 0x19, SSD1306_WHITE, 0, 1);
    display.display();

    int8_t ev = readEvent();
    if (ev == EV_UP)   { if (sel > 0) sel--; }
    if (ev == EV_DOWN) { if (sel < count - 1) sel++; }
    if (ev == EV_OK)   return sel;
    if (ev == EV_BACK) return -1;
    if (sel < top) top = sel;
    if (sel >= top + rows) top = sel - rows + 1;
  }
}

// Print wrapped text into the body area; simple word-free wrap at char width.
static void drawWrapped(int x, int y, const char* s) {
  const int cw = 6;                       // char width at size 1
  const int maxCols = (SCREEN_WIDTH - x) / cw;
  int col = 0, cy = y;
  for (const char* p = s; *p; p++) {
    if (col >= maxCols) { col = 0; cy += 9; }
    display.setCursor(x + col * cw, cy);
    display.write(*p);
    col++;
  }
}

// ===========================================================================
//  Input: numeric PIN
// ===========================================================================
// Returns "" if cancelled with BACK on an empty buffer.
static String enterPin(const char* title) {
  String buf = "";
  int cur = 0;         // 0..9 current digit
  const int maxLen = 8;
  while (true) {
    display.clearDisplay();
    header(title);
    display.setTextColor(SSD1306_WHITE);

    // masked entered digits
    display.setCursor(4, 20);
    display.setTextSize(2);
    String masked = "";
    for (uint16_t i = 0; i < buf.length(); i++) masked += "*";
    display.print(masked);
    display.setTextSize(1);

    // current digit selector
    display.setCursor(4, 44);
    display.print("Digit: ");
    display.setTextSize(2);
    display.setCursor(56, 40);
    display.print(cur);
    display.setTextSize(1);

    display.setCursor(0, 56);
    display.print("UD:val R:add L:del OK:go");
    display.display();

    int8_t ev = readEvent();
    if (ev == EV_UP)    cur = (cur + 1) % 10;
    if (ev == EV_DOWN)  cur = (cur + 9) % 10;
    if (ev == EV_RIGHT) { if ((int)buf.length() < maxLen) { buf += char('0' + cur); cur = 0; } }
    if (ev == EV_LEFT)  { if (buf.length() > 0) buf.remove(buf.length() - 1); }
    if (ev == EV_OK)    { if (buf.length() >= 4) return buf; toast("PIN too short", "min 4 digits"); display.display(); delay(700); }
    if (ev == EV_BACK)  { return String(""); }
  }
}

// ===========================================================================
//  Input: on-screen QWERTY keyboard
// ===========================================================================
//  Navigation : UP / DOWN / LEFT / RIGHT move the cursor across the keys.
//  Short OK    : type the highlighted key (or run the function key).
//  Short BACK  : backspace.
//  Long OK     : save / confirm the input.
//  Long BACK   : cancel out of the field.
//
//  Three character rows plus a function row. The function row keys are:
//    [cap]  toggle caps for letters
//    [123]  switch between letters and numbers/symbols
//    [spc]  insert a space
//    [del]  backspace
// ---------------------------------------------------------------------------
static const char* KB_LETTERS[3] = { "qwertyuiop", "asdfghjkl", "zxcvbnm" };
static const char* KB_SYMBOLS[3] = { "1234567890", "!@#$%^&*()", "-_=+.,?/:;" };

// Length of the currently active grid row (row 3 is the 4-key function row).
static int kbRowLen(int row, int layer) {
  if (row < 3) return (int)strlen(layer == 0 ? KB_LETTERS[row] : KB_SYMBOLS[row]);
  return 4;
}

// Returns true if the user confirmed (long OK), false if cancelled (long BACK).
static bool getText(const char* title, char* out, uint8_t maxLen) {
  const int ROWS  = 4;
  const int kbTop = 24;
  const int rowH  = 10;

  bool caps = false;
  int  layer = 0;          // 0 = letters, 1 = numbers/symbols
  int  r = 1, c = 0;       // cursor: start on the "asdf" row
  String buf = "";
  bool confirmed = false;

  while (true) {
    // keep the column valid for the current row
    int rlen = kbRowLen(r, layer);
    if (c >= rlen) c = rlen - 1;
    if (c < 0) c = 0;

    display.clearDisplay();
    header(title);
    display.setTextColor(SSD1306_WHITE);

    // typed text: show the tail so the caret stays visible
    display.setCursor(0, 13);
    const int maxCols = SCREEN_WIDTH / 6;      // ~21 chars
    if (buf.length() == 0) {
      display.print("[hold OK = save]");
    } else {
      String shown = buf;
      if ((int)shown.length() > maxCols - 1)
        shown = shown.substring(shown.length() - (maxCols - 1));
      display.print(shown);
      display.print('_');
    }

    // character rows
    for (int row = 0; row < 3; row++) {
      const char* rs = (layer == 0 ? KB_LETTERS[row] : KB_SYMBOLS[row]);
      int len = strlen(rs);
      int cellW = SCREEN_WIDTH / len;
      int y = kbTop + row * rowH;
      for (int k = 0; k < len; k++) {
        int x = k * cellW;
        char ch = rs[k];
        if (layer == 0 && caps && ch >= 'a' && ch <= 'z') ch -= 32;
        bool sel = (row == r && k == c);
        if (sel) {
          display.fillRect(x, y - 1, cellW, rowH, SSD1306_WHITE);
          display.setTextColor(SSD1306_BLACK);
        } else {
          display.setTextColor(SSD1306_WHITE);
        }
        display.setCursor(x + (cellW - 6) / 2, y);
        display.write(ch);
      }
    }

    // function row
    {
      int y = kbTop + 3 * rowH;
      const char* labels[4] = { caps ? "CAP" : "cap",
                                layer == 0 ? "123" : "abc",
                                "spc", "del" };
      int cellW = SCREEN_WIDTH / 4;
      for (int k = 0; k < 4; k++) {
        int x = k * cellW;
        bool sel = (r == 3 && k == c);
        if (sel) {
          display.fillRect(x, y - 1, cellW, rowH, SSD1306_WHITE);
          display.setTextColor(SSD1306_BLACK);
        } else {
          display.setTextColor(SSD1306_WHITE);
        }
        int lw = strlen(labels[k]) * 6;
        display.setCursor(x + (cellW - lw) / 2, y);
        display.print(labels[k]);
      }
    }
    display.display();

    int8_t ev = readEvent();
    if (ev == EV_UP)    r = (r + ROWS - 1) % ROWS;
    if (ev == EV_DOWN)  r = (r + 1) % ROWS;
    if (ev == EV_LEFT)  { int L = kbRowLen(r, layer); c = (c + L - 1) % L; }
    if (ev == EV_RIGHT) { int L = kbRowLen(r, layer); c = (c + 1) % L; }
    if (ev == EV_BACK)  { if (buf.length() > 0) buf.remove(buf.length() - 1); }
    if (ev == EV_OK_LONG)   { confirmed = true;  break; }
    if (ev == EV_BACK_LONG) { confirmed = false; break; }
    if (ev == EV_OK) {
      if (r < 3) {
        const char* rs = (layer == 0 ? KB_LETTERS[r] : KB_SYMBOLS[r]);
        char ch = rs[c];
        if (layer == 0 && caps && ch >= 'a' && ch <= 'z') ch -= 32;
        if ((int)buf.length() < maxLen - 1) buf += ch;
      } else {
        switch (c) {
          case 0: caps = !caps; break;
          case 1: layer ^= 1;   break;
          case 2: if ((int)buf.length() < maxLen - 1) buf += ' '; break;
          case 3: if (buf.length() > 0) buf.remove(buf.length() - 1); break;
        }
      }
    }
  }
  strncpy(out, buf.c_str(), maxLen - 1);
  out[maxLen - 1] = '\0';
  return confirmed;
}

// ===========================================================================
//  Screen: show a generated / stored password with save option
// ===========================================================================
static void showPassword(const char* pass) {
  while (true) {
    display.clearDisplay();
    header("Password");
    display.setTextColor(SSD1306_WHITE);
    drawWrapped(0, 14, pass);
    display.setCursor(0, 56);
    display.print("OK:save  BACK:exit");
    display.display();

    int8_t ev = readEvent();
    if (ev == EV_BACK) return;
    if (ev == EV_OK) {
      Entry e;
      memset(&e, 0, sizeof(e));
      if (!getText("Label", e.label, LBL_LEN)) continue;     // cancelled -> back to pw
      if (!getText("Username", e.user, USR_LEN)) continue;
      strncpy(e.pass, pass, PWD_LEN - 1);
      if (vaultAdd(e)) toast("Saved to vault.");
      else             toast("Save FAILED.");
      display.display();
      waitAnyKey();
      return;
    }
  }
}

// ===========================================================================
//  Screen: generator settings + generate
// ===========================================================================
static void generatorScreen() {
  int sel = 0;
  while (true) {
    display.clearDisplay();
    header("Generate");
    display.setTextColor(SSD1306_WHITE);

    char lenLine[24];
    snprintf(lenLine, sizeof(lenLine), "Length:  %d", g_cfg.length);
    const char* rows[6];
    char b0[24], b1[16], b2[16], b3[16], b4[16];
    strcpy(b0, lenLine);
    snprintf(b1, sizeof(b1), "abc:     %s", g_cfg.useLower   ? "ON" : "off");
    snprintf(b2, sizeof(b2), "ABC:     %s", g_cfg.useUpper   ? "ON" : "off");
    snprintf(b3, sizeof(b3), "123:     %s", g_cfg.useDigits  ? "ON" : "off");
    snprintf(b4, sizeof(b4), "!@#:     %s", g_cfg.useSymbols ? "ON" : "off");
    rows[0] = b0; rows[1] = b1; rows[2] = b2; rows[3] = b3; rows[4] = b4;
    rows[5] = ">> GENERATE <<";

    for (int r = 0; r < 6; r++) {
      int y = 14 + r * 8;
      if (r == sel) {
        display.fillRect(0, y - 1, SCREEN_WIDTH, 8, SSD1306_WHITE);
        display.setTextColor(SSD1306_BLACK);
      } else display.setTextColor(SSD1306_WHITE);
      display.setCursor(2, y);
      display.print(rows[r]);
    }
    display.display();

    int8_t ev = readEvent();
    if (ev == EV_UP)   sel = (sel + 5) % 6;
    if (ev == EV_DOWN) sel = (sel + 1) % 6;
    if (ev == EV_BACK) { cfgSave(); return; }

    if (ev == EV_LEFT || ev == EV_RIGHT) {
      int d = (ev == EV_RIGHT) ? 1 : -1;
      switch (sel) {
        case 0:
          g_cfg.length += d;
          if (g_cfg.length < 8) g_cfg.length = 8;
          if (g_cfg.length > PWD_LEN - 1) g_cfg.length = PWD_LEN - 1;
          break;
        case 1: g_cfg.useLower   = !g_cfg.useLower;   break;
        case 2: g_cfg.useUpper   = !g_cfg.useUpper;   break;
        case 3: g_cfg.useDigits  = !g_cfg.useDigits;  break;
        case 4: g_cfg.useSymbols = !g_cfg.useSymbols; break;
      }
    }
    if (ev == EV_OK) {
      if (sel >= 1 && sel <= 4) {
        // toggle on OK too, for convenience
        switch (sel) {
          case 1: g_cfg.useLower   = !g_cfg.useLower;   break;
          case 2: g_cfg.useUpper   = !g_cfg.useUpper;   break;
          case 3: g_cfg.useDigits  = !g_cfg.useDigits;  break;
          case 4: g_cfg.useSymbols = !g_cfg.useSymbols; break;
        }
      } else if (sel == 5) {
        char pw[PWD_LEN];
        if (generatePassword(g_cfg, pw, PWD_LEN)) {
          cfgSave();
          showPassword(pw);
        } else {
          toast("Enable at least", "one character set");
          display.display();
          waitAnyKey();
        }
      }
    }
  }
}

// ===========================================================================
//  Screen: vault browser
// ===========================================================================
static void viewEntry(uint32_t idx) {
  Entry e;
  if (!vaultLoad(idx, e)) { toast("Read error."); waitAnyKey(); return; }
  int page = 0;   // 0=user, 1=pass
  while (true) {
    display.clearDisplay();
    header(e.label[0] ? e.label : "(no label)");
    display.setTextColor(SSD1306_WHITE);
    if (page == 0) {
      display.setCursor(0, 14); display.print("User:");
      drawWrapped(0, 24, e.user);
    } else {
      display.setCursor(0, 14); display.print("Pass:");
      drawWrapped(0, 24, e.pass);
    }
    display.setCursor(0, 56);
    display.print("LR:page OK:del BACK:x");
    display.display();

    int8_t ev = readEvent();
    if (ev == EV_LEFT || ev == EV_RIGHT) page ^= 1;
    if (ev == EV_BACK) return;
    if (ev == EV_OK) {
      display.clearDisplay();
      header("Delete?");
      display.setTextColor(SSD1306_WHITE);
      display.setCursor(0, 20);
      display.println("Delete this entry?");
      display.setCursor(0, 56);
      display.print("OK:yes  BACK:no");
      display.display();
      int8_t c;
      do { c = readEvent(); } while (c != EV_OK && c != EV_BACK);
      if (c == EV_OK) { vaultDelete(idx); toast("Deleted."); waitAnyKey(); return; }
    }
  }
}

static void vaultScreen() {
  while (true) {
    uint32_t n = vaultCount();
    if (n == 0) { toast("Vault is empty.", "Add or generate."); waitAnyKey(); return; }

    // Build a label list (cap at a reasonable number for RAM).
    const int CAP = 32;
    int shown = (n > CAP) ? CAP : n;
    static char labels[CAP][LBL_LEN + 4];
    const char* items[CAP];
    for (int i = 0; i < shown; i++) {
      Entry e;
      if (vaultLoad(i, e) && e.label[0]) snprintf(labels[i], LBL_LEN + 4, "%s", e.label);
      else snprintf(labels[i], LBL_LEN + 4, "entry %d", i + 1);
      items[i] = labels[i];
    }
    int sel = menu("Vault", items, shown);
    if (sel < 0) return;
    viewEntry(sel);
  }
}

// ===========================================================================
//  Screen: add entry (manual or generated password)
// ===========================================================================
static void addEntryScreen() {
  Entry e;
  memset(&e, 0, sizeof(e));
  if (!getText("Label", e.label, LBL_LEN)) return;      // long BACK cancels add
  if (!getText("Username", e.user, USR_LEN)) return;

  const char* opts[] = { "Generate password", "Type password" };
  int m = menu("Password", opts, 2);
  if (m < 0) return;
  if (m == 0) {
    if (!generatePassword(g_cfg, e.pass, PWD_LEN)) {
      toast("No charset enabled."); waitAnyKey(); return;
    }
  } else {
    if (!getText("Password", e.pass, PWD_LEN)) return;
  }
  if (vaultAdd(e)) toast("Saved to vault.");
  else             toast("Save FAILED.");
  display.display();
  waitAnyKey();
}

// ===========================================================================
//  Screen: settings
// ===========================================================================
static void changePinScreen() {
  String cur = enterPin("Current PIN");
  if (cur.length() == 0) return;
  if (!authVerify(cur)) { toast("Wrong PIN."); waitAnyKey(); return; }

  // Re-encrypt vault under the new key: decrypt all with current key first.
  uint32_t n = vaultCount();
  Entry* buf = nullptr;
  if (n > 0) {
    buf = (Entry*)malloc(sizeof(Entry) * n);
    if (!buf) { toast("Low memory."); waitAnyKey(); return; }
    for (uint32_t i = 0; i < n; i++) vaultLoad(i, buf[i]);
  }

  String np = enterPin("New PIN");
  if (np.length() == 0) { if (buf) free(buf); return; }
  String np2 = enterPin("Repeat PIN");
  if (np2 != np) { toast("PINs differ."); waitAnyKey(); if (buf) free(buf); return; }

  authCreate(np);                       // new salt + key + verifier
  for (uint32_t i = 0; i < n; i++) vaultStoreRaw(i, buf[i]);  // re-encrypt
  if (buf) free(buf);
  toast("PIN changed."); waitAnyKey();
}

static void settingsScreen() {
  const char* opts[] = { "Change PIN", "Lock now", "Factory reset", "About" };
  while (true) {
    int m = menu("Settings", opts, 4);
    if (m < 0) return;
    if (m == 0) changePinScreen();
    if (m == 1) { memset(g_aesKey, 0, KEY_LEN); g_unlocked = false; return; }
    if (m == 2) {
      display.clearDisplay();
      header("Factory reset");
      display.setTextColor(SSD1306_WHITE);
      display.setCursor(0, 16);
      display.println("Erase PIN AND all");
      display.println("stored passwords?");
      display.println("This cannot be undone.");
      display.setCursor(0, 56);
      display.print("OK:erase BACK:cancel");
      display.display();
      int8_t c; do { c = readEvent(); } while (c != EV_OK && c != EV_BACK);
      if (c == EV_OK) { factoryReset(); toast("Erased."); waitAnyKey(); return; }
    }
    if (m == 3) {
      display.clearDisplay();
      header("About");
      display.setTextColor(SSD1306_WHITE);
      display.setCursor(0, 14);
      display.println("PIGEON");
      display.println("Offline pw vault");
      display.println("AES-256 + PBKDF2");
      display.println("XIAO ESP32-C3");
      display.display();
      waitAnyKey();
    }
  }
}

// ===========================================================================
//  Unlock / first-run flow
// ===========================================================================
static void firstRunSetup() {
  toast("Welcome to PIGEON", "Set a master PIN");
  display.display();
  waitAnyKey();
  while (true) {
    String p1 = enterPin("New PIN");
    if (p1.length() == 0) continue;
    String p2 = enterPin("Repeat PIN");
    if (p2 != p1) { toast("PINs differ.", "Try again."); waitAnyKey(); continue; }
    authCreate(p1);
    g_unlocked = true;
    toast("PIN set. Ready."); waitAnyKey();
    return;
  }
}

static void unlockFlow() {
  if (!authExists()) { firstRunSetup(); return; }
  int attempts = 0;
  while (true) {
    String pin = enterPin("Enter PIN");
    if (pin.length() == 0) continue;   // BACK just re-prompts
    if (authVerify(pin)) { g_unlocked = true; return; }
    attempts++;
    char l2[24];
    snprintf(l2, sizeof(l2), "Attempts: %d", attempts);
    toast("Wrong PIN.", l2);
    display.display();
    // brief lockout that grows with attempts, to slow brute force
    delay(600 + (uint32_t)attempts * 400);
    waitAnyKey();
  }
}

// ===========================================================================
//  Main menu
// ===========================================================================
static void mainMenu() {
  const char* opts[] = { "Generate", "Vault", "Add entry", "Settings", "Lock" };
  int m = menu("PIGEON", opts, 5);
  if (m < 0) return;
  switch (m) {
    case 0: generatorScreen(); break;
    case 1: vaultScreen();     break;
    case 2: addEntryScreen();  break;
    case 3: settingsScreen();  break;
    case 4: memset(g_aesKey, 0, KEY_LEN); g_unlocked = false; break;
  }
}

// ===========================================================================
//  Arduino entry points
// ===========================================================================
void setup() {
  for (int i = 0; i < 6; i++) pinMode(kBtnPins[i], INPUT_PULLUP);

  Wire.begin();                 // XIAO ESP32-C3 default SDA=D4, SCL=D5
  if (!display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR)) {
    // No display: nothing useful to do; halt.
    while (true) { delay(1000); }
  }
  display.setRotation(2);       // OLED soldered upside down -> flip 180 degrees
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(24, 26);
  display.print("PIGEON");
  display.display();
  delay(900);

  cfgLoad();
}

void loop() {
  if (!g_unlocked) { unlockFlow(); return; }
  mainMenu();
}
