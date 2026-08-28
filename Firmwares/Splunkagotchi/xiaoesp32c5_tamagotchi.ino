// =============================================================================
//  WarPet  -  a dual-band Wi-Fi + BLE wardriving rig that is also a Tamagotchi
//  Target : Seeed Studio XIAO ESP32-C5  (2.4G + 5G Wi-Fi 6, BLE 5)
//  Display: 0.96" SSD1306 128x64 OLED (I2C)
//  Input  : 6 buttons (UP/DOWN/OK/BACK/LEFT/RIGHT)
//
//  The creature earns XP from every UNIQUE discovery: Wi-Fi APs (2.4/5 GHz),
//  BLE devices, and Wi-Fi client stations harvested by the passive monitor.
//  Captures are logged to an on-device ring buffer (oldest overwritten when
//  full). Home-Wi-Fi + Wigle credentials are configured over USB serial and
//  kept in NVS for a future Wigle uploader (currently disabled: needs GPS).
//
//  SCOPE / SAFETY: this firmware is PASSIVE. It listens to broadcast beacons
//  and advertisements and, in monitor mode, observes over-the-air 802.11
//  frames to count packets and collect device MAC addresses. It does NOT
//  transmit deauthentication frames, inject packets, connect to foreign
//  networks, or capture payloads. There is intentionally no deauth attack
//  capability. Operate only where passive monitoring is lawful.
//
//  Libraries: Adafruit SSD1306, Adafruit GFX, NimBLE-Arduino (v2.x).
//  Board support: esp32 by Espressif Systems, v3.1.0+ (C5 + 5 GHz + monitor).
// =============================================================================

#include "config.h"      // defines feature toggles used by the guards below

#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <Fonts/Org_01.h>
#include <WiFi.h>
#include <Preferences.h>
#include <LittleFS.h>
#include <esp_wifi.h>
#include <esp_random.h>
#include <set>
#include <map>
#include <string>
#include <math.h>

#if ENABLE_BLE
  #include <NimBLEDevice.h>
#endif

#include "names.h"
#include "sprites.h"

// =============================================================================
//  Types  (defined before any function so the Arduino auto-prototype pass,
//  which hoists prototypes above the first function, can see them).
// =============================================================================
enum Stage  { EGG = 0, BABY, TEEN, ADULT };
enum Screen { SC_HOME, SC_STATS, SC_LOG, SC_MENU };
enum Mode   { MODE_SCAN, MODE_MONITOR, MODE_PROMISC };
enum ScanState {
  S_IDLE, S_W2G_START, S_W2G_WAIT, S_W5G_START, S_W5G_WAIT,
  S_BLE_START, S_BLE_WAIT, S_COOLDOWN
};

// Capture record type tags.
enum RecType { RT_AP2G = 0, RT_AP5G = 1, RT_BLE = 2, RT_CLIENT = 3 };

struct __attribute__((packed)) Rec {
  uint32_t ts;         // seconds since boot
  uint8_t  type;       // RecType
  uint8_t  mac[6];
  int8_t   rssi;
  uint8_t  channel;
  char     ssid[24];   // AP name / BLE name (truncated); empty for clients
};

struct Button {
  uint8_t  pin;
  bool     last      = false;
  bool     pressedEv = false;
  uint32_t tChange   = 0;
  uint32_t tRepeat   = 0;
};

struct CliObs { int8_t rssi; uint8_t ch; };  // staged client observation
struct MItem  { const char* label; int id; };  // one menu row

// =============================================================================
//  Globals
// =============================================================================
Adafruit_SSD1306 oled(OLED_WIDTH, OLED_HEIGHT, &Wire, -1);
Preferences      prefs;

// Uniqueness sets (MAC packed into uint64).
std::set<uint64_t> seenAP, seenBLE, seenClient;

// Pet / save state.
struct Pet {
  char     name[16] = "";
  uint32_t xp       = 0;     // XP into the current level
  uint16_t level    = 0;     // 0 = egg
  uint8_t  hunger   = 60;    // SATIATION: 100 = well fed, 0 = starving
  uint8_t  energy   = 80;
  uint8_t  petComp  = 50;    // "pet" component of mood (0..100)
  uint32_t bornMs   = 0;
  uint32_t ageSec   = 0;     // accumulated powered-on lifetime, in seconds
} pet;

int      activity = 30;      // recent-discovery component of mood (0..100)
uint32_t uniqueAP = 0, uniqueBLE = 0, uniqueClient = 0;   // NVS backups

// Lifetime packet counter (pktCount is session-only; this persists the total).
uint32_t lifetimePkts  = 0;  // packets counted before this session
uint32_t lastSavedPkts = 0;  // session marker used to roll pktCount into the total

// Runtime config (from serial, stored in NVS namespace "cfg").
String cfgHomeSSID, cfgHomePass, cfgWigleName, cfgWigleToken;

// UI / mode.
Screen   screen = SC_HOME;
Mode     mode   = MODE_SCAN;
bool     scanPaused = false;
int      menuIdx = 0, menuTop = 0;
bool     confirmReset = false;
String   bubble; uint32_t bubbleUntil = 0;
bool     levelUpFlag = false;
const char* bandLabel = "--";

// Scanner state.
ScanState scanState = S_IDLE;
uint32_t  scanTimer = 0;
uint32_t  lastNew2G = 0, lastNew5G = 0, lastNewBLE = 0, lastNewClient = 0;

// Monitor engine.
volatile uint32_t pktCount = 0;        // frames seen (monitor)
uint32_t pktBatchMark = 0;
uint8_t  monChannel = MON_CH_MIN;
uint32_t monHopTimer = 0, monEnergyTimer = 0;
SemaphoreHandle_t monMutex = nullptr;
std::map<uint64_t, CliObs> cliStaging; // filled by promisc cb, drained in loop

// Storage ring.
File     storeFile;
uint32_t storeHead = 0, storeCount = 0;
const uint32_t RECORD_SIZE = sizeof(Rec);

// =============================================================================
//  Small helpers
// =============================================================================
static uint64_t macToU64(const uint8_t* m) {
  uint64_t v = 0;
  for (int i = 0; i < 6; i++) v = (v << 8) | m[i];
  return v;
}
static void u64ToMac(uint64_t v, uint8_t* m) {
  for (int i = 5; i >= 0; i--) { m[i] = v & 0xFF; v >>= 8; }
}
static uint64_t macStrToU64(const std::string& s) {
  uint8_t b[6] = {0}; int idx = 0; unsigned int hi, lo;
  const char* p = s.c_str();
  while (*p && idx < 6) {
    if (sscanf(p, "%1x%1x", &hi, &lo) == 2) { b[idx++] = (uint8_t)((hi << 4) | lo); p += 2; }
    if (*p == ':') p++; else if (*p == '\0') break; else p++;
  }
  return macToU64(b);
}

