// kvm-link, target side.
//
// Plugs into the computer being controlled and shows up there as a plain USB
// keyboard plus an absolute pointer (the same kind of device a VM "USB tablet"
// is). It has no serial port of its own: commands arrive on UART0 from the
// bridge board, which is plugged into the controlling PC.
//
// Build with the Adafruit TinyUSB stack:
//   FQBN rp2040:rp2040:rpipico2:usbstack=tinyusb
//
// Wiring to the bridge board (3 wires, nothing else):
//   GP0 (TX, pin 1) -> bridge GP1 (RX, pin 2)
//   GP1 (RX, pin 2) <- bridge GP0 (TX, pin 1)
//   GND (pin 3)     -- bridge GND (pin 3)
//
// Protocol: one ASCII line per command, "<payload>*<XX>\n", where XX is the
// XOR of the payload bytes in hex. All numbers are hex. Every command gets
// exactly one reply line in the same framing; "+..." is success, "-..." is a
// failure reason.
//
//   K mm [k1..k6]      keyboard report: modifier byte, up to six key usages
//   M bb xxxx yyyy ww  pointer report: buttons, X, Y (0..7FFF), wheel (int8)
//   B bb ww            buttons and wheel at the current pointer position
//   Z                  release every key and button
//   W                  ask a suspended host to wake
//   S                  status

#include "Adafruit_TinyUSB.h"
#include <hardware/gpio.h>

#define KVM_VERSION 1
#define UART_BAUD 115200
#define PIN_UART_TX 0
#define PIN_UART_RX 1
#define HOLD_TIMEOUT_MS 5000 // drop held keys if the controller goes quiet

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

static uint8_t kbd_report[8];       // modifiers, reserved, six keys
static uint8_t ptr_buttons = 0;
static uint16_t ptr_x = 0x3FFF, ptr_y = 0x3FFF;
static volatile uint8_t kbd_leds = 0;
static uint32_t last_cmd_ms = 0;

static void on_keyboard_set_report(uint8_t report_id, hid_report_type_t type,
                                   uint8_t const *buffer, uint16_t bufsize) {
  (void)report_id;
  if (type == HID_REPORT_TYPE_OUTPUT && bufsize >= 1) {
    kbd_leds = buffer[0]; // bit0 Num, bit1 Caps, bit2 Scroll
  }
}

static void reply(const char *payload) {
  uint8_t sum = 0;
  for (const char *p = payload; *p; p++) {
    sum ^= (uint8_t)*p;
  }
  Serial1.printf("%s*%02X\n", payload, sum);
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

static void handle_line(char *line) {
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
  case 'S': {
    char buf[80];
    snprintf(buf, sizeof(buf), "+S v=%d m=%d s=%d leds=%02X held=%d up=%lu",
             KVM_VERSION, TinyUSBDevice.mounted() ? 1 : 0,
             TinyUSBDevice.suspended() ? 1 : 0, kbd_leds,
             anything_held() ? 1 : 0, (unsigned long)(millis() / 1000));
    reply(buf);
    return;
  }
  default:
    reply("-cmd");
    return;
  }

  reply(err ? err : "+");
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

  // The core has already started USB with its default serial port. Drop off
  // the bus and come back as HID only, which is what firmware setup screens
  // and boot menus are happiest with.
  if (!TinyUSBDevice.isInitialized()) {
    TinyUSBDevice.begin(0);
  }
  TinyUSBDevice.detach();
  TinyUSBDevice.clearConfiguration();
  TinyUSBDevice.setID(0x1209, 0x0001); // pid.codes test ID
  TinyUSBDevice.setManufacturerDescriptor("kvm-link");
  TinyUSBDevice.setProductDescriptor("kvm-link keyboard and pointer");

  usb_keyboard.setBootProtocol(HID_ITF_PROTOCOL_KEYBOARD);
  usb_keyboard.setPollInterval(2);
  usb_keyboard.setReportDescriptor(desc_keyboard, sizeof(desc_keyboard));
  usb_keyboard.setStringDescriptor("kvm-link keyboard");
  usb_keyboard.setReportCallback(NULL, on_keyboard_set_report);
  usb_keyboard.begin();

  usb_pointer.setBootProtocol(HID_ITF_PROTOCOL_NONE);
  usb_pointer.setPollInterval(2);
  usb_pointer.setReportDescriptor(desc_pointer, sizeof(desc_pointer));
  usb_pointer.setStringDescriptor("kvm-link pointer");
  usb_pointer.begin();

  delay(250); // long enough for the host to notice the disconnect
  TinyUSBDevice.attach();

  Serial1.setTX(PIN_UART_TX);
  Serial1.setRX(PIN_UART_RX);
  Serial1.setFIFOSize(256);
  Serial1.begin(UART_BAUD);
  gpio_pull_up(PIN_UART_RX); // idle high if the bridge is unplugged
}

void loop() {
  static char line[96];
  static size_t len = 0;
  static bool overflow = false;

  while (Serial1.available()) {
    int c = Serial1.read();
    if (c == '\r') {
      continue;
    }
    if (c == '\n') {
      if (overflow) {
        reply("-long");
      } else if (len) {
        line[len] = 0;
        handle_line(line);
      }
      len = 0;
      overflow = false;
    } else if (len < sizeof(line) - 1) {
      line[len++] = (char)c;
    } else {
      overflow = true;
    }
  }

  if (anything_held() && millis() - last_cmd_ms > HOLD_TIMEOUT_MS) {
    release_all();
    last_cmd_ms = millis();
  }

  // Solid: the target computer has enumerated us. Slow blink: it has not.
  status_led(TinyUSBDevice.mounted() || ((millis() / 500) & 1));
}
