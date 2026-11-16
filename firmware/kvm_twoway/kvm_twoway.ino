// kvm-link, both sides in one.
//
// The same image goes on both RP2350 boards. Each board is, to the computer it
// is plugged into, a USB serial port *and* a keyboard plus absolute pointer.
// A command written to one board's serial port is carried over the link wires
// and comes out of the other board as key presses and pointer moves, so either
// computer can drive the other and it does not matter which board goes where.
//
// Build with the Adafruit TinyUSB stack:
//   FQBN rp2040:rp2040:waveshare_rp2350_zero:usbstack=tinyusb
//
// Wiring between the two boards (3 connections, nothing else). The link is on
// the short edge opposite the USB socket:
//   GP13 (TX) -> other board GP9 (RX)
//   GP9 (RX)  <- other board GP13 (TX)
//   GND       -- other board GND
//
// The pins are chosen so that two RP2350-Zero boards can be soldered together
// end to end, both face up with their USB sockets pointing away from each
// other, each pad to the one it meets. The edge then reads
//
//   GP13  GP12  GP11  GP10  GP9      one board
//   GP9   GP10  GP11  GP12  GP13     the other, turned half round
//
// so each board's GP13 lands on the other's GP9, and those two corner joints
// are the link. The three joints between them only hold the boards together;
// their pins are never driven.
//
// Receiving is a hardware UART (UART1, RX on GP9), for its buffer. No pad that
// meets GP9 can be a hardware UART's TX, so sending is a PIO UART on GP13.
//
// Protocol on the serial port: the same as kvm_bridge. Lines starting with '!'
// are answered here; every other line is passed to the other board.
//
// Protocol on the wires: the same as kvm_target, in both directions at once.
// "<payload>*<XX>\n", XX the XOR of the payload bytes in hex. A line whose
// payload starts with '+' or '-' is the other board's reply to something this
// board's computer sent, and goes to the serial port. Anything else is a
// command for this board's keyboard and pointer, and gets exactly one reply.
//
//   K mm [k1..k6]      keyboard report: modifier byte, up to six key usages
//   M bb xxxx yyyy ww  pointer report: buttons, X, Y (0..7FFF), wheel (int8)
//   B bb ww            buttons and wheel at the current pointer position
//   Z                  release every key and button
//   W                  ask a suspended host to wake
//   N [text]           call the USB device <text> from now on (kept across power
//                      cycles); no text goes back to the built-in names
//   S                  status

#include "Adafruit_TinyUSB.h"
#include <EEPROM.h>
#include <hardware/gpio.h>

#define KVM_VERSION 2
#define PC Serial    // USB serial port on this board's computer
#define LINK_OUT link_tx // PIO UART, sending to the other board
#define LINK_IN Serial2  // UART1, receiving from it
#define UART_BAUD 115200
#define PIN_UART_TX 13
#define PIN_UART_RX 9
#define HOLD_TIMEOUT_MS 5000 // drop held keys if the controller goes quiet

// What the computer is told this device is called. The USB stack reads these
// buffers each time the computer asks, so writing a new name into them and
// rejoining the bus is all a rename takes. 32 characters is the most the
// stack will send.
#define NAME_MAX 32
#define DEFAULT_PRODUCT "kvm-link two-way"
#define DEFAULT_KEYBOARD "kvm-link keyboard"
#define DEFAULT_POINTER "kvm-link pointer"
#define NAME_MAGIC 0x4E4D564BUL // "KVMN": a name has been stored

struct StoredName {
  uint32_t magic;
  char name[NAME_MAX + 1];
};

struct LineReader {
  char buf[128];
  size_t len;
  bool overflow;
};

static const uint8_t desc_keyboard[] = {TUD_HID_REPORT_DESC_KEYBOARD()};