static uint32_t xpToNext(uint16_t level) {
  double v = (double)XP_LEVEL_BASE * pow((double)XP_LEVEL_GROWTH, (double)level);
  if (v > 4.0e9) return 0xFFFFFFFFUL;
  return (uint32_t)(v + 0.5);
}

static Stage stageForLevel(uint16_t lvl) {
  if (lvl < LVL_BABY) return EGG;    // one stage per 5 levels
  if (lvl < LVL_TEEN) return BABY;
  if (lvl < LVL_ADULT) return TEEN;
  return ADULT;
}
static const char* stageName(Stage s) {
  switch (s) {
    case EGG:  return "Egg";
    case BABY: return "Baby";
    case TEEN: return "Teen";
    default:   return "Adult";
  }
}
static bool monitorUnlocked()  { return pet.level >= LVL_TEEN; }
static bool promiscUnlocked()  { return pet.level >= LVL_ADULT; }

static uint32_t lifetimePktTotal() { return lifetimePkts + (pktCount - lastSavedPkts); }

// Age shown in the largest sensible unit: m -> h -> d -> mo -> y.
static String formatAge(uint32_t sec) {
  uint32_t mins = sec / 60;
  if (mins < 60) return String(mins) + "m";
  uint32_t hours = mins / 60;
  if (hours < 24) return String(hours) + "h";
  uint32_t days = hours / 24;
  if (days < 30) return String(days) + "d";
  uint32_t months = days / 30;
  if (months < 12) return String(months) + "mo";
  return String(months / 12) + "y";
}

void say(const char* s, uint32_t ms = 2200) { bubble = s; bubbleUntil = millis() + ms; }

// Forward declarations for functions referenced across #if boundaries.
void setMode(Mode m);
void onDiscovery(uint32_t n);
void addXP(uint32_t amount);

// =============================================================================
//  Config store (NVS) + serial provisioning
// =============================================================================
void loadConfig() {
  prefs.begin("cfg", true);
  cfgHomeSSID   = prefs.getString("ssid", "");
  cfgHomePass   = prefs.getString("pass", "");
  cfgWigleName  = prefs.getString("wname", "");
  cfgWigleToken = prefs.getString("wtok", "");
  prefs.end();
}
void saveConfig() {
  prefs.begin("cfg", false);
  prefs.putString("ssid", cfgHomeSSID);
  prefs.putString("pass", cfgHomePass);
  prefs.putString("wname", cfgWigleName);
  prefs.putString("wtok", cfgWigleToken);
  prefs.end();
}

// =============================================================================
//  Name generator + pet persistence
// =============================================================================
void generateName() {
  // Unique-per-device random name from hardware entropy, made once, then kept.
  uint32_t a = esp_random();
  snprintf(pet.name, sizeof(pet.name), "%s-%s",
           NAME_ADJ[a % NAME_ADJ_N],
           NAME_NOUN[(a / 31) % NAME_NOUN_N]);
}

void saveState() {
  // Roll this session's packet delta into the persisted lifetime total.
  lifetimePkts += (pktCount - lastSavedPkts);
  lastSavedPkts = pktCount;
  prefs.begin("warpet", false);
  prefs.putBytes("pet", &pet, sizeof(pet));
  prefs.putInt("act", activity);
  prefs.putUInt("uAP",  (uint32_t)seenAP.size());
  prefs.putUInt("uBLE", (uint32_t)seenBLE.size());
  prefs.putUInt("uCL",  (uint32_t)seenClient.size());
  prefs.putUInt("shead", storeHead);
  prefs.putUInt("scount", storeCount);
  prefs.putUInt("lpkts", lifetimePkts);
  prefs.end();
}
void loadState() {
  prefs.begin("warpet", true);
  size_t n = prefs.getBytesLength("pet");
  if (n == sizeof(pet)) prefs.getBytes("pet", &pet, sizeof(pet));
  activity     = prefs.getInt("act", 30);
  uniqueAP     = prefs.getUInt("uAP", 0);
  uniqueBLE    = prefs.getUInt("uBLE", 0);
  uniqueClient = prefs.getUInt("uCL", 0);
  storeHead    = prefs.getUInt("shead", 0);
  storeCount   = prefs.getUInt("scount", 0);
  lifetimePkts = prefs.getUInt("lpkts", 0);
  lastSavedPkts = 0;                       // pktCount restarts at 0 this session
  prefs.end();
  if (pet.bornMs == 0)  pet.bornMs = millis();
  if (pet.name[0] == 0) { generateName(); saveState(); }
}

// =============================================================================
//  Uniqueness set persistence (so XP isn't re-awarded after a reboot)
// =============================================================================
void saveSeenSet(const char* path, const std::set<uint64_t>& s) {
  File f = LittleFS.open(path, "w");
  if (!f) return;
  for (uint64_t v : s) f.write((uint8_t*)&v, sizeof(v));
  f.close();
}
void loadSeenSet(const char* path, std::set<uint64_t>& s, uint32_t cap) {
  File f = LittleFS.open(path, "r");
  if (!f) return;
  uint64_t v;
  while (f.read((uint8_t*)&v, sizeof(v)) == sizeof(v)) {
    if (s.size() >= cap) break;
    s.insert(v);
  }
  f.close();
}

// =============================================================================
//  Capture ring buffer (fixed-capacity; overwrites oldest when full)
// =============================================================================
void storageInit() {
  if (LittleFS.exists(STORE_PATH)) storeFile = LittleFS.open(STORE_PATH, "r+");
  else                            storeFile = LittleFS.open(STORE_PATH, "w+");
  if (!storeFile) Serial.println(F("storage: open failed"));
}
void storageAppend(const Rec& r) {
  if (!storeFile) return;
  storeFile.seek((uint32_t)storeHead * RECORD_SIZE, SeekSet);
  storeFile.write((const uint8_t*)&r, RECORD_SIZE);
  storeHead  = (storeHead + 1) % RECORD_CAP;
  if (storeCount < RECORD_CAP) storeCount++;   // else oldest just got overwritten
}
void storageFlush() { if (storeFile) storeFile.flush(); }
void storageClear() {
  if (storeFile) storeFile.close();
  LittleFS.remove(STORE_PATH);
  storeHead = storeCount = 0;
  storageInit();
}
// Log a discovery. Builds a Rec and appends it.
void logCapture(RecType type, const uint8_t* mac, int8_t rssi, uint8_t ch,
                const char* ssid) {
  Rec r{};
  r.ts = millis() / 1000;
  r.type = (uint8_t)type;
  memcpy(r.mac, mac, 6);
  r.rssi = rssi;
  r.channel = ch;
  if (ssid) { strncpy(r.ssid, ssid, sizeof(r.ssid) - 1); }
  storageAppend(r);
}

