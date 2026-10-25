#!/usr/bin/env python3
"""psp_glyphs.py - the PSP's button glyphs, for the game's button prompts.

Halo's prompts name Xbox buttons; the port shows the PSP-1000's instead
(docs/game_feature_survey.md, section 4.7), each in the shape of the button
itself: the face buttons as round buttons with their symbol, the D-pad as its
four pointed leaves, L and R as the slanted shoulder tabs with their curved
backs, the nub as its dimpled disc, START and SELECT as the small oval keys.

The glyphs are glass in the game's own colour, not pictures in fixed colours:
each texel holds a coverage (alpha, 4 bits) and a shade (4 bits), and the
draw (psp_ge.c, glyph_palette) turns the shade into colour from the colour
the game hands the icon, which is its text colour. So a prompt's glyph sits
in the HUD's or the menu's scheme. Shade 0 is the colour darkened to 15%,
0.7 the colour itself, and above it the colour lightens to white: the glass
body is the colour a little darkened, its gloss and rim lighter, the symbol
near white. glyph_palette in psp_ge.c and palette() here are the same rule.

Cells are CELL pixels high, one to three cells wide, packed into rows of 8
cells; psp_glyphs.h carries the layout (in the order of GLYPHS, which
psp_sdl.c's maps from Xbox buttons refer to) and, for psp_ge.c, the sheet as
one byte a texel (alpha << 4 | shade): the GE's 8-bit indexed format, 32 KB,
run-length coded to about 14 (psp_ge.c unpacks it into VRAM at start-up).
psp_glyphs.png beside it is a preview in the HUD's blue on the left and white
on the right.

The shapes are geometry, so they draw at any size. The sheet the game
carries is sized for the screen (32-pixel cells for glyphs drawn about 18
pixels high): larger, the GE would shrink it more than twice over with no
mipmaps, which shimmers, and it would take four times the memory. --master
writes a large copy for looking at and for other uses (the menu art, the
README): full precision, not cut to 4 bits, in white and in the HUD's blue.

usage: python3 tools/psp_glyphs.py ENGINE_CHECKOUT OUT_DIR [--cell=32] [--header=PATH]
       python3 tools/psp_glyphs.py ENGINE_CHECKOUT OUT_DIR --master [--cell=128]
(needs numpy, Pillow and scipy; the repo's copies: OUT_DIR psp/glyphs, --header=psp/src/psp_glyphs.h, and
--master --cell=128 for psp/glyphs/psp_glyphs_master.png; ENGINE_CHECKOUT for
Overpass, ChupathingyCE's port/assets/fonts)"""

import math
import os
import sys

import numpy as np
from PIL import Image, ImageDraw, ImageFont
from scipy.ndimage import distance_transform_edt

CELL = 32
# drawn this many times larger and reduced, for clean edges
SCALE = 8

# (name, kind, what, cells): a chord's cells are its two buttons' and the
# plus's between them (PLUS_CELLS)
GLYPHS = [
    ("cross", "face", "cross", 1),
    ("circle", "face", "circle", 1),
    ("square", "face", "square", 1),
    ("triangle", "face", "triangle", 1),
    ("dpad_up", "dpad", "up", 1),
    ("dpad_down", "dpad", "down", 1),
    ("dpad_left", "dpad", "left", 1),
    ("dpad_right", "dpad", "right", 1),
    ("dpad", "dpad", None, 1),
    ("l", "shoulder", "L", 2),
    ("r", "shoulder", "R", 2),
    ("nub", "nub", None, 1),
    ("face", "cluster", None, 1),
    ("start", "pill", "START", 2),
    ("select", "pill", "SELECT", 2),
    ("select_up", "chord", ("select", "dpad_up"), None),
    ("select_down", "chord", ("select", "dpad_down"), None),
    ("select_l", "chord", ("select", "l"), None),
]

