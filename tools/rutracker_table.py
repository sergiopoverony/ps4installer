#!/usr/bin/env python3
# -*- coding: utf-8 -*-
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 SergioPoverony
"""Читает выгрузку рутрекера (rutracker-YYYYMMDD.xml.xz, ~30 ГБ) и делает таблицу игр ветки PS4 (форум 973) в CSV для tools/merge_torrents.py.

    python tools/rutracker_table.py "D:\\TORRENT\\RUTRACKER_BASE\\rutracker-20261031.xml.xz" "D:\\TORRENT\\RUTRACKER_BASE\\ps4_games.csv"

Читает построчно, режет на отдельные раздачи и разбирает только нужные: память не растёт, битая запись не ломает проход (парсер ElementTree на весь файл
сразу падал с «out of memory»). Прогресс - каждые 100 000 раздач (всего их около 2,9 млн: от получаса до часа и больше).
Колонки: название, код диска, описание (500 символов), DLC, Fix, Backport, рус. интерфейс, рус. озвучка, размер ГБ, дата, страница раздачи, magnet (с трекером),
ссылка на обложку, ссылки на скриншоты (до 4, через пробел)."""
import csv
import html
import lzma
import re
import sys
import xml.etree.ElementTree as ET
from collections import Counter

FORUM_IDS = {"973"}
TRACKER = "http%3A%2F%2Fbt.t-ru.org%2Fann%3Fmagnet"

RUS = re.compile(r"рус|russian|\brus\b", re.I)
BACKPORT = re.compile(r"back\s*-?\s*port|б[эе]к\s*-?\s*порт|\bBP\b", re.I)
SKIP = re.compile(r"правил|объявлен|прилеплен", re.I)
BAD_CHARS = re.compile(rb"[\x00-\x08\x0b\x0c\x0e-\x1f]")
FORUM_RE = re.compile(rb'<forum id="(\d+)">([^<]*)</forum>')
IMG_RE = re.compile(r"\[img[^\]]*\]\s*(https?://[^\s\[\]]+)\s*\[/img\]", re.I)
HEADER = ["Название", "Код диска", "Описание (500 симв.)", "DLC", "Fix", "Backport", "Рус. интерфейс", "Рус. озвучка",
          "Размер, ГБ", "Дата", "Страница раздачи", "Magnet", "Обложка (URL)", "Скриншоты (URL)", "Год выпуска"]


def strip_bb(s):
    return re.sub(r"\[/?[A-Za-z*]+(?:=[^\]]*)?\]", " ", s)


def clean_name(title):
    t = re.sub(r"\[[^\]]*\]", "", title)
    t = re.sub(r"\(\s*v?\d[\d.]*\s*\)", "", t)
    t = re.sub(r"\s*\+.*$", "", t)
    return re.sub(r"\s+", " ", t).strip()


def get_description(content):
    i = content.find("Описание")
    if i == -1:
        return ""
    t = content[i + len("Описание"):]
    t = t.split("[spoiler")[0]
    t = strip_bb(t)
    t = re.sub(r"^[\s:]+", "", t)
    return re.sub(r"\s+", " ", t).strip()[:500]


def field(content, label):
    m = re.search(label + r"([^\n]*)", content)
    return m.group(1) if m else ""


def get_year(content):
    """Год выпуска игры из описания раздачи («Год выпуска: 2019», «Дата выпуска: 12.03.2019»). Пусто, если не указан."""
    m = re.search(r"(?:Год|Дата)\s+выпуска[^\n]{0,40}?((?:19|20)\d\d)", content, re.I)
    return m.group(1) if m else ""


def get_code(content, files_text):
    m = re.search(r"Код диска[^\n]{0,20}?([A-Z]{4}[-_ ]?\d{5})", content)
    if m:
        return re.sub(r"[-_ ]", "", m.group(1))
    m = re.search(r"CUSA\d{5}", content + " " + files_text)
    return m.group(0) if m else ""


def get_images(content):
    urls = []
    for u in IMG_RE.findall(content):
        if u not in urls:
            urls.append(u)
    return (urls[0] if urls else ""), urls[1:5]


