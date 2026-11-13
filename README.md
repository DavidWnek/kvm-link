# kvm-link

Lets one PC be the keyboard, mouse and eyes of another computer, so the other one can be
set up, installed and driven without anyone sitting at it, with a kill switch on a
touchscreen in between.

```
this PC ──USB──> [ESP32 touch panel] ──3 wires──> [RP2350] ──USB──> target computer
         serial port    kill switch       UART        keyboard + pointer

this PC <──USB── [HDMI capture card] <──HDMI── target computer
```

- **Target board**: an RP2350 (Waveshare RP2350-Zero) running `firmware/kvm_target`. The
  target computer sees a plain USB keyboard and an absolute pointer. No drivers; it works in
  firmware setup and boot menus as well as in an operating system.
- **Bridge**: an ESP32-S3 touchscreen panel (Guition JC3248W535C, 3.5") running
  `firmware/kvm_bridge_esp32`. It is a USB serial port on this PC and passes commands on to
  the target board, unless the screen says STOPPED.
- **Screen**: any HDMI capture card that Windows sees as a camera. `grab.ps1` saves a still
  from it.

`kvm.py` sends the input and `grab.ps1` gets the picture. Together they are enough to drive
firmware setup, boot menus, installers and a desktop from here. `skill/` holds a Claude
skill that teaches Claude to use the pair; see [Claude skill](#claude-skill).

## The kill switch

- **Touch the screen anywhere** while it is green ("CONTROL ON"): control stops at once and
  every held key and button is released.
- **Hold a finger on it for a second** while it is red ("STOPPED") to allow control again.

Nothing on the PC can switch it back on. The setting survives a reset or power cut, and a
panel that has never been set up starts STOPPED. Reading status is still allowed while
stopped; everything else is refused.

The lower part of the screen shows whether the RP2350 is answering, whether the target
computer has picked it up, how many commands were passed and blocked, and the last command
sent.

## Wiring

Three wires, from the panel's 4-pin power/UART socket (**P1**, next to the USB-C port,
labelled 5V / TX / RX / GND) to the RP2350-Zero. This is the arrangement in use and tested:

| JC3248W535C (P1) | RP2350-Zero |
|------------------|-------------|
| TX (IO43)        | GP1 (RX)    |
| RX (IO44)        | GP0 (TX)    |
| GND              | GND         |

- Leave the socket's 5V pin unconnected. The RP2350 is powered by the target computer's USB
  port, and joining supplies would tie the two computers together.
- The panel finds the wires by itself. It also looks on the IO17/IO18 socket (P3/P4), and it
  does not matter which way round TX and RX are: it listens on whichever pin the RP2350 is
  driving. The pins it settled on are shown in the first status line on the screen and by
  `python kvm.py status`.

On an Elecrow CrowPanel Advance 7.0 (`-Env crow70`) the link is the UART1-OUT port instead:
TX (IO20) to GP1, RX (IO19) to GP0, GND to GND, with the function switch on the back at
**0 1**.

## Flash

**RP2350-Zero**: hold **BOOT** while plugging it in and copy `build\kvm-target.uf2` to the
`RP2350` drive. It has no serial port once flashed, so reflashing always needs the BOOT
button. `.\build.ps1` rebuilds the image.

**ESP32 panel**, over its USB port:

```
.\flash-bridge.ps1
```

The very first time, the panel's factory firmware ignores the request to enter flashing
mode: hold **BOOT**, tap **RST**, release BOOT, then run the script. After that it updates
without touching the buttons. `-Env crow70` selects another panel; apart from the
JC3248W535C none has been run on real hardware, and apart from the CrowPanel their link pins
are not verified.

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
```

Coordinates are pixels on the target's screen, in a frame whose size is given by `--res` or
the `KVM_RES` environment variable (default 1920x1080). The pointer is absolute, so the
frame does not have to be the target's real resolution: it only has to match the picture
the coordinates were read from. The serial port is found automatically; `--port COM7` or
`KVM_PORT` overrides.

Typing assumes a US keyboard layout on the target. Held keys are released if this PC stops
talking for five seconds. Only one program can have the bridge's serial port open at a time.

A script can import it instead of starting a process per action:

```python
import kvm
link = kvm.Link()                 # or kvm.Link("COM7")
k = kvm.Kvm(link, 1920, 1080)
k.click(960, 540); k.type("hello"); k.tap(0, [kvm.KEYS["enter"]])
link.close()
```

`kvm.Killed` is raised when the kill switch is off and `kvm.NoTarget` when the target board
does not answer; both are kinds of `kvm.KvmError`.

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

## If it does not work

- **"no bridge board found"**: the panel is not showing up as a serial port on this PC. Try
  another USB cable (many are charge-only) and the panel's other USB/UART socket.
- **"is in use by another program"**: something else has the bridge's serial port open,
  usually a script that is still running.
- **"control is switched off at the touchscreen"**: hold a finger on the screen for a second.
- **"the target board is not answering"** / screen says "no answer": a signal wire is loose
  or on a socket pin the panel does not look at, or the RP2350 has no power from the target
  computer. With the port closed, sending `!probe` to the panel's serial port lists every
  socket pin and marks the one the RP2350 is driving as `high`.
- **"has not picked it up as a USB device"**: the target computer is off, or the cable to it
  carries power only.
- **`grab.ps1` saves a black picture**: the target is off or asleep (`python kvm.py wake`),
  or the card needs a longer `-WarmupMs`.

## Two RP2350s instead

`firmware/kvm_bridge` is a plain RP2350 bridge with no screen and no kill switch, for use
with a second RP2350 in place of the panel. Same three wires (GP0 to GP1 both ways, plus
ground). `.\build.ps1` builds it as `build\kvm-bridge.uf2`.

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
