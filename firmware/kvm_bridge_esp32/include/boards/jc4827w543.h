// Guition JC4827W543: 4.3" 480x272 panel on an NV3041A over QSPI, GT911 capacitive touch
// (the "C" variant; the "R" variant has a resistive XPT2046 and is not covered), CH340 UART.
#pragma once
#include <Arduino_GFX_Library.h>

#define BOARD_NAME "Guition JC4827W543"
#define BOARD_WIDTH 480
#define BOARD_HEIGHT 272
#define BOARD_BL_PIN 1
#define BOARD_DRAW_LINES 34
#define BOARD_DRAW_IN_PSRAM 0

#define TOUCH_SDA 8
#define TOUCH_SCL 4
#define TOUCH_INT 3
#define TOUCH_RST 38

inline Arduino_GFX *board_make_display() {
    Arduino_DataBus *bus = new Arduino_ESP32QSPI(45 /* CS */, 47 /* SCK */, 21 /* D0 */, 48 /* D1 */, 40 /* D2 */, 39 /* D3 */);
    return new Arduino_NV3041A(bus, GFX_NOT_DEFINED /* RST */, 0 /* rotation */, true /* IPS */);
}
