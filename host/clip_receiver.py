#!/usr/bin/env python3
"""
clip_receiver.py - tiny upload sink for the CG721 battery camera.

The camera (which has no wget/curl/ftp) pushes its newest SD event clip and
thumbnail here during each motion wake, via a plain HTTP/1.0 POST from
clipup.so. This drops them into Home Assistant's media folder so they show up,
browsable and playable, in the HA Media panel (Media -> Local Media -> cg721).

    POST /upload?name=<file>     body = raw file bytes

Config via env:
    CLIP_DIR   where to write files   (default /media/cg721)
    CLIP_PORT  listen port            (default 8099)
    CLIP_BIND  bind address           (default 0.0.0.0)
    CLIP_KEEP  keep newest N files    (default 300; 0 = never prune)

No auth: keep it on a trusted LAN only. Pure stdlib, Python 3.6+.
"""
import os, re, sys, http.server, socketserver

CLIP_DIR  = os.environ.get("CLIP_DIR", "/media/cg721")
CLIP_PORT = int(os.environ.get("CLIP_PORT", "8099"))
CLIP_BIND = os.environ.get("CLIP_BIND", "0.0.0.0")
CLIP_KEEP = int(os.environ.get("CLIP_KEEP", "300"))

SAFE = re.compile(r"[^A-Za-z0-9._-]")
MAX  = 64 * 1024 * 1024          # 64 MB per file cap


def log(*a):
    print(*a, flush=True)


def prune():
    if CLIP_KEEP <= 0:
        return
    try:
        files = [os.path.join(CLIP_DIR, f) for f in os.listdir(CLIP_DIR)]
        files = [f for f in files if os.path.isfile(f)]
        files.sort(key=lambda f: os.path.getmtime(f), reverse=True)
        for f in files[CLIP_KEEP:]:
            try:
                os.remove(f)
            except OSError:
                pass
    except OSError:
        pass


class Handler(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.0"

    def _reply(self, code, msg=b"ok"):
        self.send_response(code)
        self.send_header("Content-Type", "text/plain")
        self.send_header("Content-Length", str(len(msg)))
        self.end_headers()
        try:
            self.wfile.write(msg)
        except OSError:
            pass

    def do_POST(self):
        if not self.path.startswith("/upload"):
            return self._reply(404, b"not found")
        # name from query (?name=...), sanitized to a bare basename
        name = "clip.bin"
        if "?" in self.path:
            q = self.path.split("?", 1)[1]
            for kv in q.split("&"):
                if kv.startswith("name="):
                    name = kv[5:]
        name = SAFE.sub("_", os.path.basename(name)) or "clip.bin"
        try:
            n = int(self.headers.get("Content-Length", "0"))
        except ValueError:
            n = 0
        if n <= 0 or n > MAX:
            return self._reply(400, b"bad length")

        os.makedirs(CLIP_DIR, exist_ok=True)
        tmp = os.path.join(CLIP_DIR, "." + name + ".part")
        got = 0
        try:
            with open(tmp, "wb") as f:
                while got < n:
                    chunk = self.rfile.read(min(65536, n - got))
                    if not chunk:
                        break
                    f.write(chunk)
                    got += len(chunk)
        except OSError as e:
            log("write error:", e)
            try:
                os.remove(tmp)
            except OSError:
                pass
            return self._reply(500, b"write error")

        if got != n:
            try:
                os.remove(tmp)
            except OSError:
                pass
            return self._reply(400, b"short body")

        dst = os.path.join(CLIP_DIR, name)
        os.replace(tmp, dst)                      # atomic; HA sees whole file
        log("saved %s (%d bytes) from %s" % (name, got, self.client_address[0]))
        prune()
        return self._reply(200, b"ok")

    def log_message(self, *a):                    # quiet default access log
        pass


class Server(socketserver.ThreadingTCPServer):
    allow_reuse_address = True
    daemon_threads = True


if __name__ == "__main__":
    os.makedirs(CLIP_DIR, exist_ok=True)
    log("clip_receiver -> %s  listening %s:%d (keep=%d)" %
        (CLIP_DIR, CLIP_BIND, CLIP_PORT, CLIP_KEEP))
    try:
        Server((CLIP_BIND, CLIP_PORT), Handler).serve_forever()
    except KeyboardInterrupt:
        sys.exit(0)
