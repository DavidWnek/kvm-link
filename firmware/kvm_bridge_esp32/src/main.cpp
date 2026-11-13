// kvm-link bridge on an ESP32-S3 touchscreen panel.
//
// Sits between the controlling PC (USB serial through the panel's USB port) and the
// RP2350 target board (UART on two spare GPIOs), and adds a kill switch: while the
// screen says STOPPED nothing from the PC reaches the other computer's keyboard or
// mouse, and whatever was held is released. The PC cannot switch it back on; only a
// finger held on the screen can.
//
//   Touch anywhere while control is on   -> stop, at once
//   Hold anywhere for a second when off  -> allow control again
//
// The choice survives a reset or power cut. A panel that has never been set up
// starts stopped.

#include <Arduino.h>
#include <Preferences.h>

#include "kvm_board.h"

#define KVM_VERSION 1
#define PC Serial
#define LINK Serial1
#define HOLD_TO_ENABLE_MS 1000
#define QUIET_RELEASE_MS 5000 // PC went silent with something held: let go of it

static Arduino_GFX *gfx;
static BoardTouch touch;
static Preferences prefs;

static bool enabled = false;

// What the target board last told us.
static bool target_alive = false;
static bool host_mounted = false;
static bool host_asleep = false;
static bool target_held = false;

static uint32_t passed = 0, blocked = 0;
static char last_cmd[24] = "";
static uint32_t last_pc_ms = 0;

// The link wires are easy to cross or to put on the wrong socket, so the bridge works out
// where they are. The target board's TX idles high and is the only thing that can hold a pin
// high against a pull-down, so whichever pin of a pair is held high is the one to listen on,
// and its partner is the one to talk on. The arrangement that gets an answer is remembered.
struct LinkPair {
    int8_t a, b;
};
#ifdef BOARD_LINK_PAIRS
static const LinkPair link_pairs[] = BOARD_LINK_PAIRS;
#else
static const LinkPair link_pairs[] = {{KVM_LINK_TX, KVM_LINK_RX}};
#endif

static int8_t link_rx = KVM_LINK_RX, link_tx = KVM_LINK_TX; // in use now
static bool link_known = false;                             // an arrangement has worked before
static int8_t known_rx = -1, known_tx = -1;
static uint32_t link_since = 0; // when the current arrangement was last (re)started

static void link_begin(int8_t rx, int8_t tx) {
    LINK.end();
    pinMode(link_tx, INPUT_PULLUP); // stop driving whichever pin was TX before
    pinMode(link_rx, INPUT_PULLUP);
    pinMode(tx, INPUT_PULLUP);
    pinMode(rx, INPUT_PULLUP); // idle high while the target board is unpowered
    LINK.begin(115200, SERIAL_8N1, rx, tx);
    link_rx = rx;
    link_tx = tx;
    link_since = millis();
}

static bool held_high(int8_t pin) {
    pinMode(pin, INPUT_PULLDOWN);
    delay(3);
    bool high = digitalRead(pin);
    pinMode(pin, INPUT_PULLUP);
    return high;
}

static void link_search() {
    LINK.end();
    for (const LinkPair &p : link_pairs) {
        bool a = held_high(p.a), b = held_high(p.b);
        if (a != b) {
            link_begin(a ? p.a : p.b, a ? p.b : p.a);
            return;
        }
    }
    // Nothing conclusive (nothing connected, or the board pulls both pins up itself): try
    // the current pair the other way round.
    link_begin(link_tx, link_rx);
}

static bool link_pins_valid(int8_t rx, int8_t tx) {
    for (const LinkPair &p : link_pairs)
        if ((p.a == rx && p.b == tx) || (p.a == tx && p.b == rx)) return true;
    return false;
}

// ---- framing -------------------------------------------------------------------------------

static void send_framed(Stream &out, const char *payload) {
    uint8_t sum = 0;
    for (const char *p = payload; *p; p++) sum ^= (uint8_t)*p;
    out.printf("%s*%02X\n", payload, sum);
}

// ---- who is waiting for the target's next reply ---------------------------------------------
// The target answers every line with exactly one line, in order, so a queue of who asked is
// enough to route replies between the PC and this firmware's own status polls.

enum Origin : uint8_t { FROM_PC, FROM_SELF };
static const uint8_t QUEUE_MAX = 8;
static Origin waiting[QUEUE_MAX];
static uint8_t waiting_head = 0, waiting_count = 0;
static uint32_t waiting_since = 0;

