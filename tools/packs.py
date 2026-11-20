#!/usr/bin/env python3
"""Read or replace the LiPo tracker's packs.txt over USB serial (no reflashing needed).

  python3 tools/packs.py pull [packs.txt]   save the board's list to a file (default packs.txt)
  python3 tools/packs.py push [packs.txt]   send a file to the board (replaces its list)
  python3 tools/packs.py edit               pull, open in $EDITOR (or your default editor), push when it closes
  python3 tools/packs.py show               print the board's list

Options: --port /dev/cu.usbserial-10 (default: the first /dev/cu.usbserial* or /dev/ttyUSB*).
Needs pyserial (pip install pyserial; PlatformIO's Python already has it).
"""
import argparse, glob, os, subprocess, sys, tempfile, time

try:
    import serial
except ImportError:
    pio_py = os.path.expanduser("~/.platformio/penv/bin/python")  # PlatformIO ships pyserial
    if os.path.exists(pio_py) and os.path.realpath(sys.executable) != os.path.realpath(pio_py):
        os.execv(pio_py, [pio_py] + sys.argv)
    sys.exit("pyserial is missing: pip install pyserial  (or run with ~/.platformio/penv/bin/python)")


class BoardError(Exception):
    pass


def list_ports():
    return sorted(glob.glob("/dev/cu.usbserial*") + glob.glob("/dev/cu.wchusbserial*") + glob.glob("/dev/ttyUSB*"))


def find_port():
    ports = list_ports()
    if not ports:
        raise BoardError("No CYD found. Plug it in or pass --port.")
    return ports[0]


# ---------------------------------------------------------------- packs.txt format (same rules as the firmware)
STATES = ["charged", "storage", "discharged", "dead"]
MAX_NAME, MAX_PACKS = 14, 60
HEADER = (
    "# LiPo tracker packs. One per line:  name | cells | capacity | HV | kit | state\n"
    "#   cells: 1S..8S    capacity: e.g. 1300mAh    HV: write HV for high-voltage packs\n"
    "#   kit: write kit if the pack is coming with you    state: charged, storage, discharged, dead\n"
    "# Fields after the name can be in any order; missing ones default to 4S 1300mAh, not HV, home, charged.\n\n"
)


def _apply_token(p, t):
    t = t.strip().lower()
    if not t or t == "-":
        return
    digits = ""
    for ch in t:
        if not ch.isdigit():
            break
        digits += ch
    n = int(digits) if digits else 0
    if t in ("hv", "lihv"): p["hv"] = True
    elif t in ("std", "lv", "standard"): p["hv"] = False
    elif t in ("kit", "in", "yes"): p["kit"] = True
    elif t in ("home", "out", "no"): p["kit"] = False
    elif t.startswith("charg") or t == "full": p["state"] = "charged"
    elif t.startswith("stor"): p["state"] = "storage"
    elif t.startswith("disch") or t in ("empty", "flat", "used"): p["state"] = "discharged"
    elif t in ("dead", "bad", "puffy", "broken"): p["state"] = "dead"
    elif n > 0 and t.endswith("s") and len(t) <= 2: p["cells"] = max(1, min(8, n))
    elif n > 0 and (t.endswith("mah") or n >= 50): p["mah"] = max(50, min(30000, n))


def parse_text(text):
    packs = []
    for line in text.splitlines():
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        name, _, rest = line.partition("|")
        name = name.strip()[:MAX_NAME]
        if not name:
            continue
        p = {"name": name, "cells": 4, "mah": 1300, "hv": False, "kit": False, "state": "charged"}
        for tok in rest.replace("|", " ").replace(",", " ").split():
            _apply_token(p, tok)
        packs.append(p)
    return packs[:MAX_PACKS]


def clean_name(name):
    name = "".join(ch for ch in str(name) if 32 <= ord(ch) < 127 and ch != "|").strip()
    return name[:MAX_NAME]


