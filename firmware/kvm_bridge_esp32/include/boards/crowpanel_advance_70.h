// Elecrow CrowPanel Advance 7.0" HMI (DIS02170A): ESP32-S3-WROOM-1-N16R8, 800x480 IPS over
// 16-bit parallel RGB, GT911 touch on I2C (SDA 15, SCL 16), CH340K USB-UART.
//
// Pins, timings and the start-up dance are copied from Elecrow's own lesson-03 example
// (example/V1.3_and_V1.4_and_V1.5/Arduino/lesson-03/BigInch_LVGL in their GitHub repo):
//   * the backlight is not on a GPIO. V1.2 and later boards have an STC8H1K28 helper MCU at
//     I2C 0x30 that takes one byte: on V1.3+ 0 is brightest and 245 is off (250 wakes the
//     touch controller); on V1.2 0x10 is brightest and 0x05 is off. V1.0 boards have a
//     PCA9557/TCA9534 expander at 0x18 instead (pin 1 backlight, pin 2 touch reset).
//     board_init() probes for both and does the right thing.
//   * GPIO1 is the GT911's INT line; holding it low across the touch reset selects address
//     0x5D. gt911.h probes 0x5D and 0x14 anyway.
#pragma once
#include <Arduino_GFX_Library.h>
#include <Wire.h>

#define BOARD_NAME "CrowPanel Advance 7.0"
#define BOARD_WIDTH 800
#define BOARD_HEIGHT 480
#define BOARD_BL_PIN -1
#define BOARD_DRAW_LINES 40
#define BOARD_DRAW_IN_PSRAM 0
#define BOARD_HAS_INIT 1

#define TOUCH_SDA 15
#define TOUCH_SCL 16
#define TOUCH_INT -1
#define TOUCH_RST -1

static const uint8_t CROW_STC8_ADDR = 0x30;
static const uint8_t CROW_EXPANDER_ADDR = 0x18;

inline bool crow_i2c_present(uint8_t addr) {
    Wire.beginTransmission(addr);
    return Wire.endTransmission() == 0;
}

inline void crow_stc8(uint8_t command) {
    Wire.beginTransmission(CROW_STC8_ADDR);
    Wire.write(command);
    Wire.endTransmission();
}

inline void crow_expander_write(uint8_t reg, uint8_t value) {
    Wire.beginTransmission(CROW_EXPANDER_ADDR);
    Wire.write(reg);
    Wire.write(value);
    Wire.endTransmission();
}

/// Power up the panel: backlight on, touch controller reset with GPIO1 low (address 0x5D).
inline void board_init() {
#ifndef CROW_LEAVE_GPIO19_ALONE // defined by firmware that uses the UART1-OUT port (IO19/IO20)
    pinMode(19, OUTPUT); // as Elecrow's examples do before touching the bus
#endif
    Wire.begin(TOUCH_SDA, TOUCH_SCL);
    delay(50);
    bool stc8 = false, expander = false;
    for (int attempt = 0; attempt < 10 && !(stc8 || expander); attempt++) {
        stc8 = crow_i2c_present(CROW_STC8_ADDR);
        expander = crow_i2c_present(CROW_EXPANDER_ADDR);
        if (!(stc8 || expander)) delay(100);
    }
    if (stc8) {
        // V1.3+: 250 wakes the touch controller; V1.2 used 0x19 for the same. Each is a
        // harmless brightness value on the other revision.
        crow_stc8(250);
        crow_stc8(0x19);
        pinMode(1, OUTPUT);
        digitalWrite(1, LOW);
        delay(120);
        pinMode(1, INPUT);
        delay(100);
        crow_stc8(0x10); // V1.2: brightest
        crow_stc8(0);    // V1.3+: brightest
        Serial.println("CrowPanel: STC8 helper found (V1.2+), backlight on");
    } else if (expander) {
        // V1.0: PCA9557 at 0x18, register 3 = direction (0 = output), 1 = output levels.
        crow_expander_write(0x03, (uint8_t)~0b00011110); // pins 1-4 outputs
        crow_expander_write(0x01, 0b00000010);           // backlight (pin 1) on, touch reset (pin 2) low
        pinMode(1, OUTPUT);
        digitalWrite(1, LOW);
        delay(20);
        crow_expander_write(0x01, 0b00000110); // touch reset high
        delay(100);
        pinMode(1, INPUT);
        Serial.println("CrowPanel: PCA9557 expander found (V1.0), backlight on");
    } else {
        Serial.println("CrowPanel: no backlight controller answered on I2C (0x30 / 0x18)");
    }
}

inline Arduino_GFX *board_make_display() {
    Arduino_ESP32RGBPanel *panel = new Arduino_ESP32RGBPanel(
        42 /* DE */, 41 /* VSYNC */, 40 /* HSYNC */, 39 /* PCLK */,
        7 /* R0 */, 17 /* R1 */, 18 /* R2 */, 3 /* R3 */, 46 /* R4 */,
        9 /* G0 */, 10 /* G1 */, 11 /* G2 */, 12 /* G3 */, 13 /* G4 */, 14 /* G5 */,
        21 /* B0 */, 47 /* B1 */, 48 /* B2 */, 45 /* B3 */, 38 /* B4 */,
        0 /* hsync_polarity */, 8 /* hsync_front_porch */, 4 /* hsync_pulse_width */, 8 /* hsync_back_porch */,
        0 /* vsync_polarity */, 8 /* vsync_front_porch */, 4 /* vsync_pulse_width */, 8 /* vsync_back_porch */,
        0 /* pclk_active_neg */, 16000000 /* prefer_speed */, false /* useBigEndian */,
        0 /* de_idle_high */, 1 /* pclk_idle_high */);
    return new Arduino_RGB_Display(BOARD_WIDTH, BOARD_HEIGHT, panel, 0 /* rotation */, true /* auto_flush */);
}
