#!/usr/bin/env python3
"""Composes PIC1 on the approved backdrop (backdrop.py, seed 7, unchanged), at 2x (960 x 544),
then shrinks to 480 x 272.

Back to front: approved backdrop -> ring -> ship (nearer the camera) -> cyan rails -> titles.

usage: compose3.py BACKDROP_2x.png RING.png SHIP.png TITLES_2x.png OUT.png [GLOW.png]
  SHIP_SCALE, SHIP_CX, SHIP_CY (environment) place the ship; GLOW is added as light."""
import sys
import numpy as np
from PIL import Image, ImageFilter

W, H = 960, 544
CYAN = np.array([120, 220, 255])
RING = dict(scale=0.92, cx=520, cy=282)
import os
SHIP = dict(scale=float(os.environ.get("SHIP_SCALE", "1.30")), cx=int(os.environ.get("SHIP_CX", "792")),
            cy=int(os.environ.get("SHIP_CY", "268")))


def place(canvas, layer, scale, cx, cy):
    """Scales an RGBA cutout, centres it on (cx, cy) and alpha-composites it onto the canvas."""
    lay = layer.resize((round(layer.width * scale), round(layer.height * scale)), Image.LANCZOS)
    a = np.asarray(lay).astype(np.float32)
    x0, y0 = int(cx - lay.width / 2), int(cy - lay.height / 2)
    sx0, sy0 = max(0, -x0), max(0, -y0)
    sx1, sy1 = min(lay.width, W - x0), min(lay.height, H - y0)
    al = a[sy0:sy1, sx0:sx1, 3:] / 255
    region = canvas[y0 + sy0:y0 + sy1, x0 + sx0:x0 + sx1]
    canvas[y0 + sy0:y0 + sy1, x0 + sx0:x0 + sx1] = region * (1 - al) + a[sy0:sy1, sx0:sx1, :3] * al


def add_glow(canvas, glow, scale, cx, cy):
    """Adds a light layer (the plumes): scaled and centred like its ship, summed onto the canvas."""
    g = glow.resize((round(glow.width * scale), round(glow.height * scale)), Image.LANCZOS)
    a = np.asarray(g).astype(np.float32)
    x0, y0 = int(cx - g.width / 2), int(cy - g.height / 2)
    sx0, sy0 = max(0, -x0), max(0, -y0)
    sx1, sy1 = min(g.width, W - x0), min(g.height, H - y0)
    canvas[y0 + sy0:y0 + sy1, x0 + sx0:x0 + sx1] += a[sy0:sy1, sx0:sx1]


def main(bg, ring, ship, titles, out, glow=None):
    canvas = np.asarray(Image.open(bg).convert("RGB")).astype(np.float32)
    place(canvas, Image.open(ring).convert("RGBA"), **RING)
    place(canvas, Image.open(ship).convert("RGBA"), **SHIP)
    if glow:
        add_glow(canvas, Image.open(glow).convert("RGB"), **SHIP)
    rails = np.zeros((H, W), np.float32)
    xs = np.arange(W)
    for y in (115, 463):
        rails[y - 1:y + 1] = np.where(xs < 294, np.clip(1 - (294 - xs) / 260, 0, 1),
                                      np.where(xs > 666, np.clip(1 - (xs - 666) / 260, 0, 1), 0)) * 0.82
    glow = np.asarray(Image.fromarray((rails * 255).astype(np.uint8)).filter(ImageFilter.GaussianBlur(4))).astype(np.float32) / 255
    canvas += np.clip(rails + glow * 0.9, 0, 1)[..., None] * CYAN * 0.85
    scene = Image.fromarray(np.clip(canvas, 0, 255).astype(np.uint8)).convert("RGBA")
    scene = Image.alpha_composite(scene, Image.open(titles).convert("RGBA"))
    small = scene.convert("RGB").resize((480, 272), Image.LANCZOS)
    if os.environ.get("GRADE") == "1":
        # sharpen after the downscale (it softens), then an S-curve for the PSP-1000's LCD, which
        # lifts darks and flattens contrast: deeper shadows, brighter highlights, mid-tones kept
        small = small.filter(ImageFilter.UnsharpMask(radius=1.0, percent=70, threshold=2))
        a = np.asarray(small).astype(np.float32) / 255
        a = np.clip(a + 0.35 * (a - 0.5) * (1 - np.abs(2 * a - 1)) * 2 - 0.02, 0, 1)
        small = Image.fromarray((a * 255).astype(np.uint8))
    small.save(out)


if __name__ == "__main__":
    main(*sys.argv[1:7])
