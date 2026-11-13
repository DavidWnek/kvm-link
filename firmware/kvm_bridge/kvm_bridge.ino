// kvm-link, bridge side.
//
// Plugs into the controlling PC and shows up there as a USB serial port.
// Lines written to that port are passed to the target board over UART0 and
// its replies are passed back. Lines starting with '!' are answered here and
// never forwarded, so the PC can tell "found the bridge" apart from "the
// target board is answering".
//
// Build with the default USB stack:
//   FQBN rp2040:rp2040:rpipico2
//
// Wiring to the target board (3 wires, nothing else):
//   GP0 (TX, pin 1) -> target GP1 (RX, pin 2)
//   GP1 (RX, pin 2) <- target GP0 (TX, pin 1)
//   GND (pin 3)     -- target GND (pin 3)

#include <hardware/gpio.h>

#define KVM_VERSION 1
#define UART_BAUD 115200
#define PIN_UART_TX 0
#define PIN_UART_RX 1

static uint32_t activity_ms = 0;

void setup() {
#ifdef LED_BUILTIN
  pinMode(LED_BUILTIN, OUTPUT);
#endif
  Serial.begin(115200);

  Serial1.setTX(PIN_UART_TX);
  Serial1.setRX(PIN_UART_RX);
  Serial1.setFIFOSize(512);
  Serial1.begin(UART_BAUD);
  gpio_pull_up(PIN_UART_RX); // idle high while the target board is unpowered
}

void loop() {
  static char line[128];
  static size_t len = 0;
  static bool overflow = false;

  // PC -> target, one whole line at a time.
  while (Serial.available()) {
    int c = Serial.read();
    if (c == '\r') {
      continue;
    }
    if (c == '\n') {
      if (!overflow && len) {
        line[len] = 0;
        if (line[0] == '!') {
          Serial.printf("!kvm-link bridge %d\n", KVM_VERSION);
        } else {
          Serial1.write((const uint8_t *)line, len);
          Serial1.write('\n');
        }
        activity_ms = millis();
      }
      len = 0;
      overflow = false;
    } else if (len < sizeof(line) - 1) {
      line[len++] = (char)c;
    } else {
      overflow = true;
    }
  }

  // Target -> PC. Dropped when nobody has the port open.
  while (Serial1.available()) {
    int c = Serial1.read();
    if (Serial.dtr()) {
      Serial.write((uint8_t)c);
    }
  }

  // On, with a short dip on each command. Boards whose only light is an
  // addressable LED (RP2350-Zero) get no indicator.
#ifdef LED_BUILTIN
  digitalWrite(LED_BUILTIN, (millis() - activity_ms < 40) ? LOW : HIGH);
#endif
}