# the shades (0-1, see the docstring)
DARK_EDGE = 0.0
BODY_TOP, BODY_BOTTOM = 0.5, 0.36
# how much of the picture behind the glass body hides (the rest shows through)
BODY_ALPHA = 0.5
# the glass's finish (--finish=), each with the rim:
#   gloss     a body darker toward the bottom under a crisp gloss over its
#             upper part, with light along the bottom edge
#   solid     one shade (SOLID), between the gloss's light and dark
#   soft      that shade with a thin soft highlight inside the top edge
#   satin     that shade under the gloss, lighter (SATIN_GLOSS of its
#             strength): the pick, "B+ and some gloss" (2026-10-16)
#   vertical  lighter at the top, darker at the bottom (SPREAD either way)
#   underlit  the other way: darker at the top, lighter at the bottom
#   dome      lighter in the middle, darker toward the edges
#   diagonal  lighter at the top left, darker at the bottom right
#   (the pick, 2026-10-16: dome, at SPREAD 0.26)
FINISH = "dome"
SOLID = 0.72
SATIN_GLOSS = 0.45
SPREAD = 0.26
GLOSS = 0.9
GLOW = 0.8
RIM = 0.88
# the bright rim's width, for a cell's height (--rim=)
RIM_WIDTH = 0.035
SYMBOL = 0.96


class Layers:
    """a glyph as it is built up: coverage A and premultiplied shade P"""

    def __init__(self, width, height):
        self.a = np.zeros((height, width), np.float32)
        self.p = np.zeros((height, width), np.float32)

    def over(self, coverage, alpha, shade):
        """a layer on top: coverage 0-1 (an array), its alpha, its shade (a
        number or an array)"""
        a = coverage * alpha
        self.p = shade * a + self.p * (1.0 - a)
        self.a = a + self.a * (1.0 - a)


def blank(width, height):
    return Image.new("L", (width, height), 0)


def as_array(mask):
    return np.asarray(mask, np.float32) / 255.0


def distance(mask):
    """each pixel's signed distance to the shape's edge, in pixels (negative
    inside): measured in a straight line, so anything built from it (a rim,
    an edge, a rounded corner) is the same width at every angle, which
    square grow and shrink filters aren't on a slant"""
    inside = np.asarray(mask) >= 128
    return distance_transform_edt(~inside) - distance_transform_edt(inside)


def within(field, limit):
    return Image.fromarray(np.where(field <= limit, 255, 0).astype(np.uint8), "L")


def rounded(mask, radius):
    """the shape's outer corners rounded to true arcs of this radius: shrunk
    by it, then grown back by it"""
    if radius < 1:
        return mask
    return within(distance(within(distance(mask), -radius)), radius)


def grown(mask, pixels):
    return within(distance(mask), pixels) if pixels >= 0.5 else mask


def shrunk(mask, pixels):
    return within(distance(mask), -pixels) if pixels >= 0.5 else mask


def bounds(mask):
    box = mask.getbbox()
    return box if box else (0, 0, mask.width, mask.height)


def glass(layers, mask, strength=1.0, body=None):
    """a glass button of this shape, mostly see-through: a faint dark edge
    so it holds against a bright picture, a thin body shaded by FINISH (or
    the shades given), and a bright rim"""
    size = mask.height
    x0, y0, x1, y1 = bounds(mask)
    top = np.clip((np.arange(mask.height, dtype=np.float32)[:, None] - y0) / max(1, y1 - y0), 0.0, 1.0)
    left = np.clip((np.arange(mask.width, dtype=np.float32)[None, :] - x0) / max(1, x1 - x0), 0.0, 1.0)
    # how far from the middle, 0 there and 1 at the box's edge (an ellipse)
    out = np.sqrt((2.0 * left - 1.0) ** 2 + (2.0 * top - 1.0) ** 2)
    m = as_array(mask)
    layers.over(as_array(grown(mask, size * 0.025)), 0.35 * strength, DARK_EDGE)
    if body is not None:
        shade = body
    elif FINISH == "gloss":
        shade = BODY_TOP + (BODY_BOTTOM - BODY_TOP) * top
    elif FINISH == "vertical":
        shade = SOLID + SPREAD * (1.0 - 2.0 * top)
    elif FINISH == "underlit":
        shade = SOLID - SPREAD * (1.0 - 2.0 * top)
    elif FINISH == "dome":
        shade = SOLID + SPREAD * (1.0 - 2.0 * np.clip(out, 0.0, 1.0))
    elif FINISH == "diagonal":
        shade = SOLID + SPREAD * (1.0 - top - left)
    else:
        shade = SOLID
    layers.over(m, BODY_ALPHA * strength, shade)
    if FINISH in ("gloss", "satin"):
        # the gloss: an ellipse over the upper part, its lower edge crisp (a
        # reflection's edge, the glassy look) and fading a little toward it
        gloss = blank(mask.width, mask.height)
        ImageDraw.Draw(gloss).ellipse((x0 - (x1 - x0) * 0.3, y0 - (y1 - y0) * 0.6, x1 + (x1 - x0) * 0.3,
                                       y0 + (y1 - y0) * 0.48), fill=255)
        layers.over(as_array(gloss) * m * (0.55 - 0.3 * top), (1.0 if FINISH == "gloss" else SATIN_GLOSS) * strength,
                    GLOSS)
    if FINISH == "gloss":
        # light along the bottom inside edge
        layers.over(m * np.clip((top - 0.7) / 0.3, 0.0, 1.0), 0.3 * strength, GLOW)
    if FINISH == "soft":
        # a thin highlight just inside the top edge, fading downward
        edge = np.clip(1.0 - top / 0.22, 0.0, 1.0) * as_array(shrunk(mask, max(1, size * 0.03)))
        layers.over(edge, 0.35 * strength, GLOSS)
    rim = as_array(mask) - as_array(shrunk(mask, max(1, size * RIM_WIDTH)))
    layers.over(np.clip(rim, 0.0, 1.0), 0.8 * strength, RIM)


