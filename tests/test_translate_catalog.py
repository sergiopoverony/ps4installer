#!/usr/bin/env python3
# -*- coding: utf-8 -*-
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 SergioPoverony
"""Тесты tools/translate_catalog.py на фейковом движке (без интернета и без Argos)."""
import json
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "tools"))
import fetch_catalog as fc  # noqa: E402
import translate_catalog as tc  # noqa: E402


def make_game(cache, tid, desc, status="ok", lang="en"):
    d = cache / tid
    d.mkdir(parents=True)
    (d / "desc.txt").write_text(desc, encoding="utf-8")
    (d / "meta.json").write_text(json.dumps({"status": status, "lang": lang}), encoding="utf-8")
    (d / "cover.jpg").write_bytes(b"\xff\xd8cover")
    return d


class SplitTests(unittest.TestCase):
    def test_short_text_one_chunk(self):
        self.assertEqual(tc.split_chunks("Hello\n\nWorld"), ["Hello\n\nWorld"])

    def test_long_text_chunks_within_limit_and_lossless(self):
        text = "\n".join(("Sentence number %d is here. " % i) * 20 for i in range(10))
        chunks = tc.split_chunks(text, 500)
        self.assertTrue(all(len(c) <= 500 for c in chunks))
        words = lambda s: s.split()  # noqa: E731
        self.assertEqual(words(" ".join(chunks)), words(text))

    def test_huge_line_without_spaces(self):
        chunks = tc.split_chunks("x" * 1200, 500)
        self.assertEqual("".join(chunks), "x" * 1200)

    def test_google_response(self):
        raw = json.dumps([[["Привет, ", "Hello, ", None], ["мир\n", "world\n", None]], None, "en"])
        self.assertEqual(tc.parse_google_response(raw), "Привет, мир\n")


class RunTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.cache = Path(self.tmp.name) / "cache"
        self.cache.mkdir()
        self.logs = []

    def tearDown(self):
        self.tmp.cleanup()

    def engine(self, text):
        self.calls = getattr(self, "calls", 0) + 1
        return "Перевод: " + text

    def test_translates_and_resumes(self):
        a = make_game(self.cache, "CUSA00001", "Hello\n\nWorld")
        make_game(self.cache, "CUSA00002", "Привет", lang="ru")      # уже русское: не трогаем
        make_game(self.cache, "CUSA00003", "Bad", status="nopage")   # нет данных: не трогаем
        self.assertEqual(tc.run(self.cache, self.engine, log=self.logs.append), 0)
        self.assertEqual((a / "desc_ru.txt").read_text(encoding="utf-8"), "Перевод: Hello\n\nWorld")
        self.assertFalse((self.cache / "CUSA00002" / "desc_ru.txt").exists())
        self.assertFalse((self.cache / "CUSA00003" / "desc_ru.txt").exists())
        self.assertEqual(self.calls, 1)
        tc.run(self.cache, self.engine, log=self.logs.append)
        self.assertEqual(self.calls, 1)  # повторный запуск ничего не переводит
        tc.run(self.cache, self.engine, force=True, log=self.logs.append)
        self.assertEqual(self.calls, 2)

    def test_failure_leaves_no_file_and_stops_on_streak(self):
        for i in range(tc.MAX_CONSECUTIVE_FAILS + 5):
            make_game(self.cache, "CUSA%05d" % i, "Text %d" % i)

        def bad(_t):
            raise tc.TranslateError("сеть")
        self.assertEqual(tc.run(self.cache, bad, log=self.logs.append), 1)
        self.assertEqual(list(self.cache.glob("*/desc_ru.txt")), [])

    def test_non_russian_result_rejected(self):
        a = make_game(self.cache, "CUSA00001", "Hello")
        tc.run(self.cache, lambda t: t, log=self.logs.append)
        self.assertFalse((a / "desc_ru.txt").exists())

    def test_pack_prefers_russian(self):
        a = make_game(self.cache, "CUSA00001", "Hello")
        (a / "desc_ru.txt").write_text("Привет", encoding="utf-8")
        b = make_game(self.cache, "CUSA00002", "Only English")
        out = Path(self.tmp.name) / "out"
        games = [{"id": "CUSA00001", "cid": "c1", "name": "A"}, {"id": "CUSA00002", "cid": "c2", "name": "B"}]
        fc.pack(games, self.cache, out, log=lambda m: None)
        doc = json.loads((out / "catalog.json").read_text(encoding="utf-8"))
        pak = (out / "catalog.pak").read_bytes()
        got = {g["id"]: pak[g["d"][0]:g["d"][0] + g["d"][1]].decode("utf-8") for g in doc["games"]}
        self.assertEqual(got, {"CUSA00001": "Привет", "CUSA00002": "Only English"})
        self.assertTrue(b.exists())

    def test_workers_parallel(self):
        for i in range(8):
            make_game(self.cache, "CUSA%05d" % i, "Text %d" % i)
        tc.run(self.cache, self.engine, workers=3, log=self.logs.append)
        self.assertEqual(len(list(self.cache.glob("*/desc_ru.txt"))), 8)


if __name__ == "__main__":
    unittest.main(verbosity=2)
