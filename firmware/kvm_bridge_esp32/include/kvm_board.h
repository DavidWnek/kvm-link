// Board selection for the bridge. Every board provides BOARD_WIDTH/HEIGHT, BOARD_BL_PIN,
// board_init(), board_make_display() and a BoardTouch with begin(), ok() and down().
//
// The JC3248W535 has its own header. Every other panel is picked by board.h from
// include/boards; those all use a GT911 touch controller.
#pragma once

#if defined(BOARD_JC3248W535)
#include "jc3248w535.h"
#else
#include "board.h"
#include "gt911.h"

class BoardTouch {
public:
    bool begin() { return _gt.begin(TOUCH_SDA, TOUCH_SCL, TOUCH_INT, TOUCH_RST); }
    bool ok() const { return _gt.ok(); }
    bool down() {
        int16_t x, y;
        return _gt.read(x, y);
    }

private:
    Gt911 _gt;
};
#endif
