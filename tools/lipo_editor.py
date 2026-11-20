#!/usr/bin/env python3
"""LiPo Editor: edit the tracker's packs (arsenal, kit, states) in a window in your browser.

  python3 tools/lipo_editor.py            opens http://127.0.0.1:8765 (Ctrl-C or the Quit button to stop)
  python3 tools/lipo_editor.py --no-open  don't open the browser

Works with the CYD over USB (load / send) and with packs.txt files (open / save), fully offline.
Standard library only, plus pyserial (found automatically in PlatformIO's Python).
"""
import argparse, json, os, sys, threading, webbrowser
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import packs as P  # noqa: E402  (re-launches under PlatformIO's Python if pyserial is missing)

HERE = os.path.dirname(os.path.abspath(__file__))
board_lock = threading.Lock()


def with_board(port, fn):
    with board_lock:
        s = P.open_board(port or P.find_port())
        try:
            return fn(s)
        finally:
            s.close()


def api(path, body):
    if path == "/api/status":
        return {"ports": P.list_ports()}
    if path == "/api/load":
        text = with_board(body.get("port"), P.pull)
        return {"packs": P.parse_text(text)}
    if path == "/api/send":
        text = P.format_packs(body.get("packs", []))
        reply = with_board(body.get("port"), lambda s: P.push(s, text))
        return {"ok": True, "reply": reply}
    if path == "/api/parse":
        return {"packs": P.parse_text(body.get("text", ""))}
    if path == "/api/format":
        return {"text": P.format_packs(body.get("packs", []))}
    raise KeyError(path)


class Handler(BaseHTTPRequestHandler):
    def log_message(self, *a):
        pass

    def _send(self, code, data, ctype):
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(data)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(data)

    def _local_only(self):
        # The page only talks to itself: refuse requests from other sites (DNS rebinding / CSRF).
        host = (self.headers.get("Host") or "").split(":")[0]
        origin = self.headers.get("Origin")
        if host not in ("127.0.0.1", "localhost"):
            return False
        if origin and origin.split("://")[-1].split(":")[0] not in ("127.0.0.1", "localhost"):
            return False
        return True

    def do_GET(self):
        if not self._local_only():
            return self._send(403, b"forbidden", "text/plain")
        if self.path in ("/", "/index.html"):
            with open(os.path.join(HERE, "editor.html"), "rb") as f:
                return self._send(200, f.read(), "text/html; charset=utf-8")
        self._send(404, b"not found", "text/plain")

    def do_POST(self):
        if not self._local_only():
            return self._send(403, b"forbidden", "text/plain")
        if self.path == "/api/quit":
            self._send(200, b"{}", "application/json")
            threading.Thread(target=self.server.shutdown, daemon=True).start()
            return
        try:
            n = int(self.headers.get("Content-Length") or 0)
            body = json.loads(self.rfile.read(n) or b"{}")
            out = api(self.path, body)
            code = 200
        except P.BoardError as e:
            out, code = {"error": str(e)}, 200
        except KeyError:
            out, code = {"error": "unknown request"}, 404
        except Exception as e:  # keep the UI alive whatever happens
            out, code = {"error": f"{type(e).__name__}: {e}"}, 200
        self._send(code, json.dumps(out).encode(), "application/json")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", type=int, default=8765, help="local web port (default 8765)")
    ap.add_argument("--no-open", action="store_true")
    a = ap.parse_args()
    try:
        srv = ThreadingHTTPServer(("127.0.0.1", a.port), Handler)
    except OSError:
        url = f"http://127.0.0.1:{a.port}/"
        print(f"LiPo Editor already running? Opening {url}")
        webbrowser.open(url)
        return
    url = f"http://127.0.0.1:{a.port}/"
    print(f"LiPo Editor at {url}  (Ctrl-C to quit)")
    if not a.no_open:
        threading.Timer(0.3, lambda: webbrowser.open(url)).start()
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        pass
    print("Bye.")


if __name__ == "__main__":
    main()