def symbol(layers, mask, strength=1.0):
    layers.over(as_array(mask), strength, SYMBOL)


def points_at(points, size, dx=0.0):
    return [((x + dx) * size, y * size) for x, y in points]


def outline(draw, points, width):
    """a closed outline with round joins"""
    draw.line(points + points[:2], fill=255, width=int(width), joint="curve")


# a face button's radius in its cell, and its symbols' stroke for a radius
FACE_R = 0.44
STROKE = 0.17


def triangle(cx, cy, r):
    """the face buttons' triangle for a button of radius r, pointing up"""
    k = r * 0.52
    return [(cx, cy - k * 1.05), (cx + k * 0.95, cy + k * 0.6), (cx - k * 0.95, cy + k * 0.6)]


def face_symbol(draw, kind, cx, cy, r, size):
    """the PlayStation symbols as the PSP-1000 marks them: thin outlines"""
    width = size * r * STROKE
    if kind == "triangle":
        outline(draw, [(x * size, y * size) for x, y in triangle(cx, cy, r)], width)
    elif kind == "circle":
        k = r * 0.46 * size
        draw.ellipse((cx * size - k, cy * size - k, cx * size + k, cy * size + k), outline=255, width=int(width))
    elif kind == "cross":
        k = r * 0.42
        for a, b in (((cx - k, cy - k), (cx + k, cy + k)), ((cx + k, cy - k), (cx - k, cy + k))):
            draw.line([(a[0] * size, a[1] * size), (b[0] * size, b[1] * size)], fill=255, width=int(width))
    else:
        k = r * 0.4
        pts = [(cx - k, cy - k), (cx + k, cy - k), (cx + k, cy + k), (cx - k, cy + k)]
        outline(draw, [(x * size, y * size) for x, y in pts], width)


def face_button(layers, kind, size, cx=0.5, cy=0.5, r=FACE_R):
    mask = blank(layers.a.shape[1], size)
    ImageDraw.Draw(mask).ellipse(((cx - r) * size, (cy - r) * size, (cx + r) * size, (cy + r) * size), fill=255)
    glass(layers, mask)
    mark = blank(layers.a.shape[1], size)
    face_symbol(ImageDraw.Draw(mark), kind, cx, cy, r, size)
    symbol(layers, mark)


def rotate(points, quarter):
    """points about the cell's centre by quarter turns (clockwise)"""
    out = []
    for x, y in points:
        for _ in range(quarter):
            x, y = 1.0 - y, x
        out.append((x, y))
    return out


def bezier(a, control, b, steps=24):
    return [((1 - t) ** 2 * a[0] + 2 * (1 - t) * t * control[0] + t * t * b[0],
             (1 - t) ** 2 * a[1] + 2 * (1 - t) * t * control[1] + t * t * b[1])
            for t in (i / steps for i in range(steps + 1))]


# one leaf of the D-pad as the up button, as a PSP-3000's photo shows it
# (drawn by hand): its outer edge a little bowed, long straight sides, then a short
# taper to a point at the pad's middle; corners and point rounded (DPAD_ROUND,
# drawn as rounded())
DPAD_LEAF = ([(0.1, 0.62), (0.1, 0.12)] + bezier((0.1, 0.12), (0.5, 0.04), (0.9, 0.12))[1:] +
             [(0.9, 0.62), (0.5, 0.95)])
DPAD_ROUND = 0.055


def centroid(points):
    """a polygon's centre of area"""
    area = cx = cy = 0.0
    for (x0, y0), (x1, y1) in zip(points, points[1:] + points[:1]):
        cross = x0 * y1 - x1 * y0
        area += cross
        cx += (x0 + x1) * cross
        cy += (y0 + y1) * cross
    return cx / (3.0 * area), cy / (3.0 * area)


