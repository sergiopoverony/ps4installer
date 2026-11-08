#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Заплатка каталога: только то, что изменилось с прошлого раза (catalog_N.json + catalog_N.pak).

Использование (ежемесячно, когда выходит новая выгрузка rutracker):
    python tools/rutracker_table.py rutracker-НОВАЯ.xml.xz ps4_games_new.csv
    python tools/make_patch.py data ps4_games_new.csv

В папке data лежат основной catalog.json + catalog.pak и уже сделанные заплатки. Скрипт строит новое состояние (как tools/merge_torrents.py,
но во временной папке: основной каталог не меняется), сравнивает его с текущим (основной каталог + прежние заплатки) и пишет разницу
в следующий свободный номер: catalog_2.json/.pak, потом catalog_3 и так далее. Эти файлы кладут на консоль в /data/ps4installer_app/catalog/.
Заплатка содержит: новые игры (с обложками и скриншотами), изменения раздач у существующих ("m": 1) и список удалённых ("remove")."""
import argparse
import json
import shutil
import struct
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import merge_torrents as M  # noqa: E402

TF = ("th", "tu", "tv", "td", "tf", "tb", "ts", "ty")   # поля раздачи
REFS = ("c", "d")                                  # одиночные ссылки в pak; "s" - список ссылок


def apply_state(data_dir):
    """Текущее состояние: основной каталог + заплатки по порядку. Возвращает {id: запись}, номер последней заплатки."""
    doc = json.loads((data_dir / "catalog.json").read_text(encoding="utf-8"))
    state = {g["id"]: g for g in doc["games"]}
    last = 1
    for n in range(2, 100):
        p = data_dir / ("catalog_%d.json" % n)
        if not p.exists():
            continue
        last = n
        pj = json.loads(p.read_text(encoding="utf-8"))
        for i in pj.get("remove", []):
            state.pop(i, None)
        for g in pj.get("games", []):
            if g.get("m"):
                if g["id"] in state:
                    state[g["id"]].update({k: v for k, v in g.items() if k not in ("id", "m")})
            else:
                state[g["id"]] = g
    return state, last


def tf_of(g):
    """Поля раздачи записи (нет поля - ноль/пусто)."""
    return {k: (g.get(k) or ("" if k == "th" else 0)) for k in TF}


def make_patch(data_dir, table_path, media=True, log=print):
    data_dir = Path(data_dir)
    old, last = apply_state(data_dir)
    number = last + 1
    with tempfile.TemporaryDirectory() as td:
        td = Path(td)
        shutil.copyfile(data_dir / "catalog.json", td / "catalog.json")
        shutil.copyfile(data_dir / "catalog.pak", td / "catalog.pak")
        # общий кэш картинок: повторные месяцы не качают одно и то же
        cache = data_dir / ".media_cache"
        cache.mkdir(exist_ok=True)
        stats = M.merge(td, table_path, media=media, log=log, cache_dir=cache)
        new_doc = json.loads((td / "catalog.json").read_text(encoding="utf-8"))
        new = {g["id"]: g for g in new_doc["games"]}
        pak_new = (td / "catalog.pak").read_bytes()

        out_games, remove = [], []
        blob = bytearray(b"PS4CATPK" + struct.pack("<II", 1, 0))

        def put(ref):
            off = len(blob)
            blob.extend(pak_new[ref[0]:ref[0] + ref[1]])
            return [off, ref[1]]

        added = changed = 0
        for gid, g in new.items():
            o = old.get(gid)
            if o is None:   # новая игра: берём запись целиком, картинки переносим в pak заплатки
                e = dict(g)
                for k in REFS:
                    if g.get(k):
                        e[k] = put(g[k])
                if g.get("s"):
                    e["s"] = [put(r) for r in g["s"]]
                out_games.append(e)
                added += 1
            else:
                a, b = tf_of(o), tf_of(g)
                diff = {k: b[k] for k in TF if a[k] != b[k]}
                if diff:
                    out_games.append(dict({"id": gid, "m": 1}, **diff))
                    changed += 1
        for gid, o in old.items():
            if gid not in new and o.get("x"):   # раздачи этой игры больше нет
                remove.append(gid)
        if not out_games and not remove:
            log("Изменений нет: заплатка не нужна.")
            return None
        patch = {"version": 1, "patch": number, "games": out_games}
        if remove:
            patch["remove"] = remove
        (data_dir / ("catalog_%d.json" % number)).write_text(json.dumps(patch, ensure_ascii=False, separators=(",", ":")), encoding="utf-8")
        (data_dir / ("catalog_%d.pak" % number)).write_bytes(bytes(blob))
        res = {"patch": number, "added": added, "changed": changed, "removed": len(remove), "pak_mb": round(len(blob) / 1048576, 1)}
        log("Заплатка %d: новых игр %d, изменено %d, удалено %d, catalog_%d.pak: %.1f МБ" % (number, added, changed, len(remove), number, res["pak_mb"]))
        log("Положите catalog_%d.json и catalog_%d.pak на консоль в /data/ps4installer_app/catalog/" % (number, number))
        return res


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("data_dir")
    ap.add_argument("table")
    ap.add_argument("--no-media", action="store_true", help="не скачивать картинки")
    a = ap.parse_args()
    make_patch(a.data_dir, a.table, media=not a.no_media, log=lambda s: print(s, flush=True))


if __name__ == "__main__":
    main()
