#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
fetch_catalog.py - собирает офлайн-каталог игр PS4 для приложения ps4installer.

Запускается на ПК с интернетом (не на консоли). Что делает:
  1. Берёт список игр из https://github.com/Ephellon/game-store-catalog (ps4/!.json),
     оставляет только PS4 (CUSA), убирает PS5-версии и дубли одной игры.
  2. Для каждой игры открывает страницу PS Store (ru-RU), берёт описание, русское
     название и ссылки на скриншоты.
  3. Качает обложку и до 4 скриншотов, уменьшает их (JPEG) под экран консоли.
  4. Собирает всё в data/catalog.pak (картинки и описания подряд) и
     data/catalog.json (маленький индекс: названия, Title ID, смещения в pak).

Докачка: всё скачанное лежит в data/cache/<CUSA>/, повторный запуск продолжает
с места остановки. Ctrl-C безопасен.

Примеры:
  python3 tools/fetch_catalog.py --limit 10          # пробный прогон на 10 играх
  python3 tools/fetch_catalog.py --ids CUSA00419 --debug   # одна игра, подробный вывод
  python3 tools/fetch_catalog.py                     # полная выгрузка
  python3 tools/fetch_catalog.py --pack-only         # только пересобрать pak из кэша

Нужен Pillow:  sudo apt install -y python3-pil   (или: pip install pillow)
"""

import argparse
import concurrent.futures
import html as htmllib
import io
import json
import re
import http.client
import struct
import sys
import threading
import time
import urllib.error
import urllib.parse
import urllib.request
from pathlib import Path

try:
    from PIL import Image
except ImportError:  # сообщение выдаём в main(), чтобы тесты разбора страницы работали без Pillow
    Image = None

CATALOG_URL = "https://raw.githubusercontent.com/Ephellon/game-store-catalog/main/ps4/%21.json"
UA = ("Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
      "(KHTML, like Gecko) Chrome/124.0 Safari/537.36")

# Размеры и качество картинок (подбирались под бюджет ~1,3 ГБ на 8 тыс. игр)
COVER_SIZE = (640, 640)
SHOT_SIZE = (480, 270)
COVER_QUALITY = 80
SHOT_QUALITY = 70
MAX_SHOTS = 4
DESC_MAX = 1500
# Просим у CDN уже уменьшенную картинку; если не получится, качаем оригинал
COVER_FETCH_W = 800
SHOT_FETCH_W = 800

# Размеры, под которые картинки сжимаются при упаковке (приложение показывает их ровно в таком виде, поэтому
# на консоли декодируется маленький JPEG). Должны совпадать с pkg/src/ui.cpp: COVER_W/COVER_H и SHOT_W/SHOT_H;
# плитка в сетке (340x340) делается из обложки 350x350 простым уменьшением. Поменяли вёрстку - пересоберите pak
# (python3 tools/fetch_catalog.py --catalog data/ps4_catalog.json --pack-only), сеть не нужна.
PACK_COVER = (350, 350)
PACK_SHOT = (254, 143)
PACK_QUALITY = 85

PAK_MAGIC = b"PS4CATPK"
PAK_VERSION = 1
PAK_HEADER = 16  # magic(8) + version(u32) + reserved(u32)
WARN_BYTES = int(3.5 * 1024 ** 3)
LIMIT_BYTES = 4 * 1024 ** 3

MAX_CONSECUTIVE_NET_ERRORS = 40


class NetError(Exception):
    """Сеть недоступна / таймауты. Игра не помечается как обработанная, повторится при следующем запуске."""


class HttpError(Exception):
    def __init__(self, code, url):
        super().__init__("HTTP %s: %s" % (code, url))
        self.code = code
        self.url = url


# --------------------------------------------------------------------------- сеть

def safe_url(url):
    """Кодирует пробелы и прочие недопустимые символы в пути и query (уже закодированное %XX не трогает)."""
    parts = urllib.parse.urlsplit(url)
    path = urllib.parse.quote(parts.path, safe="/%:@!$&'()*+,;=~-._")
    query = urllib.parse.quote(parts.query, safe="=&%:@!$'()*+,;/?~-._")
    return urllib.parse.urlunsplit((parts.scheme, parts.netloc, path, query, ""))


def http_get(url, timeout=30, tries=4):
    url = safe_url(url)
    last = None
    for attempt in range(tries):
        req = urllib.request.Request(url, headers={
            "User-Agent": UA,
            "Accept-Language": "ru-RU,ru;q=0.9,en;q=0.5",
            "Accept": "*/*",
        })
        try:
            with urllib.request.urlopen(req, timeout=timeout) as r:
                return r.read()
        except urllib.error.HTTPError as e:
            if e.code in (429, 500, 502, 503, 504):
                last = e
                time.sleep(min(60, 3 * (2 ** attempt)))
                continue
            raise HttpError(e.code, url)
        except http.client.InvalidURL as e:
            raise HttpError(0, "%s (%s)" % (url, e))
        except (urllib.error.URLError, TimeoutError, ConnectionError, OSError, http.client.HTTPException) as e:
            last = e
            time.sleep(2 * (attempt + 1))
    raise NetError("%s: %s" % (url, last))


# --------------------------------------------------------------------------- каталог

_RATING_PRIORITY = {"Full Game": 0, "Game Bundle": 1, "Premium Edition": 2}
_DROP_RATINGS = {"Soundtrack"}
_TITLE_ID_RE = re.compile(r"-(CUSA\d{5})_")
_PS5_ID_RE = re.compile(r"-(PPSA\d{5})_")

_TITLE_PATTERNS = [
    re.compile(r"\s*[\(\[]\s*PS4[™®]?\s*(?:(?:&|and|\+|/)\s*PS5[™®]?)?\s*(?:version|edition)?\s*[\)\]]", re.I),
    re.compile(r"\s*[-–:|]?\s*\bPS4[™®]?\s*(?:&|and|\+|/)\s*PS5[™®]?(?:\s*(?:version|edition))?", re.I),
    re.compile(r"\s*[-–:|]\s*PS4[™®]?(?:\s*(?:version|edition))?\s*$", re.I),
    re.compile(r"\s+PS4[™®]?\s*$", re.I),
]


def clean_title(name):
    s = name
    for p in _TITLE_PATTERNS:
        s = p.sub("", s)
    s = s.replace("™", "").replace("®", "")
    s = re.sub(r"\s+", " ", s).strip(" -–:|")
    return s or name.strip()


def load_catalog_file(path):
    """Читает ps4/!.json. Формат Ephellon: список пар [название, объект]; на всякий случай понимаем и список объектов."""
    with open(path, "r", encoding="utf-8") as f:
        raw = json.load(f)
    items = []
    for x in raw:
        if isinstance(x, dict):
            items.append(x)
        elif isinstance(x, (list, tuple)) and len(x) == 2 and isinstance(x[1], dict):
            items.append(x[1])
    return items


def ps4_cid(it):
    """Content ID для каталога. Игры с PS5-номером (PPSA) несут настоящий PS4-номер в адресе картинки
    (.../UP1003/CUSA02557_00/...): подставляем его, чтобы игру находили по Title ID, а не только по названию."""
    uuid = it.get("uuid", "")
    if "-PPSA" not in uuid:
        return uuid
    m = re.search(r"/(CUSA\d{5})_00/", it.get("image", ""))
    return uuid.replace(re.search(r"PPSA\d{5}", uuid).group(0), m.group(1)) if m else uuid


def select_games(items):
    """Оставляет PS4 (CUSA), по одной записи на Title ID (предпочитаем основную игру, а не издание)."""
    best = {}
    alts = {}
    for it in items:
        uuid = it.get("uuid", "")
        m = _TITLE_ID_RE.search(uuid)
        if not m and "PS4" in (it.get("platforms") or []):
            m = _PS5_ID_RE.search(uuid)  # игра для PS4 и PS5 под PS5-номером (Valhalla и др.): берём по названию
        if not m:
            continue  # чисто PS5 и всё без CUSA
        if it.get("rating") in _DROP_RATINGS:
            continue
        tid = m.group(1)
        alts.setdefault(tid, []).append((it.get("href", ""), uuid))
        key = (_RATING_PRIORITY.get(it.get("rating"), 3), len(it.get("name", "")), uuid)
        if tid not in best or key < best[tid][0]:
            best[tid] = (key, it)
    games = []
    for tid, (_, it) in best.items():
        games.append({
            "id": tid,
            "cid": it["uuid"],
            "name": clean_title(it.get("name", tid)),
            "image": it.get("image", ""),
            "href": it.get("href", ""),
            "alts": [a for a in alts.get(tid, []) if a[1] != it["uuid"]],
            "cid_out": ps4_cid(it),
        })
    cusa_names = {g["name"].lower() for g in games if g["id"].startswith("CUSA")}
    games = [g for g in games if g["id"].startswith("CUSA") or g["name"].lower() not in cusa_names]
    games.sort(key=lambda g: (g["name"].lower(), g["id"]))
    return games


def pick_sample(games, limit):
    """Для --limit берём игры равномерно по списку, а не первые N (там одни цифры и символы)."""
    if not limit or limit >= len(games):
        return games
    step = max(1, len(games) // limit)
    return games[::step][:limit]


# --------------------------------------------------------------------------- разбор страницы PS Store

_JSON_SCRIPT_RE = re.compile(r'<script[^>]*(?:type="application/json"|id="__NEXT_DATA__")[^>]*>(.*?)</script>', re.S)
_OVERVIEW_RE = re.compile(r'data-qa="mfe-game-overview#description"[^>]*>(.*?)</p>', re.S)
_META_RE = r'<meta[^>]+(?:property|name)="%s"[^>]+content="([^"]*)"'
_NOTICE_RE = re.compile(r'"type":"COMPATIBILITY_NOTICE","value":"((?:[^"\\]|\\.)*)"')


def html_to_text(s):
    s = re.sub(r"(?i)<br\s*/?>", "\n", s)
    s = re.sub(r"(?i)</p\s*>", "\n\n", s)
    s = re.sub(r"(?i)<li[^>]*>", "- ", s)
    s = re.sub(r"(?i)</li\s*>", "\n", s)
    s = re.sub(r"<[^>]+>", "", s)
    s = htmllib.unescape(s)
    s = s.replace("\r", "")
    s = re.sub(r"[​‌‍﻿]", "", s)
    s = re.sub(r"[\x00-\x08\x0b\x0c\x0e-\x1f]", "", s)
    lines = [re.sub(r"[ \t ]+", " ", ln).strip() for ln in s.split("\n")]
    s = "\n".join(lines)
    return re.sub(r"\n{3,}", "\n\n", s).strip()


def truncate_text(s, limit=DESC_MAX):
    if len(s) <= limit:
        return s
    cut = s[:limit]
    floor = int(limit * 0.6)
    pos = max(cut.rfind(". "), cut.rfind(".\n"), cut.rfind("!\n"), cut.rfind("? "))
    if pos >= floor:
        return cut[:pos + 1].rstrip()
    pos = cut.rfind(" ")
    if pos >= floor:
        cut = cut[:pos]
    return cut.rstrip(" ,;:-") + "…"


def _walk(node):
    stack = [node]
    while stack:
        n = stack.pop()
        if isinstance(n, dict):
            yield n
            stack.extend(n.values())
        elif isinstance(n, list):
            stack.extend(n)


def _deref(x, state):
    if isinstance(x, dict) and "__ref" in x and isinstance(state, dict):
        return state.get(x["__ref"], x)
    return x


def _shots_from_media(media, state):
    urls = []
    for m in media or []:
        m = _deref(m, state)
        if not isinstance(m, dict):
            continue
        role = str(m.get("role", "")).upper()
        typ = str(m.get("type", "IMAGE")).upper()
        url = m.get("url")
        if role == "SCREENSHOT" and typ == "IMAGE" and isinstance(url, str) and url not in urls:
            urls.append(url)
    return urls


def _desc_from_product(p, state):
    best = ""
    descs = p.get("descriptions")
    if isinstance(descs, list):
        by_type = {}
        for d in descs:
            d = _deref(d, state)
            if isinstance(d, dict) and isinstance(d.get("value"), str):
                by_type[str(d.get("type", "")).upper()] = d["value"]
        for t in ("LONG", "SHORT"):
            if by_type.get(t):
                best = by_type[t]
                break
        if not best and by_type:
            best = next(iter(by_type.values()))
    if not best:
        for k in ("longDescription", "description"):
            if isinstance(p.get(k), str) and p[k]:
                best = p[k]
                break
    return html_to_text(best) if best else ""


def parse_page(page_html, product_id=None):
    """Возвращает {'name','desc','shots','diag'}. diag - список строк для --debug."""
    res = {"name": "", "desc": "", "shots": [], "diag": [], "concept": "", "notice": ""}
    mn = _NOTICE_RE.search(page_html)  # строка про режимы: "1 players", "Online play required" и т.п.
    if mn:
        try:
            raw_notice = json.loads('"%s"' % mn.group(1))
        except ValueError:
            raw_notice = mn.group(1)
        res["notice"] = " | ".join(x.strip() for x in re.split(r"<br\s*/?>", raw_notice) if x.strip())
    mc = re.search(r"Concept:(\d+)", page_html)  # ID концепта один на все регионы магазина
    if mc:
        res["concept"] = mc.group(1)
    # На странице десятки блоков <script type="application/json"> (__NEXT_DATA__ и env:...).
    # Скриншоты лежат в блоке media-carousel как Product.media, описание - в HTML блока overview.
    docs = []
    for m in _JSON_SCRIPT_RE.finditer(page_html):
        try:
            docs.append(json.loads(m.group(1)))
        except ValueError:
            pass
    res["diag"].append("JSON-блоков на странице: %d" % len(docs))
    state = None
    for doc in docs:
        for d in _walk(doc):
            if isinstance(d.get("apolloState"), dict):
                state = d["apolloState"]
                break
        if state:
            break
    if not product_id:  # страница, открытая по ID концепта: ID товара берём из самой страницы
        for doc in docs:
            pp = doc.get("props", {}).get("pageProps", {}) if isinstance(doc, dict) else {}
            if isinstance(pp, dict) and isinstance(pp.get("productId"), str):
                product_id = pp["productId"]
                break
    cands = []
    for root in docs + ([state] if state else []):
        for d in _walk(root):
            if isinstance(d.get("name"), str) and isinstance(d.get("media"), list):
                cands.append(d)
    res["diag"].append("кандидатов-продуктов: %d" % len(cands))
    # сначала продукт с нужным id (лучше тот, где есть скриншоты), потом любой со скриншотами
    mine = [d for d in cands if product_id and product_id in str(d.get("id", ""))]
    chosen = None
    for group in (mine, cands):
        for d in group:
            if _shots_from_media(d["media"], state):
                chosen = d
                break
        if chosen is None and group is mine and mine:
            chosen = mine[0]
        if chosen is not None:
            break
    if chosen is None and cands:
        chosen = cands[0]
    if chosen is not None:
        res["name"] = chosen["name"].strip()
        res["shots"] = _shots_from_media(chosen["media"], state)
        res["desc"] = _desc_from_product(chosen, state)
        res["diag"].append("продукт: id=%s" % chosen.get("id"))

    # основное описание игры - абзац в блоке overview
    mo = _OVERVIEW_RE.search(page_html)
    if mo:
        txt = html_to_text(mo.group(1))
        if txt:
            res["desc"] = txt
            res["diag"].append("описание взято из блока overview")
    if not res["name"]:
        mt = re.search(r"<title>(.*?)</title>", page_html, re.S | re.I)
        if mt and mt.group(1).strip():
            res["name"] = htmllib.unescape(mt.group(1)).strip()
            res["diag"].append("название взято из <title>")

    # запасной вариант - мета-теги (описание и название, скриншотов тут нет)
    if not res["desc"]:
        mm = re.search(_META_RE % "og:description", page_html, re.I) or \
            re.search(_META_RE % "description", page_html, re.I)
        if mm:
            res["desc"] = html_to_text(mm.group(1))
            res["diag"].append("описание взято из мета-тега")
    if not res["name"]:
        mm = re.search(_META_RE % "og:title", page_html, re.I)
        if mm:
            res["name"] = htmllib.unescape(mm.group(1)).strip()
            res["diag"].append("название взято из мета-тега")
    return res


# --------------------------------------------------------------------------- картинки

def sized_url(url, width):
    base = url.split("?", 1)[0]
    return "%s?w=%d" % (base, width)


def process_image(data, size, quality):
    im = Image.open(io.BytesIO(data))
    im.load()
    if im.mode in ("RGBA", "LA") or (im.mode == "P" and "transparency" in im.info):
        im = im.convert("RGBA")
        bg = Image.new("RGB", im.size, (0, 0, 0))
        bg.paste(im, mask=im.split()[-1])
        im = bg
    else:
        im = im.convert("RGB")
    im.thumbnail(size, Image.Resampling.LANCZOS)  # сохраняет пропорции, не увеличивает
    out = io.BytesIO()
    im.save(out, "JPEG", quality=quality, optimize=True, progressive=False, subsampling=2)
    return out.getvalue()


def fit_jpeg(data, size, quality=PACK_QUALITY):
    """Масштабирует с обрезкой по центру до ровно size и сохраняет обычным (не прогрессивным, 4:2:0) JPEG:
    такой быстрее всего разбирается на консоли. Если Pillow нет или картинка не читается, отдаёт как есть."""
    if Image is None:
        return data
    try:
        im = Image.open(io.BytesIO(data))
        im.load()
        im = im.convert("RGB")
        w, h = size
        scale = max(w / im.width, h / im.height)
        nw, nh = max(w, int(round(im.width * scale))), max(h, int(round(im.height * scale)))
        im = im.resize((nw, nh), Image.Resampling.LANCZOS)
        left, top = (nw - w) // 2, (nh - h) // 2
        im = im.crop((left, top, left + w, top + h))
        out = io.BytesIO()
        im.save(out, "JPEG", quality=quality, optimize=True, progressive=False, subsampling=2)
        return out.getvalue()
    except Exception:
        return data


def _convert_game_images(d):
    cover = fit_jpeg((d / "cover.jpg").read_bytes(), PACK_COVER)
    shots = []
    for i in range(1, MAX_SHOTS + 1):
        sp = d / ("s%d.jpg" % i)
        if sp.exists():
            shots.append(fit_jpeg(sp.read_bytes(), PACK_SHOT))
    return cover, shots


def fetch_image(url, size, quality, fetch_w, delay):
    """Сначала пробуем уменьшенную версию с CDN. Если CDN ответил ошибкой, битой картинкой или
    слишком маленькой миниатюрой (меньше 80% нужной ширины), качаем оригинал."""
    attempts = [sized_url(url, fetch_w), url]
    min_w = int(size[0] * 0.8)
    last = None
    for i, u in enumerate(attempts):
        time.sleep(delay)
        try:
            raw = http_get(u)
            if i == 0 and Image.open(io.BytesIO(raw)).size[0] < min_w:
                last = ValueError("CDN вернул слишком маленькую картинку")
                continue
            return process_image(raw, size, quality)
        except HttpError as e:
            last = e
        except (OSError, ValueError) as e:  # Pillow не смог открыть
            last = e
    raise last


# --------------------------------------------------------------------------- обработка одной игры

class Stats:
    def __init__(self):
        self.lock = threading.Lock()
        self.counts = {}
        self.consecutive_net = 0

    def add(self, status):
        with self.lock:
            self.counts[status] = self.counts.get(status, 0) + 1
            self.consecutive_net = self.consecutive_net + 1 if status == "neterr" else 0
            return self.consecutive_net


def is_russian(text):
    """Текст в основном на кириллице (магазин иногда отдаёт английское описание на русской странице)."""
    letters = re.findall(r"[A-Za-zЀ-ӿ]", text)
    if len(letters) < 4:
        return False
    cyr = sum(1 for c in letters if c >= "Ѐ")
    return cyr / len(letters) >= 0.4


def _has_cyrillic(s):
    return bool(re.search(r"[Ѐ-ӿ]", s))


def refetch_notice(g, cache_dir, opts):
    """Добор строки про режимы игры (один заход на страницу en-us, картинки не трогаются).
    Пишет meta['notice'] (пустая строка = на странице такой строки нет)."""
    meta_path = cache_dir / g["id"] / "meta.json"
    if not meta_path.exists():
        return "skip"
    try:
        meta = json.loads(meta_path.read_text(encoding="utf-8"))
    except ValueError:
        return "skip"
    if "notice" in meta and not opts.force:
        return "skip"
    url = re.sub(r"/[a-z]{2}-[a-z]{2}/", "/%s/" % (opts.locales[0] if opts.locales else "en-us"),
                 g["href"], count=1)
    time.sleep(opts.delay)
    try:
        raw = http_get(url).decode("utf-8", "replace")
    except HttpError:
        meta["notice"] = ""
        meta_path.write_text(json.dumps(meta, ensure_ascii=False), encoding="utf-8")
        return "nopage"
    except NetError:
        return "neterr"
    meta["notice"] = parse_page(raw, g["cid"])["notice"]
    meta_path.write_text(json.dumps(meta, ensure_ascii=False), encoding="utf-8")
    return "ok" if meta["notice"] else "empty"


def process_game(g, cache_dir, opts):
    if opts.refetch_notice:
        return refetch_notice(g, cache_dir, opts)
    d = cache_dir / g["id"]
    meta_path = d / "meta.json"
    if meta_path.exists() and not opts.force:
        try:
            st = json.loads(meta_path.read_text(encoding="utf-8")).get("status")
        except ValueError:
            st = None
        lang_done = None
        if opts.refetch_desc:
            try:
                lang_done = json.loads(meta_path.read_text(encoding="utf-8")).get("lang")
            except ValueError:
                pass
        if opts.refetch_desc and st in ("ok", "partial") and lang_done != "ru":
            pass  # описание было не русским: пробуем получить русское (картинки из кэша не качаются заново)
        elif st == "ok" or (st and not opts.retry_partial):
            return "skip"
    d.mkdir(parents=True, exist_ok=True)
    debug = opts.debug
    log = opts.log

    def say(msg):
        if debug:
            log("  [%s] %s" % (g["id"], msg))

    net_failed = False
    got_page = False
    lang = ""
    desc_raw = ""
    desc_name = ""
    shots = []
    concept_id = ""
    notice = ""
    product_id = g["cid"]
    # 1) страницы по очереди (ru-ru, ru-ua, en-us). Русское описание берём с первой страницы, где оно
    #    действительно на русском (магазин может отдать английский текст на русской странице);
    #    скриншоты - с первой страницы, где они есть.
    for loc in opts.locales:
        raw = None
        for href, cid_try in [(g["href"], g["cid"])] + list(g.get("alts") or [])[:4]:
            url = re.sub(r"/[a-z]{2}-[a-z]{2}/", "/%s/" % loc, href, count=1)
            time.sleep(opts.delay)
            try:
                raw = http_get(url).decode("utf-8", "replace")
                product_id = cid_try
                break
            except HttpError as e:
                say("%s: %s" % (loc, e))
            except NetError as e:
                say("%s: сеть: %s" % (loc, e))
                net_failed = True
                break
        if net_failed:
            break
        if raw is None:
            continue
        got_page = True
        if debug:
            opts.save_debug("%s_%s.html" % (g["id"], loc), raw)
        p = parse_page(raw, product_id)
        for ln in p["diag"]:
            say("%s: %s" % (loc, ln))
        ru = bool(p["desc"]) and is_russian(p["desc"])
        say("%s: название=%r описание=%d симв. (%s) скриншотов=%d" % (
            loc, p["name"], len(p["desc"]), "рус" if ru else "не рус", len(p["shots"])))
        if p["concept"] and not concept_id:
            concept_id = p["concept"]
        if p["notice"] and not notice:
            notice = p["notice"]
        if p["shots"] and not shots:
            shots = p["shots"]
        if p["desc"]:
            if ru and lang != "ru":
                desc_raw, desc_name, lang = p["desc"], p["name"], "ru"
            elif not desc_raw:
                desc_raw, desc_name, lang = p["desc"], p["name"], "en"
        if lang == "ru" and shots:
            break
    # 1б) русская версия по ID концепта (ID товара у каждого региона свой, концепт общий)
    if got_page and not net_failed and lang != "ru" and concept_id:
        for loc in opts.concept_locales:
            url = "https://store.playstation.com/%s/concept/%s" % (loc, concept_id)
            time.sleep(opts.delay)
            try:
                raw = http_get(url).decode("utf-8", "replace")
            except HttpError as e:
                say("%s concept: %s" % (loc, e))
                continue
            except NetError as e:
                say("%s concept: сеть: %s" % (loc, e))
                net_failed = True
                break
            if debug:
                opts.save_debug("%s_%s_concept.html" % (g["id"], loc), raw)
            p = parse_page(raw, None)
            ru = bool(p["desc"]) and is_russian(p["desc"])
            say("%s concept %s: название=%r описание=%d симв. (%s) скриншотов=%d" % (
                loc, concept_id, p["name"], len(p["desc"]), "рус" if ru else "не рус", len(p["shots"])))
            if ru:
                desc_raw, desc_name, lang = p["desc"], p["name"], "ru"
                if p["shots"] and not shots:
                    shots = p["shots"]
                break
    if net_failed:
        return "neterr"
    if not got_page:
        meta_path.write_text(json.dumps({"status": "nopage"}), encoding="utf-8")
        return "nopage"
    page = {"shots": shots, "name": desc_name}

    desc = truncate_text(desc_raw) if desc_raw else ""
    (d / "desc.txt").write_text(desc, encoding="utf-8")

    # 2) обложка из каталога
    cover_ok = (d / "cover.jpg").exists() and not opts.force
    if not cover_ok and g["image"]:
        try:
            (d / "cover.jpg").write_bytes(fetch_image(g["image"], COVER_SIZE, COVER_QUALITY, COVER_FETCH_W, opts.delay))
            cover_ok = True
        except NetError:
            net_failed = True
        except (HttpError, OSError, ValueError) as e:
            say("обложка: %s" % e)

    # 3) скриншоты
    n_shots = 0
    for i, url in enumerate(page["shots"][:MAX_SHOTS], 1):
        fp = d / ("s%d.jpg" % i)
        if fp.exists() and not opts.force:
            n_shots += 1
            continue
        try:
            fp.write_bytes(fetch_image(url, SHOT_SIZE, SHOT_QUALITY, SHOT_FETCH_W, opts.delay))
            n_shots += 1
        except NetError:
            net_failed = True
        except (HttpError, OSError, ValueError) as e:
            say("скриншот %d: %s" % (i, e))
    if net_failed:
        return "neterr"  # meta не пишем: игра будет повторена

    name_ru = ""
    if lang == "ru" and page["name"]:
        cand = clean_title(page["name"])
        if _has_cyrillic(cand) and cand.lower() != g["name"].lower():
            name_ru = cand
    status = "ok" if (cover_ok and desc and n_shots >= 1) else "partial"
    meta = {"status": status, "lang": lang, "name_ru": name_ru, "shots": n_shots,
            "has_desc": bool(desc), "has_cover": cover_ok, "notice": notice}
    meta_path.write_text(json.dumps(meta, ensure_ascii=False), encoding="utf-8")
    return status


# --------------------------------------------------------------------------- упаковка

def needs_online(notice):
    """Магазин пишет 'Online play required' - без интернета игра не запустится или бесполезна.
    ('Online play optional' - это нормально: игра работает и без сети.)"""
    return "online play required" in (notice or "").lower()


def load_keep_ids(path):
    """tools/keep_online.txt: Title ID по одному в строке, # - комментарий."""
    ids = set()
    try:
        for line in Path(path).read_text(encoding="utf-8").splitlines():
            line = line.split("#", 1)[0].strip()
            if line:
                ids.add(line.upper())
    except OSError:
        pass
    return ids