def format_packs(packs):
    out = [HEADER]
    for p in packs[:MAX_PACKS]:
        name = clean_name(p.get("name", "")) or "Pack"
        out.append("%-14s | %dS | %5dmAh | %-2s | %-3s | %s\n" % (
            name, int(p.get("cells", 4)), int(p.get("mah", 1300)), "HV" if p.get("hv") else "",
            "kit" if p.get("kit") else "", p.get("state", "charged") if p.get("state") in STATES else "charged"))
    return "".join(out)


def connect(port):
    s = serial.Serial()
    s.port, s.baudrate, s.timeout = port, 115200, 0.5
    s.dtr = False  # keep the ESP32 from resetting when the port opens
    s.rts = False
    s.open()
    deadline = time.time() + 8  # if it did reset, give it time to boot
    while time.time() < deadline:
        s.reset_input_buffer()
        s.write(b"ping\n")
        t = time.time() + 0.6
        while time.time() < t:
            if s.readline().strip() == b"pong":
                return s
    raise BoardError(f"The board on {port} didn't answer. Is the LiPo tracker firmware on it?")


def pull(s):
    s.reset_input_buffer()
    s.write(b"get\n")
    lines, started, deadline = [], False, time.time() + 5
    while time.time() < deadline:
        l = s.readline().decode("utf-8", "replace").rstrip("\r\n")
        if not started:
            started = l == "BEGIN"
        elif l == "END":
            return "\n".join(lines) + "\n"
        else:
            lines.append(l)
    raise BoardError("Timed out reading the list from the board.")


def push(s, text):
    lines = [l.rstrip("\r") for l in text.splitlines()]
    for l in lines:
        if l.strip() == "END":
            raise BoardError('A line can\'t be just "END".')
    s.reset_input_buffer()
    s.write(b"put\n")
    if s.readline().strip() != b"ready":
        raise BoardError("The board didn't accept the upload.")
    for l in lines:
        s.write(l.encode("utf-8")[:150] + b"\n")
        time.sleep(0.004)
    s.write(b"END\n")
    deadline = time.time() + 5
    while time.time() < deadline:
        r = s.readline().decode("utf-8", "replace").strip()
        if r.startswith("ok"):
            return r[3:]
    raise BoardError("No confirmation from the board.")


def open_board(port):
    try:
        return connect(port)
    except serial.SerialException as e:
        raise BoardError(f"Can't open {port}: {e}")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("cmd", choices=["pull", "push", "edit", "show"])
    ap.add_argument("file", nargs="?", default="packs.txt")
    ap.add_argument("--port")
    a = ap.parse_args()
    try:
        run(a)
    except BoardError as e:
        sys.exit(str(e))


def run(a):
    s = open_board(a.port or find_port())

    if a.cmd == "show":
        print(pull(s), end="")
    elif a.cmd == "pull":
        open(a.file, "w").write(pull(s))
        print(f"Saved {a.file}")
    elif a.cmd == "push":
        print("Board now has", push(s, open(a.file).read()))
    else:
        text = pull(s)
        fd, path = tempfile.mkstemp(suffix=".txt", prefix="packs-")
        os.write(fd, text.encode())
        os.close(fd)
        editor = os.environ.get("VISUAL") or os.environ.get("EDITOR")
        s.close()  # don't hold the port while editing
        if editor:
            subprocess.call(editor + ' "' + path + '"', shell=True)
        elif sys.platform == "darwin":
            print("Opening in TextEdit. Save, then quit TextEdit (Cmd-Q) to send it to the board.")
            subprocess.call(["open", "-W", "-t", path])  # waits until the TextEdit window closes
        else:
            subprocess.call(["nano", path])
        new = open(path).read()
        os.unlink(path)
        if new == text:
            print("No changes.")
            return
        s = open_board(a.port or find_port())
        print("Board now has", push(s, new))


if __name__ == "__main__":
    main()
