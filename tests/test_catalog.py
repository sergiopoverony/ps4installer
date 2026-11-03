#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Собирает и запускает tests/test_catalog.cpp на маленьком каталоге, собранном тем же форматом, что и tools/fetch_catalog.py."""
import io
import json
import struct
import subprocess
import tempfile
import unittest
from pathlib import Path

from PIL import Image

ROOT = Path(__file__).resolve().parent.parent


def jpeg():
    im = Image.new("RGB", (400, 300), (220, 20, 20))
    im.paste((20, 20, 220), (200, 0, 400, 300))
    b = io.BytesIO()
    im.save(b, "JPEG", quality=95)
    return b.getvalue()


class CatalogTest(unittest.TestCase):
    def test_catalog(self):
        with tempfile.TemporaryDirectory() as td:
            td = Path(td)
            pos = 16
            blob = bytearray(b"PS4CATPK" + struct.pack("<II", 1, 0))

            def put(data):
                nonlocal pos
                blob.extend(data)
                ref = [pos, len(data)]
                pos += len(data)
                return ref

            games = [
                {"id": "a", "cid": "EP0001-CUSA18535_00-GAME000000000000", "n": "Test Game", "r": "Тест",
                 "c": put(jpeg()), "d": put("Line one\nline two".encode()), "s": [put(jpeg()), put(jpeg())]},
                {"id": "b", "cid": "UP0001-CUSA00001_00-OTHER00000000000", "n": "Other", "c": put(jpeg())},
            ]
            (td / "catalog.pak").write_bytes(bytes(blob))
            (td / "catalog.json").write_text(json.dumps({"version": 1, "count": 2, "games": games}, ensure_ascii=False))
            exe = td / "t"
            src = ROOT / "pkg" / "src"
            subprocess.run(["g++", "-std=c++17", "-g", "-fsanitize=address,undefined", "-Wall", "-I" + str(src),
                            str(ROOT / "tests" / "test_catalog.cpp"), str(src / "catalog.cpp"), str(src / "json.cpp"),
                            str(src / "log.cpp"), "-o", str(exe)], check=True)
            r = subprocess.run([str(exe), str(td)], capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
            self.assertIn("ok", r.stdout)


if __name__ == "__main__":
    unittest.main()
