# CYD LiPo Tracker

Field tracker for LiPo packs on the 4" "Cheap Yellow Display" (ESP32-3248S040: ST7796 320×480, XPT2046 touch, CH340 USB), in portrait.

## Screens

- **Kit**: the packs you brought, shown as cards. Each card draws the pack as a battery with one segment per cell, filled to its state:
  green = Charged (full), blue = Storage (half), amber = Discharged (low), red ✕ = Dead. A purple **⚡HV** badge marks high-voltage packs.
  The chips under the tabs count packs per state. **Tap a card** to set its state (the picker shows the pack voltage for each state) or remove it from the kit.
  At the bottom: **All charged** / **All storage** (dead packs are left alone).
- **Arsenal**: every pack you own. **Tap** a pack to add it to or remove it from the kit. **Hold** to edit (name, 1S–8S, Standard/HV, capacity) or delete. **+ New** adds a pack.

Data is saved to flash (LittleFS, `/packs.txt`) and survives power cycles.

## Board battery and power (v1.1+)

With a 1S LiPo on the board's battery connector, the pill at the top right shows its voltage, colored green / amber (below 3.75 V) / red (at or below 3.60 V). **Tap it** for the **Power** screen.

The battery connector is wired to **IO34** through a divider that roughly halves the voltage, so no extra hardware is needed. Resistor tolerance makes the exact ratio vary from board to board, so **Calibrate** once: run the board on the LiPo (USB unplugged), measure the cell with a checker, then tap − / + until the Power screen shows the same voltage. With USB plugged in, the board's charger is topping the cell up, so the reading is the charging voltage (about 4.2 V), not the cell's resting voltage. With no LiPo connected, it shows "--".

Safety:

- **Low warning** at 3.60 V: a sheet asks you to charge it soon.
- **Cut-off** (Off / 3.3 / 3.4 / 3.5 V, default 3.4 V): 20 s under it, the board shows "Battery empty" and goes into deep sleep so the cell isn't over-discharged. It also won't start on a cell that's already under it. Press **BOOT** to wake it after charging. Tap the screen while the message is up to keep using it anyway (no cut-off until the next restart). Even asleep, the board draws a few mA, so unplug the LiPo to store it.
- **Brown-out guard**: if the cell sags so far that the ESP32 resets, it goes straight back to sleep with the screen off instead of rebooting over and over.

Energy saving:

- **Brightness**: Low / Mid / High.
- **Dim after** 30 s / 1 / 2 / 5 min idle (default 1 min). The screen turns off a minute after dimming and the CPU drops from 240 to 80 MHz. The first touch only wakes it; BOOT wakes it too.
- **Sleep after** 5 / 15 / 30 min / Off (default 15 min): deep sleep when idle with a LiPo connected. Press BOOT to wake. Opening the desktop editor or `packs.py` resets the board over USB, which wakes it too.

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

Reuses the CYD Checklist's calibration if that was on the board before; otherwise it asks for four taps on first boot. Hold **BOOT** for 2 seconds to recalibrate (pressing BOOT to wake the board from sleep doesn't count).

## Serial (115200)

- `shot`: dumps the screen as RLE RGB888 (rendered from the off-screen buffer; the on-screen keyboard keys aren't included)
- `t x y`: tap, `h x y`: long press
- `get`: prints packs.txt between `BEGIN` and `END`; `put`: the following lines up to `END` replace it
- `bat`: board battery reading (IO34, LiPo voltage, %, calibration, cut-off)
- `batsim <mV>`: pretend IO34 reads this many mV (testing the low-battery screens on USB); `batsim 0` turns it off
- `adcscan`: millivolts on the free ADC pins (32, 34, 35, 36, 39, 4, 25, 26), for finding the battery pin on other CYD variants
