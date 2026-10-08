#!/usr/bin/env python3
"""Draws the JeffPub app icon: white "JP" on the brand magenta rounded square.

    tools/make_app_icon.py

Writes resources/app.png (256 px; the Mac .icns and the Linux icon are made
from it) and resources/app.ico (16 to 256 px, for Windows). Needs Python 3
with Pillow.
"""
import os
from PIL import Image, ImageDraw, ImageFont

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
RES = os.path.join(ROOT, "resources")
ACCENT = (158, 31, 99)
TEXT = "JP"
SCALE = 4  # draw large, then shrink for smooth edges


def icon(size):
    s = size * SCALE
    im = Image.new("RGBA", (s, s), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    d.rounded_rectangle((0, 0, s - 1, s - 1), radius=int(s * 0.22), fill=ACCENT)
    f = ImageFont.truetype(os.path.join(RES, "fonts", "Montserrat-Bold.ttf"), int(s * 0.52))
    box = d.textbbox((0, 0), TEXT, font=f)
    w, h = box[2] - box[0], box[3] - box[1]
    d.text(((s - w) / 2 - box[0], (s - h) / 2 - box[1]), TEXT, font=f, fill=(255, 255, 255))
    return im.resize((size, size), Image.LANCZOS)


big = icon(256)
big.save(os.path.join(RES, "app.png"))
sizes = [16, 20, 24, 32, 40, 48, 64, 128, 256]
big.save(os.path.join(RES, "app.ico"), sizes=[(n, n) for n in sizes], append_images=[icon(n) for n in sizes[:-1]])
