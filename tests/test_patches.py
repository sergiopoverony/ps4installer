#!/usr/bin/env python3
# -*- coding: utf-8 -*-
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 SergioPoverony
"""Загрузка заплаток каталога (pkg/src/patches.cpp + https.cpp): подставной сервер выпуска, проверка сумм, основного каталога и имён файлов."""
import functools
import http.server
import json
import subprocess
import sys
import tempfile
import threading
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))
import make_release  # noqa: E402


class Quiet(http.server.SimpleHTTPRequestHandler):
    def log_message(self, *a):
        pass


class PatchesTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.td = tempfile.TemporaryDirectory()
        t = Path(cls.td.name)
        src = ROOT / "pkg" / "src"
        cls.exe = t / "t"
        subprocess.run(["g++", "-std=c++17", "-g", "-pthread", "-fsanitize=address,undefined", "-Wall", "-Wextra", "-I" + str(src),
                        str(ROOT / "tests" / "test_patches.cpp"), str(src / "patches.cpp"), str(src / "https.cpp"), str(src / "sha256.cpp"),
                        str(src / "httpc.cpp"), str(src / "net.cpp"), str(src / "json.cpp"), str(src / "log.cpp"), "-ldl", "-o", str(cls.exe)], check=True)
        data = t / "data"
        data.mkdir()
        (data / "catalog.json").write_text('{"games":[]}')
        (data / "catalog_2.json").write_text('{"games":[]}')
        (data / "catalog_2.pak").write_bytes(b"PS4CATPK" + bytes(range(50)))
        (data / "catalog_3.json").write_text('{"games":[],"remove":["x"]}')
        cls.rel = t / "rel"
        make_release.make_release(data, cls.rel)
        cls.base = data / "catalog.json"

    @classmethod
    def tearDownClass(cls):
        cls.td.cleanup()

    def serve(self, directory):
        h = functools.partial(Quiet, directory=str(directory))
        srv = http.server.ThreadingHTTPServer(("127.0.0.1", 0), h)
        threading.Thread(target=srv.serve_forever, daemon=True).start()
        self.addCleanup(srv.shutdown)
        return "http://127.0.0.1:%d/" % srv.server_address[1]

    def run_case(self, directory, mode, base=None):
        url = self.serve(directory)
        with tempfile.TemporaryDirectory() as dst:
            r = subprocess.run([str(self.exe), url, dst + "/cat", str(base or self.base), mode], capture_output=True, text=True, timeout=60)
            return r, Path(dst)

    def test_ok(self):
        r, _ = self.run_case(self.rel, "ok")
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)

    def test_bad_checksum(self):
        bad = Path(self.td.name) / "bad"
        import shutil
        shutil.copytree(self.rel, bad)
        (bad / "catalog_2.pak").write_bytes(b"x" * 58)   # тот же размер, другое содержимое
        r, _ = self.run_case(bad, "badsum")
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)

    def test_other_base(self):
        other = Path(self.td.name) / "other_base.json"
        other.write_text('{"games":[1]}')
        r, _ = self.run_case(self.rel, "otherbase", base=other)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)

    def test_bad_name(self):
        evil = Path(self.td.name) / "evil"
        import shutil
        shutil.copytree(self.rel, evil)
        idx = json.loads((evil / "index.json").read_text())
        idx["patches"][0]["files"][0]["name"] = "../../etc/passwd"
        (evil / "index.json").write_text(json.dumps(idx))
        r, _ = self.run_case(evil, "badname")
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)

    def test_server_down(self):
        r = subprocess.run([str(self.exe), "http://127.0.0.1:1/", "/tmp/nonexistent_cat", str(self.base), "down"], capture_output=True, text=True, timeout=60)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)


if __name__ == "__main__":
    unittest.main()
