#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
translate_catalog.py - переводит описания игр из кэша на русский.

Запускается после tools/fetch_catalog.py. Берёт data/cache/<CUSA>/desc.txt (английский),
пишет рядом desc_ru.txt. Уже переведённое пропускает, поэтому можно прерывать и
запускать снова. После перевода пересоберите pak: python3 tools/fetch_catalog.py --pack-only

Движки:
  argos   (по умолчанию) офлайн-модель Argos Translate. Интернет нужен только один раз,
          чтобы скачать языковую модель (~100 МБ). Качество среднее, на процессоре
          работает несколько часов на весь каталог.
          Установка: pip install argostranslate   (в WSL: pip install --break-system-packages argostranslate)
  google  неофициальный веб-эндпоинт Google Translate. Быстрее и качество выше, но нужен
          интернет, а Google может ограничить частые запросы. Используйте на свой риск.

Примеры:
  python3 tools/translate_catalog.py --limit 5            # проба на 5 играх
  python3 tools/translate_catalog.py                      # всё
  python3 tools/translate_catalog.py --engine google --workers 3
"""

import argparse
import concurrent.futures
import json
import re
import sys
import threading
import time
import urllib.parse
from pathlib import Path

import fetch_catalog as fc

GOOGLE_URL = "https://translate.googleapis.com/translate_a/single"
CHUNK_MAX = 1500          # символов в одном запросе к онлайн-движку
MAX_CONSECUTIVE_FAILS = 30


class TranslateError(Exception):
    pass


def has_cyrillic(s):
    return bool(re.search(r"[Ѐ-ӿ]", s))


def split_chunks(text, limit=CHUNK_MAX):
    """Режет текст по границам строк на куски не длиннее limit (строки не разрываются, длинные режутся по предложениям)."""
    pieces = []
    for line in text.split("\n"):
        while len(line) > limit:
            cut = max(line.rfind(". ", 0, limit), line.rfind("! ", 0, limit), line.rfind("? ", 0, limit))
            if cut < limit // 3:
                cut = line.rfind(" ", 0, limit)
            if cut <= 0:
                cut = limit
            pieces.append(line[:cut + 1])
            line = line[cut + 1:].lstrip()
        pieces.append(line)
    chunks, cur = [], ""
    for p in pieces:
        add = p if not cur else cur + "\n" + p
        if len(add) > limit and cur:
            chunks.append(cur)
            cur = p
        else:
            cur = add
    if cur or not chunks:
        chunks.append(cur)
    return chunks


def parse_google_response(raw):
    """Ответ gtx: [[[перевод, оригинал, ...], ...], ...] -> склеенный перевод."""
    data = json.loads(raw)
    segs = data[0] if data else []
    out = "".join(seg[0] for seg in segs if seg and isinstance(seg[0], str))
    return out


def make_google_engine(delay):
    def tr(text):
        parts = []
        for chunk in split_chunks(text):
            if not chunk.strip():
                parts.append(chunk)
                continue
            url = GOOGLE_URL + "?" + urllib.parse.urlencode(
                {"client": "gtx", "sl": "en", "tl": "ru", "dt": "t", "q": chunk})
            time.sleep(delay)
            try:
                raw = fc.http_get(url, timeout=30)
                parts.append(parse_google_response(raw.decode("utf-8", "replace")))
            except (fc.NetError, fc.HttpError, ValueError, IndexError, TypeError) as e:
                raise TranslateError(str(e))
        return "\n".join(parts)
    return tr


def make_argos_engine(log):
    try:
        import argostranslate.package as apkg
        import argostranslate.translate as atr
    except ImportError:
        raise TranslateError("Не установлен argostranslate. Выполните: pip install --break-system-packages argostranslate")

    def installed():
        langs = {l.code: l for l in atr.get_installed_languages()}
        return "en" in langs and "ru" in langs and langs["en"].get_translation(langs["ru"]) is not None

    if not installed():
        log("Скачиваю языковую модель en→ru (один раз, ~100 МБ)...")
        try:
            apkg.update_package_index()
            pkg = next(p for p in apkg.get_available_packages() if p.from_code == "en" and p.to_code == "ru")
            apkg.install_from_path(pkg.download())
        except Exception as e:  # сеть, формат индекса и т.д.
            raise TranslateError("Не удалось скачать модель Argos: %r" % e)
        if not installed():
            raise TranslateError("Модель en→ru не установилась")

    def tr(text):
        out = []
        for line in text.split("\n"):
            out.append(atr.translate(line, "en", "ru") if line.strip() else "")
        return "\n".join(out)
    return tr


def collect(cache_dir, ids=None, force=False):
    """Список папок игр, которым нужен перевод."""
    todo = []
    for d in sorted(cache_dir.iterdir()):
        if not d.is_dir() or (ids and d.name not in ids):
            continue
        mp, dp = d / "meta.json", d / "desc.txt"
        if not mp.exists() or not dp.exists() or not dp.stat().st_size:
            continue
        try:
            meta = json.loads(mp.read_text(encoding="utf-8"))
        except ValueError:
            continue
        if meta.get("status") not in ("ok", "partial") or meta.get("lang") == "ru":
            continue
        if (d / "desc_ru.txt").exists() and not force:
            continue
        todo.append(d)
    return todo


def run(cache_dir, engine, workers=1, ids=None, force=False, limit=0, log=print):
    todo = collect(cache_dir, ids, force)
    if limit:
        todo = todo[:limit]
    total = len(todo)
    log("К переводу: %d описаний" % total)
    state = {"done": 0, "ok": 0, "fail": 0, "streak": 0}
    lock = threading.Lock()
    stop = threading.Event()
    started = time.time()

    def job(d):
        if stop.is_set():
            return
        src = (d / "desc.txt").read_text(encoding="utf-8")
        ok = False
        try:
            ru = engine(src).strip()
            if not ru or not has_cyrillic(ru):
                raise TranslateError("пустой или нерусский результат")
            (d / "desc_ru.txt").write_text(ru, encoding="utf-8")
            ok = True
        except TranslateError as e:
            log("  [%s] не переведено: %s" % (d.name, e))
        with lock:
            state["done"] += 1
            state["ok" if ok else "fail"] += 1
            state["streak"] = 0 if ok else state["streak"] + 1
            if state["streak"] >= MAX_CONSECUTIVE_FAILS:
                stop.set()
            n = state["done"]
            if n % 25 == 0 or n == total:
                el = time.time() - started
                eta = (total - n) * el / max(1, n)
                log("%d/%d  переведено=%d ошибок=%d | прошло %d мин, осталось ~%d мин" % (
                    n, total, state["ok"], state["fail"], el // 60, eta // 60))

    if workers <= 1:
        for d in todo:
            job(d)
    else:
        with concurrent.futures.ThreadPoolExecutor(max_workers=workers) as ex:
            list(ex.map(job, todo))
    if stop.is_set():
        log("Остановлено: %d ошибок подряд. Проверьте интернет/ограничения и запустите снова, переведённое сохранится." % MAX_CONSECUTIVE_FAILS)
        return 1
    return 0


def main(argv=None):
    root = Path(__file__).resolve().parent.parent
    ap = argparse.ArgumentParser(description="Перевод описаний каталога на русский")
    ap.add_argument("--out", default=str(root / "data"), help="папка данных (по умолч. data/)")
    ap.add_argument("--engine", choices=("argos", "google"), default="argos")
    ap.add_argument("--workers", type=int, default=0, help="потоков (argos: 1, google: по умолч. 3)")
    ap.add_argument("--delay", type=float, default=0.4, help="пауза между запросами google, сек")
    ap.add_argument("--limit", type=int, default=0, help="только первые N описаний (проба)")
    ap.add_argument("--ids", help="только эти Title ID через запятую")
    ap.add_argument("--force", action="store_true", help="перевести заново")
    ap.add_argument("--no-pack", action="store_true", help="не пересобирать pak после перевода")
    opts = ap.parse_args(argv)

    out_dir = Path(opts.out)
    cache_dir = out_dir / "cache"
    if not cache_dir.is_dir():
        print("Нет папки %s. Сначала запустите tools/fetch_catalog.py" % cache_dir)
        return 1

    log = lambda m: print(m, flush=True)  # noqa: E731
    try:
        if opts.engine == "argos":
            engine, workers = make_argos_engine(log), 1
        else:
            engine, workers = make_google_engine(opts.delay), (opts.workers or 3)
    except TranslateError as e:
        log(str(e))
        return 2
    ids = {x.strip().upper() for x in opts.ids.split(",")} if opts.ids else None
    try:
        rc = run(cache_dir, engine, workers, ids, opts.force, opts.limit, log)
    except KeyboardInterrupt:
        log("\nПрервано. Запустите снова, переведённое сохранилось.")
        return 130
    if rc == 0 and not opts.no_pack:
        log("Пересобираю catalog.pak...")
        return fc.main(["--out", str(out_dir), "--pack-only"])
    return rc


if __name__ == "__main__":
    sys.exit(main())
