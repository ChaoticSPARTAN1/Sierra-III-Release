#!/usr/bin/env python3
"""Draws the placeholder XMB art for both builds.

The XMB (the PSP's home menu) shows these when the game is highlighted. The
firmware reads them straight out of EBOOT.PBP, so the sizes are a fixed
contract (see README.md). Each set is written to psp/xmb/<variant>/:

    ICON0.PNG  144x80   the tile in the game list
    PIC0.PNG   310x180  overlay drawn over the background (transparent)
    PIC1.PNG   480x272  full-screen background

These are stand-ins: obviously placeholder, entirely ours, safe to commit.
Real art replaces the PNGs in place; this script stays as the reference
for sizes and layout zones. Slots holding real art are listed in REAL and
never overwritten (the campaign's PIC1 since 2026-10-12; it carries the
titles itself, so that build has no PIC0).

usage: python3 psp/xmb/make_placeholders.py      (needs Pillow >= 10.1)"""
import os
from PIL import Image, ImageDraw, ImageFont

HERE = os.path.dirname(os.path.abspath(__file__))

# The firmware's fixed sizes. Never change these.
ICON0 = (144, 80)
PIC0 = (310, 180)
PIC1 = (480, 272)

# One accent colour per build, so the two XMB entries are told apart at a glance.
VARIANTS = {
    "campaign": {"label": "CAMPAIGN", "accent": (88, 170, 120)},
    "mp": {"label": "MULTIPLAYER", "accent": (200, 140, 60)},
}
# Slots holding real art, skipped here.
# The tiles are drawn from the Sierra-III mark by icon0.py.
REAL = {("campaign", "PIC1.PNG"), ("campaign", "PIC0.PNG"), ("campaign", "ICON0.PNG"), ("mp", "ICON0.PNG")}
STEEL_TOP, STEEL_BOTTOM = (34, 40, 46), (10, 12, 14)
TEXT = (226, 230, 232)


def font(size):
    # Pillow's bundled font: no system font path, so the output is the same on any machine.
    return ImageFont.load_default(size=size)


def gradient(size):
    w, h = size
    img = Image.new("RGB", size)
    d = ImageDraw.Draw(img)
    for y in range(h):
        t = y / (h - 1)
        d.line([(0, y), (w, y)], fill=tuple(round(a + (b - a) * t) for a, b in zip(STEEL_TOP, STEEL_BOTTOM)))
    return img


def chevrons(d, cx, cy, size, colour):
    # Three stacked chevrons: the "III" of the callsign.
    for i in range(3):
        y = cy - size + i * size * 0.7
        d.line([(cx - size, y), (cx, y + size * 0.6), (cx + size, y)], fill=colour, width=max(2, size // 5))


def make_icon0(v):
    img = gradient(ICON0)
    d = ImageDraw.Draw(img)
    d.rectangle([0, 0, ICON0[0] - 1, ICON0[1] - 1], outline=v["accent"], width=2)
    chevrons(d, 30, 34, 14, v["accent"])
    d.text((54, 22), "SIERRA", font=font(18), fill=TEXT)
    d.text((54, 42), "III", font=font(18), fill=v["accent"])
    d.text((6, 66), v["label"], font=font(9), fill=TEXT)
    return img


def make_pic1(v):
    img = gradient(PIC1)
    d = ImageDraw.Draw(img)
    # Faint grid, so the placeholder reads as a placeholder.
    for x in range(0, PIC1[0], 24):
        d.line([(x, 0), (x, PIC1[1])], fill=(26, 31, 36))
    for y in range(0, PIC1[1], 24):
        d.line([(0, y), (PIC1[0], y)], fill=(26, 31, 36))
    chevrons(d, 400, 130, 40, v["accent"])
    d.text((8, PIC1[1] - 18), "PLACEHOLDER BACKGROUND", font=font(10), fill=(90, 98, 104))
    return img


def make_pic0(v):
    img = Image.new("RGBA", PIC0, (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    d.text((4, 50), "SIERRA-III", font=font(34), fill=TEXT + (255,))
    d.line([(4, 96), (220, 96)], fill=v["accent"] + (255,), width=2)
    d.text((4, 104), v["label"], font=font(16), fill=v["accent"] + (255,))
    d.text((4, 160), "PLACEHOLDER OVERLAY", font=font(10), fill=(140, 146, 150, 255))
    return img


def save(img, path):
    # Plain, non-interlaced PNG with no metadata: the XMB is picky about anything else.
    img.save(path, format="PNG", optimize=True)
    print("wrote", os.path.relpath(path, HERE), img.size)


def main():
    for name, v in VARIANTS.items():
        out = os.path.join(HERE, name)
        os.makedirs(out, exist_ok=True)
        for slot, make in (("ICON0.PNG", make_icon0), ("PIC0.PNG", make_pic0), ("PIC1.PNG", make_pic1)):
            if (name, slot) in REAL:
                print("kept", os.path.join(name, slot), "(real art)")
            else:
                save(make(v), os.path.join(out, slot))


if __name__ == "__main__":
    main()