static bool queue_push(Origin o) {
    if (waiting_count >= QUEUE_MAX) return false;
    if (waiting_count == 0) waiting_since = millis();
    waiting[(waiting_head + waiting_count++) % QUEUE_MAX] = o;
    return true;
}

static void send_self(const char *payload) {
    if (queue_push(FROM_SELF)) send_framed(LINK, payload);
}

// ---- screen --------------------------------------------------------------------------------

#define RGB(r, g, b) ((uint16_t)((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3)))
static const uint16_t COL_BG = RGB(14, 16, 22);
static const uint16_t COL_ON = RGB(20, 130, 60);
static const uint16_t COL_OFF = RGB(185, 30, 30);
static const uint16_t COL_TEXT = RGB(255, 255, 255);
static const uint16_t COL_DIM = RGB(150, 156, 170);
static const uint16_t COL_WARN = RGB(255, 190, 60);

static const int PANEL_H = BOARD_HEIGHT * 6 / 10;
static const int BIG = BOARD_WIDTH / 88;                              // 9 on 800 wide, 5 on 480
static const int SMALL = BOARD_WIDTH / 266 < 2 ? 2 : BOARD_WIDTH / 266; // 3 on 800 wide, 2 on 480
static const int BAR_H = 6 * SMALL;

static bool panel_dirty = true, status_dirty = true;
static bool unflushed = false; // drawn since the last push to the glass (canvas-backed panels)
static int bar_shown = 0;

static void text_centered(const char *s, int y, int size, uint16_t fg, uint16_t bg) {
    gfx->setTextSize(size);
    gfx->setTextColor(fg, bg);
    gfx->setCursor((BOARD_WIDTH - (int)strlen(s) * 6 * size) / 2, y);
    gfx->print(s);
}

static void draw_panel() {
    uint16_t col = enabled ? COL_ON : COL_OFF;
    gfx->fillRect(0, 0, BOARD_WIDTH, PANEL_H, col);
    int title_y = PANEL_H / 2 - 8 * BIG / 2 - 6 * SMALL;
    text_centered(enabled ? "CONTROL ON" : "STOPPED", title_y, BIG, COL_TEXT, col);
    const char *hint = !touch.ok() ? "touch not working: stays off"
                       : enabled   ? "touch anywhere to stop"
                                   : "hold anywhere to allow control";
    text_centered(hint, title_y + 8 * BIG + 4 * SMALL, SMALL, COL_TEXT, col);
    bar_shown = 0;
}

// Progress of the hold that switches control back on, along the bottom of the panel.
static void draw_hold_bar(int width) {
    if (width == bar_shown) return;
    int y = PANEL_H - BAR_H;
    if (width > bar_shown) gfx->fillRect(bar_shown, y, width - bar_shown, BAR_H, COL_TEXT);
    else gfx->fillRect(width, y, bar_shown - width, BAR_H, COL_OFF);
    bar_shown = width;
    unflushed = true;
}

static void status_line(int row, const char *s, uint16_t fg) {
    int line_h = 8 * SMALL + 3 * SMALL;
    int y = PANEL_H + 4 * SMALL + row * line_h;
    gfx->fillRect(0, y, BOARD_WIDTH, 8 * SMALL, COL_BG);
    gfx->setTextSize(SMALL);
    gfx->setTextColor(fg, COL_BG);
    gfx->setCursor(4 * SMALL, y);
    gfx->print(s);
}

static void draw_status() {
    char buf[48];
    if (!target_alive) {
        status_line(0, "Keyboard board: no answer (wires?)", COL_WARN);
        status_line(1, "Other computer: unknown", COL_DIM);
    } else {
        snprintf(buf, sizeof buf, "Keyboard board: connected, TX%d RX%d", link_tx, link_rx);
        status_line(0, buf, COL_TEXT);
        if (!host_mounted) status_line(1, "Other computer: not plugged in / off", COL_WARN);
        else if (host_asleep) status_line(1, "Other computer: asleep", COL_DIM);
        else status_line(1, "Other computer: ready", COL_TEXT);
    }
    snprintf(buf, sizeof buf, "Passed %lu   blocked %lu", (unsigned long)passed, (unsigned long)blocked);
    status_line(2, buf, COL_DIM);
    snprintf(buf, sizeof buf, "Last: %s", last_cmd[0] ? last_cmd : "-");
    status_line(3, buf, COL_DIM);
}

