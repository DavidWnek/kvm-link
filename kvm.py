#!/usr/bin/env python3
"""Drive a second computer's keyboard and pointer through the kvm-link boards.

    python kvm.py status
    python kvm.py type "hello world" --enter
    python kvm.py key ctrl+alt+del
    python kvm.py key down down enter
    python kvm.py mash del --seconds 15        # get into firmware setup at boot
    python kvm.py click 960 540
    python kvm.py --res 1280x720 click 640 360 --button right
    python kvm.py name "Bench keyboard"        # what the target calls the device

Coordinates are pixels on the target's screen; --res (or KVM_RES) says how big
that screen is. The serial port is found automatically; --port or KVM_PORT
overrides. Needs pyserial.
"""

import argparse
import os
import sys
import time

BRIDGE_HELLO = "!kvm-link bridge"
PICO_VID = 0x2E8A
TWOWAY_VID = 0x1209  # an RP2350 running kvm_twoway
# USB IDs a bridge can show up with: RP2350, or an ESP32 panel's serial chip
# (CH340, CP210x) or native USB.
BRIDGE_VIDS = (PICO_VID, TWOWAY_VID, 0x1A86, 0x10C4, 0x303A)

MODIFIERS = {
    "ctrl": 0x01, "lctrl": 0x01, "control": 0x01,
    "shift": 0x02, "lshift": 0x02,
    "alt": 0x04, "lalt": 0x04,
    "win": 0x08, "gui": 0x08, "super": 0x08, "meta": 0x08, "cmd": 0x08,
    "rctrl": 0x10, "rshift": 0x20, "ralt": 0x40, "altgr": 0x40, "rwin": 0x80,
}

KEYS = {
    "enter": 0x28, "return": 0x28, "esc": 0x29, "escape": 0x29,
    "backspace": 0x2A, "tab": 0x2B, "space": 0x2C,
    "capslock": 0x39, "printscreen": 0x46, "prtsc": 0x46,
    "scrolllock": 0x47, "pause": 0x48, "insert": 0x49, "ins": 0x49,
    "home": 0x4A, "pageup": 0x4B, "pgup": 0x4B,
    "delete": 0x4C, "del": 0x4C, "end": 0x4D,
    "pagedown": 0x4E, "pgdn": 0x4E,
    "right": 0x4F, "left": 0x50, "down": 0x51, "up": 0x52,
    "numlock": 0x53, "menu": 0x65, "app": 0x65,
}
KEYS.update({f"f{n}": 0x39 + n for n in range(1, 13)})
KEYS.update({chr(ord("a") + i): 0x04 + i for i in range(26)})
KEYS.update({str(d): 0x1E + (d - 1) % 10 for d in range(10)})  # 1..9 then 0

# US layout: character -> (usage, needs shift)
_PLAIN = "-=[]\\;'`,./"
_PLAIN_USAGE = [0x2D, 0x2E, 0x2F, 0x30, 0x31, 0x33, 0x34, 0x35, 0x36, 0x37, 0x38]
_SHIFTED = "_+{}|:\"~<>?"
CHARS = {" ": (0x2C, False), "\n": (0x28, False), "\t": (0x2B, False)}
for _i in range(26):
    CHARS[chr(ord("a") + _i)] = (0x04 + _i, False)
    CHARS[chr(ord("A") + _i)] = (0x04 + _i, True)
for _d, _s in zip("1234567890", "!@#$%^&*()"):
    CHARS[_d] = (KEYS[_d], False)
    CHARS[_s] = (KEYS[_d], True)
for _c, _s, _u in zip(_PLAIN, _SHIFTED, _PLAIN_USAGE):
    CHARS[_c] = (_u, False)
    CHARS[_s] = (_u, True)
KEYS.update({c: u for c, (u, shifted) in CHARS.items() if not shifted and c.strip()})

BUTTONS = {"left": 0x01, "right": 0x02, "middle": 0x04}

NAME_MAX = 32  # the longest USB device name the target board will report


class KvmError(Exception):
    pass


class NoTarget(KvmError):
    """The bridge is there but nothing answers on the far side of the wires."""


class Killed(KvmError):
    """Control is switched off at the bridge's touchscreen."""


