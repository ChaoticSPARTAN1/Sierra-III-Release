#!/usr/bin/env python3
"""Draws an original space backdrop for PIC1: deep space, a nebula band, stars and a planet.

Everything here is generated, so it is ours to ship. The ship and the ring go on top as
separate layers (footage or our own art).

Drawn at 2x (960 x 544) and shrunk to 480 x 272, so edges and stars come out smooth.

usage: backdrop.py OUT.png [--seed N]   (also writes OUT_2x.png)

The seed is fixed at 7 by default: the menu art was approved at that seed. Random numbers are
drawn in a fixed order (sky, nebula, stars, planet), so tuning a later layer's maths leaves the
earlier layers identical; adding a random draw earlier re-rolls everything after it."""
import sys
import numpy as np
import cv2
from PIL import Image

W, H = 960, 544
SEED = int(sys.argv[sys.argv.index("--seed") + 1]) if "--seed" in sys.argv else 7
rng = np.random.default_rng(SEED)
yy, xx = np.mgrid[0:H, 0:W].astype(np.float32)


def noise(scale, octaves=5):
    """Smooth value noise in 0..1: random grids at several sizes, upscaled and summed."""
    out = np.zeros((H, W), np.float32)
    amp, total = 1.0, 0.0
    for o in range(octaves):
        cell = max(2, int(scale / (2 ** o)))
        g = rng.random((H // cell + 3, W // cell + 3)).astype(np.float32)
        layer = cv2.resize(g, ((W // cell + 3) * cell, (H // cell + 3) * cell), interpolation=cv2.INTER_CUBIC)
        out += amp * layer[:H, :W]
        total += amp
        amp *= 0.5
    return np.clip(out / total, 0, 1)


def warp(field, strength, scale):
    """Domain warping: sample `field` at positions pushed around by two other noise fields.
    It turns even noise into swirls and uneven, cloud-like clumps."""
    dx = (noise(scale, 4) - 0.5) * 2 * strength
    dy = (noise(scale, 4) - 0.5) * 2 * strength
    return cv2.remap(field, xx + dx, yy + dy, cv2.INTER_LINEAR, borderMode=cv2.BORDER_REFLECT)


def ramp(t, stops):
    """Maps 0..1 onto a colour gradient given as [(position, (r, g, b)), ...]."""
    pos = np.array([p for p, _ in stops], np.float32)
    cols = np.array([c for _, c in stops], np.float32)
    return np.stack([np.interp(t, pos, cols[:, i]) for i in range(3)], -1)


# 1. Deep space: dark blue-purple, never black (the PSP-1000's LCD lifts blacks anyway).
sky = np.full((H, W, 3), (18, 18, 40), np.float32)
sky += (noise(220)[..., None] - 0.5) * np.array([10, 8, 22])

# 2. The nebula: a band around a bent, zigzagging spine, rising from the planet to the upper right.
#    Distance is measured from that curve, not a straight line, so the band bends and wanders.
t = (xx - 250) / 710                                       # 0 at the planet, 1 at the right edge
spine = 300 - 190 * t + 70 * np.sin(np.pi * np.clip(t, 0, 1)) * 0.9   # a rising arc with a bend
zig = cv2.GaussianBlur((rng.random(W).astype(np.float32) - 0.5) * 70, (0, 0), 9).ravel()   # the zigzag (cv2 returns 2-D)
spine = spine + zig[None, :]
d = yy - spine                                             # vertical distance from the spine
width = (55 + 45 * noise(260, 3)) * (0.7 + 0.6 * np.sin(np.pi * np.clip(t, 0, 1)))  # swells and pinches
# No fade at the planet end: the band runs on behind the planet (drawn over it later),
# so it reads as a ring emerging from behind a gas giant. It fades only off the right edge.
core = np.exp(-(d / width) ** 2) * np.clip((1.15 - t) * 2, 0, 1)
clouds = warp(noise(70, 6), 26, 120)                       # uneven, clumpy structure
wisps = warp(noise(25, 4), 12, 60)
neb = core ** 0.8 * (0.25 + 1.1 * clouds ** 1.6) * (0.7 + 0.5 * wisps)
lane = cv2.GaussianBlur(np.exp(-((d + 6) / 9) ** 2) * (warp(noise(40, 4), 15, 80) > 0.5), (0, 0), 3)
neb *= 1 - 0.5 * lane
sky += neb[..., None] * np.array([80, 165, 215])
sky += (core ** 6 * clouds)[..., None] * np.array([90, 85, 60])          # warm, bright heart
sky += (np.exp(-(d / (width * 2.4)) ** 2) * noise(150, 4))[..., None] * np.array([18, 22, 40])   # outer haze

# 3. Stars. First a field of faint ones, then (added on top) bigger, brighter stars with halos.
def splat(img, x, y, radius, colour, power):
    r = int(radius * 11) + 2          # wide enough that the halo fades out before the box edge
    x0, x1, y0, y1 = max(0, x - r), min(W, x + r + 1), max(0, y - r), min(H, y + r + 1)
    if x0 >= x1 or y0 >= y1:
        return
    gx, gy = np.meshgrid(np.arange(x0, x1) - x, np.arange(y0, y1) - y)
    rr = np.hypot(gx, gy)
    prof = np.exp(-(rr / radius) ** 2) + 0.14 * np.exp(-(rr / (radius * 3.5)) ** 2)   # core + soft halo
    img[y0:y1, x0:x1] += prof[..., None] * np.array(colour) * power


stars = np.zeros((H, W, 3), np.float32)
n = 2600
sx, sy = rng.integers(0, W, n), rng.integers(0, H, n)
keep = rng.random(n) < 0.35 + 0.65 * core[sy, sx]
sb = rng.power(6, n) ** 3
stars[sy[keep], sx[keep]] += (110 + 135 * sb[keep])[:, None] * np.array([0.9, 0.95, 1.0])
tints = [(1.0, 1.0, 1.0), (0.8, 0.9, 1.0), (1.0, 0.92, 0.78), (0.75, 0.85, 1.0)]
for i in range(140):                                   # bigger stars: uneven size and brightness
    x, y = int(rng.integers(0, W)), int(rng.integers(0, H))
    big = rng.random() ** 3                            # most modest, a few large
    splat(stars, x, y, 0.8 + 1.9 * big, tints[i % 4], 70 + 130 * big)   # capped so faint stars still show
for _ in range(4):                                     # a few tight clusters near the band
    i = int(rng.integers(0, len(sx)))
    cx, cy = int(rng.integers(300, 900)), int(spine[0, min(W - 1, max(0, int(rng.integers(300, 900))))])
    for _ in range(18):
        splat(stars, cx + int(rng.normal(0, 14)), cy + int(rng.normal(0, 10)), 0.7 + rng.random() * 0.8,
              tints[int(rng.integers(0, 4))], 70 + 90 * rng.random())
sky += stars

# 4. The planet: warped cloud bands on a shaded sphere, lit from the right, with a bright rim.
pcx, pcy, pr = -520, 260, 760
r = np.hypot(xx - pcx, yy - pcy)
inside = np.clip((pr - r) / 2.0, 0, 1)
lat = (yy - pcy) / pr                                  # bands follow latitude...
lon = (xx - pcx) / pr
# A gas giant: latitude bands, torn into eddies where neighbouring bands slide past each other.
# "Stirred": warping a warped texture again turns smooth stripes into turbulent blotches.
turb = warp(warp(noise(60, 6), 34, 110), 22, 70)
bands = np.sin(lat * 34 + 2.6 * (turb - 0.5) * 3.2 + 1.4 * (noise(200, 3) - 0.5) * 3.2)
storms = warp(warp(noise(40, 6), 26, 90), 16, 60)
tex = 0.5 + 0.32 * bands + 0.45 * (storms - 0.5)
# Oval storms riding the bands, each a smooth spot with a darker collar.
for ox, oy, rx, ry, lift in ((150, 150, 34, 15, 0.32), (110, 390, 22, 10, -0.26)):
    q = np.hypot((xx - ox) / rx, (yy - oy) / ry)
    tex += lift * np.exp(-q ** 2) - 0.5 * lift * np.exp(-((q - 1.25) / 0.35) ** 2)
tex = np.clip(0.5 + (tex - 0.5) * 1.3, 0, 1)           # contrast: pushes storms and gaps apart
surface = ramp(tex, [(0.0, (70, 24, 20)), (0.3, (140, 54, 30)), (0.55, (200, 106, 52)),
                     (0.75, (230, 158, 92)), (1.0, (246, 212, 160))])
# Lit as a crescent: light from beyond the right edge, so only a band along the limb is bright,
# fading into night about a third of the way in. Most of the disc stays in deep shadow.
depth = pr - r                                          # px inside the limb
light = np.clip(1 - depth / 130, 0, 1) ** 1.8
shade = 0.05 + 0.95 * light
planet = surface * shade[..., None]
planet += (1 - light)[..., None] * np.array([10, 6, 12]) * (0.6 + 0.8 * storms[..., None])   # faint night side
rim = np.exp(-(depth / 7) ** 2)[..., None] * np.array([170, 130, 110])
planet += rim
sky = sky * (1 - inside[..., None]) + planet * inside[..., None]
atmo = np.exp(-((r - pr) / 9) ** 2) * (r > pr)
sky += atmo[..., None] * np.array([60, 80, 130])

# 5. Mute: pull every pixel 18% of the way toward its own grey. Same hues, calmer.
grey = (sky @ np.array([0.299, 0.587, 0.114], np.float32))[..., None]
sky = grey + (sky - grey) * 0.82

img = Image.fromarray(np.clip(sky, 0, 255).astype(np.uint8))
img.resize((480, 272), Image.LANCZOS).save(sys.argv[1])
img.save(sys.argv[1].replace(".png", "_2x.png"))
print("wrote", sys.argv[1])
