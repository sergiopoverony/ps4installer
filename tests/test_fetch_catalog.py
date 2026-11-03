#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Тесты tools/fetch_catalog.py без интернета: сеть подменяется заглушкой."""
import io
import json
import struct
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "tools"))
import fetch_catalog as fc  # noqa: E402

try:
    from PIL import Image
except ImportError:
    Image = None


def png_bytes(w, h, color=(200, 30, 30), alpha=False):
    im = Image.new("RGBA" if alpha else "RGB", (w, h), color + ((128,) if alpha else ()))
    out = io.BytesIO()
    im.save(out, "PNG")
    return out.getvalue()


def make_page(product_id, name, desc_html, shots):
    media = [{"role": "SCREENSHOT", "type": "IMAGE", "url": u} for u in shots]
    media.insert(0, {"role": "PREVIEW", "type": "VIDEO", "url": "https://x/video.mp4"})
    media.append({"role": "MASTER", "type": "IMAGE", "url": "https://x/master.png"})
    prod = {"id": product_id, "name": name, "media": media,
            "descriptions": [{"type": "SHORT", "value": "Коротко"}, {"type": "LONG", "value": desc_html}]}
    other = {"id": "OTHER-PRODUCT", "name": "Чужая игра", "media": [
        {"role": "SCREENSHOT", "type": "IMAGE", "url": "https://x/other.png"}]}
    data = {"props": {"pageProps": {"productId": product_id}, "apolloState": {"Product:other": other, "Product:main": prod}}}
    return '<html><head></head><body><script id="__NEXT_DATA__" type="application/json">%s</script></body></html>' % json.dumps(data)


class TitleTests(unittest.TestCase):
    def test_clean_title(self):
        cases = {
            "Tents and Trees PS4 & PS5": "Tents and Trees",
            "Atelier Yumia: The Alchemist (PS4)": "Atelier Yumia: The Alchemist",
            "Land of Mushrooms PS4 & PS5 - Game Modes Edition": "Land of Mushrooms - Game Modes Edition",
            "Some Game™ PS4™ & PS5™": "Some Game",
            "Plain Name": "Plain Name",
            "Game - PS4": "Game",
            "PS4": "PS4",
        }
        for src, want in cases.items():
            self.assertEqual(fc.clean_title(src), want, src)


class CatalogTests(unittest.TestCase):
    def test_select_games(self):
        items = [
            ["a", {"name": "Game Deluxe Edition (PS4)", "uuid": "EP1-CUSA11111_00-AAAA", "rating": "Premium Edition", "image": "i", "href": "h"}],
            ["b", {"name": "Game (PS4)", "uuid": "EP1-CUSA11111_00-BBBB", "rating": "Full Game", "image": "i", "href": "h"}],
            ["c", {"name": "Port", "uuid": "EP2-PPSA22222_00-CCCC", "rating": "Full Game", "image": "i", "href": "h"}],
            ["d", {"name": "OST", "uuid": "EP3-CUSA33333_00-DDDD", "rating": "Soundtrack", "image": "i", "href": "h"}],
            ["e", {"name": "Another", "uuid": "EP4-CUSA44444_00-EEEE", "rating": "Full Game", "image": "i", "href": "h"}],
        ]
        with tempfile.TemporaryDirectory() as t:
            p = Path(t) / "c.json"
            p.write_text(json.dumps(items))
            games = fc.select_games(fc.load_catalog_file(p))
        self.assertEqual([g["id"] for g in games], ["CUSA44444", "CUSA11111"])
        self.assertEqual(games[1]["name"], "Game")
        self.assertTrue(games[1]["cid"].endswith("BBBB"))

    def test_pick_sample_spread(self):
        games = [{"id": str(i)} for i in range(100)]
        s = fc.pick_sample(games, 10)
        self.assertEqual(len(s), 10)
        self.assertEqual(s[0]["id"], "0")
        self.assertGreater(int(s[-1]["id"]), 80)


