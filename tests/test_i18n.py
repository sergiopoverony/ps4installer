#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Файлы языков: одинаковые ключи, одинаковые подстановки, все ключи из кода есть в файлах."""
import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
LANG = ROOT / "pkg" / "assets" / "lang"


def load(p):
    d = {}
    for line in p.read_text(encoding="utf-8").splitlines():
        if line and not line.startswith("#") and "=" in line:
            k, v = line.split("=", 1)
            assert k not in d, "дубль ключа %s в %s" % (k, p.name)
            d[k] = v
    return d


class I18nTest(unittest.TestCase):
    def test_same_keys_and_placeholders(self):
        en = load(LANG / "en.txt")
        for p in sorted(LANG.glob("*.txt")):
            d = load(p)
            self.assertEqual(set(d), set(en), "ключи %s не совпадают с en.txt" % p.name)
            for k, v in d.items():
                self.assertEqual(sorted(re.findall(r"\{[12]\}", v)), sorted(re.findall(r"\{[12]\}", en[k])), "%s: %s" % (p.name, k))
                self.assertTrue(v.strip(), "%s: пустой текст %s" % (p.name, k))

    def test_code_keys_exist(self):
        en = load(LANG / "en.txt")
        used = set()
        for p in (ROOT / "pkg" / "src").glob("*.cpp"):
            text = p.read_text(encoding="utf-8")
            used |= set(re.findall(r'\btrf?\("([a-z_]+\.[a-z_0-9]+)"', text))
        used |= set(re.findall(r'"((?:tab)\.[a-z]+)"', (ROOT / "pkg" / "src" / "ui.cpp").read_text(encoding="utf-8")))
        missing = sorted(k for k in used if k not in en)
        self.assertEqual(missing, [], "нет в файлах языка")
        literals = set()   # ключи, переданные не прямо в tr(...), а через условие или таблицу
        for p in (ROOT / "pkg" / "src").glob("*.cpp"):
            literals |= set(re.findall(r'"([a-z_]+\.[a-z_0-9]+)"', p.read_text(encoding="utf-8")))
        unused = sorted(k for k in en if k not in used and k not in literals)
        self.assertEqual(unused, [], "ключи не используются в коде")


if __name__ == "__main__":
    unittest.main()
