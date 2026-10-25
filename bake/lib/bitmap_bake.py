"""bitmap_bake.py - prototype texture baker for the PSP.

Extracts every bitmap from an Xbox-format Halo CE map, decodes it to RGBA
(DXT1/3/5, 16- and 32-bit RGB, A8/Y8/AY8/A8Y8; Xbox-swizzled formats are
unswizzled), applies a per-role policy (resolution, drop/keep), quantizes to
CLUT8 (and CLUT4 for comparison), swizzles for the GE, and reports sizes and
quality before/after.

usage: python3 bitmap_bake.py map.map [--out DIR] [--previews N] [--limit N]

Policy "A" (default, aimed at invisible-at-480x272):
  * every texture with max side >= 64 drops its top mip (Xbox draws to 480
    lines, the PSP to 272: 2^-0.82 of the texel density, so the GE samples
    mip 0 of the original almost never); 1024s become 512 (GE maximum)
  * lightmaps keep their resolution (low frequency, already small)
  * bump maps (P8 / height / vector maps) are dropped: the GE has no per-pixel
    lighting to use them
  * cube maps: become a 64x64 sphere map (approximation; GE has no cube maps)
  * format: CLUT8, swizzled; palette 16-bit 5650 (opaque) or 32-bit 8888
    (alpha) -- the frame buffer is 16-bit so 5650 is lossless for opaque
  * mips: the chain is kept down to 8 px (GE uses up to 8 levels)
"""
import sys, os, struct, collections, json, math, argparse
import numpy as np
from PIL import Image
from halomap import Defs, CacheMap
from assets import bitmaps, FORMATS, BPP
import refs


# ---------------------------------------------------------------- decoding
def unswizzle_xbox(arr, w, h):
    """Xbox textures (non-DXT, swizzled flag) are stored in Morton order:
    interleaved x/y bits, x lowest, for as many bits as each side has."""
    xs = np.arange(w)
    ys = np.arange(h)
    ox = np.zeros(w, np.int64)
    oy = np.zeros(h, np.int64)
    bit = 0
    i = 0
    while (1 << i) < w or (1 << i) < h:
        if (1 << i) < w:
            ox |= ((xs >> i) & 1) << bit
            bit += 1
        if (1 << i) < h:
            oy |= ((ys >> i) & 1) << bit
            bit += 1
        i += 1
    idx = oy[:, None] + ox[None, :]
    return arr[idx.reshape(-1)].reshape(h, w, *arr.shape[1:])


def rgb565(v):
    r = ((v >> 11) & 31) * 255 // 31
    g = ((v >> 5) & 63) * 255 // 63
    b = (v & 31) * 255 // 31
    return r, g, b


