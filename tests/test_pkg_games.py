#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Собирает и запускает tests/test_pkg_games.cpp (логика приложения без консоли)."""
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent


class PkgGamesTest(unittest.TestCase):
    def test_logic(self):
        with tempfile.TemporaryDirectory() as td:
            exe = Path(td) / "t"
            cmd = ["g++", "-std=c++17", "-g", "-fsanitize=address,undefined", "-Wall", "-Wextra",
                   "-I" + str(ROOT / "pkg" / "src"), str(ROOT / "tests" / "test_pkg_games.cpp"),
                   str(ROOT / "pkg" / "src" / "games.cpp"), str(ROOT / "pkg" / "src" / "utf8.cpp"),
                   "-o", str(exe)]
            subprocess.run(cmd, check=True)
            r = subprocess.run([str(exe)], capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
            self.assertIn("ok", r.stdout)


if __name__ == "__main__":
    unittest.main()
