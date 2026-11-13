// Sunton ESP32-4827S043: 4.3" 480x272 RGB panel, GT911 capacitive touch (C variant), CH340 UART.
#pragma once
#include <Arduino_GFX_Library.h>

#define BOARD_NAME "Sunton ESP32-4827S043"
#define BOARD_WIDTH 480
#define BOARD_HEIGHT 272
#define BOARD_BL_PIN 2
#define BOARD_DRAW_LINES 68
#define BOARD_DRAW_IN_PSRAM 1

#define TOUCH_SDA 19
#define TOUCH_SCL 20
#define TOUCH_INT -1
#define TOUCH_RST 38

inline Arduino_GFX *board_make_display() {
    Arduino_ESP32RGBPanel *panel = new Arduino_ESP32RGBPanel(
        40 /* DE */, 41 /* VSYNC */, 39 /* HSYNC */, 42 /* PCLK */,
        45 /* R0 */, 48 /* R1 */, 47 /* R2 */, 21 /* R3 */, 14 /* R4 */,
        5 /* G0 */, 6 /* G1 */, 7 /* G2 */, 15 /* G3 */, 16 /* G4 */, 4 /* G5 */,
        8 /* B0 */, 3 /* B1 */, 46 /* B2 */, 9 /* B3 */, 1 /* B4 */,
        0 /* hsync_polarity */, 8 /* hsync_front_porch */, 4 /* hsync_pulse_width */, 43 /* hsync_back_porch */,
        0 /* vsync_polarity */, 8 /* vsync_front_porch */, 4 /* vsync_pulse_width */, 12 /* vsync_back_porch */,
        1 /* pclk_active_neg */, 12000000 /* prefer_speed */);
    return new Arduino_RGB_Display(BOARD_WIDTH, BOARD_HEIGHT, panel, 0 /* rotation */, true /* auto_flush */);
}