def frame(payload: str) -> str:
    s = 0
    for b in payload.encode("ascii"):
        s ^= b
    return f"{payload}*{s:02X}\n"


def unframe(line: str):
    """Return the payload of a well-formed reply line, or None."""
    line = line.strip()
    if len(line) < 3 or line[-3] != "*":
        return None
    payload, want = line[:-3], line[-2:]
    s = 0
    for b in payload.encode("ascii", "replace"):
        s ^= b
    try:
        return payload if int(want, 16) == s else None
    except ValueError:
        return None


def parse_combo(combo: str):
    """'ctrl+alt+del' -> (modifier byte, [usages])."""
    mods, keys = 0, []
    if combo == "+":
        parts = ["+"]
    elif combo.endswith("++"):  # the plus key itself, e.g. ctrl++
        parts = combo[:-2].split("+") + ["+"]
    else:
        parts = combo.split("+")
    for part in parts:
        if part.lower() in MODIFIERS:
            mods |= MODIFIERS[part.lower()]
        elif part.lower() in KEYS:
            keys.append(KEYS[part.lower()])
        elif part in CHARS:
            usage, shifted = CHARS[part]
            keys.append(usage)
            if shifted:
                mods |= MODIFIERS["shift"]
        else:
            raise KvmError(f"unknown key '{part}' in '{combo}'")
    if len(keys) > 6:
        raise KvmError(f"more than six keys in '{combo}'")
    return mods, keys


class DryLink:
    """Prints the protocol lines instead of sending them."""

    def cmd(self, payload, retry=True):
        sys.stdout.write(frame(payload))
        return "+"

    def close(self):
        pass


class Link:
    def __init__(self, port=None):
        try:
            import serial
            from serial.tools import list_ports
        except ImportError:
            raise KvmError("pyserial is not installed (pip install pyserial)")
        self._serial = serial

        ports = {p.device: p for p in list_ports.comports()}
        if port:
            candidates = [port]
        else:
            candidates = [d for d, p in ports.items() if p.vid in BRIDGE_VIDS]
            if not candidates:
                raise KvmError(
                    "no bridge board found: no USB serial adapter of the right kind "
                    "is plugged into this PC (data cable? bridge firmware flashed?)")

        self.ser = None
        self.enabled = None  # None: this bridge has no kill switch
        self.link_pins = None  # which pins the bridge found the target board on
        tried = []
        for dev in candidates:
            info = ports.get(dev)
            is_pico = bool(info and info.vid in (PICO_VID, TWOWAY_VID))
            try:
                ser = serial.Serial()
                ser.port, ser.baudrate = dev, 115200
                ser.timeout, ser.write_timeout = 0.4, 1
                # The RP2350 bridge wants DTR to know someone is listening. On an
                # ESP32 panel DTR and RTS are wired to reset, so leave both alone.
                ser.dtr = ser.rts = is_pico
                ser.open()
            except serial.SerialException as e:
                tried.append(f"{dev}: {e}")
                continue
            hello = self._hello(ser, 0.5 if is_pico else 4.0)
            if hello:
                self.ser, self.port = ser, dev
                if "enabled=" in hello:
                    self.enabled = "enabled=1" in hello
                fields = dict(f.split("=", 1) for f in hello.split() if "=" in f)
                if "tx" in fields and "rx" in fields:
                    self.link_pins = f"panel TX IO{fields['tx']}, RX IO{fields['rx']}"
                break
            tried.append(f"{dev}: not a kvm-link bridge")
            ser.close()
        if not self.ser:
            busy = [t.split(":")[0] for t in tried
                    if "PermissionError" in t or "denied" in t.lower() or "busy" in t.lower()]
            if busy:
                raise KvmError(
                    f"{', '.join(busy)} is in use by another program, and only one can "
                    "have the bridge open (is a script still running?)")
            raise KvmError("no bridge board found (" + "; ".join(tried) + ")")
        self.ser.timeout = 1.5

    def _hello(self, ser, patience):
        """Ask the bridge to identify itself. An ESP32 that did reset when the
        port opened needs a few seconds to boot, so keep asking."""
        deadline = time.monotonic() + patience
        try:
            ser.reset_input_buffer()
            while time.monotonic() < deadline:
                ser.write(b"!hello\n")
                line = ser.readline().decode("ascii", "replace").strip()
                while line:
                    if line.startswith(BRIDGE_HELLO):
                        return line
                    line = ser.readline().decode("ascii", "replace").strip()
        except self._serial.SerialException:
            pass
        return None

    def cmd(self, payload, retry=True):
        """Send one command and return the reply payload ('+...'), or raise."""
        for attempt in range(2 if retry else 1):
            self.ser.reset_input_buffer()
            self.ser.write(frame(payload).encode("ascii"))
            deadline = time.monotonic() + 1.5
            while time.monotonic() < deadline:
                raw = self.ser.readline()
                if not raw:
                    break
                reply = unframe(raw.decode("ascii", "replace"))
                if reply is None:
                    continue
                if reply.startswith("+"):
                    return reply
                if reply in ("-sum", "-frame", "-long") and attempt == 0 and retry:
                    break  # line noise, send it again
                if reply == "-suspended" and attempt == 0 and retry:
                    # The target board has already asked the computer to wake; give it
                    # a moment and send the same thing again.
                    time.sleep(0.8)
                    break
                if reply == "-killed":
                    raise Killed("control is switched off at the touchscreen. "
                                 "Hold a finger on it for a second to allow it again.")
                raise KvmError(explain(reply))
        raise NoTarget(
            f"bridge on {self.port} is fine but the target board is not answering. "
            "Check the three wires (TX/RX crossed, ground shared) and that the "
            "target board is powered from the other computer's USB port.")

    def close(self):
        self.ser.close()