// Buttons(5) + pad(3), X and Y absolute 0..32767, wheel relative.
static const uint8_t desc_pointer[] = {
    0x05, 0x01,       // Usage Page (Generic Desktop)
    0x09, 0x02,       // Usage (Mouse)
    0xA1, 0x01,       // Collection (Application)
    0x09, 0x01,       //   Usage (Pointer)
    0xA1, 0x00,       //   Collection (Physical)
    0x05, 0x09,       //     Usage Page (Button)
    0x19, 0x01,       //     Usage Minimum (1)
    0x29, 0x05,       //     Usage Maximum (5)
    0x15, 0x00,       //     Logical Minimum (0)
    0x25, 0x01,       //     Logical Maximum (1)
    0x95, 0x05,       //     Report Count (5)
    0x75, 0x01,       //     Report Size (1)
    0x81, 0x02,       //     Input (Data, Variable, Absolute)
    0x95, 0x01,       //     Report Count (1)
    0x75, 0x03,       //     Report Size (3)
    0x81, 0x03,       //     Input (Constant)
    0x05, 0x01,       //     Usage Page (Generic Desktop)
    0x09, 0x30,       //     Usage (X)
    0x09, 0x31,       //     Usage (Y)
    0x15, 0x00,       //     Logical Minimum (0)
    0x26, 0xFF, 0x7F, //     Logical Maximum (32767)
    0x35, 0x00,       //     Physical Minimum (0)
    0x46, 0xFF, 0x7F, //     Physical Maximum (32767)
    0x75, 0x10,       //     Report Size (16)
    0x95, 0x02,       //     Report Count (2)
    0x81, 0x02,       //     Input (Data, Variable, Absolute)
    0x09, 0x38,       //     Usage (Wheel)
    0x15, 0x81,       //     Logical Minimum (-127)
    0x25, 0x7F,       //     Logical Maximum (127)
    0x35, 0x00,       //     Physical Minimum (0)
    0x45, 0x00,       //     Physical Maximum (0)
    0x75, 0x08,       //     Report Size (8)
    0x95, 0x01,       //     Report Count (1)
    0x81, 0x06,       //     Input (Data, Variable, Relative)
    0xC0,             //   End Collection
    0xC0,             // End Collection
};

static Adafruit_USBD_HID usb_keyboard;
static Adafruit_USBD_HID usb_pointer;

static uint8_t kbd_report[8]; // modifiers, reserved, six keys
static uint8_t ptr_buttons = 0;
static uint16_t ptr_x = 0x3FFF, ptr_y = 0x3FFF;
static volatile uint8_t kbd_leds = 0;
static uint32_t last_cmd_ms = 0;

static char name_product[NAME_MAX + 1] = DEFAULT_PRODUCT;
static char name_keyboard[NAME_MAX + 1] = DEFAULT_KEYBOARD;
static char name_pointer[NAME_MAX + 1] = DEFAULT_POINTER;
static bool name_custom = false;

static SerialPIO link_tx(PIN_UART_TX, NOPIN); // send only
static LineReader pc_reader, link_reader;

static void on_keyboard_set_report(uint8_t report_id, hid_report_type_t type,
                                   uint8_t const *buffer, uint16_t bufsize) {
  (void)report_id;
  if (type == HID_REPORT_TYPE_OUTPUT && bufsize >= 1) {
    kbd_leds = buffer[0]; // bit0 Num, bit1 Caps, bit2 Scroll
  }
}

// ---- this board as the other computer's keyboard and pointer ---------------

static void reply(const char *payload) {
  uint8_t sum = 0;
  for (const char *p = payload; *p; p++) {
    sum ^= (uint8_t)*p;
  }
  LINK_OUT.printf("%s*%02X\n", payload, sum);
}