// =============================================================================
//  Creature logic
// =============================================================================
void addXP(uint32_t amount) {
  pet.xp += amount;
  while (pet.xp >= xpToNext(pet.level)) {
    pet.xp -= xpToNext(pet.level);
    pet.level++;
    pet.petComp = min(100, pet.petComp + 5);
    levelUpFlag = true;
  }
}

static int energyRate(uint8_t hunger) {          // returns x100 multiplier
  if (hunger < 25) return ENERGY_RATE_Q1;
  if (hunger < 50) return ENERGY_RATE_Q2;
  if (hunger < 75) return ENERGY_RATE_Q3;
  return ENERGY_RATE_Q4;
}

// Called when N new devices are discovered: feeds hunger + energy + activity.
void onDiscovery(uint32_t n) {
  if (!n) return;
  uint32_t h = (uint32_t)pet.hunger + (uint32_t)HUNGER_PER_DISCOVERY * n;
  pet.hunger = (h > 100) ? 100 : (uint8_t)h;
  int rate = energyRate(pet.hunger);
  uint32_t e = (uint32_t)pet.energy + ((uint32_t)ENERGY_PER_DISCOVERY * n * rate) / 100;
  pet.energy = (e > 100) ? 100 : (uint8_t)e;
  activity += (int)ACT_BUMP; if (activity > 100) activity = 100;
  if ((esp_random() & 0x7) == 0) say(PHRASE_PICK(SAY_DISCOVERY), 1400);
}

void statTick() {   // periodic decay
  pet.hunger  = (pet.hunger  > HUNGER_DECAY_PER_TICK) ? pet.hunger  - HUNGER_DECAY_PER_TICK : 0;
  pet.petComp = (pet.petComp > PET_DECAY_PER_TICK)    ? pet.petComp - PET_DECAY_PER_TICK    : 0;
  activity    = (activity    > ACT_DECAY_PER_TICK)    ? activity    - ACT_DECAY_PER_TICK    : 0;
  if (pet.hunger < 15 && (esp_random() & 1)) say(PHRASE_PICK(SAY_HUNGRY), 1600);
}

int moodValue() {                     // 0..100 ; 25% pet, 75% discovery/hunger
  int disc = (pet.hunger + activity) / 2;
  return (25 * pet.petComp + 75 * disc) / 100;
}
bool isSad() { return moodValue() < MOOD_SAD_BELOW; }

// =============================================================================
//  BLE scanning
// =============================================================================
#if ENABLE_BLE
struct BleObs { uint64_t mac; int8_t rssi; char name[24]; };
SemaphoreHandle_t  bleMutex = nullptr;
std::map<uint64_t, BleObs> bleStaging;

class ScanCB : public NimBLEScanCallbacks {
  void onResult(const NimBLEAdvertisedDevice* dev) override {
    BleObs o{}; o.mac = macStrToU64(dev->getAddress().toString());
    o.rssi = dev->getRSSI();
    std::string nm = dev->getName();
    strncpy(o.name, nm.c_str(), sizeof(o.name) - 1);
    if (bleMutex && xSemaphoreTake(bleMutex, pdMS_TO_TICKS(15)) == pdTRUE) {
      bleStaging[o.mac] = o;
      xSemaphoreGive(bleMutex);
    }
  }
};
ScanCB      bleCB;
NimBLEScan* pBLEScan = nullptr;
#endif

// =============================================================================
//  Wi-Fi scan harvesting
// =============================================================================
uint32_t harvestWiFi(RecType type, const char* band) {
  int n = WiFi.scanComplete();
  if (n < 0) return 0;
  uint32_t added = 0;
  for (int i = 0; i < n; i++) {
    if (seenAP.size() >= MAX_SEEN_AP) break;
    uint8_t mac[6]; memcpy(mac, WiFi.BSSID(i), 6);
    uint64_t bssid = macToU64(mac);
    if (seenAP.insert(bssid).second) {
      added++;
      String ssid = WiFi.SSID(i);
      logCapture(type, mac, (int8_t)WiFi.RSSI(i), (uint8_t)WiFi.channel(i),
                 ssid.length() ? ssid.c_str() : "");
    }
  }
  WiFi.scanDelete();
  return added;
}
void startWiFiScan() { WiFi.scanNetworks(true, SCAN_SHOW_HIDDEN, false, WIFI_SCAN_MS); }