def decode_dxt(buf, w, h, kind):
    bw, bh = max(1, (w + 3) // 4), max(1, (h + 3) // 4)
    bs = 8 if kind == 'DXT1' else 16
    raw = np.frombuffer(buf[:bw * bh * bs], np.uint8).reshape(bh * bw, bs)
    col = raw[:, bs - 8:]
    c0 = col[:, 0].astype(np.uint32) | (col[:, 1].astype(np.uint32) << 8)
    c1 = col[:, 2].astype(np.uint32) | (col[:, 3].astype(np.uint32) << 8)
    bits = col[:, 4].astype(np.uint32) | (col[:, 5].astype(np.uint32) << 8) | \
        (col[:, 6].astype(np.uint32) << 16) | (col[:, 7].astype(np.uint32) << 24)
    p0 = np.stack(rgb565(c0), -1).astype(np.int32)
    p1 = np.stack(rgb565(c1), -1).astype(np.int32)
    four = (c0 > c1) | (kind != 'DXT1')
    p2 = np.where(four[:, None], (2 * p0 + p1) // 3, (p0 + p1) // 2)
    p3 = np.where(four[:, None], (p0 + 2 * p1) // 3, 0)
    pal = np.stack([p0, p1, p2, p3], 1)                      # n,4,3
    alpha_pal = np.full((len(c0), 4), 255, np.int32)
    if kind == 'DXT1':
        alpha_pal[:, 3] = np.where(four, 255, 0)
    sel = (bits[:, None] >> (2 * np.arange(16, dtype=np.uint32))[None, :]) & 3   # n,16
    rgb = np.take_along_axis(pal, sel[:, :, None].astype(np.int64).repeat(3, 2), 1)
    a = np.take_along_axis(alpha_pal, sel.astype(np.int64), 1)
    if kind == 'DXT3':
        ab = raw[:, :8]
        nib = np.stack([ab & 15, ab >> 4], -1).reshape(-1, 16)
        a = nib.astype(np.int32) * 17
    elif kind == 'DXT5':
        a0 = raw[:, 0].astype(np.int32)
        a1 = raw[:, 1].astype(np.int32)
        abits = np.zeros(len(a0), np.uint64)
        for k in range(6):
            abits |= raw[:, 2 + k].astype(np.uint64) << np.uint64(8 * k)
        asel = ((abits[:, None] >> (np.uint64(3) * np.arange(16, dtype=np.uint64))[None, :]) & np.uint64(7)).astype(np.int64)
        apal = np.zeros((len(a0), 8), np.int32)
        apal[:, 0], apal[:, 1] = a0, a1
        big = a0 > a1
        for k in range(1, 7):
            apal[:, k + 1] = np.where(big, ((7 - k) * a0 + k * a1) // 7, 0)
        for k in range(1, 5):
            apal[:, k + 1] = np.where(big, apal[:, k + 1], ((5 - k) * a0 + k * a1) // 5)
        apal[:, 6] = np.where(big, apal[:, 6], 0)
        apal[:, 7] = np.where(big, apal[:, 7], 255)
        a = np.take_along_axis(apal, asel, 1)
    px = np.concatenate([rgb, a[:, :, None]], 2).reshape(bh, bw, 4, 4, 4)
    img = px.transpose(0, 2, 1, 3, 4).reshape(bh * 4, bw * 4, 4)[:h, :w]
    return img.astype(np.uint8)


def decode_plain(buf, w, h, fmt, swizzled):
    bpp = BPP[fmt] // 8
    raw = np.frombuffer(buf[:w * h * bpp], np.uint8)
    if fmt in (10, 11):            # X8R8G8B8 / A8R8G8B8 (little endian BGRA)
        p = raw.reshape(-1, 4)[:, [2, 1, 0, 3]].copy()
        if fmt == 10:
            p[:, 3] = 255
    elif fmt in (6, 8, 9):
        v = raw.view('<u2').astype(np.int32)
        if fmt == 6:
            p = np.stack(list(rgb565(v)) + [np.full_like(v, 255)], -1)
        elif fmt == 8:
            p = np.stack([((v >> 10) & 31) * 255 // 31, ((v >> 5) & 31) * 255 // 31, (v & 31) * 255 // 31,
                          np.where(v >> 15, 255, 0)], -1)
        else:
            p = np.stack([((v >> 8) & 15) * 17, ((v >> 4) & 15) * 17, (v & 15) * 17, (v >> 12) * 17], -1)
    elif fmt == 0:                 # A8
        p = np.stack([np.full_like(raw, 255)] * 3 + [raw], -1)
    elif fmt == 1:                 # Y8
        p = np.stack([raw, raw, raw, np.full_like(raw, 255)], -1)
    elif fmt == 2:                 # AY8: same byte is both
        p = np.stack([raw, raw, raw, raw], -1)
    elif fmt == 3:                 # A8Y8: low byte Y, high byte A
        v = raw.reshape(-1, 2)
        p = np.stack([v[:, 0], v[:, 0], v[:, 0], v[:, 1]], -1)
    else:
        return None
    p = p.astype(np.uint8)
    if swizzled:
        return unswizzle_xbox(p, w, h)
    return p.reshape(h, w, 4)


def decode_bump(m, b):
    """a bump map's mip 0 as an RGBA array of normals (x, y, z in red, green
    and blue, 0-255 for -1 to 1), or None. Halo's P8 bump maps (format 17)
    go through the engine's vector palette; other formats decode as usual"""
    if b['fmt'] != 17:
        return decode(m, b)
    from vector_palette import VECTOR_PALETTE
    w, h = b['w'], b['h']
    if b['type'] != '2d':
        return None
    raw = np.frombuffer(m.data[b['pixels_offset']:b['pixels_offset'] + w * h], np.uint8)
    if raw.size < w * h:
        return None
    pal = np.array(VECTOR_PALETTE, np.uint32)
    argb = pal[raw]
    p = np.stack([(argb >> 16) & 255, (argb >> 8) & 255, argb & 255, argb >> 24], -1).astype(np.uint8)
    if b['flags'] & 8:
        return unswizzle_xbox(p, w, h)
    return p.reshape(h, w, 4)


def decode(m, b):
    """mip 0 (face 0 of a cube map) as an RGBA array, or None"""
    fmt, w, h = b['fmt'], b['w'], b['h']
    buf = m.data[b['pixels_offset']:b['pixels_offset'] + b['pixels_size']]
    if b['type'] == '3d' or fmt in (17,) or BPP[fmt] == 0:
        return None
    if FORMATS[fmt].startswith('DXT'):
        return decode_dxt(buf, w, h, FORMATS[fmt])
    return decode_plain(buf, w, h, fmt, bool(b['flags'] & 8))


# ---------------------------------------------------------------- PSP sizes
def ge_level_bytes(w, h, bits):
    """one swizzled mip level: rows padded to 16 bytes, height to 8 rows"""
    row = max(16, (w * bits + 7) // 8)
    row = (row + 15) // 16 * 16
    return row * ((h + 7) // 8 * 8)


def ge_chain_bytes(w, h, bits, min_side=8, max_levels=8):
    tot, n = 0, 0
    while n < max_levels:
        tot += ge_level_bytes(w, h, bits)
        n += 1
        if w <= min_side and h <= min_side:
            break
        w, h = max(1, w // 2), max(1, h // 2)
    return tot, n


def swizzle_psp(data, width_bytes, height):
    """GE swizzle: the texture as 16-byte x 8-row blocks, each block's rows
    stored together, blocks left to right then top to bottom"""
    a = np.frombuffer(data, np.uint8).reshape(height, width_bytes)
    bx, by = width_bytes // 16, height // 8
    return a.reshape(by, 8, bx, 16).transpose(0, 2, 1, 3).tobytes()


def psnr(a, b):
    mse = np.mean((a.astype(np.float64) - b.astype(np.float64)) ** 2)
    return 99.0 if mse == 0 else 10 * math.log10(255.0 ** 2 / mse)


def kmeans(points, k, iterations, seed=0, tol=0.0):
    """(centres k x d, labels): Lloyd's k-means from a k-means++ start, numpy
    only, the same for a given seed on every machine. It stands where
    scikit-learn's KMeans(n_init=1, random_state=seed) stood: k-means++ picks
    each next centre with probability by its squared distance to the nearest
    already chosen, taking the best of 2 + log(k) candidates as scikit-learn
    does, then each round moves every centre to its points' mean (lloyd()).
    tol: see lloyd()"""
    rng = np.random.RandomState(seed)
    x = np.asarray(points, np.float64)
    n = len(x)
    xx = (x * x).sum(1)
    trials = 2 + int(math.log(k))
    centres = np.empty((k, x.shape[1]))
    centres[0] = x[rng.randint(n)]
    closest = ((x - centres[0]) ** 2).sum(1)
    for c in range(1, k):
        total = closest.sum()
        if total <= 0:
            centres[c:] = x[rng.randint(n, size=k - c)]
            break
        picks = np.searchsorted(np.cumsum(closest), rng.random_sample(trials) * total)
        picks = np.minimum(picks, n - 1)
        cand = x[picks]
        tried = np.minimum(closest[None, :], xx[None, :] - 2.0 * cand @ x.T + (cand * cand).sum(1)[:, None])
        best = int(np.argmin(tried.sum(1)))
        centres[c] = x[picks[best]]
        closest = tried[best]
    return lloyd(x, centres, iterations, tol)


def lloyd(x, centres, iterations, tol=0.0):
    """(centres, labels) after up to iterations rounds of Lloyd's k-means
    from these centres, stopping when no point changes centre. A centre
    left with no points takes the point farthest from its own centre, so
    every entry stays in use. tol > 0 also stops once every centre is in
    use and none moved by more than tol in a round (the fast palettes';
    0, the bake's own palettes, waits for the points to settle)"""
    k = len(centres)
    labels = nearest_centre(x, centres)
    for _ in range(iterations):
        counts = np.bincount(labels, minlength=k)
        sums = np.stack([np.bincount(labels, x[:, j], minlength=k) for j in range(x.shape[1])], 1)
        moved = centres.copy()
        used = counts > 0
        moved[used] = sums[used] / counts[used, None]
        if not used.all():
            far = np.argsort(((x - moved[labels]) ** 2).sum(1))[::-1]
            moved[~used] = x[far[:int((~used).sum())]]
        new_labels = nearest_centre(x, moved)
        settled = tol > 0 and used.all() and float(np.abs(moved - centres).max()) < tol
        centres = moved
        if np.array_equal(new_labels, labels):
            break
        labels = new_labels
        if settled:
            break
    return centres, labels


def best_kmeans(points, k, iterations, tries=None):
    """the centres of the best of several k-means runs (the least summed
    squared distance): a run's start decides much of where it ends, and with
    few centres one bad start can leave a texture's few strong colours
    (a muzzle flash's) without an entry of their own. Small palettes are
    cheap, so 16 colours take 8 runs; 256 take 2"""
    if tries is None:
        tries = 8 if k <= 16 else 2
    x = np.asarray(points, np.float64)
    best = None
    for seed in range(tries):
        centres, labels = kmeans(x, k, iterations, seed)
        cost = float(((x - centres[labels]) ** 2).sum())
        if best is None or cost < best[0]:
            best = (cost, centres)
    return best[1]


# the fast palettes (quantize(fast=True); texpack --fast-palettes): each
# k-means run settles on a seeded subsample of FAST_SAMPLE points, where a
# round costs a fifth as much, then takes at most FAST_REFINE rounds on the
# whole sample from there; both stop once no centre moves by FAST_TOL (a
# quarter or so of the palette's own rounding to whole levels)
FAST_SAMPLE = 8192
FAST_REFINE = 10
FAST_TOL = 0.5


def fast_kmeans(points, k, iterations, tries=None):
    """best_kmeans()'s palette, sooner: the same starts and the same number
    of tries, each settled on a subsample first, then refined on every
    point; the best by the summed squared distance over every point"""
    if tries is None:
        tries = 8 if k <= 16 else 2
    x = np.asarray(points, np.float64)
    head = x if len(x) <= FAST_SAMPLE else x[np.random.RandomState(1).choice(len(x), FAST_SAMPLE, replace=False)]
    best = None
    for seed in range(tries):
        centres, _ = kmeans(head, k, iterations, seed, FAST_TOL)
        centres, labels = lloyd(x, centres, FAST_REFINE, FAST_TOL)
        cost = float(((x - centres[labels]) ** 2).sum())
        if best is None or cost < best[0]:
            best = (cost, centres)
    return best[1]


def nearest_centre(points, centres, chunk=16384):
    """each point's nearest centre, in chunks: |c|^2 - 2 x.c (|x|^2 is the
    same for every centre, so left out), in 32-bit floats: colours and Oklab
    values are small enough that the order of the distances survives"""
    x = np.asarray(points, np.float32)
    c = np.asarray(centres, np.float32)
    cc = (c * c).sum(1)
    out = np.empty(len(x), np.int64)
    for at in range(0, len(x), chunk):
        d = x[at:at + chunk] @ (-2.0 * c.T)
        d += cc
        out[at:at + chunk] = np.argmin(d, 1)
    return out


ALPHA_SIDE = 128   # the GE's alpha test passes a texel whose alpha is above 0x7F


def split_counts(on, off, colors, cutout):
    """how many of the palette's entries each side of the alpha test gets:
    by its share of the texels, at least one each. A cut-out's hidden texels
    show only where the filter blends them into an edge, so they take at most
    a quarter of the entries"""
    if not off:
        return colors, 0
    if not on:
        return 0, colors
    k_off = int(round(colors * off / float(on + off)))
    k_off = max(1, min(k_off, colors // 4 if cutout else colors - 1))
    return colors - k_off, k_off


def quantize_split(img, colors, fast=False, cutout=False):
    """quantize() for a texture the GE alpha-tests (a cut-out): the texels
    each side of the test are fitted apart, on entries of their own. An
    entry's alpha is the mean of its texels', so no entry mixes texels the
    test passes with texels it hides: with RGBA clustered as one 4-D point,
    hidden texels whose colour is far from the rest and the shape's own
    could share an entry, and the shape lost its edge or gained a square"""
    h, w = img.shape[:2]
    pts = img.reshape(-1, 4).astype(np.float32)
    side = pts[:, 3] >= ALPHA_SIDE
    k_on, k_off = split_counts(int(side.sum()), int((~side).sum()), colors, cutout)
    pal = np.zeros((colors, 4), np.float64)
    idx = np.zeros(len(pts), np.int64)
    at = 0
    for mask, k in ((side, k_on), (~side, k_off)):
        if not k:
            continue
        group = pts[mask]
        uniq = np.unique(group, axis=0)
        if len(uniq) <= k:
            centres = uniq.astype(np.float64)
        else:
            sample = group if len(group) <= 40000 else group[np.random.RandomState(0).choice(len(group), 40000, replace=False)]
            centres = (fast_kmeans if fast else best_kmeans)(sample, k, 60)
        centres = np.clip(np.rint(centres), 0, 255)
        # rounding never crosses the test: a mean of values on one side stays there
        pal[at:at + len(centres)] = centres
        idx[mask] = at + nearest_centre(group, centres)
        at += len(centres)
    pal = pal.astype(np.uint8)
    idx = idx.reshape(h, w)
    return idx, pal, pal[idx], True


def quantize(img, colors, fast=False, split=False, cutout=False):
    """CLUT quantization; returns (indices HxW, palette Nx4 uint8, reconstructed
    RGBA, has_alpha).  Alpha is treated as data, not transparency: Halo's
    environment textures keep blend/specular masks in alpha and need the RGB
    under alpha 0, so RGBA is clustered as a 4-D point (kmeans() above,
    seeded by k-means++ on a sample).  Opaque textures cluster RGB only.
    fast: the palette by fast_kmeans() (texpack --fast-palettes).
    split: a texture whose alpha is a shape, by quantize_split() (cutout: one
    the GE alpha-tests)"""
    h, w = img.shape[:2]
    has_alpha = img[:, :, 3].min() < 255
    if split and has_alpha and img[:, :, 3].max() >= ALPHA_SIDE:
        return quantize_split(img, colors, fast, cutout)
    ch = 4 if has_alpha else 3
    pts = img.reshape(-1, 4)[:, :ch].astype(np.float32)
    uniq = np.unique(pts, axis=0)
    if len(uniq) <= colors:
        pal = uniq
        lut = {tuple(p): i for i, p in enumerate(uniq)}
        idx = np.array([lut[tuple(p)] for p in pts], np.int32) if len(pts) < 70000 else \
            np.argmin(((pts[:, None, :] - pal[None]) ** 2).sum(2), 1)
    else:
        sample = pts if len(pts) <= 40000 else pts[np.random.RandomState(0).choice(len(pts), 40000, replace=False)]
        # up to 60 rounds (scikit-learn's call stopped at 25): fewer palettes
        # end worse than its did (8 of 48 against 11 at 25, test textures)
        pal = (fast_kmeans if fast else best_kmeans)(sample, colors, 60)
        idx = nearest_centre(pts, pal)
    pal = np.clip(np.rint(pal), 0, 255).astype(np.uint8)
    if ch == 3:
        pal = np.concatenate([pal, np.full((len(pal), 1), 255, np.uint8)], 1)
    if len(pal) < colors:
        pal = np.concatenate([pal, np.zeros((colors - len(pal), 4), np.uint8)])
    idx = idx.reshape(h, w)
    return idx, pal, pal[idx], has_alpha


def downscale(img, tw, th):
    """box-filter each channel on its own: PIL premultiplies RGBA when it
    resizes, which would blacken the colour under alpha 0 (Halo keeps blend
    and specular masks in alpha, so that colour is visible)"""
    if img.shape[1] == tw and img.shape[0] == th:
        return img
    rgb = np.array(Image.fromarray(np.ascontiguousarray(img[:, :, :3]), 'RGB').resize((tw, th), Image.Resampling.BOX))
    a = np.array(Image.fromarray(np.ascontiguousarray(img[:, :, 3]), 'L').resize((tw, th), Image.Resampling.BOX))
    return np.concatenate([rgb, a[:, :, None]], 2)


def target(b, cls):
    """policy A: (action, width, height)"""
    w, h = b['w'], b['h']
    if cls == 'bump':
        return 'drop', 0, 0
    if cls == 'cube':
        return 'spheremap', 64, 64
    if cls == '3d':
        return 'keep3d', w, h
    if cls != 'lightmap' and max(w, h) >= 64:
        w, h = max(1, w // 2), max(1, h // 2)
    while max(w, h) > 512:
        w, h = max(1, w // 2), max(1, h // 2)
    return 'clut', w, h


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('map')
    ap.add_argument('--out', default=None)
    ap.add_argument('--previews', type=int, default=0)
    ap.add_argument('--limit', type=int, default=0)
    a = ap.parse_args()
    d = Defs()
    m = CacheMap(a.map)
    roles = refs.bitmap_roles(m, d)
    B = bitmaps(m, d)
    if a.limit:
        B = B[:a.limit]
    if a.out:
        os.makedirs(a.out, exist_ok=True)
    rows = []
    for i, b in enumerate(B):
        cls = refs.classify(roles.get(b['tag'], ()), b)
        act, tw, th = target(b, cls)
        row = dict(tag=b['tag'], idx=b['index'], cls=cls, fmt=b['format'], w=b['w'], h=b['h'], type=b['type'],
                   mips=b['mips'], orig=b['pixels_size'], action=act, tw=tw, th=th)
        if act == 'drop':
            row.update(clut8=0, clut4=0)
        elif act == 'spheremap':
            row.update(clut8=ge_chain_bytes(64, 64, 8)[0] + 512, clut4=ge_chain_bytes(64, 64, 4)[0] + 32)
        elif act == 'keep3d':
            row.update(clut8=b['w'] * b['h'] * b['depth'] + 512, clut4=b['w'] * b['h'] * b['depth'] // 2 + 32)
        else:
            img = decode(m, b)
            alpha = img is not None and img[:, :, 3].min() < 255
            palbytes8 = 1024 if alpha else 512
            palbytes4 = 64 if alpha else 32
            row['clut8'] = ge_chain_bytes(tw, th, 8)[0] + palbytes8
            row['clut4'] = ge_chain_bytes(tw, th, 4)[0] + palbytes4
            row['alpha'] = bool(alpha)
            if img is not None:
                small = downscale(img, tw, th)
                idx8, pal8, rec8, _ = quantize(small, 256)
                idx4, pal4, rec4, _ = quantize(small, 16)
                row['psnr8'] = round(psnr(small, rec8), 1)
                row['psnr4'] = round(psnr(small, rec4), 1)
                # the frame buffer's own 5650 quantization, for reference
                ref565 = small.copy()
                ref565[:, :, 0] &= 0xF8
                ref565[:, :, 1] &= 0xFC
                ref565[:, :, 2] &= 0xF8
                row['psnr565'] = round(psnr(small[:, :, :3], ref565[:, :, :3]), 1)
                if a.out:
                    # write the GE-ready level 0: swizzled CLUT8 + palette
                    wb = max(16, tw)
                    hb = (th + 7) // 8 * 8
                    pad = np.zeros((hb, wb), np.uint8)
                    pad[:th, :tw] = idx8
                    blob = swizzle_psp(pad.tobytes(), wb, hb)
                    name = '%04d' % i
                    open(os.path.join(a.out, name + '.clut8'), 'wb').write(pal8.tobytes() + blob)
                    if i < a.previews:
                        Image.fromarray(img, 'RGBA').save(os.path.join(a.out, name + '_xbox.png'))
                        Image.fromarray(rec8.astype(np.uint8), 'RGBA').save(os.path.join(a.out, name + '_psp8.png'))
                        Image.fromarray(rec4.astype(np.uint8), 'RGBA').save(os.path.join(a.out, name + '_psp4.png'))
        rows.append(row)
        if i % 50 == 0:
            print('  %d/%d' % (i, len(B)), file=sys.stderr)
    json.dump(rows, open('bitmap_bake_%s.json' % m.name, 'w'), indent=0)
    report(rows)


def report(rows):
    tot_o = sum(r['orig'] for r in rows)
    tot8 = sum(r['clut8'] for r in rows)
    tot4 = sum(r['clut4'] for r in rows)
    print('bitmaps %d: xbox %d B -> policy A CLUT8 %d B (%.1f%%), all-CLUT4 %d B (%.1f%%)' % (
        len(rows), tot_o, tot8, 100.0 * tot8 / tot_o, tot4, 100.0 * tot4 / tot_o))
    by = collections.defaultdict(lambda: collections.Counter())
    ps = collections.defaultdict(list)
    for r in rows:
        c = by[r['cls']]
        c['n'] += 1
        c['orig'] += r['orig']
        c['clut8'] += r['clut8']
        c['clut4'] += r['clut4']
        if 'psnr8' in r:
            ps[r['cls']].append((r['psnr8'], r['psnr4'], r['psnr565']))
    print('%-20s %4s %10s %10s %10s %7s %7s %7s' % ('role', 'n', 'xbox B', 'CLUT8 B', 'CLUT4 B', 'dB c8', 'dB c4', 'dB 565'))
    for k, c in sorted(by.items(), key=lambda x: -x[1]['orig']):
        p = ps.get(k)
        med = lambda j: sorted(x[j] for x in p)[len(p) // 2] if p else float('nan')
        print('%-20s %4d %10d %10d %10d %7.1f %7.1f %7.1f' % (k, c['n'], c['orig'], c['clut8'], c['clut4'], med(0), med(1), med(2)))


if __name__ == '__main__':
    main()
