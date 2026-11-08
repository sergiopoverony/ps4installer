#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Запросы игр через ps4torrent (pkg/src/torrents.cpp): файл <хэш>.magnet, отмена, состояние по /status подставного клиента."""
import http.server
import json
import subprocess
import tempfile
import threading
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
L2 = "8b911a531e8867750f1b7ce2eb6406eee98ae4a7"
L3 = "551c3ce9b365f750daedb0405425de704002500b"


class H(http.server.BaseHTTPRequestHandler):
    def log_message(self, *a):
        pass

    def do_GET(self):
        if self.path.startswith("/ann?"):   # подставной трекер: announce -> bencode со счётчиками
            ok = ("info_hash=" + "".join("%" + L2[i:i + 2] for i in range(0, 40, 2))) in self.path
            body = b"d8:completei12e10:incompletei3e8:intervali1800ee" if ok else b"d14:failure reason3:badee"
            self.send_response(200); self.send_header("Content-Length", str(len(body))); self.end_headers(); self.wfile.write(body); return
        if self.path != "/status":
            self.send_response(404); self.end_headers(); return
        body = json.dumps({"items": [
            {"hash": L2.upper(), "status": "downloading", "pct": 42},
            {"hash": L3, "status": "complete", "pct": 100},
            {"hash": "1" * 40, "status": "paused", "pct": 7},
        ]}).encode()
        self.send_response(200); self.send_header("Content-Length", str(len(body))); self.end_headers(); self.wfile.write(body)


class TorrentsTest(unittest.TestCase):
    def test_torrents(self):
        srv = http.server.HTTPServer(("127.0.0.1", 0), H)
        threading.Thread(target=srv.serve_forever, daemon=True).start()
        try:
            with tempfile.TemporaryDirectory() as td:
                src = ROOT / "pkg" / "src"
                exe = Path(td) / "t"
                subprocess.run(["g++", "-std=c++17", "-g", "-pthread", "-fsanitize=address,undefined", "-Wall", "-Wextra", "-I" + str(src),
                                str(ROOT / "tests" / "test_torrents.cpp"), str(src / "torrents.cpp"), str(src / "httpc.cpp"), str(src / "net.cpp"),
                                str(src / "json.cpp"), str(src / "log.cpp"), "-o", str(exe)], check=True)
                r = subprocess.run([str(exe), td, str(srv.server_address[1])], capture_output=True, text=True, timeout=60)
                self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
                self.assertIn("ok", r.stdout)
        finally:
            srv.shutdown()


if __name__ == "__main__":
    unittest.main()
