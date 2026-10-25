#!/usr/bin/env python3
"""Keys the ring and the ship out of a capture shot against black space.

Separation is by colour, not brightness: the hull and the ring are neutral grey (red, green and
blue roughly equal, however dark), while the haze and the planet's glow are strongly blue or
purple. So a pixel belongs to an object if it is above pure black and neutral, or bright (the
blue-white engine glow).
  - The object's body gets a filled mask, so dark plates stay opaque.
  - Its soft edge takes alpha from brightness, with the colour un-premultiplied (divided by
    alpha) to undo the darkening from blending with black.
Stars are tiny isolated specks: anything under MIN_AREA pixels is dropped.

usage: key.py CAPTURE.png OUTDIR"""
import sys
import numpy as np
import cv2
from PIL import Image

MIN_AREA = 25
src = np.asarray(Image.open(sys.argv[1]).convert("RGB")).astype(np.float32)
out = sys.argv[2]
H, W = src.shape[:2]
r, g, b = src[..., 0], src[..., 1], src[..., 2]
lum = src.max(axis=2)
yy, xx = np.mgrid[0:H, 0:W]

neutral = b <= 1.3 * np.maximum(r, g) + 2          # grey metal: blue no stronger than red or green
bright = lum >= 18                                 # blue engine housings and glow: brighter than the dim haze (4-12)
SHIP_BOX = (xx >= 230) & (yy <= 212)


def components(mask, min_area=MIN_AREA):
    n, lab, st, _ = cv2.connectedComponentsWithStats(mask.astype(np.uint8), 8)
    keep = np.zeros(n, bool)
    keep[1:] = st[1:, cv2.CC_STAT_AREA] >= min_area
    return keep[lab].astype(np.uint8)


def fill_holes(mask):
    flood = mask.copy() * 255
    cv2.floodFill(flood, np.zeros((H + 2, W + 2), np.uint8), (0, 0), 128)
    return (flood != 128).astype(np.uint8)


def save(name, solid, soft=False):
    if soft:                                       # an already anti-aliased silhouette: use it as is
        a = solid
    else:
        near = cv2.dilate(solid, np.ones((3, 3), np.uint8))
        a = np.maximum(cv2.GaussianBlur(solid.astype(np.float32), (0, 0), 0.6), np.clip(lum / 60, 0, 1) * near)
    rgb = np.where(a[..., None] > 0.02, src / np.maximum(a[..., None], 0.02), 0)
    rgba = np.dstack([np.clip(rgb, 0, 255), a * 255]).astype(np.uint8)
    ys, xs = np.nonzero(a > 0.02)
    crop = rgba[ys.min():ys.max() + 1, xs.min():xs.max() + 1]
    Image.fromarray(crop, "RGBA").save(f"{out}/{name}.png")
    # two checks: on mid-grey, and "lifted" (dark tones brightened) to show that dark parts survived
    for tag, fn in (("on_grey", lambda c: c), ("lifted", lambda c: (np.clip((c / 255) ** 0.35, 0, 1) * 255))):
        base = Image.new("RGBA", (crop.shape[1], crop.shape[0]), (90, 90, 90, 255))
        im = np.asarray(Image.alpha_composite(base, Image.fromarray(crop, "RGBA")).convert("RGB")).astype(np.float32)
        Image.fromarray(fn(im).astype(np.uint8)).resize((crop.shape[1] * 2, crop.shape[0] * 2), Image.LANCZOS).save(
            f"{out}/{name}_{tag}.png")
    print(name, "bounds x %d-%d y %d-%d" % (xs.min(), xs.max(), ys.min(), ys.max()))


obj = (lum >= 1) & (neutral | bright)
# Ship: its shadowed body is pure black, the same as space, so no colour or brightness rule can find
# its edge. The silhouette is hand-traced instead, point by point along the hull's lit edge, from a
# brightened 5x view with a 5 px grid. Inside the trace is ship (shadowed areas stay solid); outside
# is space, including the haze-filled gap under the lower engine. Drawn at 4x and shrunk, so the
# edge is anti-aliased.
SHIP_OUTLINE = [
    # top edge, left to right
    (330, 28), (338, 23), (345, 19.5), (370, 19), (392, 19.5), (396, 22), (402, 32), (403, 38),
    # right side of the upper engine, then the right-hand light cluster
    (404, 50), (405, 70), (406, 76), (413, 82), (422, 84), (425, 88), (425, 110), (420, 114),
    (415, 124), (412, 130), (406, 138),
    # bottom of the lower engine housing, right to left
    (400, 144), (392, 150), (380, 153), (366, 154), (352, 153), (344, 150), (342, 148),
    # down the hull's lower right edge, then its bottom edge, right to left
    (346, 164), (336, 176), (332, 180), (316, 182), (302, 182), (286, 186), (276, 190), (268, 194),
    (264, 190), (262, 184),
    # up the hull's left side
    (260, 182), (258, 174), (251, 150), (244, 138), (243, 122), (246, 118), (250, 117),
    # along the hull's top-left edge, back to the start
    (251, 110), (258, 102), (266, 86), (271, 79), (276, 76), (283, 70), (289, 63), (296, 56),
    (302, 49), (310, 42), (318, 36), (324, 31)]
