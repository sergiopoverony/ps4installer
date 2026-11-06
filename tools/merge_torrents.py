#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Склеивает офлайн-каталог (data/catalog.json + catalog.pak) с таблицей раздач рутрекера (tools/rutracker_table.py).

    python tools/merge_torrents.py data ps4_games.csv            # data - папка с catalog.json и catalog.pak
    python tools/merge_torrents.py data ps4_games.csv --no-media # не качать картинки для игр, которых нет в каталоге магазина

Что делает:
  1. Игре каталога ставит поля раздачи: th - хэш (40 hex), tu - русский интерфейс, tv - русская озвучка, td - DLC, tf - исправление, tb - Backport,
     ts - размер, МБ. Приложение показывает во вкладке «Каталог игр» только игры с th (если в каталоге есть хоть одна раздача).
     Совпадение сначала по коду CUSA, затем по названию (у европейских раздач код другой). Если у игры несколько раздач: сначала с русским
     интерфейсом, потом с русской озвучкой, потом новее.
  2. Раздачи, которых нет в каталоге магазина, добавляются отдельными играми (id "T<хэш>", метка x=1). Обложка и до 4 скриншотов качаются по ссылкам
     из раздачи, уменьшаются как в fetch_catalog.py и дописываются в конец catalog.pak; описание - из раздачи (до 500 символов).
  3. Запуск можно повторять: прежние добавленные игры убираются, pak обрезается до исходного размера (base_pak в catalog.json), потом пишется заново.
