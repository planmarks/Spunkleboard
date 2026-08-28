#pragma once
//
// Thin wrapper over U8g2 for the SSD1306 OLED. Keeps U8g2 details out of the
// application code and provides a few reusable screens.

#include <U8g2lib.h>

class Display {
public:
  void begin();

  // Centered title + subtitle boot screen.
  void splash(const char *title, const char *subtitle);

  // Scrollable list menu with an inverted title bar and a '>' cursor.
  void menu(const char *title, const char *const *items, uint8_t count, uint8_t selected);

  // Title bar + body. Body may contain '\n' to break lines (no auto-wrap).
  void message(const char *title, const char *body);

  // Render `text` as a QR code, centered and scaled to fit the panel. Drawn as
  // dark modules on a lit background so a phone camera can read it.
  void qr(const char *text);

  // Power the OLED panel down (sleep) or back up. setPowerSave(1) issues the
  // SSD1306 "display off" (panel + charge pump off → ~0 mA); the frame buffer is
  // retained, so wake() — or any later draw — restores the last image.
  void sleep() { u8g2_.setPowerSave(1); }
  void wake()  { u8g2_.setPowerSave(0); }

private:
  U8G2_SSD1306_128X64_NONAME_F_HW_I2C u8g2_ =
      U8G2_SSD1306_128X64_NONAME_F_HW_I2C(U8G2_R0, U8X8_PIN_NONE);

  void drawTitleBar_(const char *title);
};
