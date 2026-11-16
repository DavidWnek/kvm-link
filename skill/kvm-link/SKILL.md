---
name: kvm-link
description: Operate a second, physical computer from this PC through the kvm-link hardware, which sends it real keyboard and mouse input (kvm.py, over a pair of RP2350 USB boards, or an ESP32 kill-switch panel and one RP2350) and shows its screen through an HDMI capture card (grab.ps1). Use this whenever the user wants something done on a machine reached this way and not over the network, for example the mini PC, console, kiosk, test box or "the N100" on the bench. That covers looking at what is on its screen, clicking or typing on it, changing BIOS/UEFI settings, picking a boot device, installing or setting up an operating system, signing in, running commands there, copying a build onto it, or scripting clicks for a test. Also use it when kvm.py, grab.ps1, the capture card, the kill switch or the touch panel are mentioned, or when one of them reports an error.
---

# kvm-link: hands and eyes on another computer

Two scripts give you a real keyboard, a real mouse and a view of the screen of a computer
that is wired to this PC:

```
this PC ──USB──> RP2350 ═╪═ RP2350 ──USB──> target computer            (kvm.py)
this PC <──USB── HDMI capture card <──HDMI── target computer             (grab.ps1)
```

The target sees ordinary USB devices, so this works where remote-desktop tools cannot:
firmware setup, boot menus, installers, lock screens, a machine with no network.

The usual link is two RP2350 boards soldered together end to end, one end in each
computer. The board on this PC is called the bridge and the one on the target the target
board. An ESP32 touch panel with a kill switch can be the bridge instead, wired to a single
RP2350; `kvm.py status` prints a `kill switch:` line when it is.

Both scripts are in `scripts/` beside this file:

```powershell
python "$HOME/.claude/skills/kvm-link/scripts/kvm.py" status
powershell -NoProfile -ExecutionPolicy Bypass -File "$HOME/.claude/skills/kvm-link/scripts/grab.ps1" -List
```

The examples below shorten these to `kvm.py` and `grab.ps1`. `kvm.py` needs `pyserial`.
The firmware, wiring and flashing instructions are in the project named in
`project-path.txt` beside this file (its `README.md`); read that when a board has to be
built, flashed or rewired.

## The kill switch belongs to the user

When the bridge is the touch panel, it is a kill switch. Green means control is on;
a touch turns it red and every command except `status` is refused with *"control is
switched off at the touchscreen"*. Only a finger held on the panel for a second turns it
back on.

When you see that message, stop and tell the user. They may have stopped you on purpose, so
do not look for another route to the machine (network, another port, reflashing the panel)
unless they ask for one. Carry on when they say control is back.

## Start by checking

```
kvm.py status
```

```
bridge: COM15
kill switch: control ON
target board: answering (firmware v2, up 5120s), wired to panel TX IO43, RX IO44
shows up on the target as: kvm-link keyboard and pointer
target computer: connected, awake
```