# the leaf's arrow: the face buttons' triangle smaller (as for a button of
# radius ARROW_R), outlined with their stroke, centred on the leaf (its
# centre of area on the leaf's)
ARROW_R = 0.27
# the arrow's stroke, as a share of the face buttons' (--arrow=)
ARROW_STROKE = 0.75
LEAF_MIDDLE = centroid(DPAD_LEAF)
DPAD_ARROW = [(x - sum(p[0] for p in triangle(0, 0, ARROW_R)) / 3 + LEAF_MIDDLE[0],
               y - sum(p[1] for p in triangle(0, 0, ARROW_R)) / 3 + LEAF_MIDDLE[1]) for x, y in triangle(0, 0, ARROW_R)]
DPAD_TURNS = {"up": 0, "right": 1, "down": 2, "left": 3}


def dpad_leaf(layers, turns, size, scale=1.0, dx=0.0, dy=0.0):
    """a leaf turned to its direction, drawn at scale about the point dx, dy
    from the cell's middle"""
    def place(points):
        return [((0.5 + (x - 0.5) * scale + dx) * size, (0.5 + (y - 0.5) * scale + dy) * size)
                for x, y in rotate(points, turns)]

    mask = blank(size, size)
    ImageDraw.Draw(mask).polygon(place(DPAD_LEAF), fill=255)
    glass(layers, rounded(mask, size * DPAD_ROUND * scale))
    arrow = blank(size, size)
    outline(ImageDraw.Draw(arrow), place(DPAD_ARROW), size * FACE_R * STROKE * ARROW_STROKE * scale)
    symbol(layers, arrow)


def dpad(layers, lit, size):
    """a D-pad button: the one leaf of the PSP's pad, pointing in to the
    middle; with none named, the whole pad, its four leaves spaced round the
    middle as the PSP's are (a gap between the points)"""
    if lit is not None:
        dpad_leaf(layers, DPAD_TURNS[lit], size)
        return
    offsets = {"up": (0.0, -0.275), "right": (0.275, 0.0), "down": (0.0, 0.275), "left": (-0.275, 0.0)}
    for name, turns in DPAD_TURNS.items():
        dpad_leaf(layers, turns, size, 0.44, *offsets[name])


