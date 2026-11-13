// Sunton ESP32-8048S070: 7.0" 800x480 RGB panel, GT911 capacitive touch, CH340 UART.
#pragma once
#include <Arduino_GFX_Library.h>

#define BOARD_NAME "Sunton ESP32-8048S070"
#define BOARD_WIDTH 800
#define BOARD_HEIGHT 480
#define BOARD_BL_PIN 2
#define BOARD_DRAW_LINES 80
#define BOARD_DRAW_IN_PSRAM 1

#define TOUCH_SDA 19
#define TOUCH_SCL 20
#define TOUCH_INT -1
#define TOUCH_RST 38

inline Arduino_GFX *board_make_display() {
    Arduino_ESP32RGBPanel *panel = new Arduino_ESP32RGBPanel(
        41 /* DE */, 40 /* VSYNC */, 39 /* HSYNC */, 42 /* PCLK */,
        14 /* R0 */, 21 /* R1 */, 47 /* R2 */, 48 /* R3 */, 45 /* R4 */,
        9 /* G0 */, 46 /* G1 */, 3 /* G2 */, 8 /* G3 */, 16 /* G4 */, 1 /* G5 */,
        15 /* B0 */, 7 /* B1 */, 6 /* B2 */, 5 /* B3 */, 4 /* B4 */,
        0 /* hsync_polarity */, 180 /* hsync_front_porch */, 30 /* hsync_pulse_width */, 16 /* hsync_back_porch */,
        0 /* vsync_polarity */, 12 /* vsync_front_porch */, 13 /* vsync_pulse_width */, 10 /* vsync_back_porch */,
        1 /* pclk_active_neg */, 16000000 /* prefer_speed */);
    return new Arduino_RGB_Display(BOARD_WIDTH, BOARD_HEIGHT, panel, 0 /* rotation */, true /* auto_flush */);
}