// Queue one report and wait until the host has collected it, so the reply to
// the controller doubles as pacing. Returns a failure reason or NULL.
static const char *send_report(Adafruit_USBD_HID &hid, const void *report,
                               uint8_t len) {
  if (!TinyUSBDevice.mounted()) {
    return "-nohost";
  }
  if (TinyUSBDevice.suspended()) {
    TinyUSBDevice.remoteWakeup();
  }
  uint32_t t0 = millis();
  while (!hid.ready()) {
    if (millis() - t0 > 300) {
      return TinyUSBDevice.suspended() ? "-suspended" : "-busy";
    }
    delay(1);
  }
  if (!hid.sendReport(0, report, len)) {
    return "-busy";
  }
  t0 = millis();
  while (!hid.ready() && millis() - t0 < 300) {
    delay(1);
  }
  return NULL;
}

static const char *send_keyboard(void) {
  return send_report(usb_keyboard, kbd_report, sizeof(kbd_report));
}

static const char *send_pointer(int8_t wheel) {
  uint8_t r[6] = {ptr_buttons,
                  (uint8_t)(ptr_x & 0xFF), (uint8_t)(ptr_x >> 8),
                  (uint8_t)(ptr_y & 0xFF), (uint8_t)(ptr_y >> 8),
                  (uint8_t)wheel};
  return send_report(usb_pointer, r, sizeof(r));
}

static bool anything_held(void) {
  if (ptr_buttons) {
    return true;
  }
  for (uint8_t i = 0; i < sizeof(kbd_report); i++) {
    if (kbd_report[i]) {
      return true;
    }
  }
  return false;
}

static const char *release_all(void) {
  memset(kbd_report, 0, sizeof(kbd_report));
  ptr_buttons = 0;
  const char *e1 = send_keyboard();
  const char *e2 = send_pointer(0);
  return e1 ? e1 : e2;
}

// ---- the name the computer sees ---------------------------------------------

// 1 to 32 printable ASCII characters.
static bool valid_name(const char *name) {
  size_t n = strlen(name);
  if (n < 1 || n > NAME_MAX) {
    return false;
  }
  for (const char *p = name; *p; p++) {
    if (*p < 0x20 || *p > 0x7E) {
      return false;
    }
  }
  return true;
}

// A given name is used for the device and for both of its parts, because an
// operating system may show any of the three. NULL: the built-in names.
static void use_name(const char *name) {
  strcpy(name_product, name ? name : DEFAULT_PRODUCT);
  strcpy(name_keyboard, name ? name : DEFAULT_KEYBOARD);
  strcpy(name_pointer, name ? name : DEFAULT_POINTER);
  name_custom = name != NULL;
}

static void load_name(void) {
  StoredName s;
  EEPROM.begin(sizeof(s));
  EEPROM.get(0, s);
  s.name[NAME_MAX] = 0;
  if (s.magic == NAME_MAGIC && valid_name(s.name)) {
    use_name(s.name);
  }
}

// Keep the name (NULL: none) for the next power-up.
static bool store_name(const char *name) {
  StoredName s;
  memset(&s, 0, sizeof(s));
  if (name) {
    s.magic = NAME_MAGIC;
    strcpy(s.name, name);
  }
  EEPROM.put(0, s);
  return EEPROM.commit();
}

// Leave the bus and come back, so the computer asks who this is again.
static void rejoin_usb(void) {
  memset(kbd_report, 0, sizeof(kbd_report));
  ptr_buttons = 0;
  TinyUSBDevice.detach();
  delay(250); // long enough for the host to notice the disconnect
  TinyUSBDevice.attach();
}

// Parse up to `max` space-separated hex numbers. Returns the count, or -1 on
// anything that is not hex.
static int parse_hex_fields(const char *s, uint32_t *out, int max) {
  int n = 0;
  while (*s) {
    while (*s == ' ') {
      s++;
    }
    if (!*s) {
      break;
    }
    if (n >= max) {
      return -1;
    }
    char *end;
    out[n++] = strtoul(s, &end, 16);
    if (end == s || (*end && *end != ' ')) {
      return -1;
    }
    s = end;
  }
  return n;
}