def corner(at, toward_a, toward_b, reach, steps=96):
    """a corner where two straight edges meet, rounded so that it bends
    without a kink: the bend starts from nothing where it leaves edge a,
    builds to its tightest at the middle and fades out again onto edge b
    (its curvature rises and falls smoothly, as an icon's "continuous"
    corners do: a circular arc's bend switches on all at once where the
    straight ends, which the eye reads as a kink). reach: how far along each
    edge from the corner the rounding starts. The points from edge a's side
    to edge b's; toward_a and toward_b are points along each edge"""
    if reach <= 0:
        return [at]

    def unit(v):
        length = math.hypot(v[0], v[1])
        return (v[0] / length, v[1] / length)

    def cross(p, q):
        return p[0] * q[1] - p[1] * q[0]

    a = unit((toward_a[0] - at[0], toward_a[1] - at[1]))
    b = unit((toward_b[0] - at[0], toward_b[1] - at[1]))
    heading = math.atan2(-a[1], -a[0])
    turn = (math.atan2(b[1], b[0]) - heading + math.pi) % (2 * math.pi) - math.pi
    # the path for a length of 1: the heading turns by turn * (u - sin(2 pi u) / 2 pi),
    # whose rate (the bend) is 0 at both ends and greatest in the middle
    n = steps * 20
    x = y = 0.0
    path = [(0.0, 0.0)]
    for k in range(n):
        u = (k + 0.5) / n
        angle = heading + turn * (u - math.sin(2 * math.pi * u) / (2 * math.pi))
        x += math.cos(angle) / n
        y += math.sin(angle) / n
        path.append((x, y))
    # scaled so that it ends on edge b (by symmetry, reach from the corner too)
    length = -reach * cross(a, b) / cross(path[-1], b)
    start = (at[0] + a[0] * reach, at[1] + a[1] * reach)
    return [(start[0] + length * px, start[1] + length * py) for px, py in path[::n // steps]]


# the shoulder's shape (the picks, 2026-10-16): the two slanted sides at
# SHOULDER_ANGLE degrees to the top and bottom; the outer top and inner
# bottom corners rounded broadly (SHOULDER_CURVES: how far along each edge,
# in units of the height); the two sharp points softened a little
# (SHOULDER_POINTS)
SHOULDER_ANGLE = 55.0
SHOULDER_TOP = 2.1
SHOULDER_CURVES = (0.78, 0.91)
SHOULDER_POINTS = 0.07


def shoulder_outline():
    """the PSP's L button (2026-10-16: "the exact shape"), in its own
    box from 0 to 1 each way, and the box's width over its height. After a
    trace of a PSP-3000 photo (the trace itself too ragged), shaped by hand:
    a flat top and bottom; the outer side and the inner side straight slants
    at SHOULDER_ANGLE, the outer one curving broadly into the top and the
    inner one into the bottom; the other two corners points. Worked in units
    of the height, then scaled into the box"""
    slant = 1.0 / math.tan(math.radians(SHOULDER_ANGLE))
    top_outer, top_inner = (slant, 0.0), (slant + SHOULDER_TOP, 0.0)
    bottom_outer, bottom_inner = (0.0, 1.0), (SHOULDER_TOP, 1.0)
    points = (corner(top_inner, top_outer, bottom_inner, SHOULDER_POINTS) +
              corner(bottom_inner, top_inner, bottom_outer, SHOULDER_CURVES[1]) +
              corner(bottom_outer, bottom_inner, top_outer, SHOULDER_POINTS) +
              corner(top_outer, bottom_outer, top_inner, SHOULDER_CURVES[0]))
    xs = [x for x, y in points]
    left, width = min(xs), max(xs) - min(xs)
    return [((x - left) / width, y) for x, y in points], width


SHOULDER_L, SHOULDER_ASPECT = shoulder_outline()


def shoulder(layers, letter, cells, size, font):
    """the PSP's L or R in its traced shape (SHOULDER_L; R is L mirrored),
    as wide as the glyph allows and centred, its letter in the middle"""
    width = cells * size
    shape_width = width * 0.94
    shape_height = shape_width / SHOULDER_ASPECT
    x0, y0 = (width - shape_width) / 2, (size - shape_height) / 2
    mask = blank(width, size)
    ImageDraw.Draw(mask).polygon([(x0 + x * shape_width, y0 + y * shape_height) for x, y in SHOULDER_L], fill=255)
    if letter == "R":
        mask = mask.transpose(Image.FLIP_LEFT_RIGHT)
    glass(layers, mask)
    mark = blank(width, size)
    draw = ImageDraw.Draw(mark)
    box = draw.textbbox((0, 0), letter, font=font)
    tw, th = box[2] - box[0], box[3] - box[1]
    draw.text(((width - tw) / 2 - box[0], size / 2 - th / 2 - box[1]), letter, font=font, fill=255)
    symbol(layers, mark)


def nub(layers, size, font):
    """the PSP's nub: a dark disc, its grip a dense cross-hatch (fine lines
    both ways across it, lighter than the disc), named above it as the
    Xbox's stick icons are ("RIGHT", "LEFT"): "NUB"."""
    r, cy = 0.33, 0.64
    mask = blank(size, size)
    ImageDraw.Draw(mask).ellipse(((0.5 - r) * size, (cy - r) * size, (0.5 + r) * size, (cy + r) * size), fill=255)
    # as dense as still shows at 32 pixels (finer averages to a flat grey)
    lines = 5
    y, x = np.mgrid[0:size, 0:size].astype(np.float32)
    step = 2 * r * size / lines
    # each line a third of the spacing wide, at 45 degrees each way
    one = ((x + y) % step) < step / 3
    other = ((x - y) % step) < step / 3
    hatch = (one | other).astype(np.float32)
    body = 0.1 + 0.5 * hatch - 0.06 * (y / size)
    glass(layers, mask, body=body)
    mark = blank(size, size)
    draw = ImageDraw.Draw(mark)
    box = draw.textbbox((0, 0), "NUB", font=font)
    width, height = box[2] - box[0], box[3] - box[1]
    draw.text(((size - width) / 2 - box[0], 0.03 * size - box[1]), "NUB", font=font, fill=255)
    symbol(layers, mark)


def cluster(layers, size):
    """the four face buttons in their diamond (the face buttons as a stick),
    spaced as the D-pad's leaves are: the same distance from the middle and
    the same gap between them"""
    r, out = 0.19, 0.285
    for kind, (cx, cy) in (("triangle", (0.5, 0.5 - out)), ("square", (0.5 - out, 0.5)), ("circle", (0.5 + out, 0.5)),
                           ("cross", (0.5, 0.5 + out))):
        face_button(layers, kind, size, cx, cy, r)


def pill(layers, text, cells, size, font):
    """START or SELECT, after a hand sketch: the PSP-1000's key, flat along
    its top and curved underneath like a shallow bowl, its name on it (the
    two keys are one shape)"""
    width = cells * size
    left, right, top, bottom = 0.06 * size, width - 0.06 * size, 0.22 * size, 0.8 * size
    # the bowl: half an ellipse below the flat top, its ends a little
    # rounded where they meet the top
    steps = 48
    edge = [(left + (right - left) * (0.5 - 0.5 * math.cos(math.pi * i / steps)),
             top + (bottom - top) * math.sin(math.pi * i / steps) ** 0.8) for i in range(steps + 1)]
    mask = blank(width, size)
    ImageDraw.Draw(mask).polygon([(left, top)] + edge + [(right, top)], fill=255)
    glass(layers, rounded(mask, size * 0.015))
    mark = blank(width, size)
    draw = ImageDraw.Draw(mark)
    box = draw.textbbox((0, 0), text, font=font)
    tw, th = box[2] - box[0], box[3] - box[1]
    draw.text(((width - tw) / 2 - box[0], 0.44 * size - th / 2 - box[1]), text, font=font, fill=255)
    symbol(layers, mark)


def paste(target, source, x, y, scale=1.0):
    """one glyph's layers into another's, at x, y, smaller by scale"""
    a, p = source.a, source.p
    if scale != 1.0:
        new = (max(1, round(a.shape[1] * scale)), max(1, round(a.shape[0] * scale)))
        a = np.asarray(Image.fromarray(a, "F").resize(new, Image.BOX))
        p = np.asarray(Image.fromarray(p, "F").resize(new, Image.BOX))
    h, w = a.shape
    region = (slice(y, y + h), slice(x, x + w))
    target.p[region] = p + target.p[region] * (1.0 - a)
    target.a[region] = a + target.a[region] * (1.0 - a)


# how tall a glyph draws, as a share of the Xbox icon's box it replaces
# (80%, on the text's line); a glyph named over its button, as the
# Xbox's stick icons are, takes the whole box as they do
HEIGHT_SHARE = 0.8
FULL_HEIGHT = {"nub"}

# the room a chord's plus takes between its two buttons, in cells: narrow, so
# the chords keep the sheet at 256 x 128
PLUS_CELLS = 0.45


def chord(layers, first, second, size, font):
    """two buttons pressed together: each at its own size, never scaled
    to fit, with a plus between them"""
    total = layers.a.shape[1]
    paste(layers, first, 0, 0)
    paste(layers, second, total - second.a.shape[1], 0)
    mark = blank(total, size)
    draw = ImageDraw.Draw(mark)
    box = draw.textbbox((0, 0), "+", font=font)
    width, height = box[2] - box[0], box[3] - box[1]
    middle = (first.a.shape[1] + total - second.a.shape[1]) / 2
    draw.text((middle - width / 2 - box[0], (size - height) / 2 - box[1]), "+", font=font, fill=255)
    symbol(layers, mark)


def palette(tint, fade=1.0):
    """psp_ge.c's glyph_palette: index alpha << 4 | shade to RGBA"""
    out = np.zeros((256, 4), np.float32)
    base = np.array(tint, np.float32)
    for index in range(256):
        a, s = (index >> 4) / 15.0, (index & 15) / 15.0
        if s <= 0.7:
            colour = base * (0.15 + 0.85 * s / 0.7)
        else:
            colour = base + (255.0 - base) * ((s - 0.7) / 0.3)
        out[index, :3] = colour
        out[index, 3] = a * 255.0 * fade
    return np.clip(out, 0, 255).astype(np.uint8)


def packbits(data):
    """run-length code the sheet as psp_ge.c's glyph_sheet_unpack reads it: a
    control byte under 128 is followed by that many plus one bytes as they
    are; 128 and over, by one byte repeated (control - 125) times, so runs of
    3 to 130. The sheet is mostly empty: 32 KB codes to about 14"""
    out, at, literal = bytearray(), 0, bytearray()

    def flush():
        if literal:
            out.append(len(literal) - 1)
            out.extend(literal)
            literal.clear()
    while at < len(data):
        run = 1
        while at + run < len(data) and data[at + run] == data[at] and run < 130:
            run += 1
        if run >= 3:
            flush()
            out += bytes((run + 125, data[at]))
            at += run
        else:
            literal.append(data[at])
            at += 1
            if len(literal) == 128:
                flush()
    flush()
    return bytes(out)


def unpackbits(data, size):
    out, at = bytearray(), 0
    while at < len(data):
        control = data[at]
        if control < 128:
            out += data[at + 1:at + 2 + control]
            at += 2 + control
        else:
            out += bytes((data[at + 1],)) * (control - 125)
            at += 2
    assert len(out) == size
    return bytes(out)


def write_sheet(header, flat):
    """the texels for psp_ge.c, which draws them (the only unit that asks):
    one byte each, alpha << 4 | shade (glyph_palette there turns them to
    colour), run-length coded; psp_ge.c unpacks them once into VRAM, so the
    program, and with it each build's heap, carries 14 KB, not the sheet's 32"""
    flat = bytes(int(b) for b in flat)
    packed = packbits(flat)
    assert unpackbits(packed, len(flat)) == flat
    header.write("#ifdef PSP_GLYPH_PIXELS\n")
    header.write("/* run-length coded (tools/psp_glyphs.py, packbits()): %d bytes unpacked */\n" % len(flat))
    header.write("static const unsigned char psp_glyph_packed[%d] =\n{\n" % len(packed))
    for at in range(0, len(packed), 32):
        header.write("\t" + ",".join("%d" % b for b in packed[at:at + 32]) + ",\n")
    header.write("};\n#endif\n")


def main():
    global CELL
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    header_path = None
    master = "--master" in sys.argv[1:]
    global FINISH, SOLID, SATIN_GLOSS, RIM_WIDTH, SPREAD, ARROW_STROKE
    for a in sys.argv[1:]:
        if a.startswith("--finish="):
            FINISH = a.split("=", 1)[1]
        elif a.startswith("--solid="):
            SOLID = float(a.split("=", 1)[1])
        elif a.startswith("--satin="):
            SATIN_GLOSS = float(a.split("=", 1)[1])
        elif a.startswith("--rim="):
            RIM_WIDTH = float(a.split("=", 1)[1])
        elif a.startswith("--spread="):
            SPREAD = float(a.split("=", 1)[1])
        elif a.startswith("--arrow="):
            ARROW_STROKE = float(a.split("=", 1)[1])
    for a in sys.argv[1:]:
        if a.startswith("--cell="):
            CELL = int(a.split("=", 1)[1])
        elif a.startswith("--header="):
            header_path = a.split("=", 1)[1]
    if len(args) != 2:
        sys.exit(__doc__)
    engine, out = args
    letter_path = os.path.join(engine, "port/assets/fonts/Overpass-900.ttf")
    big = CELL * SCALE
    letter_font = ImageFont.truetype(letter_path, big * 2 // 5)
    pill_font = ImageFont.truetype(letter_path, big * 3 // 10)
    plus_font = ImageFont.truetype(letter_path, big // 2)

    # each glyph's width in the sheet's pixels: whole cells, or for a chord its
    # parts' widths and the plus's
    widths = {}
    for name, kind, what, cells in GLYPHS:
        widths[name] = (widths[what[0]] + round(PLUS_CELLS * CELL) + widths[what[1]] if kind == "chord"
                        else cells * CELL)
    drawn = {}
    for name, kind, what, cells in GLYPHS:
        layers = Layers(widths[name] * SCALE, big)
        if kind == "face":
            face_button(layers, what, big)
        elif kind == "dpad":
            dpad(layers, what, big)
        elif kind == "shoulder":
            shoulder(layers, what, cells, big, letter_font)
        elif kind == "nub":
            nub(layers, big, pill_font)
        elif kind == "cluster":
            cluster(layers, big)
        elif kind == "pill":
            pill(layers, what, cells, big, pill_font)
        else:
            chord(layers, drawn[what[0]], drawn[what[1]], big, plus_font)
        drawn[name] = layers

    # packed left to right in rows 8 cells wide, a glyph never split; places
    # in pixels (a chord isn't whole cells)
    columns = 8
    places, x, y = [], 0, 0
    for name, kind, what, cells in GLYPHS:
        if x + widths[name] > columns * CELL:
            x, y = 0, y + 1
        places.append((name, x, y, widths[name]))
        x += widths[name]
    rows = y + 1
    sheet_a = np.zeros((rows * CELL, columns * CELL), np.float32)
    sheet_p = np.zeros_like(sheet_a)
    for name, x, y, width in places:
        layers = drawn[name]
        # reduced by averaging each SCALE x SCALE block (premultiplied, so
        # the shade at an edge stays the edge's own)
        h, w = layers.a.shape
        a = layers.a.reshape(h // SCALE, SCALE, w // SCALE, SCALE).mean(axis=(1, 3))
        p = layers.p.reshape(h // SCALE, SCALE, w // SCALE, SCALE).mean(axis=(1, 3))
        sheet_a[y * CELL:(y + 1) * CELL, x:x + width] = a
        sheet_p[y * CELL:(y + 1) * CELL, x:x + width] = p
    shade = np.where(sheet_a > 1e-4, sheet_p / np.maximum(sheet_a, 1e-4), 0.0)
    os.makedirs(out, exist_ok=True)
    if master:
        # palette()'s rule at full precision: shade and alpha as drawn
        def tinted(tint):
            base = np.array(tint, np.float32)
            s3 = shade[..., None]
            colour = np.where(s3 <= 0.7, base * (0.15 + 0.85 * s3 / 0.7), base + (255.0 - base) * ((s3 - 0.7) / 0.3))
            rgba = np.concatenate([colour, sheet_a[..., None] * 255.0], axis=2)
            return Image.fromarray(np.clip(np.rint(rgba), 0, 255).astype(np.uint8), "RGBA")

        image = Image.new("RGBA", (columns * CELL * 2 + CELL // 4, rows * CELL), (0, 0, 0, 0))
        image.alpha_composite(tinted((96, 160, 255)), (0, 0))
        image.alpha_composite(tinted((235, 235, 235)), (columns * CELL + CELL // 4, 0))
        image.save(os.path.join(out, "psp_glyphs_master.png"), optimize=True)
        print("psp_glyphs: master, %d glyphs at %d-pixel cells" % (len(places), CELL))
        return
    alpha4 = np.clip(np.rint(sheet_a * 15.0), 0, 15).astype(np.uint8)
    shade4 = np.clip(np.rint(shade * 15.0), 0, 15).astype(np.uint8)
    indices = (alpha4 << 4) | np.where(alpha4 > 0, shade4, 0).astype(np.uint8)

    # the preview: the sheet as the GE draws it, in the HUD's blue and in white
    blue = palette((96, 160, 255))[indices]
    white = palette((235, 235, 235))[indices]
    preview = Image.new("RGBA", (columns * CELL * 2 + 8, rows * CELL), (0, 0, 0, 0))
    preview.alpha_composite(Image.fromarray(blue, "RGBA"), (0, 0))
    preview.alpha_composite(Image.fromarray(white, "RGBA"), (columns * CELL + 8, 0))
    preview.save(os.path.join(out, "psp_glyphs.png"))

    with open(header_path or os.path.join(out, "psp_glyphs.h"), "w") as header:
        header.write("/* psp_glyphs.h - the button glyph sheet (tools/psp_glyphs.py writes it, with a preview,\n"
                     "psp/glyphs/psp_glyphs.png): each glyph's cell in pixels, x y width height */\n")
        header.write("#define PSP_GLYPH_SHEET_WIDTH %d\n#define PSP_GLYPH_SHEET_HEIGHT %d\n" % (columns * CELL, rows * CELL))
        header.write("enum psp_glyph\n{\n")
        for name, *_ in places:
            header.write("\tPSP_GLYPH_%s,\n" % name.upper())
        header.write("\tPSP_GLYPH_COUNT\n};\n")
        header.write("static const unsigned short psp_glyph_cells[PSP_GLYPH_COUNT][4] =\n{\n")
        for name, x, y, width in places:
            header.write("\t{ %d, %d, %d, %d }, /* %s */\n" % (x, y * CELL, width, CELL, name))
        header.write("};\n")
        header.write("/* how tall each draws, in percent of the Xbox icon's box (psp_ge.c, glyph_quad; psp_sdl.c,\n"
                     "psp_button_glyph_extra) */\n")
        header.write("static const unsigned char psp_glyph_height[PSP_GLYPH_COUNT] =\n{\n\t")
        header.write(", ".join(str(100 if name in FULL_HEIGHT else round(HEIGHT_SHARE * 100)) for name, *_ in places))
        header.write("\n};\n")
        write_sheet(header, indices.reshape(-1))
    print("psp_glyphs: %d glyphs, sheet %dx%d" % (len(places), columns * CELL, rows * CELL))


if __name__ == "__main__":
    main()
