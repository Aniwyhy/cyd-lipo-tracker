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
    sys.exit("pyserial is missing: pip install pyserial  (or run with ~/.platformio/penv/bin/python)")


def find_port():
    ports = sorted(glob.glob("/dev/cu.usbserial*") + glob.glob("/dev/cu.wchusbserial*") + glob.glob("/dev/ttyUSB*"))
    if not ports:
        sys.exit("No CYD found. Plug it in or pass --port.")
    return ports[0]


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
    sys.exit(f"The board on {port} didn't answer. Is the LiPo tracker firmware on it?")


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
    sys.exit("Timed out reading the list from the board.")


def push(s, text):
    lines = [l.rstrip("\r") for l in text.splitlines()]
    for l in lines:
        if l.strip() == "END":
            sys.exit('A line can\'t be just "END".')
    s.reset_input_buffer()
    s.write(b"put\n")
    if s.readline().strip() != b"ready":
        sys.exit("The board didn't accept the upload.")
    for l in lines:
        s.write(l.encode("utf-8")[:150] + b"\n")
        time.sleep(0.004)
    s.write(b"END\n")
    deadline = time.time() + 5
    while time.time() < deadline:
        r = s.readline().decode("utf-8", "replace").strip()
        if r.startswith("ok"):
            return r[3:]
    sys.exit("No confirmation from the board.")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("cmd", choices=["pull", "push", "edit", "show"])
    ap.add_argument("file", nargs="?", default="packs.txt")
    ap.add_argument("--port")
    a = ap.parse_args()
    s = connect(a.port or find_port())

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
        s = connect(a.port or find_port())
        print("Board now has", push(s, new))


if __name__ == "__main__":
    main()
