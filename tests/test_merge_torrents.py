#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""tools/merge_torrents.py: поля раздачи в каталоге, выбор лучшей раздачи, добавление игр вне каталога с картинками, повторный запуск."""
import csv
import io
import json
import struct
import sys
import tempfile
import unittest
from pathlib import Path

from PIL import Image

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))
import merge_torrents as M          # noqa: E402
import rutracker_table as RT        # noqa: E402

H = lambda c: c * 40
TR = "&tr=http%3A%2F%2Fbt.t-ru.org%2Fann%3Fmagnet"


def jpg(path, color=(200, 30, 30), size=(300, 200)):
    Image.new("RGB", size, color).save(path, "JPEG")
    return path.as_uri()


def row(name, code, h, ru="", voice="", date="2023-01-01", cover="", shots="", dlc="", fix="", desc="d", size="1,5"):
    return [name, code, desc, dlc, fix, "", ru, voice, size, date, "https://rutracker.org/forum/viewtopic.php?t=1", "magnet:?xt=urn:btih:" + h.upper() + TR, cover, shots]


class MergeTest(unittest.TestCase):
    def test_merge(self):
        with tempfile.TemporaryDirectory() as td:
            td = Path(td)
            pak = bytearray(b"PS4CATPK" + struct.pack("<II", 1, 0))
            ref = [16, 5]
            pak.extend(b"hello")
            games = [
                {"id": "a", "cid": "UP0001-CUSA00010_00-AAAA000000000000", "n": "Alpha Game", "c": ref},
                {"id": "b", "cid": "UP0001-CUSA00020_00-BBBB000000000000", "n": "Beta: The Game II", "c": ref},
                {"id": "c", "cid": "UP0001-CUSA00030_00-CCCC000000000000", "n": "Gamma", "c": ref},
            ]
            (td / "catalog.pak").write_bytes(bytes(pak))
            (td / "catalog.json").write_text(json.dumps({"version": 1, "count": 3, "games": games}))
            cover = jpg(td / "cover.jpg", size=(640, 900))
            s1, s2 = jpg(td / "s1.jpg", (30, 200, 30), (800, 450)), jpg(td / "s2.jpg", (30, 30, 200), (800, 450))
            rows = [
                row("Alpha Game", "CUSA00010", H("a"), ru="Да"),                                      # по коду
                row("Alpha Game", "CUSA00010", H("b"), ru="", date="2024-01-01"),                      # новее, но без русского: проигрывает
                row("Beta The Game 2 Deluxe Edition", "CUSA55555", H("c"), voice="Да", dlc="Да", fix="Да", size="2,0"),   # по названию (другой регион)
                row("Zeta", "CUSA77777", H("d"), ru="Да", desc="About Zeta", cover=cover, shots=s1 + " " + s2),            # вне каталога, с картинками
                row("Gamma Special", "CUSA99999", H("9")),                                           # игра + одно слово -> к Gamma, не отдельной записью
                row("Omega", "", H("e")),                                                                # вне каталога, без кода и картинок
                row("Broken", "CUSA88888", H("f"), cover="file:///nonexistent.jpg"),                    # картинка не качается: игра остаётся без неё
            ]
            with open(td / "t.csv", "w", newline="", encoding="utf-8-sig") as f:
                w = csv.writer(f, delimiter=";"); w.writerow(RT.HEADER); w.writerows(rows)

            logs = []
            st = M.merge(td, td / "t.csv", media=True, log=logs.append)
            self.assertEqual((st["catalog"], st["with_torrent"], st["by_code"], st["by_name"], st["extras"], st["extras_with_cover"]), (3, 3, 1, 2, 3, 1))
            doc = json.loads((td / "catalog.json").read_text(encoding="utf-8"))
            g = {x["id"]: x for x in doc["games"]}
            self.assertEqual(g["c"]["th"], H("9"))
            self.assertEqual(g["a"]["th"], H("a"))                       # русская раздача выбрана вместо новой без русского
            self.assertEqual((g["a"]["tu"], g["a"]["tv"], g["a"]["td"]), (1, 0, 0))
            self.assertEqual((g["b"]["th"], g["b"]["tv"], g["b"]["td"], g["b"]["tf"], g["b"]["ts"]), (H("c"), 1, 1, 1, 2048))
            self.assertEqual(g["c"]["th"], H("9")) if False else self.assertEqual(g["c"]["th"], H("9"))
            ex = {x["n"]: x for x in doc["games"] if x.get("x")}
            self.assertEqual(sorted(ex), ["Broken", "Omega", "Zeta"])
            z = ex["Zeta"]
            self.assertEqual(z["th"], H("d")); self.assertTrue(z["cid"].startswith("XX0000-CUSA77777_00-")); self.assertEqual(len(z["s"]), 2)
            blob = (td / "catalog.pak").read_bytes()
            self.assertEqual(blob[z["d"][0]:z["d"][0] + z["d"][1]], b"About Zeta")
            im = Image.open(io.BytesIO(blob[z["c"][0]:z["c"][0] + z["c"][1]]))
            self.assertEqual(im.size, (350, 350))
            self.assertEqual(Image.open(io.BytesIO(blob[z["s"][0][0]:z["s"][0][0] + z["s"][0][1]])).size, (254, 143))
            self.assertNotIn("c", ex["Omega"]); self.assertEqual(ex["Omega"]["cid"], "")
            self.assertNotIn("c", ex["Broken"]); self.assertTrue(any("не скачалась" in x for x in logs))
            self.assertTrue((td / "catalog.json.bak").exists())
            self.assertEqual(blob[16:21], b"hello")                          # исходная часть pak не тронута
            size1 = len(blob)

            # повторный запуск: ничего не копится (pak не растёт, добавленные игры не дублируются)
            st2 = M.merge(td, td / "t.csv", media=True, log=lambda s: None)
            self.assertEqual(st2["extras"], 3)
            self.assertEqual(len((td / "catalog.pak").read_bytes()), size1)
            doc2 = json.loads((td / "catalog.json").read_text(encoding="utf-8"))
            self.assertEqual(len(doc2["games"]), 6)
            # раздачу убрали из таблицы: поля у игры исчезают
            with open(td / "t2.csv", "w", newline="", encoding="utf-8-sig") as f:
                w = csv.writer(f, delimiter=";"); w.writerow(RT.HEADER); w.writerow(rows[0])
            M.merge(td, td / "t2.csv", media=False, log=lambda s: None)
            doc3 = json.loads((td / "catalog.json").read_text(encoding="utf-8"))
            self.assertEqual(len(doc3["games"]), 3)
            self.assertNotIn("th", {x["id"]: x for x in doc3["games"]}["b"])
            self.assertEqual(len((td / "catalog.pak").read_bytes()), 21)

    def test_norm(self):
        same = [("Assassin's Creed IV Black Flag Gold Edition / Чёрный Флаг", "Assassin’s Creed IV Black Flag"),
                ("Beyond Two Souls / ЗА ГРАНЬЮ Две души", "BEYOND: Two Souls"), ("Blood & Truth | Кровь и Истина", "Blood & Truth"),
                ("Control 1.04 /", "CONTROL Ultimate Edition"), ("Dreams™", "Dreams"),
                ("Fallout 4: Game of the Year Edition ReMOD 1.12", "Fallout 4")]
        for a, b in same:
            self.assertEqual(M.norm(a), M.norm(b), a)
        for a, b in [("Dark Cloud / Dark Cloud 2", "Dark Cloud"), ("Call of Duty: Black Ops 6", "Call of Duty: Black Ops")]:
            self.assertNotEqual(M.norm(a), M.norm(b), a)

    def test_table_images(self):
        sample = Path("/mnt/user-data/uploads/ps4.xml")
        if not sample.exists():
            self.skipTest("нет образца выгрузки")
        rows, total, *_ = RT.scan(open(sample, "rb"))
        self.assertEqual(len(rows), total)
        self.assertTrue(all(r[11].startswith("magnet:?xt=urn:btih:") and r[11].endswith(RT.TRACKER) for r in rows))
        self.assertTrue(sum(1 for r in rows if r[12].startswith("http")) >= 30)


if __name__ == "__main__":
    unittest.main()