static void handle_command(char *line) {
  char *star = strrchr(line, '*');
  if (!star || strlen(star + 1) != 2) {
    reply("-frame");
    return;
  }
  uint8_t sum = 0;
  for (char *p = line; p < star; p++) {
    sum ^= (uint8_t)*p;
  }
  char *end;
  unsigned long want = strtoul(star + 1, &end, 16);
  if (*end || want != sum) {
    reply("-sum");
    return;
  }
  *star = 0;
  if (!line[0]) {
    reply("-empty");
    return;
  }

  last_cmd_ms = millis();
  uint32_t f[8];
  const char *err = NULL;

  switch (line[0]) {
  case 'K': {
    int n = parse_hex_fields(line + 1, f, 7);
    if (n < 1) {
      reply("-args");
      return;
    }
    memset(kbd_report, 0, sizeof(kbd_report));
    kbd_report[0] = (uint8_t)f[0];
    for (int i = 1; i < n; i++) {
      kbd_report[1 + i] = (uint8_t)f[i];
    }
    err = send_keyboard();
    break;
  }
  case 'M': {
    if (parse_hex_fields(line + 1, f, 4) != 4) {
      reply("-args");
      return;
    }
    ptr_buttons = (uint8_t)(f[0] & 0x1F);
    ptr_x = (uint16_t)(f[1] > 0x7FFF ? 0x7FFF : f[1]);
    ptr_y = (uint16_t)(f[2] > 0x7FFF ? 0x7FFF : f[2]);
    err = send_pointer((int8_t)(uint8_t)f[3]);
    break;
  }
  case 'B': {
    if (parse_hex_fields(line + 1, f, 2) != 2) {
      reply("-args");
      return;
    }
    ptr_buttons = (uint8_t)(f[0] & 0x1F);
    err = send_pointer((int8_t)(uint8_t)f[1]);
    break;
  }
  case 'Z':
    err = release_all();
    break;
  case 'W':
    if (!TinyUSBDevice.suspended()) {
      err = TinyUSBDevice.mounted() ? NULL : "-nohost";
    } else if (!TinyUSBDevice.remoteWakeup()) {
      err = "-nowake"; // host did not arm remote wakeup before sleeping
    }
    break;
  case 'N': {
    char *name = line + 1;
    while (*name == ' ') {
      name++;
    }
    size_t n = strlen(name);
    while (n && name[n - 1] == ' ') {
      name[--n] = 0;
    }
    if (!n) {
      name = NULL; // back to the built-in names
    } else if (!valid_name(name)) {
      reply("-args");
      return;
    }
    if (name ? (name_custom && strcmp(name, name_product) == 0) : !name_custom) {
      break; // already called that
    }
    if (!store_name(name)) {
      reply("-store");
      return;
    }
    // Answer first: the computer takes a second or two to pick the device up
    // again, and until then every report is refused with -nohost.
    reply("+");
    LINK_OUT.flush();
    use_name(name);
    rejoin_usb();
    return;
  }
  case 'S': {
    // The name comes last because it may have spaces in it.
    char buf[128];
    snprintf(buf, sizeof(buf), "+S v=%d m=%d s=%d leds=%02X held=%d up=%lu n=%s",
             KVM_VERSION, TinyUSBDevice.mounted() ? 1 : 0,
             TinyUSBDevice.suspended() ? 1 : 0, kbd_leds,
             anything_held() ? 1 : 0, (unsigned long)(millis() / 1000),
             name_product);
    reply(buf);
    return;
  }
  default:
    reply("-cmd");
    return;
  }

  reply(err ? err : "+");
}

// ---- lines in ---------------------------------------------------------------

// Collect bytes until a whole line is in r.buf. Returns true once per line;
// too_long says the line did not fit and r.buf holds only its start. Empty
// lines are skipped.
static bool read_line(Stream &in, LineReader &r, bool &too_long) {
  while (in.available()) {
    int c = in.read();
    if (c == '\r') {
      continue;
    }
    if (c == '\n') {
      bool got = r.len > 0 || r.overflow;
      too_long = r.overflow;
      r.buf[r.len] = 0;
      r.len = 0;
      r.overflow = false;
      if (got) {
        return true;
      }
    } else if (r.len < sizeof(r.buf) - 1) {
      r.buf[r.len++] = (char)c;
    } else {
      r.overflow = true;
    }
  }
  return false;
}

