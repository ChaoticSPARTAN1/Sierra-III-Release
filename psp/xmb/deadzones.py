#!/usr/bin/env python3
"""The XMB's dead zones over PIC1: where the firmware draws its own UI on top of the wallpaper.

Measured from photos of a PSP-1000 (2026-10-12), in screen pixels (480 x 272).
Photo measurements are about +/-4 px, and each zone carries a few pixels of margin
beyond what was measured (the list column measured 55-140, the clock from 341).

usage: deadzones.py OUT.png [UNDER.png]
  Writes the zones as a translucent overlay, over UNDER (a 480 x 272 PIC1) when given."""
import sys
from PIL import Image, ImageDraw, ImageFont

# (name, x0, y0, x1, y1, when it is there)
ZONES = [
    ("other titles above", 50, 0, 146, 96, "always; the list scrolls through this column"),
    ("highlighted tile", 18, 96, 176, 188, "always; the game's own ICON0 / ICON1"),
    ("other titles below", 50, 188, 146, 272, "always; the list scrolls through this column"),
    ("category icon", 0, 50, 32, 86, "always"),
    ("corner mark", 0, 0, 18, 18, "always"),
    ("list arrow", 4, 128, 18, 146, "always"),
    ("clock + battery", 336, 10, 480, 34, "always"),
    ("Options button", 390, 218, 480, 258, "after a moment on the entry"),
]


def draw(under=None):
    base = (Image.open(under).convert("RGBA").resize((480, 272)) if under
            else Image.new("RGBA", (480, 272), (19, 24, 30, 255)))
    layer = Image.new("RGBA", base.size, (0, 0, 0, 0))
    d = ImageDraw.Draw(layer)
    font = ImageFont.load_default(size=8)
    for name, x0, y0, x1, y1, _ in ZONES:
        d.rectangle([x0, y0, x1, y1], fill=(220, 60, 60, 90), outline=(255, 110, 110, 220))
        d.text((x0 + 2, y0 + 2), name, fill=(255, 220, 220, 255), font=font)
    return Image.alpha_composite(base, layer).convert("RGB")


if __name__ == "__main__":
    draw(sys.argv[2] if len(sys.argv) > 2 else None).save(sys.argv[1])
    for name, x0, y0, x1, y1, when in ZONES:
        print("%-20s x %3d-%3d  y %3d-%3d  %s" % (name, x0, x1, y0, y1, when))