def make_row(elem):
    tid = elem.get("id")
    title = (elem.findtext("title") or "").strip()
    if SKIP.search(title):
        return None
    content = html.unescape(elem.findtext("content") or "")
    files_text = " ".join(f.get("name", "") for f in elem.iter("file"))
    head = title + " " + files_text
    inner = elem.find("torrent")
    thash = (inner.get("hash") if inner is not None else "") or ""
    dlc = "Да" if re.search(r"dlc", head, re.I) else ("в описании" if re.search(r"dlc", content, re.I) else "")
    fix = "Да" if re.search(r"fix|fxd", head, re.I) else ""
    backport = "Да" if BACKPORT.search(head + " " + content) else ""
    iface, voice, trans = field(content, "Язык интерфейса"), field(content, "Язык озвучки"), field(content, "Перевод")
    rus_ui = bool(RUS.search(iface)) if iface else bool(RUS.search(title))
    rus_voice = bool(RUS.search(voice)) or ("Звук" in trans)
    try:
        size_gb = round(int(elem.get("size", "0")) / 1024 ** 3, 2)
    except ValueError:
        size_gb = 0
    cover, shots = get_images(content)
    magnet = "magnet:?xt=urn:btih:%s&tr=%s" % (thash, TRACKER) if thash else ""
    return [clean_name(title), get_code(content, files_text), get_description(content), dlc, fix, backport,
            "Да" if rus_ui else "", "Да" if rus_voice else "", str(size_gb).replace(".", ","), elem.get("registred_at", ""),
            "https://rutracker.org/forum/viewtopic.php?t=%s" % tid, magnet, cover, " ".join(shots), get_year(content)]


def scan(lines, forum_ids=FORUM_IDS, progress=None):
    """lines - итератор по строкам (bytes) выгрузки. Возвращает (строки таблицы, всего раздач, пропущено, битых, форумы)."""
    rows, total, skipped, broken = [], 0, 0, 0
    counts, names = Counter(), {}
    want = [('<forum id="%s"' % i).encode() for i in forum_ids]
    chunk = None
    for line in lines:
        if chunk is None:
            if line.startswith(b"<torrent id="):
                chunk = [line]
            continue
        chunk.append(line)
        if not line.startswith(b"</torrent>"):
            continue
        data = b"".join(chunk)
        chunk = None
        total += 1
        m = FORUM_RE.search(data)
        if m:
            counts[m.group(1).decode()] += 1
            names[m.group(1).decode()] = m.group(2).decode("utf-8", "replace").strip()
        if any(w in data for w in want):
            try:
                row = make_row(ET.fromstring(BAD_CHARS.sub(b"", data)))
                if row is None:
                    skipped += 1
                else:
                    rows.append(row)
            except Exception as e:
                broken += 1
                print("Битая запись, пропущена:", data[:80], e, flush=True)
        if progress and total % 100000 == 0:
            progress(total, len(rows))
    rows.sort(key=lambda r: r[0].lower())
    return rows, total, skipped, broken, (counts, names)


def write_csv(rows, path):
    with open(path, "w", newline="", encoding="utf-8-sig") as f:
        w = csv.writer(f, delimiter=";")
        w.writerow(HEADER)
        w.writerows(rows)


def main():
    if len(sys.argv) != 3:
        print(__doc__)
        return 2
    src, out = sys.argv[1], sys.argv[2]
    with lzma.open(src, "rb") as f:
        rows, total, skipped, broken, (counts, names) = scan(f, progress=lambda t, n: print("Обработано раздач: %d, найдено игр: %d" % (t, n), flush=True))
    write_csv(rows, out)
    print("\nВсего раздач в базе:", total)
    print("Пропущено (правила и т.п.):", skipped)
    print("Битых записей:", broken)
    print("Записано в таблицу:", len(rows))
    print("Без кода диска:", sum(1 for r in rows if not r[1]))
    print("С обложкой:", sum(1 for r in rows if r[12]))
    print("Файл:", out)
    print("\nВетки, где в названии есть PS4 / PlayStation 4:")
    for fid, c in counts.most_common():
        n = names.get(fid, "")
        if "PS4" in n or "PlayStation 4" in n:
            print("  %s: %d раздач, %s" % (fid, c, n))
    return 0


if __name__ == "__main__":
    sys.exit(main())
