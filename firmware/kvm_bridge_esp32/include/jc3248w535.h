// Guition JC3248W535C: 3.5" 320x480 panel on an AXS15231B over QSPI, with the capacitive
// touch built into the same chip (I2C 0x3B on SDA 4 / SCL 8). The USB-C port is the
// ESP32-S3's own USB, not a serial chip. Used here in landscape, 480x320.
//
// The panel only takes whole frames, so drawing goes to an off-screen canvas that is pushed
// out with flush(). Display pins are the ones in Arduino_GFX's own definition of this board.
#pragma once
#include <Arduino_GFX_Library.h>
#include <Wire.h>

#define BOARD_NAME "Guition JC3248W535"
#define BOARD_WIDTH 480
#define BOARD_HEIGHT 320
#define BOARD_BL_PIN 1

#define TOUCH_SDA 4
#define TOUCH_SCL 8

// Every GPIO that reaches a socket on the back (P1: 43 44, P2: 5 6 7 15 16 46 9 14,
// P3/P4: 17 18), for the "!probe" wiring check.
#define BOARD_SOCKET_PINS {5, 6, 7, 9, 14, 15, 16, 17, 18, 43, 44, 46}

// Where the link to the target board may be plugged in: the TX/RX pins of P1, or P3/P4.
#define BOARD_LINK_PAIRS {{43, 44}, {17, 18}}

inline void board_init() {}

inline Arduino_GFX *board_make_display() {
    Arduino_DataBus *bus = new Arduino_ESP32QSPI(45 /* CS */, 47 /* SCK */, 21 /* D0 */, 48 /* D1 */, 40 /* D2 */, 39 /* D3 */);
    Arduino_GFX *panel = new Arduino_AXS15231B(bus, GFX_NOT_DEFINED /* RST */, 0 /* rotation */, false /* IPS */, 320, 480);
    return new Arduino_Canvas(320, 480, panel, 0, 0, 1 /* landscape */);
}

/// Is a finger on the glass? Coordinates are not needed: the whole screen is one button.
class BoardTouch {
public:
    bool begin() {
        Wire.begin(TOUCH_SDA, TOUCH_SCL, 400000);
        Wire.beginTransmission(ADDR);
        _ok = Wire.endTransmission() == 0;
        return _ok;
    }

    bool ok() const { return _ok; }

    bool down() {
        if (!_ok) return false;
        // "Read touch points" request: 8 bytes back, enough for one point.
        static const uint8_t request[11] = {0xB5, 0xAB, 0xA5, 0x5A, 0x00, 0x00, 0x00, 0x08, 0x00, 0x00, 0x00};
        Wire.beginTransmission(ADDR);
        Wire.write(request, sizeof request);
        if (Wire.endTransmission() != 0) return false;
        uint8_t d[8];
        if (Wire.requestFrom((int)ADDR, (int)sizeof d) != (int)sizeof d) return false;
        for (uint8_t &b : d) b = Wire.read();
        memcpy(last_raw, d, sizeof d);

        uint8_t points = d[1];
        uint8_t event = d[2] >> 6; // 0 down, 1 lifted, 2 still down
        uint16_t x = ((d[2] & 0x0F) << 8) | d[3];
        uint16_t y = ((d[4] & 0x0F) << 8) | d[5];
        // Anything that does not look like a real point counts as "no finger": a misread
        // must never be able to hold the switch on.
        return points >= 1 && points <= 5 && event != 1 && x < 320 && y < 480;
    }

    uint8_t last_raw[8] = {0}; // most recent answer from the chip, for the "!touch" check

private:
    static const uint8_t ADDR = 0x3B;
    bool _ok = false;
};
#define BOARD_TOUCH_HAS_RAW 1
