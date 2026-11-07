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
                 "c": put(jpeg()), "d": put("Line one\nline two".encode()), "s": [put(jpeg()), put(jpeg())],
                 "th": "4403a31a21012cd5fd942887d1f5c255922b20f9", "tu": 1, "tv": 0, "td": 1, "tf": 1, "tb": 0, "ts": 944},
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


    def test_patches(self):
        with tempfile.TemporaryDirectory() as td:
            td = Path(td)

            def pak_with(items):
                pos = 16
                blob = bytearray(b"PS4CATPK" + struct.pack("<II", 1, 0))
                refs = []
                for data in items:
                    blob.extend(data)
                    refs.append([pos, len(data)])
                    pos += len(data)
                return bytes(blob), refs

            jp = jpeg()
            pak0, (c0, c1) = pak_with([jp, jp])
            (td / "catalog.pak").write_bytes(pak0)
            base = [{"id": "a", "cid": "UP0001-CUSA00001_00-AAAAAAAAAAAAAAAA", "n": "Alpha", "c": c0},
                    {"id": "b", "cid": "UP0001-CUSA00002_00-BBBBBBBBBBBBBBBB", "n": "Beta", "c": c1}]
            (td / "catalog.json").write_text(json.dumps({"version": 1, "games": base}))
            pak2, (p2c, p2d) = pak_with([jp, jp])
            (td / "catalog_2.pak").write_bytes(pak2)
            p2 = {"version": 1, "patch": 2, "games": [
                {"id": "a", "m": 1, "th": "b" * 40, "tu": 0, "tv": 1, "ts": 2048},
                {"id": "b", "m": 1, "th": "c" * 40},
                {"id": "c", "cid": "UP0001-CUSA00003_00-CCCCCCCCCCCCCCCC", "n": "Gamma", "c": p2c, "th": "d" * 40},
                {"id": "d", "cid": "UP0001-CUSA00004_00-DDDDDDDDDDDDDDDD", "n": "Delta", "c": p2d},
                {"id": "zzz", "m": 1, "th": "e" * 40}]}   # правка несуществующей игры игнорируется
            (td / "catalog_2.json").write_text(json.dumps(p2))
            pak3, (p3c,) = pak_with([jp])
            (td / "catalog_3.pak").write_bytes(pak3)
            p3 = {"version": 1, "patch": 3, "remove": ["b"], "games": [
                {"id": "c", "cid": "UP0001-CUSA00003_00-CCCCCCCCCCCCCCCC", "n": "Gamma v2", "c": p3c}]}
            (td / "catalog_3.json").write_text(json.dumps(p3))
            exe = td / "tp"
            src = ROOT / "pkg" / "src"
            subprocess.run(["g++", "-std=c++17", "-g", "-fsanitize=address,undefined", "-Wall", "-I" + str(src),
                            str(ROOT / "tests" / "test_catalog_patch.cpp"), str(src / "catalog.cpp"), str(src / "json.cpp"),
                            str(src / "log.cpp"), "-o", str(exe)], check=True)
            r = subprocess.run([str(exe), str(td)], capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, r.stdout + r.stderr)


if __name__ == "__main__":
    unittest.main()
