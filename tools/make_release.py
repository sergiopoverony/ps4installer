#!/usr/bin/env python3
# -*- coding: utf-8 -*-
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 SergioPoverony
"""Готовит выпуск (Release) для репозитория с заплатками каталога: папка с index.json и файлами catalog_N.json / catalog_N.pak.

    python tools/make_release.py data release

Что делает: берёт из папки data все заплатки catalog_2.json, catalog_3.json ... (и их .pak, если есть), считает размеры и SHA-256
и пишет release/index.json. Затем в репозитории sergiopoverony/ps4catalog на GitHub: Releases -> Draft a new release ->
тег (например patch-3) -> перетащить ВСЕ файлы из папки release -> Publish release.
Приложение берёт https://github.com/sergiopoverony/ps4catalog/releases/latest/download/index.json (всегда последний выпуск),
сравнивает номер с применённой заплаткой и скачивает недостающие, проверяя SHA-256.
В index.json есть отпечаток основного каталога (data/catalog.json): если ты пересоберёшь основной каталог целиком, старые заплатки
нужно удалить из data (приложение с другим основным каталогом их не поставит). Каждый выпуск содержит ВСЕ заплаты с 2-й (приложение, которого давно не обновляли, догоняет по порядку)."""
import hashlib
import json
import shutil
import sys
from pathlib import Path

FORMAT = 1


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for b in iter(lambda: f.read(1 << 20), b""):
            h.update(b)
    return h.hexdigest()


def make_release(data_dir, out_dir):
    data_dir, out_dir = Path(data_dir), Path(out_dir)
    patches = []
    for n in range(2, 100):
        j = data_dir / ("catalog_%d.json" % n)
        if not j.exists():
            continue
        files = [j]
        p = data_dir / ("catalog_%d.pak" % n)
        if p.exists():
            files.append(p)
        patches.append((n, files))
    if not patches:
        raise SystemExit("в %s нет ни одной заплатки catalog_N.json (сначала tools/make_patch.py)" % data_dir)
    out_dir.mkdir(parents=True, exist_ok=True)
    for old in out_dir.glob("catalog_*"):
        old.unlink()
    index = {"format": FORMAT, "patch": patches[-1][0], "patches": []}
    base = data_dir / "catalog.json"
    if base.exists():   # отпечаток основного каталога: приложение не ставит заплатки, сделанные для другого основного каталога
        index["base"] = sha256(base)
    for n, files in patches:
        entry = {"n": n, "files": []}
        for f in files:
            shutil.copy2(f, out_dir / f.name)
            entry["files"].append({"name": f.name, "size": f.stat().st_size, "sha256": sha256(f)})
        index["patches"].append(entry)
    (out_dir / "index.json").write_text(json.dumps(index, ensure_ascii=False, indent=1) + "\n", encoding="utf-8")
    return index


if __name__ == "__main__":
    if len(sys.argv) != 3:
        raise SystemExit(__doc__)
    idx = make_release(sys.argv[1], sys.argv[2])
    print("Последняя заплатка: %d, заплат в выпуске: %d. Загрузи ВСЕ файлы из папки %s в новый Release." % (idx["patch"], len(idx["patches"]), sys.argv[2]))