// ---- kill switch ---------------------------------------------------------------------------

static void set_enabled(bool on) {
    if (on == enabled) return;
    enabled = on;
    prefs.putBool("on", on);
    if (!on) send_self("Z"); // let go of everything on the other computer
    panel_dirty = status_dirty = true;
}

static void poll_touch() {
    static uint32_t last_poll = 0, down_since = 0;
    // Nothing counts until the screen has been seen untouched once, so a controller that
    // reports a finger from power-up cannot switch control on by itself.
    static bool was_down = false, need_release = true;
    if (millis() - last_poll < 15) return;
    last_poll = millis();

    // Some touch chips only have a point to report every so often and answer "nothing" in
    // between, so a finger counts as lifted only after a short run of empty reads.
    static uint32_t last_seen = 0;
    static bool down = false;
    if (touch.down()) {
        last_seen = millis();
        down = true;
    } else if (down && millis() - last_seen > 150) {
        down = false;
    }
    if (down && !was_down) down_since = millis();
    was_down = down;

    if (!down) {
        need_release = false;
        if (!enabled) draw_hold_bar(0);
        return;
    }
    if (need_release) return;

    if (enabled) {
        set_enabled(false);
        need_release = true; // the same touch must not start switching it back on
    } else {
        uint32_t held = millis() - down_since;
        if (held >= HOLD_TO_ENABLE_MS) {
            set_enabled(true);
            need_release = true;
        } else {
            draw_hold_bar((int)((uint64_t)BOARD_WIDTH * held / HOLD_TO_ENABLE_MS));
        }
    }
}

// ---- PC side -------------------------------------------------------------------------------

// Wiring check: with the UART let go, see what is holding each socket pin. The target
// board's TX idles high and is the only thing that can hold a pin high against a pull-down,
// so "high" marks where that wire really landed.
static void probe_pins() {
#ifdef BOARD_SOCKET_PINS
    static const int8_t pins[] = BOARD_SOCKET_PINS;
#else
    static const int8_t pins[] = {KVM_LINK_RX, KVM_LINK_TX};
#endif
    LINK.end();
    PC.print("!probe");
    for (int8_t pin : pins) {
        pinMode(pin, INPUT_PULLDOWN);
        delay(5);
        bool with_down = digitalRead(pin);
        pinMode(pin, INPUT_PULLUP);
        delay(5);
        bool with_up = digitalRead(pin);
        pinMode(pin, INPUT);
        PC.printf(" %d=%s", pin, with_down ? "high" : with_up ? "open" : "low");
    }
    PC.print("\n");
    waiting_count = 0;
    link_begin(link_rx, link_tx);
}

static void handle_pc_line(char *line, size_t len) {
    if (strcmp(line, "!probe") == 0) {
        probe_pins();
        return;
    }
    if (strcmp(line, "!touch") == 0) { // what the touch chip is saying right now
        PC.printf("!touch ok=%d", touch.ok() ? 1 : 0);
#ifdef BOARD_TOUCH_HAS_RAW
        for (uint8_t b : touch.last_raw) PC.printf(" %02X", b);
#endif
        PC.print("\n");
        return;
    }
    if (line[0] == '!') {
        PC.printf("!kvm-link bridge %d esp32 enabled=%d link=%d tx=%d rx=%d\n", KVM_VERSION,
                  enabled ? 1 : 0, target_alive ? 1 : 0, link_tx, link_rx);
        return;
    }
    last_pc_ms = millis();
    bool is_status = (line[0] == 'S' && line[1] == '*'); // reading status is always allowed
    if (!enabled && !is_status) {
        blocked++;
        status_dirty = true;
        send_framed(PC, "-killed");
        return;
    }
    if (!queue_push(FROM_PC)) {
        send_framed(PC, "-busy");
        return;
    }
    LINK.write((const uint8_t *)line, len);
    LINK.write('\n');
    if (!is_status) {
        passed++;
        size_t n = 0;
        while (n < sizeof(last_cmd) - 1 && line[n] && line[n] != '*') {
            last_cmd[n] = line[n];
            n++;
        }
        last_cmd[n] = 0;
        status_dirty = true;
    }
}

// ---- target side ---------------------------------------------------------------------------

static int field(const char *reply, const char *key) {
    const char *p = strstr(reply, key);
    return p ? atoi(p + strlen(key)) : 0;
}

