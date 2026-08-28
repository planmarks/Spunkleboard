# WarPet 🐾📡

A pocket **dual-band Wi-Fi + BLE wardriving rig that is also a Tamagotchi**, for the
**Seeed Studio XIAO ESP32-C5** with a 0.96" OLED and 6 buttons.

Your creature earns XP from every **unique** discovery it makes as you walk around —
Wi-Fi access points (2.4 GHz *and* 5 GHz), BLE devices, and Wi-Fi client stations
picked up by the passive monitor. Captures are logged to an on-device ring buffer.
Feed it, play with it, and watch it grow from an egg into an Elder — leveling is
infinite, and higher stages unlock new sensing abilities.

> The ESP32-C5 is Espressif's first dual-band chip, which is why real 5 GHz scanning
> is possible here — most ESP32 boards are 2.4 GHz only.

---

## ⚖️ Scope & responsible use — please read

This firmware is **passive by design**. It:
- listens to Wi-Fi beacons and BLE advertisements (the same info your phone's Wi-Fi
  list shows), and
- in monitor mode, **observes** over-the-air 802.11 frames to count packets and
  collect device MAC addresses.

It does **not**, and intentionally **cannot**:
- transmit deauthentication frames or perform any denial-of-service ("deauth attack"),
- inject or forge packets,
- connect to or authenticate against foreign networks,
- capture or store packet payloads/contents.

**There is deliberately no deauth/attack capability in this project.** A roaming device
that knocks other people's gear off their networks is an indiscriminate attack on third
parties, so it's out of scope. The "Promiscuous" unlock here is a *passive client
harvester*, not an attack tool. Operate only where passive monitoring is lawful, and
don't store or share data you shouldn't.

---

## Hardware & wiring

| Function | Button | XIAO pin | GPIO |
|----------|--------|----------|------|
| Up / Feed      | UP    | D0 | 1  |
| Down / Play    | DOWN  | D1 | 0  |
| OK / Pet       | OK    | D2 | 25 |
| Back           | BACK  | D3 | 7  |
| Left (screen)  | LEFT  | D8 | 8  |
| Right (screen) | RIGHT | D9 | 9  |

- **Buttons:** each connects its pin to **GND** (internal pull-ups, active-low — no
  resistors).
- **OLED (SSD1306, I²C):** `VCC`→3V3, `GND`→GND, `SDA`→**D4 (GPIO23)**, `SCL`→**D5
  (GPIO24)**, address `0x3C` (some are `0x3D`). Display is flipped 180° in firmware
  (`OLED_ROTATION` in `config.h`).

Pins and every tunable live in [`config.h`](config.h).

---

## Software setup (Arduino IDE)

1. **Boards Manager URL** (*File → Preferences*):
   ```
   https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json
   ```
   Install **esp32 by Espressif Systems**, **v3.1.0+** (needed for C5, 5 GHz, and
   promiscuous monitor).
2. **Board:** *Tools → Board → esp32 →* **XIAO_ESP32C5**.
3. **Partition scheme:** pick one with a decent filesystem, e.g. *"Default 4MB with
   spiffs (1.2MB APP / 1.5MB SPIFFS)"*. The capture ring buffer + uniqueness sets need
   room in LittleFS (default ring ≈ 6000 records; see `RECORD_CAP`).
4. **Libraries** (*Manage Libraries*): `Adafruit SSD1306`, `Adafruit GFX Library`,
   `NimBLE-Arduino` (v2.x).
5. Open `xiaoesp32c5_tamagotchi.ino` (keep `config.h` and `names.h` alongside it),
   select the port, **Upload**. Serial monitor at **115200 baud**.

On first boot the pet is given a **random, unique name** generated from the board's
hardware entropy (e.g. `Byte-Gremlin`), stored in NVS.

---

## Controls

| Screen | LEFT / RIGHT | UP | DOWN | OK | BACK |
|--------|--------------|----|------|----|------|
| **Home** | switch screen | Feed | Play | Pet (interact) | Home |
| **Pet** (name/H·M·E/age) | switch screen | — | — | open Menu | Home |
| **Stats** (lifetime totals) | switch screen | — | — | open Menu | Home |
| **Menu** | switch screen | up | down | select | Home |

