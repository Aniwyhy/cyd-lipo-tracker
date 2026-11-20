# CYD LiPo Tracker

Field tracker for LiPo packs on the 4" "Cheap Yellow Display" (ESP32-3248S040: ST7796 320×480, XPT2046 touch, CH340 USB), in portrait.

## Screens

- **Kit**: the packs you brought, shown as cards. Each card draws the pack as a battery with one segment per cell, filled to its state:
  green = Charged (full), blue = Storage (half), amber = Discharged (low), red ✕ = Dead. A purple **⚡HV** badge marks high-voltage packs.
  The chips under the tabs count packs per state. **Tap a card** to set its state (the picker shows the pack voltage for each state) or remove it from the kit.
  At the bottom: **All charged** / **All storage** (dead packs are left alone).
- **Arsenal**: every pack you own. **Tap** a pack to add it to or remove it from the kit. **Hold** to edit (name, 1S–8S, Standard/HV, capacity) or delete. **+ New** adds a pack.

Data is saved to flash (LittleFS, `/packs.txt`) and survives power cycles. The screen dims after 2 minutes idle; the first touch only wakes it.

## LiPo Editor (desktop)

A graphical editor for the whole list: names, cells, capacity, HV, in kit or not, and state, with the same battery icons as the board.
Double-click **LiPo Editor.command** in Finder, or run:

```bash
python3 tools/lipo_editor.py
```

It opens in your browser (served from your Mac only, works offline). With the CYD plugged in it loads the board's list automatically;
edit, then **Send to board** (⌘S). **Open file… / Save file…** work with `packs.txt` files when the board isn't around.
Close the Terminal window or press **Quit** to stop it. pyserial is picked up from PlatformIO's Python automatically.

## Editing the list as a file

The arsenal and kit live in a plain-text file on the board, one pack per line:

```
Race A         | 6S |  1300mAh |    | kit | charged
Whoop 1        | 1S |   450mAh | HV | kit | discharged
Long range     | 6S |  3000mAh |    |     | storage
```

- `cells`: 1S–8S. `HV`: high-voltage pack. `kit`: it's coming with you (blank = stays home). `state`: charged, storage, discharged, dead.
- After the name, fields can be in any order and spacing doesn't matter: `Race A | 6s 1300 kit` works. Missing fields default to 4S, 1300mAh, not HV, home, charged.
- Lines starting with `#` are comments. The board rewrites the file in its own tidy format whenever something changes on screen, so your comments don't survive.

With the board plugged in over USB (no reflashing; the screen updates right away):

```bash
python3 tools/packs.py edit
```

Opens the list in `$EDITOR` (TextEdit if unset: save, then quit it with Cmd-Q) and sends it back. Or work with a file:

```bash
python3 tools/packs.py pull packs.txt
```

```bash
python3 tools/packs.py push packs.txt
```

`push` replaces the whole list. [packs.example.txt](packs.example.txt) is a starting point. Needs pyserial (`pip3 install pyserial`, or use `~/.platformio/penv/bin/python`).

## Build and flash

```bash
pio run -t upload --upload-port /dev/cu.usbserial-10
```

## Touch calibration

Reuses the CYD Checklist's calibration if that was on the board before; otherwise it asks for four taps on first boot. Hold **BOOT** for 2 seconds to recalibrate.

## Serial (115200)

- `shot`: dumps the screen as RLE RGB888 (rendered from the off-screen buffer; the on-screen keyboard keys aren't included)
- `t x y`: tap, `h x y`: long press
- `get`: prints packs.txt between `BEGIN` and `END`; `put`: the following lines up to `END` replace it
