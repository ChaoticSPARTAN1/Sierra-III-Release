#!/usr/bin/env python3
"""Cuts the ship out of the third capture (1081 x 660): a solid hull layer and an additive glow
layer for the engine plumes.

Hull: GrabCut, seeded from a rough hand outline (probably ship), its eroded core (definitely ship)
and everything well outside it (definitely space). GrabCut learns the hull's neutral greys against
the bluish, star-speckled space and settles the real edge.
Plumes: outside the hull, inside a region around each plume, the glow is the pixel minus the dark
space behind it; it is added onto the scene as light, so it stays soft and see-through.

usage: cut.py CAPTURE.png OUTDIR"""
import sys
import numpy as np
import cv2
from PIL import Image

src = np.asarray(Image.open(sys.argv[1]).convert("RGB"))
out = sys.argv[2]
H, W = src.shape[:2]
f = src.astype(np.float32)
lum = f.max(axis=2)

# Rough outline: hull and engine nozzles (up to each nozzle's rim), not the plumes beyond.
HULL = [(255, 275), (300, 238), (380, 190), (440, 148), (480, 110), (520, 76), (560, 50), (600, 40),
        (650, 48), (700, 60), (745, 78), (770, 120), (780, 190), (775, 250), (760, 272), (800, 282),
        (812, 300), (800, 318), (770, 328), (790, 360), (800, 450), (790, 520), (760, 548), (735, 600),
        (700, 622), (650, 630), (560, 602), (470, 562), (400, 532), (330, 520), (270, 515), (240, 492),
        (215, 440), (200, 400), (205, 350), (220, 300)]
rough = np.zeros((H, W), np.uint8)
cv2.fillPoly(rough, [np.array(HULL, np.int32)], 1)

mask = np.full((H, W), cv2.GC_BGD, np.uint8)
mask[cv2.dilate(rough, np.ones((31, 31), np.uint8)) == 1] = cv2.GC_PR_BGD
mask[rough == 1] = cv2.GC_PR_FGD
mask[cv2.erode(rough, np.ones((31, 31), np.uint8)) == 1] = cv2.GC_FGD
bgd, fgd = np.zeros((1, 65)), np.zeros((1, 65))
cv2.grabCut(cv2.cvtColor(src, cv2.COLOR_RGB2BGR), mask, None, bgd, fgd, 8, cv2.GC_INIT_WITH_MASK)
hull = ((mask == cv2.GC_FGD) | (mask == cv2.GC_PR_FGD)).astype(np.uint8)
n, lab, st, _ = cv2.connectedComponentsWithStats(hull, 8)
hull = (lab == 1 + np.argmax(st[1:, cv2.CC_STAT_AREA])).astype(np.uint8)
flood = hull * 255
cv2.floodFill(flood, np.zeros((H + 2, W + 2), np.uint8), (0, 0), 128)
hull = (flood != 128).astype(np.uint8)                       # fill holes: shadowed plates stay solid
hull = cv2.morphologyEx(hull, cv2.MORPH_OPEN, np.ones((3, 3), np.uint8))
alpha = cv2.GaussianBlur(hull.astype(np.float32), (0, 0), 0.8)
np.save(f"{out}/hull_mask.npy", hull)

# Plumes: the glow is what the plume adds over dark space. Space here is near-black with stars, so
# its level is estimated by a wide median of the darker pixels, and stars are removed with a small
# median first so they don't end up in the glow.
# Region: everything right of the nozzles, outside the hull. Stars are removed by size (tiny
# specks) rather than by drawing a tight region, so the plumes keep their natural soft ends.
region = ((np.mgrid[0:H, 0:W][1] >= 640) & (hull == 0)).astype(np.uint8)
clean = cv2.medianBlur(src, 5).astype(np.float32)
space = np.array([12, 10, 22], np.float32)
strength = np.clip((clean.max(axis=2) - 60) / 90, 0, 1) * region         # plume, not faint haze/nebula
blobs = (strength > 0.05).astype(np.uint8)
n, lab, st, _ = cv2.connectedComponentsWithStats(blobs, 8)
keep = np.zeros(n, bool); keep[1:] = st[1:, cv2.CC_STAT_AREA] >= 400   # plumes are big; stars are specks
strength *= keep[lab]
strength = cv2.GaussianBlur(strength, (0, 0), 1.5)
glow = np.clip(clean - space, 0, 255) * strength[..., None]

rgba = np.dstack([src, (alpha * 255).astype(np.uint8)])
ys, xs = np.nonzero(alpha > 0.02)
gys, gxs = np.nonzero(glow.max(axis=2) > 2)
x0, x1 = min(xs.min(), gxs.min()), max(xs.max(), gxs.max()) + 1
y0, y1 = min(ys.min(), gys.min()), max(ys.max(), gys.max()) + 1
Image.fromarray(rgba[y0:y1, x0:x1], "RGBA").save(f"{out}/hull.png")
Image.fromarray(np.clip(glow, 0, 255).astype(np.uint8)[y0:y1, x0:x1]).save(f"{out}/plume_glow.png")
print("crop x %d-%d y %d-%d" % (x0, x1, y0, y1))