// Boards whose only light is an addressable LED (RP2350-Zero) get no indicator.
static void status_led(bool on) {
#ifdef LED_BUILTIN
  digitalWrite(LED_BUILTIN, on ? HIGH : LOW);
#else
  (void)on;
#endif
}

void setup() {
#ifdef LED_BUILTIN
  pinMode(LED_BUILTIN, OUTPUT);
#endif

  // The core has already started USB with its serial port. Drop off the bus,
  // add the keyboard and the pointer next to it, and come back as all three.
  if (!TinyUSBDevice.isInitialized()) {
    TinyUSBDevice.begin(0);
  }
  TinyUSBDevice.detach();
  TinyUSBDevice.setID(0x1209, 0x0002); // pid.codes test ID
  TinyUSBDevice.setManufacturerDescriptor("kvm-link");
  load_name();
  TinyUSBDevice.setProductDescriptor(name_product);

  usb_keyboard.setBootProtocol(HID_ITF_PROTOCOL_KEYBOARD);
  usb_keyboard.setPollInterval(2);
  usb_keyboard.setReportDescriptor(desc_keyboard, sizeof(desc_keyboard));
  usb_keyboard.setStringDescriptor(name_keyboard);
  usb_keyboard.setReportCallback(NULL, on_keyboard_set_report);
  usb_keyboard.begin();

  usb_pointer.setBootProtocol(HID_ITF_PROTOCOL_NONE);
  usb_pointer.setPollInterval(2);
  usb_pointer.setReportDescriptor(desc_pointer, sizeof(desc_pointer));
  usb_pointer.setStringDescriptor(name_pointer);
  usb_pointer.begin();

  delay(250); // long enough for the host to notice the disconnect
  TinyUSBDevice.attach();

  LINK_OUT.begin(UART_BAUD);

  LINK_IN.setTX(UART_PIN_NOT_DEFINED);
  LINK_IN.setRX(PIN_UART_RX);
  LINK_IN.setFIFOSize(512);
  LINK_IN.begin(UART_BAUD);
  gpio_pull_up(PIN_UART_RX); // idle high while the other board is unpowered
}

void loop() {
  bool too_long;

  // This computer -> the other board, one whole line at a time.
  while (read_line(PC, pc_reader, too_long)) {
    if (too_long) {
      continue;
    }
    if (pc_reader.buf[0] == '!') {
      PC.printf("!kvm-link bridge %d twoway\n", KVM_VERSION);
      PC.flush();
    } else {
      LINK_OUT.print(pc_reader.buf);
      LINK_OUT.write('\n');
    }
  }

  // The other board -> here: its replies go to this computer (and are dropped
  // when nobody has the port open), its commands go to the keyboard and pointer.
  while (read_line(LINK_IN, link_reader, too_long)) {
    char first = link_reader.buf[0];
    if (first == '+' || first == '-') {
      // Only when it fits: a program that holds the port open without reading
      // must not be able to stall this board's keyboard duty.
      size_t n = strlen(link_reader.buf);
      if (!too_long && (size_t)PC.availableForWrite() > n) {
        PC.write((const uint8_t *)link_reader.buf, n);
        PC.write('\n');
        PC.flush();
      }
    } else if (too_long) {
      reply("-long");
    } else {
      handle_command(link_reader.buf);
    }
  }

  if (anything_held() && millis() - last_cmd_ms > HOLD_TIMEOUT_MS) {
    release_all();
    last_cmd_ms = millis();
  }

  // Solid: this board's computer has enumerated us. Slow blink: it has not.
  status_led(TinyUSBDevice.mounted() || ((millis() / 500) & 1));
}
