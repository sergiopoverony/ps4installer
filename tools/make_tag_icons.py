#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 SergioPoverony
"""Значки пометок на плитках: русификатор (флаг), патч (стрелка вверх), DLC (плюс). PNG 40x40 в pkg/assets/images/."""
import os
from PIL import Image, ImageDraw

S = 40          # итоговый размер
K = 8           # сглаживание: рисуем в 8 раз крупнее
OUT = os.path.join(os.path.dirname(__file__), "..", "pkg", "assets", "images")


def base(color):
    im = Image.new("RGBA", (S * K, S * K), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    d.rounded_rectangle((0, 0, S * K - 1, S * K - 1), radius=7 * K, fill=color)
    return im, d


def save(im, name):
    im.resize((S, S), Image.LANCZOS).save(os.path.join(OUT, name))


# флаг: тёмная плашка и трёхцветный флаг
im, d = base((24, 26, 34, 245))
x0, x1, y0, y1 = 6 * K, 34 * K, 10 * K, 30 * K
h = (y1 - y0) // 3
d.rectangle((x0, y0, x1, y0 + h), fill=(245, 245, 245, 255))
d.rectangle((x0, y0 + h, x1, y0 + 2 * h), fill=(40, 90, 200, 255))
d.rectangle((x0, y0 + 2 * h, x1, y1), fill=(215, 45, 45, 255))
save(im, "tag_rus.png")

# патч: оранжевая плашка, белая стрелка вверх
im, d = base((240, 140, 30, 250))
w = (255, 255, 255, 255)
d.polygon([(20 * K, 7 * K), (31 * K, 19 * K), (24 * K, 19 * K), (24 * K, 32 * K), (16 * K, 32 * K), (16 * K, 19 * K), (9 * K, 19 * K)], fill=w)
save(im, "tag_patch.png")

# DLC: синяя плашка, белый плюс
im, d = base((60, 120, 230, 250))
d.rectangle((17 * K, 8 * K, 23 * K, 32 * K), fill=w)
d.rectangle((8 * K, 17 * K, 32 * K, 23 * K), fill=w)
save(im, "tag_dlc.png")
print("готово")