Each line is one link in the chain, so the first line that is wrong says where the problem
is. "connected, asleep" is cured by `kvm.py wake`. See [Problems](#problems) for the rest.

The "shows up on the target as" line is the USB device name the target computer is given.
`kvm.py name "Some name"` changes it (32 ASCII characters at most; `kvm.py name --reset`
undoes it). The board remembers it, and the keyboard drops off the target for a second or
two while it changes, so only do this when the user asks for it.

Then find the capture card once (`grab.ps1 -List` prints the camera names; pass any part of
the right one as `-Device`) and take a first picture.

## Look, act, look again

You cannot see the target between pictures, and nothing tells you whether a click landed.
So work in short steps: take a picture, do one thing (or a few that cannot go wrong
together), take another picture and check it did what you meant before going on. Guessing
where a dialog will be, or typing into a window you have not seen, is how commands end up
in the wrong place.

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File grab.ps1 -Device Elgato -Width 1920 -Out <scratch folder>\screen.jpg
```

then open the JPEG with the Read tool. Save pictures in your scratchpad or a temp folder,
not in the user's project.

- **`-Width 1920` makes picture pixels and click coordinates the same thing.** `kvm.py`
  measures coordinates in a 1920x1080 frame by default, and the pointer is absolute, so the
  target's real resolution does not matter. If the picture you are shown is some other
  size, scale: x = 1920 × (distance from the left ÷ picture width), likewise for y.
- **`-Crop x,y,w,h`** (same coordinates) saves only that rectangle. Use it to read small
  text or to check one control without paying for a whole frame. The line the script prints
  says how to turn a point in the crop back into a screen coordinate.
- **`-WarmupMs`** is how long the card is given to produce a real frame. The default (1500)
  suits a first picture; 400 is enough when you took one a moment ago. A black or stale
  picture usually means it needs longer, or that the target is asleep or switching video
  mode (reboots do this: wait a few seconds and take another).
- Things take time on the target. After a click that opens something, a short wait
  before the picture saves a second round trip; after starting an installer or a reboot,
  wait and look again instead of typing into the dark.

## Keyboard

```
kvm.py type 'Get-ChildItem C:\' --enter      # text, then Enter
kvm.py type 'slow firmware field' --delay 0.03
kvm.py key win+r                             # a combination
kvm.py key down down enter                   # several presses in order
kvm.py key tab --repeat 4
kvm.py key ctrl+alt+del                      # reaches the target, not this PC
kvm.py mash del --seconds 15                 # tap through a reboot to catch a prompt
kvm.py release                               # let go of everything
```

- `type` presses the keys of a **US keyboard**. If the target uses another layout, symbols
  come out wrong: check the first thing you type.
- Quote for the shell you are in. In PowerShell and bash, single quotes keep `$`, `\` and
  `"` as they are; a `'` inside the text needs the shell's own escaping. For anything
  awkward, type it in pieces.
- Long commands are fine, but a typo cannot be seen until the next picture. For a command
  that matters, type it without `--enter`, look, then `kvm.py key enter`.
- `mash` is for moments you cannot time: reboot the target, then `mash del` (or `f2`, `f11`,
  `f12`, `esc`, depending on the board) for long enough to cover the firmware splash. It
  keeps going while the target drops off USB during the reset.
- Key names: letters, digits, `f1`–`f12`, `enter esc tab space backspace delete insert home
  end pageup pagedown up down left right menu printscreen`, modifiers `ctrl shift alt win`
  (and `rctrl ralt` …).

Useful on Windows targets: `win+r` opens Run; typing `powershell` there and pressing
`ctrl+shift+enter` asks for an elevated one, and `alt+y` answers the consent prompt Yes
(the prompt accepts this because it is a real keyboard). `alt+f4` closes, `alt+tab`
switches, `win+x` then `a` is another way to an admin terminal.

## Mouse

```
kvm.py click 960 540
kvm.py click 960 540 --double
kvm.py click 960 540 --button right
kvm.py move 960 540                 # hover
kvm.py drag 400 300 800 300         # press at the first point, release at the second
kvm.py scroll -3                    # wheel clicks; negative is down
```

Aim for the middle of a control. If a click seems to miss, take a picture before clicking
again: the usual causes are a screen that had not finished drawing, or a coordinate read
from a picture of a different size.

## Getting files onto the target

There is no clipboard or file channel, only keys. When both machines are on the same
network, serve the folder from this PC and type the download on the target:

```
python -m http.server 8765 --bind <this PC's LAN address> --directory <folder>     # here, in the background
kvm.py type 'iwr http://<address>:8765/file.exe -OutFile file.exe -UseBasicParsing' --enter
```

(`curl -O` on Linux.) Stop the server when the file is across. Without a network, small
text files can be typed in with a here-string or `cat > file`, and anything else needs the
user to move a USB drive.

## Scripting it

For many actions in a row (a test loop, a long install), import the module instead of
starting a process per click:

```python
import sys; sys.path.insert(0, r"<skill folder>\scripts")
import kvm
link = kvm.Link()                      # finds the bridge; kvm.Link("COM15") to name it
k = kvm.Kvm(link, 1920, 1080)
k.click(960, 540); k.type("hello"); k.tap(0, [kvm.KEYS["enter"]])
k.drag(400, 300, 800, 300); k.scroll(-3); print(k.status())
link.close()
```

`kvm.Killed` (kill switch) and `kvm.NoTarget` (target board silent) are both `kvm.KvmError`.
Let `Killed` stop the script; do not retry through it.

Only one program can hold the bridge's serial port. *"COM15 is in use by another program"*
means something else has it, often a long-running script of this kind. Find out
what it is before doing anything about it; it may be a test the user wants left alone.

## Care

This is a keyboard on a real machine, with nothing to undo a keystroke.

- **Secrets stay with the user.** Do not type passwords, PINs, licence keys or recovery
  codes; ask the user to type them at the target, or wait until they have. Everything typed
  also passes through this PC's shell history and logs.
- **Ask before the irreversible**: erasing or partitioning a disk, installing an operating
  system over another, firmware updates, changing Secure Boot or TPM settings, accepting
  licence terms. Say exactly what is about to happen and wait for a yes.
- **Leave it tidy.** When you stop, no key should be held (`kvm.py release` if in doubt),
  no elevated shell left open that the user did not ask for, and any file server you
  started on this PC stopped.
- **Do not disturb this PC.** `grab.ps1` and `kvm.py` open no windows; keep it that way and
  run helpers in the background.

## Keep target-specific knowledge elsewhere

This skill is about the link, not about any one machine. What you learn about a particular
target (its accounts, where its buttons are, its firmware keys, its address) belongs in the
project's memory or notes, so the next session finds it there. Check those notes before
exploring a machine from scratch.

## Problems

| Message or symptom | What it means |
| --- | --- |
| `no bridge board found` | The board on this PC (RP2350 or panel) is not showing up as a serial port: a charge-only USB cable, the wrong socket on a panel, or no firmware on it. |
| `… is in use by another program` | Another program has the bridge's serial port open, usually a script that is still running. Find out what it is before stopping anything. |
| `control is switched off at the touchscreen` | The kill switch. See above: tell the user. |
| `the target board is not answering` | The far board has no power because the target is off or unplugged, or a joint, wire or the ground jumper between the two boards is open. |
| `has not picked it up as a USB device` | The target is off, or its cable carries power only. |
| `asleep and did not wake` / `has not allowed this device to wake it` | Try `kvm.py wake`; otherwise someone has to press its power button. |
| `cannot type '…': not on a US keyboard` | The text has a character outside US ASCII; type around it. |
| `No video device matching …` | Wrong `-Device`; the message lists the names that exist. |
| Black, frozen or old picture | Longer `-WarmupMs`; target asleep or changing video mode; HDMI cable. |
| Clicks land in the wrong place | The picture and `--res` disagree: grab with `-Width 1920` and use the default, or pass `--res` to match the picture. |
| The target's own display is blank | Normal when its HDMI output is on the capture card. |

Anything about the boards themselves (wiring, flashing, the panel's `!probe` command) is in
the project README named in `project-path.txt`.