**Menu:** Feed · Play · Pet · Scan ON/OFF · *Pkt Monitor* (Teen+) · *Promisc Harvest*
(Adult+) · Config (serial) · Save now · Reset pet (OK twice to confirm).

---

## Gameplay

**XP per unique discovery** (tunable in `config.h`):
| Discovery | XP |
|-----------|----|
| Wi-Fi AP (2.4 or 5 GHz) | 10 |
| BLE device | 8 |
| Wi-Fi client station (monitor) | 12 |
| Packets observed | +1 per 25 frames |

**Leveling is infinite** — each level needs **15% more XP** than the last
(`xpToNext = 100 × 1.15^level`).

**Evolution stages & unlocks** (one new stage every 5 levels, animated 2-frame idle):
| Stage | Levels | Unlocks |
|-------|--------|---------|
| Egg   | 0–4   | — |
| Baby  | 5–9   | — |
| Teen  | 10–14 | **Packet Monitoring** |
| Adult | 15+   | **Promiscuous Harvest** |

**Stats:**
- **Hunger** = *satiation* (100 = well fed). Discoveries raise it; it slowly falls when
  nothing is found. Feeding from the menu tops it up.
- **Energy** is restored by discoveries at a rate that depends on hunger: 0–25% hunger
  → no restore, 25–50% → 75%, 50–75% → 100%, 75–100% → 125%. Monitor/promiscuous modes
  drain **1% energy per 5 s**; at 0 energy the pet "sleeps" and returns to normal
  scanning. Play also costs energy.
- **Mood** = 25% from petting/interaction + 75% from discoveries & hunger. Below 25% the
  creature looks sad.
- **Age** is accumulated powered-on lifetime (persists across reboots) and auto-scales
  its unit: minutes → hours (>60 m) → days (>24 h) → months (>30 d) → years (>12 mo).

The creature **talks** — random lines when greeted, fed, played with, petted, on
level-up, and when hungry/sleepy.

---

## On-device storage (ring buffer)

Every unique capture is appended to `/captures.bin` in LittleFS as a fixed-size record:

```
timestamp_s, type(AP2G/AP5G/BLE/CLIENT), mac, rssi, channel, ssid
```

When the buffer fills (`RECORD_CAP`), the **oldest record is overwritten**. Uniqueness
sets survive reboots (stored in LittleFS) so XP isn't re-awarded for the same devices.

---

## Serial console (115200 baud)

Type a command + Enter:

| Cmd | Action |
|-----|--------|
| `i` | status / info |
| `w <ssid> <pass>` | set home Wi-Fi (for the future Wigle sync) |
| `k <apiname> <token>` | set Wigle API name + token |
| `n <name>` | rename the pet |
| `d` | dump the capture log as CSV (oldest first) |
| `s` | save state now |
| `c` | clear capture storage |
| `x` | reset the pet (wipes progress + logs) |
| `h` | help |

---

## Wigle upload — deferred (needs GPS)

Wigle.net keys uploads on **GPS coordinates**, and this board has no GPS, so the
uploader is turned **off** (`ENABLE_WIGLE_UPLOAD 0`). The plumbing is in place: your
home-Wi-Fi and Wigle API credentials are captured over serial and stored in NVS, and
captures are already logged locally. When you add a GPS module (UART NMEA), the next
step is: parse coordinates into each record, connect to home Wi-Fi when in range, and
POST the CSV to Wigle's file-upload API. Say the word and I'll wire that up.

---

## Tuning

All gameplay, scan, monitor, XP, level, stat, and storage parameters live in
[`config.h`](config.h). Feature toggles: `ENABLE_5G`, `ENABLE_BLE`, `ENABLE_MONITOR`,
`ENABLE_WIGLE_UPLOAD`.

## Notes & limits
- Wi-Fi and BLE are **time-sliced**, not simultaneous — stable coexistence on the
  single-core C5. Monitor mode is a separate radio mode that pauses normal scanning.
- Monitor/promiscuous channel-hop the **2.4 GHz** band (client activity lives there);
  5 GHz monitor isn't included.
- Client "devices" are transmitter MACs seen in management/data frames; many modern
  phones randomize their MAC, so counts reflect observed addresses, not unique humans.
