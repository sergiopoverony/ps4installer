#!/usr/bin/env python3
# -*- coding: utf-8 -*-
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 SergioPoverony
"""
cache_stats.py - сводка по скачанному кэшу (data/cache): сколько игр полных, чего не хватает
у неполных и примеры Title ID. Ничего не качает и не меняет.

  python3 tools/cache_stats.py
  python3 tools/cache_stats.py --out /путь/к/data
"""
import argparse
import collections
import json
from pathlib import Path


def main():
    root = Path(__file__).resolve().parent.parent
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default=str(root / "data"))
    ap.add_argument("--examples", type=int, default=6)
    opts = ap.parse_args()
    cache = Path(opts.out) / "cache"
    if not cache.is_dir():
        print("Нет папки", cache)
        return 1

    status = collections.Counter()
    reasons = collections.Counter()
    langs = collections.Counter()
    examples = collections.defaultdict(list)
    no_meta = []
    for d in sorted(cache.iterdir()):
        if not d.is_dir():
            continue
        mp = d / "meta.json"
        if not mp.exists():
            no_meta.append(d.name)
            continue
        try:
            m = json.loads(mp.read_text(encoding="utf-8"))
        except ValueError:
            status["битый meta.json"] += 1
            continue
        st = m.get("status", "?")
        status[st] += 1
        if st == "ok":
            langs[m.get("lang") or "?"] += 1
        elif st == "partial":
            miss = []
            if not m.get("has_desc"):
                miss.append("нет описания")
            if not m.get("has_cover"):
                miss.append("нет обложки")
            if not m.get("shots"):
                miss.append("нет скриншотов")
            key = ", ".join(miss) or "другое"
            reasons[key] += 1
            if len(examples[key]) < opts.examples:
                examples[key].append(d.name)

    print("Статусы:", dict(status))
    print("Язык описания у ok:", dict(langs))
    if reasons:
        print("\nПочему partial:")
        for k, v in reasons.most_common():
            print("  %5d  %s   например: %s" % (v, k, " ".join(examples[k])))
    if no_meta:
        print("\nНе дообработаны (будут повторены при следующем запуске): %d, например %s" % (
            len(no_meta), " ".join(no_meta[:opts.examples])))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