Рядом с catalog.json сохраняется catalog.json.bak (при первом запуске)."""
import argparse
import csv
import hashlib
import io
import json
import re
import shutil
import sys
import time
import unicodedata
import urllib.request
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import fetch_catalog as FC   # размеры картинок и fit_jpeg - те же, что у основного каталога

ROMAN = {"i": "1", "ii": "2", "iii": "3", "iv": "4", "v": "5", "vi": "6", "vii": "7", "viii": "8", "ix": "9", "x": "10",
         "xi": "11", "xii": "12", "xiii": "13", "xiv": "14", "xv": "15"}
STOP = {"the", "edition", "editions", "definitive", "deluxe", "complete", "remastered", "remake", "ultimate", "gold", "goty",
        "standard", "digital", "premium", "ps4", "version", "fpkg", "repack", "russound", "collectors", "legendary",
        "anniversary", "hd", "bundle", "game", "of", "year", "remod", "xxl", "mod"}
# слова, после которых это уже другая игра, а не издание той же (Doom Eternal, ... VR)
SEQUEL = {"eternal", "vr", "origins", "odyssey", "legacy", "rising", "unleashed", "reborn", "reloaded", "returns", "redux", "arena",
          "tactics", "online", "world", "racing", "war", "ii", "iii", "iv", "ex", "zero", "next", "rush", "remix", "legends", "heroes"}
FIELDS = ("th", "tu", "tv", "td", "tf", "tb", "ts")


def tokens(s):
    return _norm(s)


def norm(s):
    return "".join(_norm(s))


def _norm(s):
    """Название -> ключ для сравнения (без регистра, знаков, 'Deluxe', римских цифр)."""
    s = re.sub(r"[™®©℠]", "", s)   # до NFKD: иначе ™ превращается в буквы «TM»
    s = re.split(r"\s[/|]\s*(?=[^/|]*[а-яё])", s, maxsplit=1, flags=re.I)[0]   # «Game / Русское название» -> «Game»
    s = re.sub(r"\b(?:v|ver\.?)?\s?\d+(?:\.\d+)+\b", " ", s, flags=re.I)   # номера версий: 1.04, v1.2.3
    s = unicodedata.normalize("NFKD", s)
    s = "".join(c for c in s if not unicodedata.combining(c))
    s = s.lower().replace("&", " and ")
    s = re.sub(r"[™®©]", "", s)
    s = re.sub(r"\([^)]*\)", " ", s)
    s = re.sub(r"русская озвучка|русский звук", " ", s)
    out = []
    for x in re.findall(r"[0-9a-zа-я]+", s):
        if x in STOP:
            continue
        x = ROMAN.get(x, x)
        if x.isdigit() and out and out[-1] == x:
            continue
        out.append(x)
    return out


def yes(v):
    return 1 if (v or "").strip() else 0


def num(v):
    try:
        return float(str(v).replace(",", "."))
    except ValueError:
        return 0.0


def hash_of(row):
    m = re.search(r"btih:([0-9A-Fa-f]{40})", row.get("Magnet") or "")
    return m.group(1).lower() if m else ""


def rank(r):
    return (yes(r["Рус. интерфейс"]), yes(r["Рус. озвучка"]), r["Дата"])


def fields_of(r):
    return {"th": hash_of(r), "tu": yes(r["Рус. интерфейс"]), "tv": yes(r["Рус. озвучка"]),
            "td": yes(r["DLC"]) if (r["DLC"] or "").strip() == "Да" else 0, "tf": yes(r["Fix"]), "tb": yes(r["Backport"]),
            "ts": int(num(r["Размер, ГБ"]) * 1024)}


def read_table(path):
    with open(path, encoding="utf-8-sig", newline="") as f:
        return [r for r in csv.DictReader(f, delimiter=";") if hash_of(r)]


CACHE = None   # папка с уже скачанными картинками: при повторном запуске их не качаем заново


def download(url, tries=3, delay=0.4):
    cf = None
    if CACHE is not None:
        cf = CACHE / (hashlib.sha1(url.encode("utf-8")).hexdigest() + ".bin")
        if cf.exists():
            return cf.read_bytes()
    data = _download(url, tries, delay)
    if cf is not None:
        cf.write_bytes(data)
    return data


def _download(url, tries, delay):
    last = None
    for _ in range(tries):
        time.sleep(delay)
        try:
            req = urllib.request.Request(url, headers={"User-Agent": "Mozilla/5.0", "Referer": "https://rutracker.org/"})
            with urllib.request.urlopen(req, timeout=25) as r:
                return r.read()
        except Exception as e:   # сеть, 404, защита от хотлинка: пробуем ещё, потом сдаёмся
            last = e
    raise last


def fit_whole(data, size, quality=88):
    """Картинка целиком, без обрезки: вписывается в size, остальное заполняет размытая копия той же картинки."""
    try:
        from PIL import Image, ImageFilter
        import io as _io
        im = Image.open(_io.BytesIO(data)); im.load(); im = im.convert("RGB")
        w, h = size
        scale = max(w / im.width, h / im.height)
        bg = im.resize((max(w, int(im.width * scale)), max(h, int(im.height * scale))), Image.Resampling.BILINEAR)
        l, t = (bg.width - w) // 2, (bg.height - h) // 2
        bg = bg.crop((l, t, l + w, t + h)).filter(ImageFilter.GaussianBlur(18))
        sc = min(w / im.width, h / im.height)
        fg = im.resize((max(1, int(round(im.width * sc))), max(1, int(round(im.height * sc)))), Image.Resampling.LANCZOS)
        bg.paste(fg, ((w - fg.width) // 2, (h - fg.height) // 2))
        out = _io.BytesIO()
        bg.save(out, "JPEG", quality=quality, optimize=True, progressive=False, subsampling=2)
        return out.getvalue()
    except Exception:
        return FC.fit_jpeg(data, size)


def media_for(row, log):
    """(обложка JPEG или None, [скриншоты JPEG]) в размерах основного каталога."""
    cover, shots = None, []
    cu = (row.get("Обложка (URL)") or "").strip()
    if cu:
        try:
            cover = fit_whole(download(cu), FC.PACK_COVER)
        except Exception as e:
            log("  обложка не скачалась (%s): %s" % (e, cu))
    for u in (row.get("Скриншоты (URL)") or "").split()[:FC.MAX_SHOTS]:
        try:
            shots.append(FC.fit_jpeg(download(u), FC.PACK_SHOT))
        except Exception as e:
            log("  скриншот не скачался (%s): %s" % (e, u))
    return cover, shots


def merge(data_dir, table_path, media=True, log=print):
    data_dir = Path(data_dir)
    cj, pk = data_dir / "catalog.json", data_dir / "catalog.pak"
    doc = json.loads(cj.read_text(encoding="utf-8"))
    bak = data_dir / "catalog.json.bak"
    if not bak.exists():
        shutil.copy(cj, bak)
    # возвращаем pak и каталог к исходному состоянию (без добавленных ранее игр)
    base = doc.get("base_pak") or pk.stat().st_size
    with open(pk, "r+b") as f:
        f.truncate(base)
    games = [g for g in doc["games"] if not g.get("x")]
    for g in games:
        for k in FIELDS:
            g.pop(k, None)

    global CACHE
    CACHE = data_dir / ".media_cache"
    CACHE.mkdir(exist_ok=True)
    table = read_table(table_path)
    by_code, by_name = {}, {}
    for r in table:
        code = (r["Код диска"] or "").strip().upper()
        if re.fullmatch(r"CUSA\d{5}", code):
            by_code.setdefault(code, []).append(r)
        by_name.setdefault(norm(r["Название"]), []).append(r)

    used = set()
    n_code = n_name = 0
    for g in games:
        m = re.search(r"-(CUSA\d{5})_", g.get("cid", ""))
        rows = by_code.get(m.group(1)) if m else None
        how = "code"
        if not rows:
            rows, how = by_name.get(norm(g["n"])), "name"
        if not rows:
            continue
        rows = sorted(rows, key=rank, reverse=True)
        g.update(fields_of(rows[0]))
        used.update(id(x) for x in rows)
        if how == "code":
            n_code += 1
        else:
            n_name += 1

    # третий проход: раздача называется «Игра + 1 слово» (reMod, Anniversary и т.п.), а игра Sony ещё без раздачи -> тоже она.
    # Так у игры остаётся оригинальная обложка из PS Store, а не отдельная запись с обложкой из торрента.
    free = [(tokens(g["n"]), g) for g in games if "th" not in g and len(norm(g["n"])) >= 5]
    n_pref = 0
    for r in sorted((x for x in table if id(x) not in used), key=rank, reverse=True):
        if id(r) in used:
            continue
        tr = tokens(r["Название"])
        best = None
        for tg, g in free:
            if "th" in g or len(tr) <= len(tg) or len(tr) - len(tg) > 1 or tr[:len(tg)] != tg:
                continue
            if tr[len(tg)].isdigit() or tr[len(tg)] in SEQUEL:
                continue
            if best is None or len(tg) > len(best[0]):
                best = (tg, g)
        if best:
            same = [x for x in table if id(x) not in used and tokens(x["Название"]) == tr]
            best[1].update(fields_of(sorted(same, key=rank, reverse=True)[0]))
            used.update(id(x) for x in same)
            n_pref += 1
    n_name += n_pref

    # раздачи без пары в каталоге магазина: отдельные игры, по одной лучшей на игру
    rest = {}
    for r in table:
        if id(r) not in used:
            rest.setdefault(norm(r["Название"]) or hash_of(r), []).append(r)
    extras = []
    with open(pk, "r+b") as pak:
        pak.seek(0, 2)
        pos = pak.tell()
        got_media = 0
        for i, key in enumerate(sorted(rest, key=lambda k: sorted(rest[k], key=rank, reverse=True)[0]["Название"].lower()), 1):
            r = sorted(rest[key], key=rank, reverse=True)[0]
            code = (r["Код диска"] or "").strip().upper()
            e = {"id": "T" + hash_of(r)[:12], "n": r["Название"], "x": 1}
            if re.fullmatch(r"CUSA\d{5}", code):   # cid в формате каталога, чтобы приложение нашло Title ID
                e["cid"] = "XX0000-%s_00-TORRENT000000000" % code
            else:
                e["cid"] = ""
            blobs = {}
            if r["Описание (500 симв.)"].strip():
                blobs["d"] = r["Описание (500 симв.)"].strip().encode("utf-8")
            if media:
                cover, shots = media_for(r, log)
                if cover:
                    blobs["c"] = cover
                if shots:
                    blobs["s"] = shots
                if cover:
                    got_media += 1
                if i % 50 == 0:
                    log("  картинки: %d из %d" % (i, len(rest)))
            for k in ("c", "d"):
                if k in blobs:
                    pak.write(blobs[k])
                    e[k] = [pos, len(blobs[k])]
                    pos += len(blobs[k])
            if "s" in blobs:
                e["s"] = []
                for b in blobs["s"]:
                    pak.write(b)
                    e["s"].append([pos, len(b)])
                    pos += len(b)
            e.update(fields_of(r))
            extras.append(e)
    # подозрительные пары: название раздачи начинается с названия игры магазина, но сопоставлено не было. Только отчёт, ничего не меняется.
    shop = [(norm(g["n"]), g) for g in games if len(norm(g["n"])) >= 5]
    with open(data_dir / "near_matches.csv", "w", encoding="utf-8-sig", newline="") as f:
        w = csv.writer(f, delimiter=";")
        w.writerow(["Раздача", "Игра магазина", "Код"])
        for e in extras:
            ne = norm(e["n"])
            for ns, g in shop:
                if ne.startswith(ns) and ne != ns:
                    w.writerow([e["n"], g["n"], g.get("cid", "")])
    games.extend(extras)
    doc["games"] = games
    doc["count"] = len(games)
    doc["base_pak"] = base
    cj.write_text(json.dumps(doc, ensure_ascii=False, separators=(",", ":")), encoding="utf-8")
    stats = {"catalog": len(games) - len(extras), "with_torrent": n_code + n_name, "by_code": n_code, "by_name": n_name,
             "extras": len(extras), "extras_with_cover": got_media if media else 0, "pak_mb": round(pos / 1048576.0, 1)}
    return stats


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("data_dir", help="папка с catalog.json и catalog.pak")
    ap.add_argument("table", help="ps4_games.csv (tools/rutracker_table.py)")
    ap.add_argument("--no-media", action="store_true", help="не качать обложки и скриншоты")
    a = ap.parse_args()
    st = merge(a.data_dir, a.table, media=not a.no_media, log=lambda s: print(s, flush=True))
    print("Игр магазина в каталоге:", st["catalog"])
    print("  из них с раздачей:", st["with_torrent"], "(по коду %d, по названию %d)" % (st["by_code"], st["by_name"]))
    print("Раздач вне каталога магазина, добавлено игр:", st["extras"], "(с обложкой: %d)" % st["extras_with_cover"])
    print("catalog.pak: %s МБ" % st["pak_mb"])
    if st["pak_mb"] > 3500:
        print("ВНИМАНИЕ: pak больше 3,5 ГБ, лимит PKG 4 ГБ")
    return 0


if __name__ == "__main__":
    sys.exit(main())