def pack(games, cache_dir, out_dir, log=print, keep_online=False, keep_ids=()):
    """Собирает catalog.pak + catalog.json из кэша. Возвращает (кол-во игр, размер pak).
    Игры, которым нужен интернет, не включаются (keep_online=True оставляет их)."""
    out_dir.mkdir(parents=True, exist_ok=True)
    removed = []
    pak_tmp = out_dir / "catalog.pak.tmp"
    index = []
    skipped = 0
    with open(pak_tmp, "wb") as pak:
        pak.write(PAK_MAGIC + struct.pack("<II", PAK_VERSION, 0))
        pos = PAK_HEADER

        def put(blob):
            nonlocal pos
            pak.write(blob)
            ref = [pos, len(blob)]
            pos += len(blob)
            return ref

        selected = []   # (игра, папка кэша, meta)
        for g in games:
            d = cache_dir / g["id"]
            mp = d / "meta.json"
            if not mp.exists() or not (d / "cover.jpg").exists():
                skipped += 1
                continue
            meta = json.loads(mp.read_text(encoding="utf-8"))
            if meta.get("status") not in ("ok", "partial"):
                skipped += 1
                continue
            if not keep_online and g["id"] not in keep_ids and needs_online(meta.get("notice")):
                removed.append("%s  %s" % (g["id"], g["name"]))
                continue
            selected.append((g, d, meta))

        # Сжатие картинок - самая долгая часть: идёт в несколько потоков, результаты берутся по порядку.
        log("Сжимаю картинки под размеры экрана (%d игр)..." % len(selected))
        with concurrent.futures.ThreadPoolExecutor(max_workers=8) as pool:
            converted = pool.map(lambda t: _convert_game_images(t[1]), selected)
            for n, ((g, d, meta), (cover, shots_data)) in enumerate(zip(selected, converted), 1):
                if n % 500 == 0:
                    log("  %d / %d" % (n, len(selected)))
                entry = {"id": g["id"], "cid": g.get("cid_out", g["cid"]), "n": g["name"]}
                if meta.get("name_ru"):
                    entry["r"] = meta["name_ru"]
                entry["c"] = put(cover)
                dp = d / "desc_ru.txt"  # перевод на русский (tools/translate_catalog.py), если есть
                if not (dp.exists() and dp.stat().st_size):
                    dp = d / "desc.txt"
                if dp.exists() and dp.stat().st_size:
                    entry["d"] = put(dp.read_bytes())
                shots = [put(b) for b in shots_data]
                if shots:
                    entry["s"] = shots
                index.append(entry)
    pak_tmp.replace(out_dir / "catalog.pak")
    doc = {"version": PAK_VERSION, "count": len(index), "games": index}
    (out_dir / "catalog.json").write_text(json.dumps(doc, ensure_ascii=False, separators=(",", ":")), encoding="utf-8")
    log("Упаковано игр: %d, пропущено (нет данных): %d, убрано (нужен интернет): %d" % (
        len(index), skipped, len(removed)))
    (out_dir / "removed_online.txt").write_text("\n".join(removed) + "\n", encoding="utf-8")
    log("catalog.pak: %.1f МБ, catalog.json: %.1f КБ" % (pos / 1048576.0, (out_dir / "catalog.json").stat().st_size / 1024.0))
    if pos > LIMIT_BYTES:
        log("ВНИМАНИЕ: pak больше 4 ГБ, PKG не поместится в лимит. Уменьшите число игр или размеры картинок.")
    elif pos > WARN_BYTES:
        log("Внимание: pak больше 3,5 ГБ, запас до лимита 4 ГБ мал.")
    return len(index), pos