static void handle_target_line(char *line, size_t len) {
    if (waiting_count == 0) return; // nobody asked
    Origin who = waiting[waiting_head];
    waiting_head = (waiting_head + 1) % QUEUE_MAX;
    waiting_count--;
    waiting_since = millis();

    if (!target_alive) {
        target_alive = true;
        status_dirty = true;
        if (!link_known || known_rx != link_rx || known_tx != link_tx) {
            link_known = true;
            known_rx = link_rx;
            known_tx = link_tx;
            prefs.putChar("rx", link_rx);
            prefs.putChar("tx", link_tx);
        }
    }
    if (who == FROM_PC) {
        PC.write((const uint8_t *)line, len);
        PC.write('\n');
        return;
    }
    if (strncmp(line, "+S ", 3) == 0) {
        bool mounted = field(line, " m=") == 1, asleep = field(line, " s=") == 1;
        if (mounted != host_mounted || asleep != host_asleep) status_dirty = true;
        host_mounted = mounted;
        host_asleep = asleep;
        target_held = field(line, " held=") == 1;
    }
}

template <size_t N> struct LineReader {
    char buf[N];
    size_t len = 0;
    bool overflow = false;

    template <typename Fn> void pump(Stream &in, Fn on_line) {
        while (in.available()) {
            int c = in.read();
            if (c == '\r') continue;
            if (c == '\n') {
                if (!overflow && len) {
                    buf[len] = 0;
                    on_line(buf, len);
                }
                len = 0;
                overflow = false;
            } else if (len < N - 1) {
                buf[len++] = (char)c;
            } else {
                overflow = true;
            }
        }
    }
};

static LineReader<128> pc_reader;
static LineReader<128> target_reader;

// ---- Arduino -------------------------------------------------------------------------------

void setup() {
    PC.begin(115200);

    prefs.begin("kvm-link", false);
    enabled = prefs.getBool("on", false);

    board_init(); // boards with an I2C backlight / touch-reset helper power up here
    gfx = board_make_display();
    gfx->begin();
    gfx->fillScreen(COL_BG);
#if BOARD_BL_PIN >= 0
    pinMode(BOARD_BL_PIN, OUTPUT);
    digitalWrite(BOARD_BL_PIN, HIGH);
#endif

    // Without a working touchscreen there is no way to stop, so do not start.
    if (!touch.begin()) enabled = false;

    known_rx = prefs.getChar("rx", -1);
    known_tx = prefs.getChar("tx", -1);
    link_known = link_pins_valid(known_rx, known_tx);
    if (link_known) link_begin(known_rx, known_tx);
    else link_search();

    if (!enabled) send_self("Z");
}

void loop() {
    pc_reader.pump(PC, handle_pc_line);
    target_reader.pump(LINK, handle_target_line);
    if (touch.ok()) poll_touch();

    // The target stopped answering: forget who was waiting and say so on screen.
    if (waiting_count && millis() - waiting_since > 1500) {
        waiting_count = 0;
        if (target_alive) {
            status_dirty = true;
            link_since = millis(); // give the arrangement that was working its full turn
        }
        target_alive = false;
    }

    // Silent target: look for the wires again. An arrangement that has worked before gets a
    // long turn before it is given up on.
    if (!target_alive) {
        bool on_known = link_known && link_rx == known_rx && link_tx == known_tx;
        if (millis() - link_since > (on_known ? 20000u : 3500u)) {
            waiting_count = 0;
            link_search();
        }
    }

    // Ask the target how it is once a second, when the PC is not mid-conversation.
    static uint32_t last_poll = 0;
    if (waiting_count == 0 && millis() - last_poll >= 1000 && millis() - last_pc_ms >= 300) {
        last_poll = millis();
        if (target_held && (!enabled || millis() - last_pc_ms > QUIET_RELEASE_MS)) {
            target_held = false;
            send_self("Z");
        }
        send_self("S");
    }

    static uint32_t last_draw = 0, last_flush = 0;
    if (panel_dirty) {
        panel_dirty = false;
        draw_panel();
        unflushed = true;
    }
    if (status_dirty && millis() - last_draw >= 150) {
        status_dirty = false;
        last_draw = millis();
        draw_status();
        unflushed = true;
    }
    // A no-op on panels drawn directly; on canvas-backed ones this sends the whole frame,
    // so not more than about 25 times a second.
    if (unflushed && millis() - last_flush >= 40) {
        unflushed = false;
        gfx->flush();
        last_flush = millis();
    }
    delay(1);
}
