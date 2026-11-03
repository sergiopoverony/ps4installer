#!/usr/bin/env python3
"""Почему игры нет в каталоге: python3 tools/why_missing.py "hollow knight" ratchet valhalla ...
Ищет по data/ps4_catalog.json и показывает состояние кэша и попадание в data/catalog.json."""
import json, re, sys
from pathlib import Path

data = Path(__file__).resolve().parent.parent / "data"
raw = (data / "ps4_catalog.json").read_text(encoding="utf-8")
cat = json.loads((data / "catalog.json").read_text(encoding="utf-8"))
packed = {g["id"] for g in cat["games"]}
seen = set()
for word in sys.argv[1:]:
    w = word.lower()
    print("==", word)
    for m in sorted(set(re.findall(r'"uuid"\s*:\s*"([^"]*CUSA\d{5}[^"]*)"', raw))):
        pass
    # перебираем все объекты с name+uuid
    for mm in re.finditer(r'\{[^{}]*?"name"\s*:\s*"([^"]*)"[^{}]*?\}', raw):
        blk = mm.group(0)
        if w not in mm.group(1).lower():
            continue
        u = re.search(r'(?:CUSA|PPSA)\d{5}', blk)
        if not u or u.group(0) in seen:
            continue
        tid = u.group(0); seen.add(tid)
        d = data / "cache" / tid
        mp = d / "meta.json"
        if mp.exists():
            meta = json.loads(mp.read_text(encoding="utf-8"))
            st = "meta=%s notice=%r" % (meta.get("status"), meta.get("notice"))
        else:
            st = "meta.json нет"
        st += ", cover=%s" % ("да" if (d / "cover.jpg").exists() else "нет")
        print("  %s %-50s %s | в каталоге: %s" % (tid, mm.group(1)[:50], st, "ДА" if tid in packed else "нет"))
