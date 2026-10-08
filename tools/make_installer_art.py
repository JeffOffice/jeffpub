#!/usr/bin/env python3
"""Draws the Windows installer artwork in the JeffPub brand colors.

    tools/make_installer_art.py

Writes packaging/windows/welcome.bmp (the side panel of the Welcome and
Finish pages) and packaging/windows/header.bmp (the banner on the other
pages), both at twice NSIS's size so they stay sharp on high-DPI screens.
Needs Python 3 with Pillow.
"""
import os
from PIL import Image, ImageDraw, ImageFilter, ImageFont

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FONTS = os.path.join(ROOT, "resources", "fonts")
OUT = os.path.join(ROOT, "packaging", "windows")
ACCENT = (158, 31, 99)
DEEP = (74, 14, 52)
SCALE = 4  # draw large, then shrink for smooth edges


def font(name, size):
    return ImageFont.truetype(os.path.join(FONTS, name), size * SCALE)


def badge(size, fg=ACCENT, bg=(255, 255, 255)):
    """The rounded "JP" badge from the app icon."""
    s = size * SCALE
    im = Image.new("RGBA", (s, s), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    d.rounded_rectangle((0, 0, s - 1, s - 1), radius=int(s * 0.24), fill=bg)
    f = font("Montserrat-Bold.ttf", int(size * 0.5))
    box = d.textbbox((0, 0), "JP", font=f)
    d.text(((s - (box[2] - box[0])) / 2 - box[0], (s - (box[3] - box[1])) / 2 - box[1]), "JP", font=f, fill=fg)
    return im


def welcome():
    w, h = 328, 628
    W, H = w * SCALE, h * SCALE
    im = Image.new("RGB", (W, H))
    px = im.load()
    for y in range(H):
        t = y / (H - 1)
        c = tuple(int(ACCENT[i] + (DEEP[i] - ACCENT[i]) * t) for i in range(3))
        for x in range(W):
            px[x, y] = c
    # Soft, tilted "pages" in the background.
    layer = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    for (cx, cy, pw, ph, ang, a) in [(250, 470, 190, 250, -14, 26), (90, 540, 170, 220, 10, 18), (280, 130, 150, 200, 18, 14)]:
        page = Image.new("RGBA", (pw * SCALE, ph * SCALE), (0, 0, 0, 0))
        pd = ImageDraw.Draw(page)
        pd.rounded_rectangle((0, 0, pw * SCALE - 1, ph * SCALE - 1), radius=14 * SCALE, fill=(255, 255, 255, a))
        for i in range(4):  # text lines
            y0 = (40 + i * 26) * SCALE
            pd.rounded_rectangle((22 * SCALE, y0, (pw - 22 - (40 if i == 3 else 0)) * SCALE, y0 + 8 * SCALE), radius=4 * SCALE, fill=(255, 255, 255, a + 18))
        page = page.rotate(ang, expand=True, resample=Image.BICUBIC)
        layer.alpha_composite(page, (cx * SCALE - page.width // 2, cy * SCALE - page.height // 2))
    im = Image.alpha_composite(im.convert("RGBA"), layer)
    # Badge with a soft shadow.
    b = badge(104)
    shadow = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    bx, by = (W - b.width) // 2, 92 * SCALE
    sd = ImageDraw.Draw(shadow)
    sd.rounded_rectangle((bx, by + 8 * SCALE, bx + b.width, by + b.height + 8 * SCALE), radius=int(b.width * 0.24), fill=(0, 0, 0, 70))
    im = Image.alpha_composite(im, shadow.filter(ImageFilter.GaussianBlur(10 * SCALE)))
    im.alpha_composite(b, (bx, by))
    d = ImageDraw.Draw(im)
    def centered(text, f, y, fill):
        box = d.textbbox((0, 0), text, font=f)
        d.text(((W - (box[2] - box[0])) / 2 - box[0], y * SCALE), text, font=f, fill=fill)
    centered("JeffPub", font("Montserrat-Bold.ttf", 34), 228, (255, 255, 255))
    centered("Desktop publishing,", font("Lato-Regular.ttf", 20), 282, (255, 255, 255, 215))
    centered("free and open.", font("Lato-Regular.ttf", 20), 308, (255, 255, 255, 215))
    im = im.convert("RGB").resize((w, h), Image.LANCZOS)
    im.save(os.path.join(OUT, "welcome.bmp"))


def header():
    w, h = 300, 114
    W, H = w * SCALE, h * SCALE
    im = Image.new("RGBA", (W, H), (255, 255, 255, 255))
    b = badge(64, fg=(255, 255, 255), bg=ACCENT)
    im.alpha_composite(b, (W - b.width - 26 * SCALE, (H - b.height) // 2))
    im.convert("RGB").resize((w, h), Image.LANCZOS).save(os.path.join(OUT, "header.bmp"))


if __name__ == "__main__":
    welcome()
    header()
