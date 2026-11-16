# kvm-link

Lets one PC be the keyboard, mouse and eyes of another computer, so the other one can be
set up, installed and driven without anyone sitting at it.

![How the parts connect: this PC to a pair of RP2350 boards soldered together to the target computer, and the target's HDMI back through a capture card](docs/overview.svg)

- **Two RP2350 boards**, soldered together end to end and both running
  `firmware/kvm_twoway`. One end plugs into this PC, where it is a USB serial port. The
  other end plugs into the target computer, which sees a USB keyboard and an absolute
  pointer. No drivers, and it does not matter which end goes where.
- **Screen**: any HDMI capture card that Windows sees as a camera. `grab.ps1` saves a still
  from it.

`kvm.py` sends the input and `grab.ps1` gets the picture. Together they are enough to drive
installers and a desktop from here.

In what follows, and in what `kvm.py` prints, the board on this PC is the **bridge** and the
one on the target computer is the **target board**.

Two more things are in here:

- An ESP32 touchscreen panel can be the bridge instead, driving a single RP2350 that is a
  keyboard and pointer and nothing else. It adds a **kill switch** that only a finger can
  turn back on: see
  [With a kill switch](#with-a-kill-switch-an-esp32-touch-panel-as-the-bridge).
- `skill/` holds a Claude skill that teaches Claude to use the pair: see
  [Claude skill](#claude-skill).

## Wiring

The two boards are soldered together on the short edge opposite the USB socket, end to end,
both face up with their USB sockets pointing away from each other. Each of the five pads of
one edge is soldered to the pad it meets, five separate joints, and one wire joins the two
grounds:

![Two RP2350-Zero boards soldered end to end: GP13 of each board meets GP9 of the other, and a wire joins the two GND pads](docs/wiring-twoway.svg)

| One board  | The other  |
|------------|------------|
| GP13 (TX)  | GP9 (RX)   |
| GP9 (RX)   | GP13 (TX)  |
| GND        | GND        |

- The two corner joints are the link: each board's GP13 to the other's GP9. The three
  joints between them carry nothing and only hold the boards together.
- No joint may touch its neighbour.
- **There is no ground on that edge**, and the link needs one. GND is the second pad from
  the USB socket on one long side of each board; join the two with a wire.
- Turned half round is the only way that fits. With one board face down, GP13 would meet
  GP13 and the two outputs would fight each other.
- Leave the 5V and 3V3 pads alone. Each board is powered by its own USB port, and joining
  the supplies would tie the two computers together.

The picture shows the Waveshare RP2350-Zero. Boards that cannot be soldered edge to edge
take three wires instead: GP13 of each to GP9 of the other, and GND to GND. On a Raspberry
Pi Pico 2 those are pins 17 (GP13), 12 (GP9) and 13 or 18 (GND).

## What goes across

Each board is a serial port *and* a keyboard and pointer on the computer it is plugged
into. A command written to one board's serial port comes out of the other board as key
presses and pointer moves, and the answer comes back the same way:

![How a command travels from kvm.py on one computer through both boards to the other computer, and how the answer returns](docs/dataflow-twoway.svg)

Two things follow from both boards being the same:

- **It works in both directions at once.** A program on the target computer that opens the
  board's serial port can type and click on this PC, exactly as this PC can on the target.
  Use the [panel](#with-a-kill-switch-an-esp32-touch-panel-as-the-bridge) when the target
  is not a machine you trust: the board it drives has no way to talk back.
- **The target sees a serial port next to the keyboard.** An operating system does not
  mind. A firmware setup screen or boot menu might ignore a keyboard that comes with one,
  and this has not been tried yet; the board the panel drives is a keyboard and pointer
  and nothing else, and has been through firmware setup and an install.

## Flash

Both boards get the same file. Hold **BOOT** while plugging a board in and it appears as a
drive named `RP2350`; copy `build\kvm-twoway.uf2` to it. Then do the same with the other
board. Soldered together they can still be flashed one at a time, each through its own USB
socket.

Once a board is running it, it can also be put into flashing mode without the button, by
opening its serial port at 1200 baud and closing it again.

There are two files in `build\`:

| File | Goes on |
|------|---------|
| `kvm-twoway.uf2` | both boards of the soldered pair |
| `kvm-target.uf2` | the one RP2350 wired to a [touch panel](#with-a-kill-switch-an-esp32-touch-panel-as-the-bridge) |

The files in `build\` are for the RP2350-Zero. `.\build.ps1` rebuilds them, and
`.\build.ps1 -Board rpipico2` builds for a Pico 2.

> **What has been run on real hardware.** `kvm-twoway.uf2` has run on two RP2350-Zero boards
> soldered end to end, both plugged into one Windows PC: each is picked up as a serial
> port, keyboard and mouse, and in both directions `status` answers (1000 round trips
> without a failure), pointer moves land on the pixel asked for, a key press arrives, and
> `name` renames the far board and `--reset` undoes it. It has not met a firmware setup
> screen yet, clicks, typing, wheel and wake have not been tried on it, and nor has a name
> surviving a power cut.
>
> The touch panel, the board it drives (`kvm-target.uf2`), `kvm.py` and `grab.ps1` have
> done real work, including an operating system install. `name` was added to that
> firmware afterwards and has not been run there, so the `kvm-target.uf2` here is firmware
> v2, a newer build than the one that was tested.

## Keyboard and mouse

```
python kvm.py status
python kvm.py type "ipconfig /all" --enter
python kvm.py key ctrl+alt+del
python kvm.py key down down enter
python kvm.py mash del --seconds 15      # tap a key through a reboot to reach setup
python kvm.py click 960 540
python kvm.py click 960 540 --double
python kvm.py --res 1280x720 click 640 360 --button right
python kvm.py drag 400 300 800 300
python kvm.py scroll -3
python kvm.py release
python kvm.py name "Bench keyboard"      # what the target computer is told it is called
```

Coordinates are pixels on the target's screen, in a frame whose size is given by `--res` or
the `KVM_RES` environment variable (default 1920x1080). The pointer is absolute, so the
frame does not have to be the target's real resolution: it only has to match the picture
the coordinates were read from. The serial port is found automatically, whichever kind of
bridge it is; `--port COM7` or `KVM_PORT` overrides.

Typing assumes a US keyboard layout on the target. Held keys are released if this PC stops
talking for five seconds. Only one program can have the bridge's serial port open at a time.

`name` sets the USB device name the target board gives the target computer, in place of
the built-in one ("kvm-link two-way"). It can be 32 plain ASCII characters at most. The
board stores it, so it is still called that after a power cut or a reflash, and it leaves
the USB bus for a second or two to announce it. `python kvm.py name` shows the current name,
`python kvm.py name --reset` goes back to the built-in one, and `status` shows it too.
On Windows the new name shows at once in Device Manager (Details, "Bus reported device
description"); the board's serial port keeps its own name, "TinyUSB Serial". Settings may
go on showing the name the device had when Windows first saw it, until the device is
removed there and plugged in again.

A script can import it instead of starting a process per action:

```python
import kvm
link = kvm.Link()                 # or kvm.Link("COM7")
k = kvm.Kvm(link, 1920, 1080)
k.click(960, 540); k.type("hello"); k.tap(0, [kvm.KEYS["enter"]])
k.set_name("Bench keyboard")      # k.set_name(None) for the built-in name
link.close()
```

`kvm.NoTarget` is raised when the target board does not answer and `kvm.Killed` when a
panel's kill switch is off; both are kinds of `kvm.KvmError`.

## Screen

```
.\grab.ps1 -List                                   # the camera names Windows knows
.\grab.ps1 -Device Elgato -Width 1920 -Out C:\temp\target.jpg
.\grab.ps1 -Device Elgato -Width 1920 -Crop 1500,0,420,120 -Out C:\temp\corner.jpg
```

`grab.ps1` uses the camera API built into Windows, so it needs no ffmpeg. `-Width 1920`
scales the still to 1920 wide, so that a point in the picture is the point to give
`kvm.py click`. `-Crop x,y,w,h` keeps one rectangle of that frame for a closer look.
`-WarmupMs` is how long it waits for the card to deliver a real frame (default 1500; 400 is
enough for a card that was used a moment ago).

Many capture cards ask the source for their largest mode, so a target may switch to
3840x2160 while its HDMI output is on the card, and its own panel shows nothing meanwhile.

## With a kill switch: an ESP32 touch panel as the bridge

An ESP32-S3 touchscreen panel (Guition JC3248W535C, 3.5") running
`firmware/kvm_bridge_esp32` can be the bridge instead, and its screen is a switch that
software on the PC cannot override. Worth having when the thing at the keyboard is a
script or an AI agent. `kvm.py` and `grab.ps1` are used the same way.

The panel drives a single RP2350 running `firmware/kvm_target` (`build\kvm-target.uf2`),
joined to it by three wires. That board is a keyboard and pointer on the target computer
and nothing else. It has no serial port, so nothing on the target can talk back through
it, and a firmware setup screen or boot menu sees a plain keyboard. For the same reason
reflashing it always needs the BOOT button. Its built-in name is "kvm-link keyboard and
pointer".

![The panel's two screens: green CONTROL ON and red STOPPED](docs/kill-switch.svg)

- **Touch the screen anywhere** while it is green ("CONTROL ON"): control stops at once and
  every held key and button is released.
- **Hold a finger on it for a second** while it is red ("STOPPED") to allow control again.

Nothing on the PC can switch it back on. The setting survives a reset or power cut, and a
panel that has never been set up starts STOPPED. Reading status is still allowed while
stopped; everything else is refused.

The lower part of the screen shows whether the target board is answering, whether the
target computer has picked it up, how many commands were passed and blocked, and the last
command sent.

### Wiring the panel

![Wiring between the panel's P1 socket and the RP2350-Zero](docs/wiring-panel.svg)

Three wires, from the panel's 4-pin power/UART socket (**P1**, next to the USB-C port,
labelled 5V / TX / RX / GND) to the target board. This is the arrangement in use and tested:

| JC3248W535C (P1) | Target board |
|------------------|--------------|
| TX (IO43)        | GP1 (RX)     |
| RX (IO44)        | GP0 (TX)     |
| GND              | GND          |

- Leave the socket's 5V pin unconnected: joining the supplies would tie the two computers
  together.
- On a Raspberry Pi Pico 2 in place of the RP2350-Zero, the three are pins 1 (GP0), 2 (GP1)
  and 3 (GND).
- The panel finds the wires by itself. It also looks on the IO17/IO18 socket (P3/P4), and it
  does not matter which way round TX and RX are: it listens on whichever pin the target
  board is driving. The pins it settled on are shown in the first status line on the screen
  and by `python kvm.py status`.

On an Elecrow CrowPanel Advance 7.0 (`-Env crow70`) the link is the UART1-OUT port instead:
TX (IO20) to GP1, RX (IO19) to GP0, GND to GND, with the function switch on the back at
**0 1**.

### Flashing the panel

Over its USB port:

```
.\flash-bridge.ps1
```

The very first time, the panel's factory firmware ignores the request to enter flashing
mode: hold **BOOT**, tap **RST**, release BOOT, then run the script. After that it updates
without touching the buttons. `-Env crow70` selects another panel; apart from the
JC3248W535C none has been run on real hardware, and apart from the CrowPanel their link pins
are not verified.

## If it does not work

- **"no bridge board found"**: the bridge is not showing up as a serial port on this PC. Try
  another USB cable (many are charge-only); on a panel, also its other USB/UART socket.
- **"is in use by another program"**: something else has the bridge's serial port open,
  usually a script that is still running.
- **"the target board is not answering"**: a joint or wire is open or on the wrong pad, the
  ground wire is missing, or the target board has no power from the target computer. A
  panel says "no answer" on its screen; with the port closed, sending `!probe` to its
  serial port lists every socket pin and marks the one the target board is driving as
  `high`.
- **"has not picked it up as a USB device"**: the target computer is off, or the cable to it
  carries power only.
- **"control is switched off at the touchscreen"** (panel only): hold a finger on the screen
  for a second.
- **`grab.ps1` saves a black picture**: the target is off or asleep (`python kvm.py wake`),
  or the card needs a longer `-WarmupMs`.

## Rebuild

`.\build.ps1` needs Arduino IDE 2 with the "Raspberry Pi Pico/RP2040/RP2350" board package
(earlephilhower). The ESP32 firmware needs PlatformIO (`pip install platformio`), and
`kvm.py` needs `pyserial`.

## Claude skill

`skill\kvm-link\SKILL.md` teaches Claude Code how to work a computer through this: check
the link, look at the screen, act, look again, and leave the kill switch to a person.

```
.\install-skill.ps1
```

copies it to `%USERPROFILE%\.claude\skills\kvm-link` together with `kvm.py` and `grab.ps1`,
so the skill works from any project. Run it again after changing any of the three.