void serviceScanner() {
  if (mode != MODE_SCAN || scanPaused) { scanState = S_IDLE; return; }
  switch (scanState) {
    case S_IDLE: scanState = S_W2G_START; break;

    case S_W2G_START:
      WiFi.setBandMode(WIFI_BAND_MODE_2G_ONLY);
      startWiFiScan(); bandLabel = "2.4G"; scanState = S_W2G_WAIT; break;
    case S_W2G_WAIT: {
      int r = WiFi.scanComplete();
      if (r == WIFI_SCAN_RUNNING) break;
      if (r >= 0) { uint32_t a = harvestWiFi(RT_AP2G, "2.4G"); lastNew2G = a;
                    if (a) { addXP(a * XP_PER_NEW_AP); onDiscovery(a); } }
      else WiFi.scanDelete();
#if ENABLE_5G
      scanState = S_W5G_START;
#elif ENABLE_BLE
      scanState = S_BLE_START;
#else
      scanState = S_COOLDOWN; scanTimer = millis();
#endif
      break; }

#if ENABLE_5G
    case S_W5G_START:
      WiFi.setBandMode(WIFI_BAND_MODE_5G_ONLY);
      startWiFiScan(); bandLabel = "5G"; scanState = S_W5G_WAIT; break;
    case S_W5G_WAIT: {
      int r = WiFi.scanComplete();
      if (r == WIFI_SCAN_RUNNING) break;
      if (r >= 0) { uint32_t a = harvestWiFi(RT_AP5G, "5G"); lastNew5G = a;
                    if (a) { addXP(a * XP_PER_NEW_AP); onDiscovery(a); } }
      else WiFi.scanDelete();
      WiFi.setBandMode(WIFI_BAND_MODE_AUTO);
  #if ENABLE_BLE
      scanState = S_BLE_START;
  #else
      scanState = S_COOLDOWN; scanTimer = millis();
  #endif
      break; }
#endif

#if ENABLE_BLE
    case S_BLE_START:
      if (pBLEScan && !pBLEScan->isScanning()) pBLEScan->start(BLE_SCAN_MS);
      bandLabel = "BT"; scanTimer = millis(); scanState = S_BLE_WAIT; break;
    case S_BLE_WAIT:
      if (millis() - scanTimer >= (uint32_t)BLE_SCAN_MS + 150) {
        std::map<uint64_t, BleObs> batch;
        if (bleMutex && xSemaphoreTake(bleMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
          batch.swap(bleStaging); xSemaphoreGive(bleMutex);
        }
        uint32_t added = 0;
        for (auto& kv : batch) {
          if (seenBLE.size() >= MAX_SEEN_BLE) break;
          if (seenBLE.insert(kv.first).second) {
            added++;
            uint8_t mac[6]; u64ToMac(kv.first, mac);
            logCapture(RT_BLE, mac, kv.second.rssi, 0, kv.second.name);
          }
        }
        if (pBLEScan) pBLEScan->clearResults();
        lastNewBLE = added;
        if (added) { addXP(added * XP_PER_NEW_BLE); onDiscovery(added); }
        scanState = S_COOLDOWN; scanTimer = millis();
      }
      break;
#endif
    case S_COOLDOWN:
      if (millis() - scanTimer >= 400) scanState = S_IDLE; break;
    default: scanState = S_IDLE; break;
  }
}

// =============================================================================
//  Passive monitor engine (promiscuous)  -  observation only, no injection
// =============================================================================
#if ENABLE_MONITOR
// Runs in the Wi-Fi task context (not an ISR). Keep it short and non-blocking.
void promiscCb(void* buf, wifi_promiscuous_pkt_type_t type) {
  pktCount++;
  const wifi_promiscuous_pkt_t* p = (const wifi_promiscuous_pkt_t*)buf;
  if (p->rx_ctrl.sig_len < 16) return;           // too short to hold addr2
  const uint8_t* pl = p->payload;
  uint8_t ftype = (pl[0] >> 2) & 0x3;            // 0=mgmt, 2=data
  if (ftype != 0 && ftype != 2) return;
  uint64_t mac = macToU64(pl + 10);              // addr2 = transmitter
  if (mac == 0) return;
  if (monMutex && xSemaphoreTake(monMutex, 0) == pdTRUE) {  // never block cb
    if (cliStaging.size() < 256) cliStaging[mac] = { p->rx_ctrl.rssi, (uint8_t)p->rx_ctrl.channel };
    xSemaphoreGive(monMutex);
  }
}

void enterMonitor() {
#if ENABLE_BLE
  if (pBLEScan && pBLEScan->isScanning()) pBLEScan->stop();
#endif
  WiFi.scanDelete();
  wifi_promiscuous_filter_t filt;
  filt.filter_mask = WIFI_PROMIS_FILTER_MASK_MGMT | WIFI_PROMIS_FILTER_MASK_DATA;
  esp_wifi_set_promiscuous(false);
  esp_wifi_set_promiscuous_filter(&filt);
  esp_wifi_set_promiscuous_rx_cb(&promiscCb);
  esp_wifi_set_promiscuous(true);
  monChannel = MON_CH_MIN;
  esp_wifi_set_channel(monChannel, WIFI_SECOND_CHAN_NONE);
  monHopTimer = monEnergyTimer = millis();
  pktBatchMark = pktCount;
}
void exitMonitor() {
  esp_wifi_set_promiscuous(false);
  WiFi.setBandMode(WIFI_BAND_MODE_AUTO);
}

void serviceMonitor() {
  if (mode == MODE_SCAN) return;
  uint32_t now = millis();

  // Channel hop (faster in aggressive promiscuous harvest).
  uint32_t hop = (mode == MODE_PROMISC) ? (MON_HOP_MS / 2) : MON_HOP_MS;
  if (now - monHopTimer >= hop) {
    monHopTimer = now;
    monChannel++; if (monChannel > MON_CH_MAX) monChannel = MON_CH_MIN;
    esp_wifi_set_channel(monChannel, WIFI_SECOND_CHAN_NONE);
  }

  // XP per batch of frames observed.
  uint32_t pk = pktCount;
  while (pk - pktBatchMark >= PKT_BATCH_SIZE) {
    pktBatchMark += PKT_BATCH_SIZE;
    addXP(XP_PER_PKT_BATCH);
  }

  // Drain harvested client MACs.
  std::map<uint64_t, CliObs> batch;
  if (monMutex && xSemaphoreTake(monMutex, pdMS_TO_TICKS(20)) == pdTRUE) {
    batch.swap(cliStaging); xSemaphoreGive(monMutex);
  }
  uint32_t added = 0;
  for (auto& kv : batch) {
    if (seenClient.size() >= MAX_SEEN_CLIENT) break;
    if (seenAP.count(kv.first)) continue;        // it's a known AP, not a client
    if (seenClient.insert(kv.first).second) {
      added++;
      uint8_t mac[6]; u64ToMac(kv.first, mac);
      logCapture(RT_CLIENT, mac, kv.second.rssi, kv.second.ch, "");
    }
  }
  if (added) { lastNewClient = added; addXP(added * XP_PER_NEW_CLIENT); onDiscovery(added); }

  // Energy drain (1% per interval). Auto-sleep back to scan if exhausted.
  if (now - monEnergyTimer >= MON_ENERGY_MS) {
    monEnergyTimer = now;
    if (pet.energy > 0) pet.energy--;
    if (pet.energy == 0) { setMode(MODE_SCAN); say(PHRASE_PICK(SAY_SLEEPY)); return; }
  }
  if (mode != MODE_SCAN) bandLabel = (mode == MODE_PROMISC) ? "PROM" : "MON";
}
#endif // ENABLE_MONITOR

// Central mode switch.
void setMode(Mode m) {
#if ENABLE_MONITOR
  bool wasMon = (mode != MODE_SCAN);
  bool willMon = (m != MODE_SCAN);
  mode = m;
  if (willMon && !wasMon) enterMonitor();
  else if (!willMon && wasMon) exitMonitor();
  else if (willMon && wasMon) { /* switching monitor<->promisc: keep radio */ }
#else
  mode = MODE_SCAN;
#endif
  if (m == MODE_SCAN) { scanState = S_IDLE; bandLabel = scanPaused ? "off" : "--"; }
}

// =============================================================================
//  Buttons
// =============================================================================
Button bUp{PIN_BTN_UP}, bDown{PIN_BTN_DOWN}, bOk{PIN_BTN_OK},
       bBack{PIN_BTN_BACK}, bLeft{PIN_BTN_LEFT}, bRight{PIN_BTN_RIGHT};

const uint32_t DEBOUNCE_MS = 25, REPEAT_DELAY = 420, REPEAT_RATE = 120;

void pollButton(Button& b, bool allowRepeat) {
  b.pressedEv = false;
  bool raw = (digitalRead(b.pin) == LOW);
  uint32_t now = millis();
  if (raw != b.last && (now - b.tChange) > DEBOUNCE_MS) {
    b.last = raw; b.tChange = now;
    if (raw) { b.pressedEv = true; b.tRepeat = now; }
  } else if (raw && b.last && allowRepeat) {
    if ((now - b.tChange) > REPEAT_DELAY && (now - b.tRepeat) > REPEAT_RATE) {
      b.pressedEv = true; b.tRepeat = now;
    }
  }
}
void pollButtons() {
  pollButton(bUp, true);   pollButton(bDown, true);
  pollButton(bOk, false);  pollButton(bBack, false);
  pollButton(bLeft, false); pollButton(bRight, false);
}

// =============================================================================
//  Actions
// =============================================================================
void doFeed() { pet.hunger = min(100, pet.hunger + 25); say(PHRASE_PICK(SAY_FED)); }
void doPlay() {
  if (pet.energy >= PLAY_ENERGY_COST) {
    pet.energy -= PLAY_ENERGY_COST;
    pet.petComp = min(100, pet.petComp + 12);
    activity = min(100, activity + 12);
    say(PHRASE_PICK(SAY_PLAY));
  } else say(PHRASE_PICK(SAY_SLEEPY));
}
void doPet()  { pet.petComp = min(100, pet.petComp + PET_BOOST); say(PHRASE_PICK(SAY_PET)); }
void doResetPet() {
  Pet fresh; pet = fresh; generateName(); pet.bornMs = millis();
  activity = 30;
  lifetimePkts = 0; lastSavedPkts = pktCount;   // zero the lifetime packet total
  seenAP.clear(); seenBLE.clear(); seenClient.clear();
  LittleFS.remove(SEEN_PATH_AP); LittleFS.remove(SEEN_PATH_BLE); LittleFS.remove(SEEN_PATH_CLIENT);
  storageClear(); saveState();
  say("reborn!", 1800);
}

// =============================================================================
//  UI
// =============================================================================
// ---- Ellipse helpers (Adafruit_GFX has none) --------------------------------
void gfxDrawEllipse(int16_t xc, int16_t yc, int16_t rx, int16_t ry, uint16_t color) {
  if (rx < 0 || ry < 0) return;
  if (rx == 0) { oled.drawFastVLine(xc, yc - ry, 2 * ry + 1, color); return; }
  if (ry == 0) { oled.drawFastHLine(xc - rx, yc, 2 * rx + 1, color); return; }
  int32_t rx2 = (int32_t)rx * rx, ry2 = (int32_t)ry * ry;
  int32_t x = 0, y = ry, px = 0, py = 2 * rx2 * y;
  #define PLOT4(X, Y) do { oled.drawPixel(xc + (X), yc + (Y), color); \
    oled.drawPixel(xc - (X), yc + (Y), color); \
    oled.drawPixel(xc + (X), yc - (Y), color); \
    oled.drawPixel(xc - (X), yc - (Y), color); } while (0)
  int32_t p = ry2 - rx2 * ry + rx2 / 4;             // region 1
  while (px < py) {
    PLOT4(x, y); x++; px += 2 * ry2;
    if (p < 0) p += ry2 + px;
    else { y--; py -= 2 * rx2; p += ry2 + px - py; }
  }
  p = ry2 * (2 * x + 1) * (2 * x + 1) / 4 + rx2 * (y - 1) * (y - 1) - rx2 * ry2;  // region 2
  while (y >= 0) {
    PLOT4(x, y); y--; py -= 2 * rx2;
    if (p > 0) p += rx2 - py;
    else { x++; px += 2 * ry2; p += rx2 - py + px; }
  }
  #undef PLOT4
}
void gfxFillEllipse(int16_t xc, int16_t yc, int16_t rx, int16_t ry, uint16_t color) {
  if (rx < 0 || ry < 0) return;
  for (int16_t dy = -ry; dy <= ry; dy++) {
    float t = 1.0f - (float)(dy * dy) / (float)(ry * ry);
    if (t < 0) t = 0;
    int16_t dx = (int16_t)(rx * sqrtf(t) + 0.5f);
    oled.drawFastHLine(xc - dx, yc + dy, 2 * dx + 1, color);
  }
}