# --------------------------------------------------------------------------- main

def main(argv=None):
    root = Path(__file__).resolve().parent.parent
    ap = argparse.ArgumentParser(description="Офлайн-каталог игр PS4 для ps4installer")
    ap.add_argument("--out", default=str(root / "data"), help="папка результата (по умолчанию data/)")
    ap.add_argument("--catalog", help="локальный ps4/!.json (иначе скачивается с GitHub)")
    ap.add_argument("--limit", type=int, default=0, help="обработать только N игр (равномерно по списку)")
    ap.add_argument("--ids", help="только эти Title ID через запятую, например CUSA00419,CUSA01234")
    ap.add_argument("--workers", type=int, default=6, help="параллельных загрузок (по умолч. 6)")
    ap.add_argument("--delay", type=float, default=0.15, help="пауза перед запросом, сек (по умолч. 0.15)")
    ap.add_argument("--locales", default="en-us",
                    help="какие версии страницы пробовать по порядку (по умолч. en-us)")
    ap.add_argument("--concept-locales", default="ru-ua",
                    help="где искать русское описание по ID концепта (по умолч. ru-ua; пусто = не искать)")
    ap.add_argument("--refetch-desc", action="store_true",
                    help="для уже скачанных игр без русского описания попробовать получить русское")
    ap.add_argument("--refetch-notice", action="store_true",
                    help="только добрать у скачанных игр строку про режимы (одиночная/сетевая), картинки не трогать")
    ap.add_argument("--retry-partial", action="store_true", help="повторить игры с неполными данными")
    ap.add_argument("--force", action="store_true", help="перекачать всё выбранное заново")
    ap.add_argument("--pack-only", action="store_true", help="не качать, только собрать pak из кэша")
    ap.add_argument("--keep-online", action="store_true",
                    help="не убирать игры с 'Online play required' (по умолч. убираются)")
    ap.add_argument("--no-pack", action="store_true", help="только скачать, pak не собирать")
    ap.add_argument("--debug", action="store_true", help="подробный вывод и сохранение страниц в data/debug/")
    opts = ap.parse_args(argv)

    opts.locales = [x.strip().lower() for x in opts.locales.split(",") if x.strip()]
    opts.concept_locales = [x.strip().lower() for x in opts.concept_locales.split(",") if x.strip()]
    out_dir = Path(opts.out)
    cache_dir = out_dir / "cache"
    out_dir.mkdir(parents=True, exist_ok=True)
    cache_dir.mkdir(parents=True, exist_ok=True)

    def log(msg):
        print(msg, flush=True)
    opts.log = log

    debug_dir = out_dir / "debug"
    saved = [0]

    def save_debug(name, text):
        if saved[0] >= 40:
            return
        debug_dir.mkdir(parents=True, exist_ok=True)
        (debug_dir / name).write_text(text, encoding="utf-8")
        saved[0] += 1
    opts.save_debug = save_debug

    if not opts.pack_only and Image is None:
        log("Не найден Pillow. Установите: sudo apt install -y python3-pil   (или: pip install pillow)")
        return 2

    # каталог
    cat_path = Path(opts.catalog) if opts.catalog else out_dir / "ps4_catalog.json"
    if not cat_path.exists():
        log("Скачиваю список игр с GitHub (около 7 МБ)...")
        try:
            cat_path.write_bytes(http_get(CATALOG_URL, timeout=120))
        except (NetError, HttpError) as e:
            log("Не удалось скачать список игр: %s" % e)
            log("Скачайте файл ps4/!.json из Ephellon/game-store-catalog вручную и укажите --catalog <путь>")
            return 1
    games = select_games(load_catalog_file(cat_path))
    log("Игр PS4 в каталоге (без PS5 и дублей): %d" % len(games))

    if opts.ids:
        want = {x.strip().upper() for x in opts.ids.split(",") if x.strip()}
        games = [g for g in games if g["id"] in want]
        missing = want - {g["id"] for g in games}
        if missing:
            log("Нет в каталоге: %s" % ", ".join(sorted(missing)))
    else:
        games = pick_sample(games, opts.limit)

    if not opts.pack_only:
        stats = Stats()
        stop = threading.Event()
        total = len(games)
        started = time.time()
        done = [0]
        log("К обработке: %d игр, потоков: %d" % (total, opts.workers))

        def job(g):
            if stop.is_set():
                return
            try:
                st = process_game(g, cache_dir, opts)
            except Exception as e:  # одна игра не должна валить весь прогон
                log("  [%s] ошибка: %r" % (g["id"], e))
                st = "error"
            streak = stats.add(st)
            if streak >= MAX_CONSECUTIVE_NET_ERRORS:
                stop.set()
            done[0] += 1
            if opts.debug or done[0] % 25 == 0 or done[0] == total:
                el = time.time() - started
                real = max(1, done[0] - stats.counts.get("skip", 0))
                eta = (total - done[0]) * el / real
                log("%d/%d  %s  | %s | прошло %d мин, осталось ~%d мин" % (
                    done[0], total, g["id"],
                    " ".join("%s=%d" % kv for kv in sorted(stats.counts.items())),
                    el // 60, eta // 60))

        ex = concurrent.futures.ThreadPoolExecutor(max_workers=max(1, opts.workers))
        try:
            futs = [ex.submit(job, g) for g in games]
            for f in futs:
                f.result()
        except KeyboardInterrupt:
            stop.set()
            log("\nПрервано. Запустите команду снова, загрузка продолжится с этого места.")
            ex.shutdown(wait=False, cancel_futures=True)
            return 130
        ex.shutdown()
        if stop.is_set():
            log("Остановлено: подряд %d сетевых ошибок. Проверьте интернет (или подождите: возможно, сайт "
                "временно ограничил запросы) и запустите снова, докачка продолжится." % MAX_CONSECUTIVE_NET_ERRORS)
            return 1
        log("Итого: " + ", ".join("%s=%d" % kv for kv in sorted(stats.counts.items())))
        log("ok = полные данные, partial = чего-то не хватило (нет описания/скриншотов), "
            "nopage = страницы нет, neterr = сеть (повторится при следующем запуске), skip = уже было")

    if not opts.no_pack:
        pack(games, cache_dir, out_dir, log, keep_online=opts.keep_online,
             keep_ids=load_keep_ids(Path(__file__).resolve().parent / "keep_online.txt"))
        log("Готово. Файлы: %s и %s" % (out_dir / "catalog.pak", out_dir / "catalog.json"))
    return 0


if __name__ == "__main__":
    sys.exit(main())
