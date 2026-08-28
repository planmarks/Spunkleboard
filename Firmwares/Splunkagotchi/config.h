// =============================================================================
//  config.h  -  Hardware pins and gameplay tuning for the WarPet firmware
//  Board: Seeed Studio XIAO ESP32-C5  (dual-band Wi-Fi 6 + BLE 5)
// =============================================================================
#pragma once

// ----------------------------------------------------------------------------
//  Buttons  (XIAO silkscreen labels -> Arduino D-macros defined by the variant).
//  Wired to GND, using internal pull-ups (active LOW).
// ----------------------------------------------------------------------------
#define PIN_BTN_UP     D0   // GPIO1
#define PIN_BTN_DOWN   D1   // GPIO0
#define PIN_BTN_OK     D2   // GPIO25
#define PIN_BTN_BACK   D3   // GPIO7
#define PIN_BTN_LEFT   D8   // GPIO8
#define PIN_BTN_RIGHT  D9   // GPIO9

// ----------------------------------------------------------------------------
//  OLED  (0.96" SSD1306, 128x64, I2C).  Default XIAO I2C = D4(SDA)/D5(SCL).
// ----------------------------------------------------------------------------
#define OLED_WIDTH     128
#define OLED_HEIGHT    64
#define OLED_ADDR      0x3C   // most 0.96" modules; some are 0x3D
#define OLED_ROTATION  2      // 0=normal, 2=180 (flipped)

// ----------------------------------------------------------------------------
//  Feature toggles
// ----------------------------------------------------------------------------
#define ENABLE_5G            1   // 5 GHz Wi-Fi scan pass
#define ENABLE_BLE           1   // BLE advertisement scanning
#define ENABLE_MONITOR       1   // passive promiscuous packet capture engine
#define ENABLE_WIGLE_UPLOAD  0   // deferred: needs GPS + home-Wi-Fi (see README)

// ----------------------------------------------------------------------------
//  Scanning
// ----------------------------------------------------------------------------
#define WIFI_SCAN_MS       260   // per-channel dwell for Wi-Fi scans (ms)
#define BLE_SCAN_MS        3000  // BLE active-scan window (ms)
#define SCAN_SHOW_HIDDEN   true

#define MAX_SEEN_AP        4000  // in-RAM uniqueness caps
#define MAX_SEEN_BLE       4000
#define MAX_SEEN_CLIENT    4000

// ----------------------------------------------------------------------------
//  Passive packet monitor (promiscuous). Purely observational: counts frames
//  and harvests unique client MACs from management/data frames. No injection.
// ----------------------------------------------------------------------------
#define MON_HOP_MS         300   // channel dwell before hopping (ms)
#define MON_CH_MIN         1     // 2.4 GHz channels to sweep in monitor mode
#define MON_CH_MAX         13
#define MON_ENERGY_MS      5000  // drain 1% energy per this interval while monitoring
#define XP_PER_PKT_BATCH   1     // +XP each time this many frames are seen
#define PKT_BATCH_SIZE     25

// ----------------------------------------------------------------------------
//  XP per discovery type (each unique thing found)
// ----------------------------------------------------------------------------
#define XP_PER_NEW_AP      10    // new access point (either band)
#define XP_PER_NEW_BLE     8     // new BLE device
#define XP_PER_NEW_CLIENT  12    // new Wi-Fi client station (from monitor)

// ----------------------------------------------------------------------------
//  Leveling: infinite, each level needs +15% more XP than the previous.
//  xpToNext(level) = XP_LEVEL_BASE * XP_LEVEL_GROWTH ^ level
// ----------------------------------------------------------------------------
#define XP_LEVEL_BASE      100.0f
#define XP_LEVEL_GROWTH    1.15f

// Evolution stages: one new stage every 5 levels (Egg/Baby/Teen/Adult).
//   Egg  L0-4 | Baby L5-9 | Teen L10-14 | Adult L15+
#define LVL_BABY           5     // egg hatches -> baby
#define LVL_TEEN           10    // Teen unlocks Packet Monitoring
#define LVL_ADULT          15    // Adult unlocks Promiscuous (client harvest)

// ----------------------------------------------------------------------------
//  Stats (all 0..100). NOTE: "hunger" here is a SATIATION meter -- high = well
//  fed. Discoveries raise it; inactivity lowers it (per the design spec).
// ----------------------------------------------------------------------------
#define STAT_TICK_MS       10000UL   // stat decay/update cadence

#define HUNGER_PER_DISCOVERY   4     // +satiation per new device discovered
#define HUNGER_DECAY_PER_TICK  2     // satiation lost each tick with no food

#define ENERGY_PER_DISCOVERY   3     // base energy gained per discovery
// Energy restore multiplier by hunger band (x100). 0-25 / 25-50 / 50-75 / 75-100
#define ENERGY_RATE_Q1     0         // hunger 0-25%   -> no restore
#define ENERGY_RATE_Q2     75        // hunger 25-50%  -> 75%
#define ENERGY_RATE_Q3     100       // hunger 50-75%  -> 100%
#define ENERGY_RATE_Q4     125       // hunger 75-100% -> 125%

#define PET_BOOST          35        // 'pet' interaction adds to the pet component
#define PET_DECAY_PER_TICK 3         // pet component fades over time
#define ACT_BUMP           18        // discovery activity bump (mood input)
#define ACT_DECAY_PER_TICK 4         // activity fades over time
#define PLAY_ENERGY_COST   10        // 'play' costs energy
#define MOOD_SAD_BELOW     25        // mood < this = Sad

// ----------------------------------------------------------------------------
//  Persistence & storage
// ----------------------------------------------------------------------------
#define AUTOSAVE_MS        60000UL

// On-device capture ring buffer (internal flash / LittleFS). When full, the
// oldest record is overwritten. RECORD_CAP * RECORD_SIZE bytes are reserved.
#define STORE_PATH         "/captures.bin"
#define RECORD_CAP         6000        // ~6000 records; see RECORD_SIZE in .ino
#define SEEN_PATH_AP       "/seen_ap.bin"
#define SEEN_PATH_BLE      "/seen_ble.bin"
#define SEEN_PATH_CLIENT   "/seen_cl.bin"