// ---- Creature stages (bitmaps from Lopaka; +dy gives the idle bob) ----------
void drawCreatureEgg(int dy) {
  // Shifted down (+18) so the egg shares the same on-screen height as the
  // baby/teen/adult bodies instead of floating up near the level bar.
  gfxDrawEllipse(85, 41 + dy, 16, 19, SSD1306_WHITE);
  gfxFillEllipse(95, 55 + dy, 1, 1, SSD1306_WHITE);
  gfxFillEllipse(77, 56 + dy, 1, 1, SSD1306_WHITE);
  gfxFillEllipse(90, 46 + dy, 2, 3, SSD1306_WHITE);
  gfxFillEllipse(82, 46 + dy, 1, 1, SSD1306_WHITE);
}
void drawCreatureBaby(int dy) {
  oled.drawBitmap(73, 25 + dy, baby_body_bits, 26, 27, SSD1306_WHITE);
  oled.drawBitmap(70, 40 + dy, baby_mid_bits, 32, 6, SSD1306_WHITE);
  oled.fillCircle(80, 33 + dy, 2, SSD1306_WHITE);
  oled.fillCircle(89, 32 + dy, 3, SSD1306_WHITE);
  oled.drawBitmap(69, 40 + dy, baby_low_bits, 33, 21, SSD1306_WHITE);
}
void drawCreatureTeen(int dy) {
  oled.drawBitmap(74, 25 + dy, teen_body_bits, 41, 38, SSD1306_WHITE);
  oled.fillCircle(80, 33 + dy, 2, SSD1306_WHITE);
  oled.fillCircle(89, 32 + dy, 3, SSD1306_WHITE);
}
void drawCreatureAdult(int dy) {
  oled.fillCircle(80, 33 + dy, 2, SSD1306_WHITE);
  oled.fillCircle(89, 32 + dy, 3, SSD1306_WHITE);
  oled.drawBitmap(72, 22 + dy, adult_body_bits, 49, 41, SSD1306_WHITE);
}
void drawCreature() {
  // Two-frame idle animation: swap frame every 1s (frame 1 bobs up 1px).
  int dy = ((millis() / 1000) % 2) ? -1 : 0;
  switch (stageForLevel(pet.level)) {
    case EGG:  drawCreatureEgg(dy);  break;
    case BABY: drawCreatureBaby(dy); break;
    case TEEN: drawCreatureTeen(dy); break;
    default:   drawCreatureAdult(dy); break;
  }
  if (pet.energy < 15) oled.drawChar(118, 20, 'z', SSD1306_WHITE, SSD1306_BLACK, 1);
}

