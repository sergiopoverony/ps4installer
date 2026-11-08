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
        if self.path != "/status":
            self.send_response(404); self.end_headers(); return
        body = json.dumps({"items": [
            {"hash": L2.upper(), "status": "downloading", "pct": 42, "peers": 5},
            {"hash": L3, "status": "complete", "pct": 100},
            {"hash": "1" * 40, "status": "paused", "pct": 7},
        ]}).encode()
        self.send_response(200); self.send_header("Content-Length", str(len(body))); self.end_headers(); self.wfile.write(body)


def dns_server():
    """Подставной DNS: на любой запрос A отвечает 10.1.2.3 (с указателем-сжатием имени, как настоящие серверы)."""
    import socket
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.bind(("127.0.0.1", 0))

    def run():
        while True:
            try:
                q, addr = s.recvfrom(512)
            except OSError:
                return
            resp = q[:2] + b"\x81\x80" + q[4:6] + b"\x00\x01\x00\x00\x00\x00" + q[12:] + b"\xc0\x0c\x00\x01\x00\x01\x00\x00\x00\x3c\x00\x04\x0a\x01\x02\x03"
            s.sendto(resp, addr)
    threading.Thread(target=run, daemon=True).start()
    return s


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
                dns = dns_server()
                r = subprocess.run([str(exe), td, str(srv.server_address[1]), str(dns.getsockname()[1])], capture_output=True, text=True, timeout=60)
                self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
                self.assertIn("ok", r.stdout)
        finally:
            srv.shutdown()
            dns.close()


if __name__ == "__main__":
    unittest.main()
