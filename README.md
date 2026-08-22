# Spunkleboard

**Spunkleboard** is a compact, hackable **ESP32-C3 / ESP32-C5 development board** with everything you
need for portable, interactive projects built in: **6 buttons**, a **0.96" OLED display**, an
onboard **LiPo battery** with charging, and an **SMA antenna connector** for serious wireless range.

It's a blank canvas. Flash it with whatever you like — the hardware is general-purpose and
firmware-agnostic. **Pre-made firmwares are coming soon.**

---

## ✨ Highlights

- 🧠 **ESP32-C3 / ESP32-C5** RIS-V Wi-Fi + Bluetooth LE SoC
- 🕹️ **6 tactile buttons** — a full directional + action pad for menus, games, and UIs
- 🖥️ **0.96" 128×64 OLED** (SSD1306/SSD1315, I²C) — crisp monochrome graphics and text
- 🔋 **Onboard LiPo battery + charging** — go fully untethered
- 📡 **SMA connector** — attach an external antenna for extended Wi-Fi/BLE (and sub-GHz on C5) range
- 🔌 **USB-C** for programming, power, and serial
- 🧩 **Firmware-agnostic** — Arduino, ESP-IDF, MicroPython, and more

---

## 🛠️ Hardware Overview

| Component | Details |
|-----------|---------|
| **MCU module** | Seeed XIAO ESP32-C3 or ESP32-C5 (RISC-V, Wi-Fi + BLE) |
| **Display** | 0.96" OLED, 128×64, monochrome, I²C (`0x3C`) |
| **Buttons** | 6× momentary tactile switches (e.g. Up / Down / Left / Right / OK / Back) |
| **Power** | Single-cell LiPo with onboard charging via USB-C |
| **Antenna** | SMA connector for an external antenna |
| **Interface** | USB-C (power / flashing / serial), exposed GPIO |

> The board accepts the pin-compatible **XIAO ESP32-C3** and **ESP32-C5** modules, so you can pick
> the SoC that fits your project. The C5 adds dual-band Wi-Fi (2.4 + 5 GHz), which pairs nicely with
> the SMA antenna option.

---

## 💡 What You Can Build

Because it's just a well-equipped ESP32 board, the possibilities are wide open:

- 📶 **Wi-Fi / BLE tools** — scanners, signal meters, and network utilities (great with the SMA antenna)
- 🎮 **Handheld games & toys** — the 6-button pad + OLED make a natural tiny game console
- 📟 **Portable dashboards** — sensor readouts, clocks, timers, notifiers
- 🏠 **Remote controls & IoT nodes** — battery-powered, wireless, pocket-sized
- 🧪 **Learning & prototyping** — a friendly platform for exploring embedded development
- 🎛️ **Menu-driven gadgets** — anything that benefits from a screen and buttons

---

## 🚀 Getting Started

### 1. Install a toolchain

**Arduino IDE**
1. Add the ESP32 board package (Espressif) via **Boards Manager**.
2. Select the matching **XIAO ESP32-C3** or **XIAO ESP32-C5** board.
3. Install a display library such as **U8g2** for the OLED.

**ESP-IDF** (advanced)
- Set the target with `idf.py set-target esp32c3` (or `esp32c5`) and build/flash as usual.

### 2. Flash

1. Connect the board over **USB-C**.
2. Select the correct serial port.
3. Upload your sketch/firmware. If needed, hold the module's **BOOT** button while connecting to
   enter download mode.

### 3. Hello, board

A first sketch typically:
- Brings up the OLED over I²C (`Wire.begin()`),
- Reads the 6 buttons (configured as `INPUT_PULLUP`, active-LOW),
- Draws a simple menu you can navigate.

---

## 🎛️ Using the Peripherals

- **Buttons** are wired between a GPIO and GND — enable the internal pull-ups (`INPUT_PULLUP`) and
  treat a LOW reading as "pressed."
- **OLED** is on the default I²C bus (SDA/SCL); most 0.96" modules use address `0x3C`.
- **LiPo battery** charges from USB-C; use a protected single-cell pack and mind polarity.
- **SMA antenna** connects to the module's RF path — fit an antenna rated for your band (2.4 GHz for
  C3; 2.4/5 GHz for C5) before transmitting.

> Exact GPIO assignments depend on your board revision and chosen module — check the silkscreen and
> your board's pin map, and consult the Seeed XIAO pin-multiplexing reference for the module you use.

---

## 📦 Firmware

**Pre-made firmwares are coming soon** — ready-to-flash images that showcase what Spunkleboard can do
out of the box. Until then, bring your own: any ESP32-C3/C5 project that uses an I²C OLED, GPIO
buttons, and (optionally) the battery and antenna will run happily here.

Want to share what you've built? Contributions and firmware submissions are welcome.

---

## 🤝 Contributing

Issues, ideas, hardware notes, and firmware contributions are all welcome. Open an issue or a pull
request to get involved.

---

## 📄 License

See the `LICENSE` file for details.

---

*Spunkleboard — a little board with a screen, some buttons, a battery, and an antenna. Make it do
something fun.*