void drawBar(int x, int y, int w, int h, int pct) {
  oled.drawRect(x, y, w, h, SSD1306_WHITE);
  int fill = (int)((w - 2) * (constrain(pct, 0, 100) / 100.0));
  if (fill > 0) oled.fillRect(x + 1, y + 1, fill, h - 2, SSD1306_WHITE);
}
void drawHomeHUD() {
  oled.setFont();                       // default 6x8 font
  oled.setTextSize(1); oled.setTextColor(SSD1306_WHITE); oled.setTextWrap(false);

  // Level
  oled.setCursor(3, 3);  oled.print("L");
  oled.setCursor(10, 3); oled.print(pet.level);

  // Status dot (centered vertically with the label) + current band being
  // scanned (2.4G / 5G / BT / MON...). Dot blinks while actively scanning.
  if (mode != MODE_SCAN || !scanPaused) {
    bool on = (millis() / 400) % 2;
    if (on) oled.fillCircle(90, 6, 3, SSD1306_WHITE);
    else    oled.drawCircle(90, 6, 3, SSD1306_WHITE);
  } else {
    oled.drawCircle(90, 6, 3, SSD1306_WHITE);
  }
  oled.setCursor(96, 3); oled.print(bandLabel);

  // XP progress bar (raised a few px so it sits clear of the yellow/blue split
  // on two-tone OLEDs and looks tidy on mono panels).
  oled.drawRect(0, 11, 128, 4, SSD1306_WHITE);
  uint32_t need = xpToNext(pet.level);
  int xpw = need ? (int)(126.0 * pet.xp / need) : 126;
  if (xpw > 126) xpw = 126;
  if (xpw > 0) oled.fillRect(1, 12, xpw, 2, SSD1306_WHITE);

  // Discovery counters: top = devices (BLE + clients), bottom = Wi-Fi APs.
  uint32_t devCount = (uint32_t)seenBLE.size() + (uint32_t)seenClient.size();
  oled.drawBitmap(5, 20, ic_dev_bits, 5, 8, SSD1306_WHITE);
  oled.setCursor(14, 21); oled.print(devCount);
  oled.drawBitmap(3, 31, ic_wifi_bits, 9, 7, SSD1306_WHITE);
  oled.setCursor(14, 31); oled.print((uint32_t)seenAP.size());

  // H / M / E bars, labels in the tiny Org_01 font
  oled.setFont(&Org_01);
  oled.setCursor(5, 45); oled.print("H");
  oled.setCursor(5, 53); oled.print("M");
  oled.setCursor(5, 61); oled.print("E");
  oled.setFont();                       // restore default font

  oled.drawRect(12, 41, 25, 5, SSD1306_WHITE);
  oled.drawRect(12, 49, 25, 5, SSD1306_WHITE);
  oled.drawRect(12, 57, 25, 5, SSD1306_WHITE);
  int hw = 23 * (int)pet.hunger  / 100;
  int mw = 23 * moodValue()      / 100;
  int ew = 23 * (int)pet.energy  / 100;
  if (hw > 0) oled.fillRect(13, 42, hw, 3, SSD1306_WHITE);
  if (mw > 0) oled.fillRect(13, 50, mw, 3, SSD1306_WHITE);
  if (ew > 0) oled.fillRect(13, 58, ew, 3, SSD1306_WHITE);
}
void drawBubble() {
  if (millis() >= bubbleUntil || !bubble.length()) return;
  int w = bubble.length() * 6 + 6, x = (OLED_WIDTH - w) / 2;
  if (x < 0) { x = 0; w = OLED_WIDTH; }
  oled.fillRect(x, 12, w, 12, SSD1306_BLACK);
  oled.drawRect(x, 12, w, 12, SSD1306_WHITE);
  oled.setTextColor(SSD1306_WHITE); oled.setCursor(x + 3, 14); oled.print(bubble);
}
void drawHome() {
  drawHomeHUD();
  drawCreature();
  drawBubble();
}
void drawStats() {
  oled.setTextSize(1); oled.setTextColor(SSD1306_WHITE);
  oled.setCursor(0, 0); oled.print(pet.name);
  oled.setCursor(96, 0); oled.print("Lv"); oled.print(pet.level);
  oled.drawFastHLine(0, 10, OLED_WIDTH, SSD1306_WHITE);
  int y = 13;
  oled.setCursor(0, y); oled.print("Hung"); drawBar(34, y - 1, 92, 8, pet.hunger);  y += 10;
  oled.setCursor(0, y); oled.print("Mood"); drawBar(34, y - 1, 92, 8, moodValue()); y += 10;
  oled.setCursor(0, y); oled.print("Enrg"); drawBar(34, y - 1, 92, 8, pet.energy);  y += 11;
  oled.setCursor(0, y);
  oled.print(stageName(stageForLevel(pet.level)));
  oled.print(isSad() ? " :(" : " :)");
  oled.print(" Age "); oled.print(formatAge(pet.ageSec));
}
void drawLog() {
  // Lifetime statistics for the whole device (persist across reboots).
  oled.setFont(); oled.setTextSize(1); oled.setTextColor(SSD1306_WHITE);
  oled.setCursor(0, 0); oled.print("Stats");
  oled.setCursor(66, 0); oled.print("Age "); oled.print(formatAge(pet.ageSec));
  oled.drawFastHLine(0, 10, OLED_WIDTH, SSD1306_WHITE);
  uint32_t nAP = seenAP.size(), nBLE = seenBLE.size(), nCli = seenClient.size();
  int y = 13;
  oled.setCursor(0, y); oled.print("APs "); oled.print(nAP);
  oled.setCursor(70, y); oled.print("BLE "); oled.print(nBLE); y += 9;
  oled.setCursor(0, y); oled.print("Clients "); oled.print(nCli);
  y += 9;
  oled.setCursor(0, y); oled.print("Found "); oled.print(nAP + nBLE + nCli); y += 9;
  oled.setCursor(0, y); oled.print("Packets "); oled.print(lifetimePktTotal()); y += 9;
  uint32_t pctFull = (RECORD_CAP ? (100UL * storeCount / RECORD_CAP) : 0);
  oled.setCursor(0, y); oled.print("Store "); oled.print(storeCount);
  oled.print("/"); oled.print((uint32_t)RECORD_CAP);
  oled.print(" "); oled.print(pctFull); oled.print("%");
}