class ParseTests(unittest.TestCase):
    def test_parse_picks_right_product(self):
        page = make_page("EP1-CUSA11111_00-BBBB", "Игра", "<p>Первый <b>абзац</b>.</p><p>Второй&nbsp;абзац &amp; всё.</p>",
                         ["https://x/s1.png", "https://x/s2.png", "https://x/s1.png"])
        r = fc.parse_page(page, "EP1-CUSA11111_00-BBBB")
        self.assertEqual(r["name"], "Игра")
        self.assertEqual(r["shots"], ["https://x/s1.png", "https://x/s2.png"])
        self.assertEqual(r["desc"], "Первый абзац.\n\nВторой абзац & всё.")

    def test_real_page_structure(self):
        """Фикстура собрана из настоящей страницы PS Store (блок media-carousel + абзац overview)."""
        html = (Path(__file__).resolve().parent / "fixtures" / "ps_store_cusa15080.html").read_text(encoding="utf-8")
        r = fc.parse_page(html, "UP0006-CUSA15080_00-STARWARSQUADRONS")
        self.assertEqual(r["name"], "STAR WARS™: Squadrons")
        self.assertEqual(len(r["shots"]), 7)
        self.assertTrue(all(u.startswith("https://image.api.playstation.com/") for u in r["shots"]))
        self.assertEqual(r["desc"], "Synthetic test description for a game.\n\nSecond paragraph & more text.\n\nThird paragraph.")

    def test_parse_refs(self):
        state = {
            "Media:1": {"role": "SCREENSHOT", "type": "IMAGE", "url": "https://x/a.png"},
            "Product:p": {"id": "P1", "name": "N", "media": [{"__ref": "Media:1"}], "descriptions": []},
        }
        page = '<script id="__NEXT_DATA__">%s</script>' % json.dumps({"props": {"apolloState": state}})
        r = fc.parse_page(page, "P1")
        self.assertEqual(r["shots"], ["https://x/a.png"])

    def test_meta_fallback(self):
        page = '<html><head><meta property="og:description" content="Описание &amp; текст"><meta property="og:title" content="Заголовок"></head></html>'
        r = fc.parse_page(page)
        self.assertEqual(r["desc"], "Описание & текст")
        self.assertEqual(r["name"], "Заголовок")
        self.assertEqual(r["shots"], [])

    def test_garbage(self):
        r = fc.parse_page("<html>ничего</html>")
        self.assertEqual((r["name"], r["desc"], r["shots"]), ("", "", []))

    def test_truncate(self):
        s = ("Предложение номер один. " * 100).strip()
        t = fc.truncate_text(s, 200)
        self.assertLessEqual(len(t), 200)
        self.assertTrue(t.endswith("."))
        self.assertEqual(fc.truncate_text("коротко"), "коротко")


@unittest.skipIf(Image is None, "нужен Pillow")
class ImageTests(unittest.TestCase):
    def test_resize_keeps_aspect_and_no_upscale(self):
        out = fc.process_image(png_bytes(1920, 1080), fc.SHOT_SIZE, 70)
        im = Image.open(io.BytesIO(out))
        self.assertEqual(im.format, "JPEG")
        self.assertEqual(im.size, (480, 270))
        small = Image.open(io.BytesIO(fc.process_image(png_bytes(100, 50), fc.COVER_SIZE, 75)))
        self.assertEqual(small.size, (100, 50))

    def test_alpha(self):
        out = fc.process_image(png_bytes(64, 64, alpha=True), (32, 32), 75)
        self.assertEqual(Image.open(io.BytesIO(out)).mode, "RGB")

    def test_sized_url(self):
        self.assertEqual(fc.sized_url("https://a/b.png?w=10", 640), "https://a/b.png?w=640")


