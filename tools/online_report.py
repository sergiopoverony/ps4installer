#!/usr/bin/env python3
# -*- coding: utf-8 -*-
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 SergioPoverony
"""
online_report.py - отчёт по режимам игр (только сетевые / есть одиночный) из data/cache.
Ничего не качает и не меняет. Сначала нужно добрать строки:
  python3 tools/fetch_catalog.py --refetch-notice

  python3 tools/online_report.py              # сводка и кандидаты на удаление
  python3 tools/online_report.py --all        # весь список кандидатов
Правило: у игры в строке есть "Online play required" - без интернета она не работает, её убираем.
"Online play optional" и "N network players" само по себе - не причина (игра идёт и без сети).
При упаковке такие игры не попадают в каталог (список - data/removed_online.txt).
"""
import argparse
import collections
import json
import re
import sys
from pathlib import Path


sys.path.insert(0, str(Path(__file__).resolve().parent))


def classify(notice):
    """Возвращает 'online_only' | 'has_offline' | 'unknown'."""
    if not notice:
        return "unknown"
    from fetch_catalog import needs_online
    return "online_only" if needs_online(notice) else "has_offline"


def main():
    root = Path(__file__).resolve().parent.parent
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default=str(root / "data"))
    ap.add_argument("--all", action="store_true", help="показать всех кандидатов, а не первых 40")
    opts = ap.parse_args()
    cache = Path(opts.out) / "cache"
    names = {}
    cp = Path(opts.out) / "catalog.json"
    if cp.exists():
        for g in json.loads(cp.read_text(encoding="utf-8")).get("games", []):
            names[g["id"]] = g.get("n", "")

    kinds = collections.Counter()
    cands = []
    phrases = collections.Counter()
    for d in sorted(cache.iterdir()):
        mp = d / "meta.json"
        if not mp.exists():
            continue
        try:
            m = json.loads(mp.read_text(encoding="utf-8"))
        except ValueError:
            continue
        if "notice" not in m:
            kinds["строка ещё не добрана"] += 1
            continue
        k = classify(m["notice"])
        kinds[k] += 1
        for p in m["notice"].split("|"):
            phrases[re.sub(r"\d+", "N", p.strip())] += 1
        if k == "online_only":
            cands.append((d.name, names.get(d.name, ""), m["notice"]))

    print("Итого:", dict(kinds))
    print("\nЧастые пункты строки режимов:")
    for p, n in phrases.most_common(25):
        print("  %5d  %s" % (n, p))
    print("\nКандидаты 'только сетевая': %d" % len(cands))
    for cid, n, s in cands if opts.all else cands[:40]:
        print("  %s  %s  [%s]" % (cid, n, s))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