int buildMenu(MItem* out) {
  int n = 0;
  out[n++] = {"Feed", 0};
  out[n++] = {"Play", 1};
  out[n++] = {"Pet", 2};
  out[n++] = {scanPaused ? "Scan: OFF" : "Scan: ON", 3};
  if (monitorUnlocked())
    out[n++] = {mode == MODE_MONITOR ? "Monitor: ON" : "Pkt Monitor", 4};
  if (promiscUnlocked())
    out[n++] = {mode == MODE_PROMISC ? "Promisc: ON" : "Promisc Harvest", 5};
  out[n++] = {"Config: serial", 6};
  out[n++] = {"Save now", 7};
  out[n++] = {confirmReset ? "Reset? OK=yes" : "Reset pet", 8};
  return n;
}
void drawMenu() {
  MItem items[10]; int n = buildMenu(items);
  if (menuIdx >= n) menuIdx = n - 1;
  const int rows = 5;
  if (menuIdx < menuTop) menuTop = menuIdx;
  if (menuIdx >= menuTop + rows) menuTop = menuIdx - rows + 1;
  oled.setTextSize(1); oled.setTextColor(SSD1306_WHITE);
  oled.setCursor(0, 0); oled.print("Menu");
  oled.drawFastHLine(0, 10, OLED_WIDTH, SSD1306_WHITE);
  for (int i = 0; i < rows && (menuTop + i) < n; i++) {
    int idx = menuTop + i, y = 13 + i * 10;
    if (idx == menuIdx) { oled.fillRect(0, y - 1, OLED_WIDTH, 9, SSD1306_WHITE);
                          oled.setTextColor(SSD1306_BLACK); }
    else oled.setTextColor(SSD1306_WHITE);
    oled.setCursor(4, y); oled.print(items[idx].label);
  }
  oled.setTextColor(SSD1306_WHITE);
}
void render() {
  oled.clearDisplay();
  switch (screen) {
    case SC_HOME:  drawHome();  break;
    case SC_STATS: drawStats(); break;
    case SC_LOG:   drawLog();   break;
    case SC_MENU:  drawMenu();  break;
  }
  if (levelUpFlag) { say(PHRASE_PICK(SAY_LEVELUP), 1800); levelUpFlag = false; }
  oled.display();
}

// =============================================================================
//  Input dispatch
// =============================================================================
void handleMenuSelect(int id) {
  switch (id) {
    case 0: doFeed(); break;
    case 1: doPlay(); break;
    case 2: doPet();  break;
    case 3: scanPaused = !scanPaused; setMode(MODE_SCAN);
            say(scanPaused ? "scan off" : "scan on"); break;
    case 4: setMode(mode == MODE_MONITOR ? MODE_SCAN : MODE_MONITOR);
            say(mode == MODE_MONITOR ? "monitoring" : "scan"); break;
    case 5: setMode(mode == MODE_PROMISC ? MODE_SCAN : MODE_PROMISC);
            say(mode == MODE_PROMISC ? "harvesting" : "scan"); break;
    case 6: say("use USB serial", 2200); break;
    case 7: saveState(); saveSeenSet(SEEN_PATH_AP, seenAP);
            saveSeenSet(SEEN_PATH_BLE, seenBLE); saveSeenSet(SEEN_PATH_CLIENT, seenClient);
            storageFlush(); say("saved"); break;
    case 8: if (confirmReset) { doResetPet(); confirmReset = false; } else confirmReset = true; break;
  }
}
void handleInput() {
  const Screen order[] = {SC_HOME, SC_STATS, SC_LOG, SC_MENU};
  auto idxOf = [&](Screen s){ for (int i = 0; i < 4; i++) if (order[i] == s) return i; return 0; };
  if (bRight.pressedEv) { screen = order[(idxOf(screen) + 1) % 4]; confirmReset = false; }
  if (bLeft.pressedEv)  { screen = order[(idxOf(screen) + 3) % 4]; confirmReset = false; }

  if (screen == SC_MENU) {
    MItem items[10]; int n = buildMenu(items);
    if (bUp.pressedEv)   { menuIdx = (menuIdx + n - 1) % n; confirmReset = false; }
    if (bDown.pressedEv) { menuIdx = (menuIdx + 1) % n; confirmReset = false; }
    if (bOk.pressedEv)   handleMenuSelect(items[menuIdx].id);
    if (bBack.pressedEv) { screen = SC_HOME; confirmReset = false; }
  } else {
    if (screen == SC_HOME) {
      if (bOk.pressedEv) doPet();     // quick interact
      if (bUp.pressedEv) doFeed();
      if (bDown.pressedEv) doPlay();
    } else if (bOk.pressedEv) { screen = SC_MENU; }
    if (bBack.pressedEv) screen = SC_HOME;
  }
}