@unittest.skipIf(Image is None, "нужен Pillow")
class PipelineTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)
        items = []
        self.pages = {}
        for i in range(3):
            tid = "CUSA%05d" % (i + 1)
            cid = "EP0001-%s_00-GAME%08d" % (tid, i)
            href = "https://store.playstation.com/en-us/product/" + cid
            items.append([("G%d" % i), {"name": "Game %d PS4 & PS5" % i, "uuid": cid, "rating": "Full Game",
                                         "image": "https://img/cover%d.png" % i, "href": href}])
            ru = href.replace("/en-us/", "/ru-ru/")
            self.pages[ru] = make_page(cid, "Игра %d" % i if i != 1 else "Game 1", "<p>Описание %d</p>" % i,
                                       ["https://img/s%d_%d.png" % (i, k) for k in range(1, 7)])
        self.catalog = self.root / "cat.json"
        self.catalog.write_text(json.dumps(items))
        self.calls = []
        self.fail_net = set()
        orig = fc.http_get
        self.addCleanup(setattr, fc, "http_get", orig)
        fc.http_get = self.fake_get

    def tearDown(self):
        self.tmp.cleanup()

    def fake_get(self, url, timeout=30, tries=4):
        self.calls.append(url)
        if any(s in url for s in self.fail_net):
            raise fc.NetError("нет сети")
        if url in self.pages:
            return self.pages[url].encode("utf-8")
        if url.startswith("https://img/"):
            if "?w=" in url and "s0_3" in url:  # проверка запасного пути без ?w=
                raise fc.HttpError(400, url)
            if "?w=" in url and "cover0" in url:  # CDN отдал крошечную миниатюру -> нужен оригинал
                return png_bytes(100, 100)
            return png_bytes(1920, 1080) if "/s" in url else png_bytes(600, 600)
        raise fc.HttpError(404, url)

    def run_main(self, *extra):
        return fc.main(["--out", str(self.root / "data"), "--catalog", str(self.catalog),
                        "--workers", "2", "--delay", "0", "--locales", "ru-ru,ru-ua,en-us"] + list(extra))

    def test_full_run_and_pak_format(self):
        self.assertEqual(self.run_main(), 0)
        data = self.root / "data"
        doc = json.loads((data / "catalog.json").read_text(encoding="utf-8"))
        self.assertEqual(doc["count"], 3)
        pak = (data / "catalog.pak").read_bytes()
        self.assertEqual(pak[:8], b"PS4CATPK")
        self.assertEqual(struct.unpack("<II", pak[8:16]), (1, 0))
        names = {g["id"]: g for g in doc["games"]}
        g0 = names["CUSA00001"]
        self.assertEqual(g0["n"], "Game 0")
        self.assertEqual(g0["r"], "Игра 0")
        self.assertNotIn("r", names["CUSA00002"])  # ru == en (нет кириллицы) -> не дублируем
        self.assertEqual(len(g0["s"]), 4)
        off, ln = g0["d"]
        self.assertEqual(pak[off:off + ln].decode("utf-8"), "Описание 0")
        off, ln = g0["c"]
        self.assertEqual(Image.open(io.BytesIO(pak[off:off + ln])).size, (600, 600))  # оригинал 600x600 не увеличиваем
        off, ln = g0["s"][0]
        self.assertEqual(Image.open(io.BytesIO(pak[off:off + ln])).size, (480, 270))
        # смещения идут подряд без дыр и перекрытий
        refs = []
        for g in doc["games"]:
            refs.append(g["c"])
            refs.append(g["d"])
            refs.extend(g["s"])
        refs.sort()
        pos = fc.PAK_HEADER
        for o, n in refs:
            self.assertEqual(o, pos)
            pos += n
        self.assertEqual(pos, len(pak))

    def test_russian_from_second_locale(self):
        cid = "EP0001-CUSA00003_00-GAME00000002"
        href = "https://store.playstation.com/en-us/product/" + cid
        shots = ["https://img/s2_%d.png" % k for k in range(1, 3)]
        self.pages[href.replace("/en-us/", "/ru-ru/")] = make_page(cid, "Game 2", "<p>This is a plain English description of the game.</p>", shots)
        self.pages[href.replace("/en-us/", "/ru-ua/")] = make_page(cid, "Игра два", "<p>Это описание игры на русском языке.</p>", [])
        self.run_main("--ids", "CUSA00003")
        doc = json.loads((self.root / "data" / "catalog.json").read_text(encoding="utf-8"))
        g = doc["games"][0]
        pak = (self.root / "data" / "catalog.pak").read_bytes()
        off, ln = g["d"]
        self.assertEqual(pak[off:off + ln].decode("utf-8"), "Это описание игры на русском языке.")
        self.assertEqual(g["r"], "Игра два")
        self.assertEqual(len(g["s"]), 2)  # скриншоты взяты со страницы, где они были

    def test_english_kept_if_no_russian_anywhere(self):
        cid = "EP0001-CUSA00003_00-GAME00000002"
        href = "https://store.playstation.com/en-us/product/" + cid
        self.pages[href.replace("/en-us/", "/ru-ru/")] = make_page(cid, "Game 2", "<p>Only English text exists here for the game.</p>", ["https://img/s2_1.png"])
        self.run_main("--ids", "CUSA00003")
        doc = json.loads((self.root / "data" / "catalog.json").read_text(encoding="utf-8"))
        pak = (self.root / "data" / "catalog.pak").read_bytes()
        off, ln = doc["games"][0]["d"]
        self.assertEqual(pak[off:off + ln].decode("utf-8"), "Only English text exists here for the game.")

    def test_is_russian(self):
        self.assertTrue(fc.is_russian("Это описание игры на русском языке."))
        self.assertFalse(fc.is_russian("This is an English description."))
        self.assertFalse(fc.is_russian("Да"))

    def _concept_setup(self):
        cid = "EP0001-CUSA00003_00-GAME00000002"
        href = "https://store.playstation.com/en-us/product/" + cid
        shots = ["https://img/s2_%d.png" % k for k in range(1, 4)]
        en = make_page(cid, "Game 2", "<p>Only English text exists here for the game.</p>", shots)
        self.pages[href] = en + "<!-- Concept:555 -->"
        self.concept_url = "https://store.playstation.com/ru-ua/concept/555"
        self.pages[self.concept_url] = make_page("OTHER-REGION-ID", "Игра два", "<p>Это описание игры на русском языке.</p>", [])

    def _read_pak_desc(self):
        doc = json.loads((self.root / "data" / "catalog.json").read_text(encoding="utf-8"))
        g = doc["games"][0]
        pak = (self.root / "data" / "catalog.pak").read_bytes()
        off, ln = g["d"]
        return g, pak[off:off + ln].decode("utf-8")

    def test_russian_via_concept_id(self):
        self._concept_setup()
        self.run_main("--ids", "CUSA00003", "--locales", "en-us")
        g, desc = self._read_pak_desc()
        self.assertEqual(desc, "Это описание игры на русском языке.")
        self.assertEqual(g["r"], "Игра два")
        self.assertEqual(len(g["s"]), 3)  # скриншоты остались с английской страницы
        self.assertIn(self.concept_url, self.calls)

    def test_concept_failure_keeps_english(self):
        self._concept_setup()
        del self.pages[self.concept_url]  # русской страницы нет (404)
        self.run_main("--ids", "CUSA00003", "--locales", "en-us")
        _, desc = self._read_pak_desc()
        self.assertEqual(desc, "Only English text exists here for the game.")

    def test_refetch_desc_upgrades_without_redownloading_images(self):
        self._concept_setup()
        self.run_main("--ids", "CUSA00003", "--locales", "en-us", "--concept-locales", "")
        _, desc = self._read_pak_desc()
        self.assertEqual(desc, "Only English text exists here for the game.")
        img_calls = len([u for u in self.calls if u.startswith("https://img/")])
        self.run_main("--ids", "CUSA00003", "--locales", "en-us")          # без --refetch-desc: пропуск
        self.assertNotIn(self.concept_url, self.calls)
        self.run_main("--ids", "CUSA00003", "--locales", "en-us", "--refetch-desc")
        _, desc = self._read_pak_desc()
        self.assertEqual(desc, "Это описание игры на русском языке.")
        self.assertEqual(len([u for u in self.calls if u.startswith("https://img/")]), img_calls)

    def test_resume_skips_done(self):
        self.run_main("--no-pack")
        n = len(self.calls)
        self.assertGreater(n, 0)
        self.run_main("--no-pack")
        self.assertEqual(len(self.calls), n)

    def test_net_error_not_marked_done(self):
        self.fail_net = {"cover1.png"}
        self.run_main("--no-pack")
        meta = self.root / "data" / "cache" / "CUSA00002" / "meta.json"
        self.assertFalse(meta.exists())
        self.fail_net = set()
        self.run_main("--no-pack")
        self.assertTrue(meta.exists())

    def test_limit_and_ids(self):
        self.run_main("--limit", "1", "--no-pack")
        self.assertEqual(len(list((self.root / "data" / "cache").iterdir())), 1)
        self.run_main("--ids", "cusa00003", "--no-pack")
        self.assertTrue((self.root / "data" / "cache" / "CUSA00003" / "meta.json").exists())


if __name__ == "__main__":
    unittest.main(verbosity=2)
