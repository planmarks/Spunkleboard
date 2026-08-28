
// [BEGIN lopaka generated]
#include "Org_01.h"

static const unsigned char PROGMEM image_paint_9_1_bits[] = {0x20,0x30,0xa8,0x70,0x70,0xa8,0x30,0x20};

static const unsigned char PROGMEM image_paint_9_bits[] = {0x3e,0x00,0x41,0x00,0x80,0x80,0x1c,0x00,0x22,0x00,0x00,0x00,0x08,0x00};

void drawScreen_1(void) {
    gfx->fillScreen(0x0);

    // string 2
    gfx->setTextColor(0xFFFF);
    gfx->setTextWrap(false);
    gfx->setCursor(3, 4);
    gfx->println("L");
    // rect 3
    gfx->drawRect(0, 14, 128, 4, 0xFFFF);
    // string 5
    gfx->setCursor(10, 4);
    gfx->println("21");
    // string 8
    gfx->setCursor(14, 21);
    gfx->println("311");
    // string 9
    gfx->setCursor(14, 31);
    gfx->println("512");
    // paint 9
    gfx->drawBitmap(3, 31, image_paint_9_bits, 9, 7, 0xFFFF);
    // paint 9
    gfx->drawBitmap(5, 20, image_paint_9_1_bits, 5, 8, 0xFFFF);
    // string 9
    gfx->setFont(&Org_01);
    gfx->setCursor(5, 45);
    gfx->println("H");
    // string 9 copy 1
    gfx->setCursor(5, 53);
    gfx->println("M");
    // ellipse 12
    gfx->drawEllipse(90, 4, 3, 3, 0xFFFF);
    // string 9 copy 2
    gfx->setCursor(5, 61);
    gfx->println("E");
    // string 13
    gfx->setFont();
    gfx->setCursor(96, 4);
    gfx->println("5G/BT");
    // rect 14
    gfx->drawRect(12, 41, 25, 5, 0xFFFF);
    // rect 14 copy 1
    gfx->drawRect(12, 49, 25, 5, 0xFFFF);
    // rect 16
    gfx->fillRect(1, 15, 98, 2, 0xFFFF);
    // rect 14 copy 2
    gfx->drawRect(12, 57, 25, 5, 0xFFFF);
    // rect 17
    gfx->fillRect(13, 42, 19, 3, 0xFFFF);
    // rect 18
    gfx->fillRect(13, 50, 14, 3, 0xFFFF);
    // rect 19
    gfx->fillRect(13, 58, 22, 3, 0xFFFF);
}
// [END lopaka generated]