// =============================================================================
//  Serial console (config + maintenance)
// =============================================================================
void printHelp() {
  Serial.println(F("\nWarPet serial console:"));
  Serial.println(F("  i                 - status/info"));
  Serial.println(F("  w <ssid> <pass>   - set home Wi-Fi (for future Wigle sync)"));
  Serial.println(F("  k <name> <token>  - set Wigle API name + token"));
  Serial.println(F("  n <name>          - rename the pet"));
  Serial.println(F("  d                 - dump capture log (oldest first)"));
  Serial.println(F("  s                 - save state now"));
  Serial.println(F("  c                 - clear capture storage"));
  Serial.println(F("  x                 - reset the pet (wipes progress + logs)"));
  Serial.println(F("  h                 - this help"));
}
void dumpCaptures() {
  Serial.println(F("ts,type,mac,rssi,ch,ssid"));
  const char* tn[] = {"AP2G","AP5G","BLE","CLIENT"};
  for (uint32_t i = 0; i < storeCount; i++) {
    uint32_t idx = (storeHead + RECORD_CAP - storeCount + i) % RECORD_CAP;
    Rec r; storeFile.seek(idx * RECORD_SIZE, SeekSet);
    if (storeFile.read((uint8_t*)&r, RECORD_SIZE) != (int)RECORD_SIZE) break;
    char macs[18];
    snprintf(macs, sizeof(macs), "%02X:%02X:%02X:%02X:%02X:%02X",
             r.mac[0], r.mac[1], r.mac[2], r.mac[3], r.mac[4], r.mac[5]);
    Serial.printf("%u,%s,%s,%d,%u,%s\n", r.ts,
                  tn[r.type & 3], macs, r.rssi, r.channel, r.ssid);
  }
  Serial.printf("(%u records)\n", storeCount);
}
void serviceSerial() {
  if (!Serial.available()) return;
  String line = Serial.readStringUntil('\n'); line.trim();
  if (!line.length()) return;
  char cmd = line.charAt(0);
  String rest = (line.length() > 1) ? line.substring(1) : ""; rest.trim();
  switch (cmd) {
    case 'i':
      Serial.printf("Pet=%s Lv=%u XP=%u/%u Stage=%s\n", pet.name, pet.level,
                    pet.xp, xpToNext(pet.level), stageName(stageForLevel(pet.level)));
      Serial.printf("Hunger=%u Mood=%d Energy=%u Age=%s\n", pet.hunger,
                    moodValue(), pet.energy, formatAge(pet.ageSec).c_str());
      Serial.printf("Uniq: AP=%u BLE=%u Client=%u  Store=%u/%u  Pkts=%u\n",
                    (unsigned)seenAP.size(), (unsigned)seenBLE.size(),
                    (unsigned)seenClient.size(), storeCount, (unsigned)RECORD_CAP, pktCount);
      Serial.printf("Home SSID=%s  Wigle=%s  Mode=%s\n",
                    cfgHomeSSID.c_str(), cfgWigleName.length() ? "set" : "unset",
                    mode == MODE_SCAN ? "scan" : (mode == MODE_PROMISC ? "promisc" : "monitor"));
      break;
    case 'w': {
      int sp = rest.indexOf(' ');
      if (sp > 0) { cfgHomeSSID = rest.substring(0, sp); cfgHomePass = rest.substring(sp + 1);
                    cfgHomePass.trim(); saveConfig(); Serial.println(F("home wifi saved.")); }
      else Serial.println(F("usage: w <ssid> <pass>"));
      break; }
    case 'k': {
      int sp = rest.indexOf(' ');
      if (sp > 0) { cfgWigleName = rest.substring(0, sp); cfgWigleToken = rest.substring(sp + 1);
                    cfgWigleToken.trim(); saveConfig(); Serial.println(F("wigle creds saved.")); }
      else Serial.println(F("usage: k <apiname> <token>"));
      break; }
    case 'n':
      if (rest.length()) { strncpy(pet.name, rest.c_str(), sizeof(pet.name) - 1);
                           pet.name[sizeof(pet.name)-1] = 0; saveState(); Serial.println(F("renamed.")); }
      break;
    case 'd': dumpCaptures(); break;
    case 's': saveState(); saveSeenSet(SEEN_PATH_AP, seenAP); saveSeenSet(SEEN_PATH_BLE, seenBLE);
              saveSeenSet(SEEN_PATH_CLIENT, seenClient); storageFlush(); Serial.println(F("saved.")); break;
    case 'c': storageClear(); Serial.println(F("storage cleared.")); break;
    case 'x': doResetPet(); Serial.println(F("pet reset.")); break;
    default:  printHelp(); break;
  }
}

// =============================================================================
//  setup / loop
// =============================================================================
void setup() {
  Serial.begin(115200); delay(150);
  Serial.println(F("\nWarPet booting..."));

  pinMode(PIN_BTN_UP, INPUT_PULLUP);   pinMode(PIN_BTN_DOWN, INPUT_PULLUP);
  pinMode(PIN_BTN_OK, INPUT_PULLUP);   pinMode(PIN_BTN_BACK, INPUT_PULLUP);
  pinMode(PIN_BTN_LEFT, INPUT_PULLUP); pinMode(PIN_BTN_RIGHT, INPUT_PULLUP);

  Wire.begin();
  if (!oled.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR))
    Serial.println(F("SSD1306 not found (check 0x3C/0x3D + wiring)"));
  oled.setRotation(OLED_ROTATION);
  oled.clearDisplay(); oled.setTextColor(SSD1306_WHITE);
  oled.setCursor(8, 28); oled.print(F("WarPet waking...")); oled.display();

  if (!LittleFS.begin(true)) Serial.println(F("LittleFS mount failed"));

  loadConfig();
  loadState();
  storageInit();
  loadSeenSet(SEEN_PATH_AP, seenAP, MAX_SEEN_AP);
  loadSeenSet(SEEN_PATH_BLE, seenBLE, MAX_SEEN_BLE);
  loadSeenSet(SEEN_PATH_CLIENT, seenClient, MAX_SEEN_CLIENT);
  Serial.printf("Pet '%s' Lv%u  AP=%u BLE=%u Cli=%u  Store=%u\n", pet.name, pet.level,
                (unsigned)seenAP.size(), (unsigned)seenBLE.size(),
                (unsigned)seenClient.size(), storeCount);

  WiFi.mode(WIFI_STA);
  WiFi.disconnect(false, true);
  WiFi.setBandMode(WIFI_BAND_MODE_AUTO);

#if ENABLE_MONITOR
  monMutex = xSemaphoreCreateMutex();
#endif
#if ENABLE_BLE
  bleMutex = xSemaphoreCreateMutex();
  NimBLEDevice::init("");
  pBLEScan = NimBLEDevice::getScan();
  pBLEScan->setScanCallbacks(&bleCB, false);
  pBLEScan->setActiveScan(true);
  pBLEScan->setInterval(100);
  pBLEScan->setWindow(99);
#endif

  say(PHRASE_PICK(SAY_GREET), 2500);
  printHelp();
  delay(300);
}

void loop() {
  uint32_t now = millis();

  pollButtons();
  handleInput();

  if (mode == MODE_SCAN) serviceScanner();
#if ENABLE_MONITOR
  else                   serviceMonitor();
#endif

  serviceSerial();

  // Accumulate powered-on lifetime (whole seconds; survives millis() wrap).
  static uint32_t lastAgeMs = millis();
  uint32_t dAge = now - lastAgeMs;
  if (dAge >= 1000) { pet.ageSec += dAge / 1000; lastAgeMs += (dAge / 1000) * 1000; }

  static uint32_t lastTick = 0;
  if (now - lastTick >= STAT_TICK_MS) { lastTick = now; statTick(); }

  static uint32_t lastSave = 0;
  if (now - lastSave >= AUTOSAVE_MS) {
    lastSave = now;
    saveState();
    saveSeenSet(SEEN_PATH_AP, seenAP);
    saveSeenSet(SEEN_PATH_BLE, seenBLE);
    saveSeenSet(SEEN_PATH_CLIENT, seenClient);
    storageFlush();
  }

  static uint32_t lastFrame = 0;
  if (now - lastFrame >= 33) { lastFrame = now; render(); }
}