def explain(reply: str) -> str:
    return {
        "-nohost": "the target board is running but the other computer has not "
                   "picked it up as a USB device (off, or cable is power-only?)",
        "-suspended": "the other computer is asleep and did not wake",
        "-nowake": "the other computer is asleep and has not allowed this "
                   "device to wake it",
        "-busy": "the other computer is not reading the device right now",
        "-cmd": "the target board does not know this command; its firmware is "
                "older than this script (flash the current build)",
        "-store": "the target board could not save the name",
    }.get(reply, f"target board refused the command ({reply})")


class Kvm:
    def __init__(self, link, width, height):
        self.link, self.w, self.h = link, width, height

    def _abs(self, x, y):
        ax = round((min(max(x, 0), self.w - 1) + 0.5) / self.w * 0x7FFF)
        ay = round((min(max(y, 0), self.h - 1) + 0.5) / self.h * 0x7FFF)
        return ax, ay

    def keys(self, mods=0, usages=()):
        self.link.cmd("K " + " ".join(f"{v:02X}" for v in (mods, *usages)))

    def tap(self, mods, usages, hold=0.03):
        self.keys(mods, usages)
        if hold > 0:
            time.sleep(hold)
        self.keys()

    def type(self, text, delay=0.0):
        for ch in text.replace("\r\n", "\n"):
            if ch not in CHARS:
                raise KvmError(f"cannot type {ch!r}: not on a US keyboard")
            usage, shifted = CHARS[ch]
            self.tap(MODIFIERS["shift"] if shifted else 0, [usage], hold=0)
            if delay > 0:
                time.sleep(delay)

    def pointer(self, x, y, buttons=0, wheel=0):
        ax, ay = self._abs(x, y)
        self.link.cmd(f"M {buttons:02X} {ax:04X} {ay:04X} {wheel & 0xFF:02X}",
                      retry=(wheel == 0))

    def buttons(self, buttons=0, wheel=0):
        self.link.cmd(f"B {buttons:02X} {wheel & 0xFF:02X}", retry=(wheel == 0))

    def click(self, x=None, y=None, button=0x01, count=1):
        if x is not None:
            self.pointer(x, y)
            time.sleep(0.03)
        for i in range(count):
            self.buttons(button)
            time.sleep(0.04)
            self.buttons(0)
            if i + 1 < count:
                time.sleep(0.08)

    def drag(self, x1, y1, x2, y2, button=0x01, steps=12):
        self.pointer(x1, y1)
        time.sleep(0.05)
        self.pointer(x1, y1, button)
        for i in range(1, steps + 1):
            time.sleep(0.02)
            self.pointer(x1 + (x2 - x1) * i / steps, y1 + (y2 - y1) * i / steps, button)
        time.sleep(0.05)
        self.pointer(x2, y2, 0)

    def scroll(self, clicks):
        step = 1 if clicks > 0 else -1
        for _ in range(abs(clicks)):
            self.buttons(0, step)
            time.sleep(0.02)

    def status(self):
        """What the target board says about itself. 'name' is what it tells the
        target computer it is called (firmware v2 and later)."""
        reply = self.link.cmd("S")
        # The name comes last and may have spaces in it.
        fields, has_name, name = reply.partition(" n=")
        s = dict(f.split("=", 1) for f in fields.split()[1:] if "=" in f)
        if has_name:
            s["name"] = name
        return s

    def set_name(self, name=None, wait=8.0):
        """Change what the target computer is told this keyboard and pointer are
        called; None goes back to the built-in names. The board keeps the name
        when unplugged. It leaves the USB bus and rejoins to announce it, so this
        waits up to `wait` seconds for the target computer to pick it up again,
        and returns whether it did."""
        name = (name or "").strip()
        if len(name) > NAME_MAX:
            raise KvmError(f"the name is {len(name)} characters long and a USB device "
                           f"name can be {NAME_MAX} at most")
        if any(not " " <= ch <= "~" for ch in name):
            raise KvmError("the name can only have plain ASCII letters, digits, "
                           "spaces and punctuation in it")
        self.link.cmd(("N " + name).strip())
        deadline = time.monotonic() + wait
        while time.monotonic() < deadline:
            time.sleep(0.3)
            s = self.status()
            if not s or s.get("m") == "1":  # no fields: a dry run
                return True
        return False


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--port", default=os.environ.get("KVM_PORT"),
                    help="bridge serial port, e.g. COM7 (default: find it)")
    ap.add_argument("--res", default=os.environ.get("KVM_RES", "1920x1080"),
                    help="target screen size for pixel coordinates (default 1920x1080)")
    ap.add_argument("--dry-run", action="store_true",
                    help="print the protocol lines instead of sending them")
    sub = ap.add_subparsers(dest="command", required=True)

    sub.add_parser("status", help="show whether both boards and the target are alive")

    p = sub.add_parser("type", help="type text (US layout)")
    p.add_argument("text")
    p.add_argument("--enter", action="store_true", help="press Enter afterwards")
    p.add_argument("--delay", type=float, default=0.0, help="seconds between characters")

    p = sub.add_parser("key", help="press key combinations in order, e.g. ctrl+alt+del f2")
    p.add_argument("combo", nargs="+")
    p.add_argument("--hold", type=float, default=0.05, help="seconds each is held")
    p.add_argument("--repeat", type=int, default=1)
    p.add_argument("--gap", type=float, default=0.05, help="seconds between presses")

    p = sub.add_parser("mash", help="tap a key repeatedly, e.g. to catch a boot menu")
    p.add_argument("combo")
    p.add_argument("--seconds", type=float, default=10.0)
    p.add_argument("--rate", type=float, default=4.0, help="taps per second")

    p = sub.add_parser("move", help="move the pointer")
    p.add_argument("x", type=float)
    p.add_argument("y", type=float)

    p = sub.add_parser("click", help="click, optionally moving first")
    p.add_argument("x", type=float, nargs="?")
    p.add_argument("y", type=float, nargs="?")
    p.add_argument("--button", choices=sorted(BUTTONS), default="left")
    p.add_argument("--double", action="store_true")

    p = sub.add_parser("drag", help="press at one point, release at another")
    for name in ("x1", "y1", "x2", "y2"):
        p.add_argument(name, type=float)
    p.add_argument("--button", choices=sorted(BUTTONS), default="left")

    p = sub.add_parser("scroll", help="wheel clicks; positive is up")
    p.add_argument("clicks", type=int)

    p = sub.add_parser("name", help="show, or with TEXT set, the name the target "
                                    "computer is given for this keyboard and pointer")
    p.add_argument("text", nargs="?", help=f"new name, {NAME_MAX} characters at most")
    p.add_argument("--reset", action="store_true", help="go back to the built-in name")

    sub.add_parser("release", help="let go of every key and button")
    sub.add_parser("wake", help="wake the target from sleep")

    a = ap.parse_args(argv)

    try:
        w, h = (int(v) for v in a.res.lower().split("x"))
        if w < 1 or h < 1:
            raise ValueError
    except ValueError:
        ap.error(f"--res must look like 1920x1080, got {a.res!r}")
    if a.command == "click" and (a.x is None) != (a.y is None):
        ap.error("click takes both X and Y, or neither")
    if a.command == "name" and a.reset and a.text is not None:
        ap.error("name takes TEXT or --reset, not both")
    if a.command == "name" and a.text is not None and not a.text.strip():
        ap.error("the name is empty; use --reset to go back to the built-in name")

    link = None
    try:
        link = DryLink() if a.dry_run else Link(a.port)
        kvm = Kvm(link, w, h)
        try:
            run(a, kvm, link)
        except (NoTarget, Killed):
            raise  # nothing is getting through, so there is nothing to release
        except BaseException:
            # Never leave a key or button held on the other machine.
            try:
                link.cmd("Z")
            except Exception:
                pass
            raise
    except KvmError as e:
        print(f"kvm: {e}", file=sys.stderr)
        return 1
    except KeyboardInterrupt:
        return 130
    finally:
        if link:
            link.close()
    return 0