SS = 4
big = np.zeros((H * SS, W * SS), np.uint8)
cv2.fillPoly(big, [np.round((np.array(SHIP_OUTLINE) + 0.5) * SS).astype(np.int32)], 255, cv2.LINE_AA)
ship = cv2.resize(big.astype(np.float32), (W, H), interpolation=cv2.INTER_AREA) / 255
np.save(f"{out}/ship_mask.npy", (ship > 0.5).astype(np.uint8))
save("ship", ship, soft=True)
purple = (b - g > 4) & (lum < 90)                  # the planet's edge: dim and bluer than green
ring = (obj & ~SHIP_BOX & ~purple).astype(np.uint8)
ring = cv2.morphologyEx(ring, cv2.MORPH_CLOSE, np.ones((3, 3), np.uint8))
n, lab, st, _ = cv2.connectedComponentsWithStats(ring, 8)
ring = (lab == 1 + np.argmax(st[1:, cv2.CC_STAT_AREA])).astype(np.uint8)   # the loop: the largest piece
# Thin dark streaks touch the ring. The ring itself follows an ellipse, so fit one to its pixels and
# keep only a corridor around that curve: the streaks leave it, the band stays whole. Small gaps in
# the band's dark texture are closed inside the corridor.
# Two passes: a loose corridor (14 px) around a first fit, which the streaks still bias, then a
# tight one (8 px) around a refit to that cleaner result, which drops the streaks' stubs.
for width in (14, 8):
    ys, xs = np.nonzero(ring)
    fit = cv2.fitEllipse(np.stack([xs, ys], 1).astype(np.float32))
    curve = np.zeros((H, W), np.uint8)
    cv2.ellipse(curve, fit, 255, 1)
    corridor = cv2.distanceTransform(255 - curve, cv2.DIST_L2, 5) <= width
    ring = (cv2.morphologyEx((ring.astype(bool) & corridor).astype(np.uint8), cv2.MORPH_CLOSE,
                             np.ones((5, 5), np.uint8)).astype(bool) & corridor).astype(np.uint8)
# The band itself: in each of 180 slices around the fitted ellipse, measure where the band sits
# (its centre and half-width in "ellipse radius" units), smooth those around the loop, and take
# every pixel inside that band whatever its colour. That bridges the gap where the ring crosses the
# planet's glow (the colour rules cut it there) and drops debris sticking out of the band.
(ecx, ecy), (ew, eh), eang = fit
t = np.radians(eang)
u = (xx - ecx) * np.cos(t) + (yy - ecy) * np.sin(t)
v = -(xx - ecx) * np.sin(t) + (yy - ecy) * np.cos(t)
rho = np.hypot(u / (ew / 2), v / (eh / 2))
slice_ = ((np.arctan2(v / (eh / 2), u / (ew / 2)) + np.pi) / (2 * np.pi) * 180).astype(int) % 180
centre, half = np.full(180, np.nan), np.full(180, np.nan)
for k in range(180):
    sel = (slice_ == k) & ring.astype(bool)
    if sel.sum() >= 8:
        lo, hi = np.percentile(rho[sel], [10, 90])
        centre[k], half[k] = (lo + hi) / 2, (hi - lo) / 2
for arr in (centre, half):                               # fill gaps, then smooth around the loop
    idx = np.arange(180); ok = ~np.isnan(arr)
    arr[:] = np.interp(idx, idx[ok], arr[ok], period=180)
    arr[:] = np.convolve(np.r_[arr[-6:], arr, arr[:6]], np.ones(13) / 13, mode="same")[6:-6]
gy, gx = np.gradient(rho)                                # rho units per pixel, for a soft 1 px edge
dist_px = (np.abs(rho - centre[slice_]) - half[slice_] - 0.006) / np.maximum(np.hypot(gx, gy), 1e-6)
ring = np.clip(0.5 - dist_px, 0, 1).astype(np.float32)
save("ring", ring, soft=True)
