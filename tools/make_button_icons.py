#!/usr/bin/env python3
"""Рисует значки кнопок геймпада для приложения: pkg/assets/images/btn_*.png (с прозрачностью).
Рисуем в 8 раз крупнее и уменьшаем: края получаются гладкими. Размеры должны совпадать с ICON_S в pkg/src/ui.cpp.
Запуск: python3 tools/make_button_icons.py"""
from pathlib import Path
from PIL import Image, ImageDraw, ImageFont

ICON_S = 38          # квадратные кнопки
SHOULDER = (76, 28)  # L1 / R1
K = 8                # во сколько раз рисуем крупнее
OUT = Path(__file__).resolve().parent.parent / "pkg" / "assets" / "images"
FONT = Path(__file__).resolve().parent.parent / "pkg" / "assets" / "fonts" / "DejaVuSansMono.ttf"
COL = {"cross": (110, 160, 255), "circle": (255, 105, 105), "square": (235, 135, 205),
       "triangle": (95, 215, 150), "shoulder": (190, 195, 212)}


def ring_icon(kind):
    n = ICON_S * K
    im = Image.new("RGBA", (n, n), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    c = COL[kind]
    ring = int(2.6 * K)
    d.ellipse((0, 0, n - 1, n - 1), outline=c, width=ring)
    m = n / 2
    r = n * 0.25          # половина размера знака
    w = int(3.2 * K)      # толщина линий знака
    if kind == "cross":
        d.line((m - r, m - r, m + r, m + r), fill=c, width=w)
        d.line((m - r, m + r, m + r, m - r), fill=c, width=w)
        for x, y in ((m - r, m - r), (m + r, m + r), (m - r, m + r), (m + r, m - r)):   # скруглённые концы
            d.ellipse((x - w / 2, y - w / 2, x + w / 2, y + w / 2), fill=c)
    elif kind == "circle":
        d.ellipse((m - r, m - r, m + r, m + r), outline=c, width=w)
    elif kind == "square":
        d.rounded_rectangle((m - r, m - r, m + r, m + r), radius=int(0.8 * K), outline=c, width=w)
    else:
        top, bot = m - r * 1.05, m + r * 0.8
        pts = [(m, top), (m - r * 1.15, bot), (m + r * 1.15, bot)]
        d.line(pts + [pts[0]], fill=c, width=w, joint="curve")
        for x, y in pts:
            d.ellipse((x - w / 2, y - w / 2, x + w / 2, y + w / 2), fill=c)
    return im.resize((ICON_S, ICON_S), Image.Resampling.LANCZOS)


def shoulder_icon(label):
    w, h = SHOULDER[0] * K, SHOULDER[1] * K
    im = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    c = COL["shoulder"]
    d.rounded_rectangle((0, 0, w - 1, h - 1), radius=int(9 * K), outline=c, width=int(2.6 * K))
    f = ImageFont.truetype(str(FONT), int(19 * K))
    bb = d.textbbox((0, 0), label, font=f)
    d.text(((w - (bb[2] - bb[0])) / 2 - bb[0], (h - (bb[3] - bb[1])) / 2 - bb[1]), label, font=f, fill=c)
    return im.resize(SHOULDER, Image.Resampling.LANCZOS)


def main():
    OUT.mkdir(parents=True, exist_ok=True)
    for kind in ("cross", "circle", "square", "triangle"):
        ring_icon(kind).save(OUT / ("btn_%s.png" % kind), optimize=True)
    shoulder_icon("L1").save(OUT / "btn_l1.png", optimize=True)
    shoulder_icon("R1").save(OUT / "btn_r1.png", optimize=True)
    print("Готово:", ", ".join(sorted(p.name for p in OUT.glob("btn_*.png"))))


if __name__ == "__main__":
    main()