def run(a, kvm, link):
    if a.command == "status":
        s = kvm.status()
        if a.dry_run:
            return
        leds = int(s.get("leds", "0"), 16)
        print(f"bridge: {link.port}")
        if link.enabled is not None:
            print("kill switch: " + ("control ON" if link.enabled
                                     else "STOPPED at the touchscreen"))
        print(f"target board: answering (firmware v{s.get('v', '?')}, up {s.get('up', '?')}s)"
              + (f", wired to {link.link_pins}" if link.link_pins else ""))
        if "name" in s:
            print(f"shows up on the target as: {s['name']}")
        if s.get("m") != "1":
            print("target computer: has NOT picked up the keyboard (off or not connected)")
        elif s.get("s") == "1":
            print("target computer: connected, asleep")
        else:
            locks = [n for bit, n in ((1, "Num"), (2, "Caps"), (4, "Scroll")) if leds & bit]
            print("target computer: connected, awake"
                  + (f" (locks on: {', '.join(locks)})" if locks else ""))
    elif a.command == "type":
        kvm.type(a.text, a.delay)
        if a.enter:
            kvm.tap(0, [KEYS["enter"]])
    elif a.command == "key":
        combos = [parse_combo(c) for c in a.combo]
        for r in range(a.repeat):
            for i, (mods, usages) in enumerate(combos):
                kvm.tap(mods, usages, a.hold)
                if r + 1 < a.repeat or i + 1 < len(combos):
                    time.sleep(a.gap)
    elif a.command == "mash":
        mods, usages = parse_combo(a.combo)
        period = 1.0 / max(a.rate, 0.1)
        end = time.monotonic() + a.seconds
        taps = 0
        while time.monotonic() < end:
            try:
                kvm.tap(mods, usages, min(0.05, period / 2))
                taps += 1
            except Killed:
                raise
            except KvmError:
                pass  # the target drops off USB while it resets; keep going
            time.sleep(period / 2)
        print(f"{taps} taps delivered")
    elif a.command == "move":
        kvm.pointer(a.x, a.y)
    elif a.command == "click":
        kvm.click(a.x, a.y, BUTTONS[a.button], 2 if a.double else 1)
    elif a.command == "drag":
        kvm.drag(a.x1, a.y1, a.x2, a.y2, BUTTONS[a.button])
    elif a.command == "scroll":
        kvm.scroll(a.clicks)
    elif a.command == "name":
        if a.text is None and not a.reset:
            s = kvm.status()
            if a.dry_run:
                return
            if "name" not in s:
                raise KvmError(explain("-cmd"))
            print(s["name"])
            return
        back = kvm.set_name(None if a.reset else a.text)
        if a.dry_run:
            return
        print(f"the target board now calls itself: {kvm.status().get('name', '?')}")
        if not back:
            print("the target computer has not picked it up again yet "
                  "(off or not connected?); it will see the new name when it does")
    elif a.command == "release":
        link.cmd("Z")
    elif a.command == "wake":
        link.cmd("W")


if __name__ == "__main__":
    sys.exit(main())
