# PIGEON — Private Independent Generator & Encrypted Offline Notebook

An **offline** secure password generator and encrypted vault built on the
Seeed Studio **XIAO ESP32-C3**, a 0.96" SSD1306 OLED, and 6 push buttons.

The device never starts WiFi or Bluetooth. Passwords are generated with the
ESP32-C3 hardware RNG and stored **encrypted** in flash. Nothing readable is
written to flash without your master PIN.

---

## Hardware

| Part | Notes |
|------|-------|
| Seeed XIAO ESP32-C3 | Target MCU |
| 0.96" OLED, SSD1306 | 128×64, I²C, address `0x3C` |
| 6× momentary buttons | Active-low, wired to GND |

### Button wiring

| Pin | Function |
|-----|----------|
| D0  | UP     |
| D1  | DOWN   |
| D2  | OK     |
| D3  | BACK   |
| D8  | LEFT   |
| D9  | RIGHT  |

Each button connects its pin to **GND**. Internal pull-ups are enabled in
firmware (`INPUT_PULLUP`), so no external resistors are needed.

> **Note:** D8 (GPIO8) and D9 (GPIO9) are ESP32-C3 strapping/boot pins. A plain
> button to GND is fine at runtime, but do **not** hold LEFT/RIGHT down while
> powering on/flashing — that can interfere with boot mode. GPIO9 doubles as the
> BOOT button.

### OLED wiring (XIAO ESP32-C3 default I²C)

| OLED | XIAO |
|------|------|
| SDA  | D4 (GPIO6) |
| SCL  | D5 (GPIO7) |
| VCC  | 3V3 |
| GND  | GND |

---

## Building / flashing (Arduino IDE)

1. Install the **ESP32 board package** (Boards Manager → "esp32" by Espressif).
2. Install libraries (Library Manager):
   - **Adafruit GFX Library**
   - **Adafruit SSD1306**
   - (`Preferences` and mbedTLS ship with the ESP32 core.)
3. Open `PIGEON - Password Generator.ino`.
4. Select board: **XIAO_ESP32C3**. Choose the correct COM port.
5. Upload.

---

## Security design

- **Master PIN** (4–8 digits) gates the device and is never stored.
- A 32-byte **AES-256 key** is derived from PIN + a per-device random 16-byte
  salt using **PBKDF2-HMAC-SHA256** (20,000 iterations).
- The flash stores only `salt` and a **verifier** = `SHA-256(AES key)`. The key
  itself is never persisted, so dumping flash cannot recover it without the PIN.
- Each vault entry is encrypted with **AES-256-CBC** using a fresh random IV.
- Wrong-PIN attempts trigger a growing delay to slow brute-forcing.
- **Change PIN** transparently re-encrypts the whole vault under the new key.
- **Factory reset** wipes PIN, vault, and settings.

> This is a hobbyist cold-storage aid, not a certified secure element. The AES
> key exists in RAM while unlocked, and flash is not read-protected against a
> determined physical attacker with chip-level access. Use a strong PIN, and
> lock the device when idle.

---

## Controls

**PIN entry:** UP/DOWN change the current digit · RIGHT adds it · LEFT deletes ·
OK submits.

**Text entry** (labels/usernames/manual passwords): LEFT/RIGHT move by one
character, UP/DOWN jump by five · OK picks · BACK backspaces · select `<DONE>`
then OK to finish.

**Menus:** UP/DOWN move · OK select · BACK exit.

### Menu map

- **Generate** — set length (8–63) and character sets (abc / ABC / 123 / !@#),
  then generate. On the result screen, OK saves it to the vault, BACK exits.
- **Vault** — browse stored entries by label; LEFT/RIGHT flip between username
  and password; OK offers delete.
- **Add entry** — enter label + username, then generate or type a password.
- **Settings** — Change PIN · Lock now · Factory reset · About.
- **Lock** — clears the key from RAM and returns to the PIN screen.

---

## First run

On first boot you'll be asked to set a master PIN (entered twice). After that,
every power-up requires the PIN before anything else is accessible.
