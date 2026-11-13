// Board selection. One BOARD_* flag comes from platformio.ini; it picks a header that defines:
//
//   BOARD_NAME            printed on the setup screen
//   BOARD_WIDTH/HEIGHT    panel size in landscape
//   BOARD_BL_PIN          backlight GPIO (-1 for none / always on)
//   BOARD_DRAW_LINES      LVGL partial draw buffer height (lines)
//   BOARD_DRAW_IN_PSRAM   1 to put the draw buffers in PSRAM (RGB panels), 0 for internal DMA RAM
//   TOUCH_SDA/SCL/INT/RST GT911 pins
//   TOUCH_SWAP_XY, TOUCH_MIRROR_X, TOUCH_MIRROR_Y   0/1 touch orientation fixes
//   Arduino_GFX *board_make_display()   constructs the Arduino_GFX display
//   board_init()          optional (BOARD_HAS_INIT 1): power sequencing before the display
//
// Pin maps come from the community-maintained Arduino_GFX examples for each panel. If a board
// shows nothing or touch lands mirrored, that header is the one place to fix.

#pragma once

#if defined(BOARD_CROWPANEL_ADVANCE_70)
#include "boards/crowpanel_advance_70.h"
#elif defined(BOARD_JC8048W550)
#include "boards/jc8048w550.h"
#elif defined(BOARD_JC4827W543)
#include "boards/jc4827w543.h"
#elif defined(BOARD_SUNTON_8048S070)
#include "boards/sunton_8048s070.h"
#elif defined(BOARD_SUNTON_8048S050)
#include "boards/sunton_8048s050.h"
#elif defined(BOARD_SUNTON_4827S043)
#include "boards/sunton_4827s043.h"
#else
#error "No board selected: build with -D BOARD_CROWPANEL_ADVANCE_70 (or another include/boards/*.h), see platformio.ini"
#endif

#ifndef BOARD_HAS_INIT
inline void board_init() {}
#endif

#ifndef TOUCH_SWAP_XY
#define TOUCH_SWAP_XY 0
#endif
#ifndef TOUCH_MIRROR_X
#define TOUCH_MIRROR_X 0
#endif
#ifndef TOUCH_MIRROR_Y
#define TOUCH_MIRROR_Y 0
#endif
