#!/usr/bin/env python3
"""Measure rendered text width (px) using the firmware's own bitmap font tables.

Mirrors epaper_font::MeasureText: UTF-8 decode, typographic fold, sum of advances.
Usage:
  python scripts/measure_text.py <role> "<text>" ["<text>" ...]
Roles: body, label-small, label-small-bold, label-small-black, label-medium,
  label-medium-bold, label-medium-black, label-large, label-large-bold,
  label-large-black, body-large, h1, h2, h3, display, label-xl, status
"""
import re
import sys
from pathlib import Path

ROLES = {
    "body": "kInter22SemiBold", "label-small": "kInter22SemiBold", "detail": "kInter22SemiBold",
    "input": "kInter22SemiBold", "label-small-bold": "kInter22Bold", "status": "kInter22Bold",
    "label-small-black": "kInter22Black", "label-medium": "kInter26SemiBold",
    "body-large": "kInter26SemiBold", "label-medium-bold": "kInter26Bold", "h3": "kInter26Bold",
    "label-medium-black": "kInter26Black", "label-large": "kInter32SemiBold",
    "label-large-bold": "kInter32Bold", "h2": "kInter32Bold", "label-large-black": "kInter32Black",
    "h1": "kInter38Black", "label-xl": "kInter38Black", "display": "kInter46Black",
}
FOLD = {0x2018: "'", 0x2019: "'", 0x201C: '"', 0x201D: '"', 0x2013: "-", 0x2014: "-",
        0x2026: "...", 0x2022: "·", 0x00A0: " "}

src = (Path(__file__).resolve().parent.parent / "components/epaper_ui/generated_epaper_fonts.cpp").read_text(encoding="utf-8")


def advances(symbol):
    body = re.search(symbol + r"_glyphs\[\] = \{(.*?)\};", src, re.S).group(1)
    rows = re.findall(r"\{\s*([-\d,\s]+?)\s*\}", body)
    head = re.search(r"BitmapFont " + symbol + r" = \{\s*\"\w+\",\s*(\d+),\s*(\d+),", src)
    first, last = int(head.group(1)), int(head.group(2))
    return first, last, [int(r.split(",")[6]) for r in rows]


def measure(symbol, text):
    first, last, adv = advances(symbol)
    width = 0
    for ch in text:
        for c in FOLD.get(ord(ch), ch):
            cp = ord(c)
            if cp < first or cp > last or 127 <= cp <= 159:
                cp = ord("?")
            width += adv[cp - first]
    return width


if __name__ == "__main__":
    if len(sys.argv) < 3 or sys.argv[1] not in ROLES:
        print(__doc__)
        sys.exit(1)
    for t in sys.argv[2:]:
        print(f"{measure(ROLES[sys.argv[1]], t):4d}px  {t}")
