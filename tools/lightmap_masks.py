"""lightmap_masks.py - which texels of each lightmap page the level uses
(build 102, the asset track's smaller lightmaps).

A lightmap page holds many charts (patches of the level unwrapped flat) with
dark gutters between them. Shrunk as it is, each chart's edge texels average
in the gutter and the level's seams go dark (b47's dark lines, when the
pages had mip levels). So, before shrinking, each page's charts are found by
drawing the level's own lightmap texture coordinates into a mask, and the
colours are grown outward from the charts into the gutters; then the gutters
hold the charts' own edge colours and a box filter mixes nothing dark in.

A chart only a few texels across can't keep its gradient at half size: it
comes out one flat colour, and where it meets its neighbour chart along an
edge of the level the two no longer agree, a hard step the full-size page
doesn't have (b30's bunker doorway). page_seams finds those edges, so
texpack.py can measure what the halving does to them (seam_steps), and
chart_halve refits the half-size texels of the charts it breaks
(--lm-chart-halve; keeping such pages whole, --lightmap-seam-whole, kept too
many).

usage (library):
    masks = page_masks(map_path, pgeo_path, sizes)  # {(bitmap tag name, page): bool HxW}
    img = dilate(img, mask, steps)                  # RGBA uint8, grown outward
    seams = page_seams(map_path, pgeo_path, sizes)  # {(bitmap tag name, page): spots (n, 2, 2)}
    steps = seam_steps(img, seams[page])            # 0-255 a spot, as the GE draws img"""
import struct, sys, os
import numpy as np

sys.path.insert(0, os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), 'bake', 'lib'))  # the bake's readers (halomap.py, ...)
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from halomap import Defs, CacheMap

PACK_BASE = 0x0C000000
HEADER = 64


def fo(d, s):
    return {f.get('name'): x for x, f, sz in d.fields(s)}


def _raster(mask, uv, tris):
    """marks every texel whose centre a triangle covers (and the texels its
    corners fall in: a sliver thinner than a texel still owns its texels)"""
    h, w = mask.shape
    p = uv[tris]                                   # (n, 3, 2) in [0, 1]
    px = p[:, :, 0] * w - 0.5
    py = p[:, :, 1] * h - 0.5
    for k in range(3):
        xi = np.clip(np.round(px[:, k]).astype(int), 0, w - 1)
        yi = np.clip(np.round(py[:, k]).astype(int), 0, h - 1)
        mask[yi, xi] = True
    x0 = np.clip(np.floor(px.min(1)).astype(int), 0, w - 1)
    x1 = np.clip(np.ceil(px.max(1)).astype(int), 0, w - 1)
    y0 = np.clip(np.floor(py.min(1)).astype(int), 0, h - 1)
    y1 = np.clip(np.ceil(py.max(1)).astype(int), 0, h - 1)
    for t in range(len(tris)):
        if x1[t] < x0[t] or y1[t] < y0[t]:
            continue
        xs = np.arange(x0[t], x1[t] + 1)
        ys = np.arange(y0[t], y1[t] + 1)
        gx, gy = np.meshgrid(xs, ys)
        (ax, ay), (bx, by), (cx, cy) = zip(px[t], py[t])
        d = (by - cy) * (ax - cx) + (cx - bx) * (ay - cy)
        if abs(d) < 1e-12:
            continue
        l1 = ((by - cy) * (gx - cx) + (cx - bx) * (gy - cy)) / d
        l2 = ((cy - ay) * (gx - cx) + (ax - cx) * (gy - cy)) / d
        l3 = 1 - l1 - l2
        e = -1e-4
        inside = (l1 >= e) & (l2 >= e) & (l3 >= e)
        mask[gy[inside], gx[inside]] = True


def _page_materials(map_path, pgeo_path, sizes):
    """yields (tag name, page), its lightmap coordinates (n, 2) as the map's
    16-bit numbers, the material's triangles (m, 3) and its vertices' positions
    (n, 3) (None when the pack has no rendered vertices for it), for every
    material lit by one of the pages in sizes"""
    d = Defs()
    m = CacheMap(map_path)
    pack = open(pgeo_path, 'rb').read() if pgeo_path and os.path.exists(pgeo_path) else b''
    # a slim pack's rendered vertices are 24 bytes (levelslim.py); the lightmap's follow them
    stride = 24 if len(pack) >= HEADER and struct.unpack_from('<I', pack, 40)[0] & 0x10 else 32
    scn = m.tags[m.scenario_id & 0xffff]
    sf = fo(d, 'Scenario')
    bf = fo(d, 'ScenarioStructureBSP')
    lmf = fo(d, 'ScenarioStructureBSPLightmap')
    mf = fo(d, 'ScenarioStructureBSPMaterial')
    n, p = struct.unpack_from('<iI', m.data, m.off(scn.address) + sf['structure bsps'])
    for which in range(n):
        start, size, addr = struct.unpack_from('<III', m.data, m.off(p) + 32 * which)
        tr = lambda a: start + (a - addr)
        base, = struct.unpack_from('<I', m.data, start)
        so = tr(base)
        lm_tag_id, = struct.unpack_from('<i', m.data, so + bf['lightmaps bitmap'] + 12)
        lm_tag = m.by_id.get(lm_tag_id)
        if not lm_tag:
            continue
        sn, sp = struct.unpack_from('<iI', m.data, so + bf['surfaces'])
        surfaces = np.frombuffer(m.data, '<u2', sn * 3, tr(sp)).reshape(sn, 3)
        ln, lp = struct.unpack_from('<iI', m.data, so + bf['lightmaps'])
        for li in range(ln):
            lo = tr(lp) + 32 * li
            page, = struct.unpack_from('<h', m.data, lo)
            if page < 0 or (lm_tag.name, page) not in sizes:
                continue
            mn, mp = struct.unpack_from('<iI', m.data, lo + lmf['materials'])
            for mi in range(mn):
                mo = tr(mp) + 256 * mi
                first, count = struct.unpack_from('<ii', m.data, mo + mf['surfaces'])
                rcount, = struct.unpack_from('<I', m.data, mo + mf['rendered vertices count'])
                lcount, = struct.unpack_from('<I', m.data, mo + mf['lightmap vertices count'])
                if not lcount:
                    continue
                ip, = struct.unpack_from('<I', m.data, mo + mf['rendered vertices index pointer'])
                key, = struct.unpack_from('<I', m.data, tr(ip) + 4)
                positions = None
                if key >= PACK_BASE:
                    if not pack:
                        continue
                    at = HEADER + key - PACK_BASE
                    raw = pack[at + rcount * stride: at + rcount * stride + lcount * 8]
                    if rcount >= lcount:
                        # each rendered vertex (32 bytes, or 24 slim) starts with its position
                        rendered = np.frombuffer(pack[at: at + rcount * stride], np.uint8).reshape(rcount, stride)
                        positions = rendered[:lcount, :12].copy().view('<f4').reshape(lcount, 3).astype(np.float64)
                else:
                    raw = m.data[tr(key) + rcount * 32: tr(key) + rcount * 32 + lcount * 8]
                lm = np.frombuffer(raw, np.uint8).reshape(lcount, 8)
                shorts = lm[:, 4:8].copy().view('<i2').reshape(lcount, 2).astype(np.float64)
                tris = surfaces[first:first + count].astype(np.int64)
                tris = tris[(tris < lcount).all(1)]
                yield (lm_tag.name, page), shorts, tris, positions


def page_masks(map_path, pgeo_path, sizes):
    """sizes: {(tag name, page): (w, h)} of the lightmap pages to mask.
    Returns {(tag name, page): bool array (h, w)}"""
    masks = {}
    for page, shorts, tris, _ in _page_materials(map_path, pgeo_path, sizes):
        w, h = sizes[page]
        _raster(masks.setdefault(page, np.zeros((h, w), bool)), shorts / 32767.0, tris)
    return masks


SEAM_SPOTS = (0.2, 0.5, 0.8)


def page_seams(map_path, pgeo_path, sizes):
    """The lightmap seams of each page: the level's edges whose two
    triangles (any materials on that page) read the page at different
    places, a chart's edge against its neighbour's. Returns {(tag name,
    page): array (n, 2, 2)}: n spots along those edges, each as the two
    sides' lightmap coordinates there (SEAM_SPOTS of the way along)"""
    edges = {}
    for page, shorts, tris, positions in _page_materials(map_path, pgeo_path, sizes):
        if positions is None:
            continue
        st = (shorts * 2.0 + 1.0) / 65535.0    # as the engine decodes them
        grid = np.round(positions * 1000.0).astype(np.int64)    # a millimetre: one corner, whichever material
        book = edges.setdefault(page, {})
        for t in tris:
            for a, b in ((t[0], t[1]), (t[1], t[2]), (t[2], t[0])):
                ka, kb = tuple(grid[a]), tuple(grid[b])
                if ka == kb:
                    continue
                if kb < ka:
                    a, b, ka, kb = b, a, kb, ka
                book.setdefault((ka, kb), []).append((st[a], st[b]))
    seams = {}
    for page, book in edges.items():
        w, h = sizes[page]
        texel = np.array([w, h], np.float64)
        spots = []
        for sides in book.values():
            if len(sides) < 2:
                continue
            a0, b0 = sides[0]
            for a1, b1 in sides[1:]:
                # the same chart on both sides (an ordinary inner edge): no seam
                if max(np.abs((a1 - a0) * texel).max(), np.abs((b1 - b0) * texel).max()) < 0.5:
                    continue
                for f in SEAM_SPOTS:
                    spots.append((a0 + (b0 - a0) * f, a1 + (b1 - a1) * f))
        if spots:
            seams[page] = np.array(spots, np.float64)
    return seams


def sample(img, uv):
    """img (h, w, c) at the coordinates uv (n, 2) as the GE filters a
    clamped texture: bilinear, texel centres at (i + 0.5) / size"""
    h, w = img.shape[:2]
    x = uv[:, 0] * w - 0.5
    y = uv[:, 1] * h - 0.5
    x0 = np.floor(x).astype(np.int64)
    y0 = np.floor(y).astype(np.int64)
    fx = (x - x0)[:, None]
    fy = (y - y0)[:, None]
    def at(yy, xx):
        return img[np.clip(yy, 0, h - 1), np.clip(xx, 0, w - 1)].astype(np.float64)
    top = at(y0, x0) * (1 - fx) + at(y0, x0 + 1) * fx
    bottom = at(y0 + 1, x0) * (1 - fx) + at(y0 + 1, x0 + 1) * fx
    return top * (1 - fy) + bottom * fy


def seam_steps(img, seams):
    """the largest colour difference (0-255, any channel) across each seam
    spot when img is drawn"""
    return np.abs(sample(img[:, :, :3], seams[:, 0]) - sample(img[:, :, :3], seams[:, 1])).max(1)


def dilate(img, mask, steps=8):
    """the colours of the masked texels grown outward, steps texels"""
    out = img.astype(np.float32).copy()
    have = mask.copy()
    h, w = mask.shape
    for _ in range(steps):
        if have.all():
            break
        acc = np.zeros_like(out)
        cnt = np.zeros((h, w), np.float32)
        for dy, dx in ((-1, 0), (1, 0), (0, -1), (0, 1), (-1, -1), (-1, 1), (1, -1), (1, 1)):
            src = np.roll(np.roll(out, dy, 0), dx, 1)
            ok = np.roll(np.roll(have, dy, 0), dx, 1)
            # no wrapping across the page's edges
            if dy == -1: ok[-1, :] = False
            if dy == 1: ok[0, :] = False
            if dx == -1: ok[:, -1] = False
            if dx == 1: ok[:, 0] = False
            acc += src * ok[:, :, None]
            cnt += ok
        grow = (~have) & (cnt > 0)
        out[grow] = acc[grow] / cnt[grow][:, None]
        have |= grow
    return np.clip(out + 0.5, 0, 255).astype(np.uint8)


def _components(labels_edges, count):
    """connected components of count nodes joined by the (a, b) pairs: a label each"""
    lab = np.arange(count)
    a, b = labels_edges[:, 0], labels_edges[:, 1]
    while True:
        lo = np.minimum(lab[a], lab[b])
        new = lab.copy()
        np.minimum.at(new, a, lo)
        np.minimum.at(new, b, lo)
        new = new[new]
        if (new == lab).all():
            return np.unique(lab, return_inverse=True)[1]
        lab = new


def page_charts(map_path, pgeo_path, sizes):
    """Each page's charts and the seams between them, for chart-aware
    halving (texpack.py --lm-chart-halve). A chart is the triangles joined
    through shared lightmap corners (any material on the page). Returns
    {(tag name, page): dict(st=(T, 3, 2) coordinates as the engine decodes
    them, chart=(T,) a chart each, seams=(n, 2, 2) as page_seams,
    seam_chart=(n, 2) the chart of each side)}"""
    acc = {}
    for page, shorts, tris, positions in _page_materials(map_path, pgeo_path, sizes):
        if not len(tris):
            continue
        acc.setdefault(page, []).append((shorts[tris], None if positions is None else positions[tris]))
    out = {}
    for page, parts in acc.items():
        w, h = sizes[page]
        raw = np.concatenate([s for s, _ in parts]).astype(np.int64)        # (T, 3, 2)
        _, vid = np.unique(raw.reshape(-1, 2), axis=0, return_inverse=True)
        vid = vid.reshape(-1, 3)
        pairs = np.concatenate([vid[:, [0, 1]], vid[:, [1, 2]]])
        chart = _components(pairs, int(vid.max()) + 1)[vid[:, 0]]
        st = (raw * 2.0 + 1.0) / 65535.0
        # the seams, as page_seams finds them, with each side's chart
        texel = np.array([w, h], np.float64)
        book = {}
        at = 0
        for s, pos in parts:
            k = len(s)
            if pos is not None:
                grid = np.round(pos * 1000.0).astype(np.int64)
                for t in range(k):
                    for i, j in ((0, 1), (1, 2), (2, 0)):
                        ka, kb = tuple(grid[t, i]), tuple(grid[t, j])
                        if ka == kb:
                            continue
                        a, b = st[at + t, i], st[at + t, j]
                        if kb < ka:
                            a, b, ka, kb = b, a, kb, ka
                        book.setdefault((ka, kb), []).append((a, b, chart[at + t]))
            at += k
        spots, sides = [], []
        for edge in book.values():
            if len(edge) < 2:
                continue
            a0, b0, c0 = edge[0]
            for a1, b1, c1 in edge[1:]:
                if max(np.abs((a1 - a0) * texel).max(), np.abs((b1 - b0) * texel).max()) < 0.5:
                    continue
                for f in SEAM_SPOTS:
                    spots.append((a0 + (b0 - a0) * f, a1 + (b1 - a1) * f))
                    sides.append((c0, c1))
        out[page] = dict(st=st, chart=chart, seams=np.array(spots, np.float64).reshape(-1, 2, 2),
                         seam_chart=np.array(sides, np.int64).reshape(-1, 2))
    return out


CHART_EDGE_WEIGHT = 4.0     # a chart's edge samples count this much: its seams
CHART_PULL = 0.1            # each free texel's pull toward its plain halving
CHART_SIZES_LOGGED = (4, 8, 16, 32)  # the log counts blamed charts at most this many texels across
CHART_GROUP_MAX = 1024      # a fit of more texels than this is left as the plain halving (a dense solve)


def _samples(tri, w, h):
    """points on the triangles tri (k, 3, 2) where the GE reads the page:
    every half texel inside, and each corner and edge (weighted): (n, 2), (n,)"""
    pts, wts = [], []
    for t in tri:
        p = t * np.array([w, h], np.float64)
        x0, y0 = np.floor(p.min(0) * 2.0) / 2.0 + 0.25
        x1, y1 = p.max(0)
        gx, gy = np.meshgrid(np.arange(x0, x1, 0.5), np.arange(y0, y1, 0.5))
        gx, gy = gx.ravel(), gy.ravel()
        (ax, ay), (bx, by), (cx, cy) = p
        d = (by - cy) * (ax - cx) + (cx - bx) * (ay - cy)
        if abs(d) > 1e-12 and len(gx):
            l1 = ((by - cy) * (gx - cx) + (cx - bx) * (gy - cy)) / d
            l2 = ((cy - ay) * (gx - cx) + (ax - cx) * (gy - cy)) / d
            inside = (l1 >= -1e-6) & (l2 >= -1e-6) & (1 - l1 - l2 >= -1e-6)
            pts.append(np.stack([gx[inside], gy[inside]], 1))
            wts.append(np.ones(int(inside.sum())))
        for i in range(3):
            a, b = p[i], p[(i + 1) % 3]
            e = np.array([a + (b - a) * f for f in (0.0, 0.25, 0.5, 0.75)])
            pts.append(e)
            wts.append(np.full(len(e), CHART_EDGE_WEIGHT))
    pts = np.concatenate(pts) / np.array([w, h], np.float64)
    return pts, np.concatenate(wts)


def _taps(uv, w, h):
    """the four texels (n, 4) of a w x h page that bilinear filtering reads at
    uv, and their weights (n, 4), clamped at the page's edges as the GE does"""
    x = uv[:, 0] * w - 0.5
    y = uv[:, 1] * h - 0.5
    x0 = np.floor(x).astype(np.int64)
    y0 = np.floor(y).astype(np.int64)
    fx, fy = x - x0, y - y0
    idx, wt = [], []
    for dy, dx, ww in ((0, 0, (1 - fx) * (1 - fy)), (0, 1, fx * (1 - fy)), (1, 0, (1 - fx) * fy), (1, 1, fx * fy)):
        idx.append(np.clip(y0 + dy, 0, h - 1) * w + np.clip(x0 + dx, 0, w - 1))
        wt.append(ww)
    return np.stack(idx, 1), np.stack(wt, 1)


def chart_halve(full, half, info, step, spots, chart_max=0):
    """Chart-aware halving. half is the page as plain halving makes it (the
    box over the charts grown into the gutters); full the page at its own
    size. A chart a few texels across comes out of the box one flat colour,
    its edge no longer meeting its neighbour's across a seam (b30's bunker
    doorway). For each chart whose halving widens a seam spot's step by more
    than step, at least spots times (counted on the side that moved), the
    half-size texels its reads touch are refitted on that chart's own
    reads: chosen so the half-size page, filtered as the GE filters it, reads
    as the full page does across the chart, its edges weighted (clamped to
    the full page's edge colours, nothing averaged in from across the
    boundary). Only charts whose longer side is chart_max texels or fewer
    (0: any). Any other chart reading those texels is in the fit too, so
    it keeps its look. Every other texel is the plain halving's.
    Returns (page, changed (h, w) bool or None, stats)"""
    h, w = full.shape[:2]
    h2, w2 = half.shape[:2]
    seams, sides = info['seams'], info['seam_chart']
    stats = dict(charts=0, texels=0, before=0.0, after=0.0, skipped=0, sizes=[0] * (len(CHART_SIZES_LOGGED) + 1))
    if not len(seams):
        return half, None, stats
    f3 = full[:, :, :3]
    worse = seam_steps(half, seams) - seam_steps(full, seams)
    stats['before'] = float(worse.max())
    broken = np.nonzero(worse > step)[0]
    if not len(broken):
        return half, None, stats
    moved = np.stack([np.abs(sample(half[:, :, :3], seams[broken, k]) - sample(f3, seams[broken, k])).max(1)
                      for k in range(2)], 1)
    blame = sides[broken, moved.argmax(1)]
    st, chart = info['st'], info['chart']
    n = int(chart.max()) + 1
    counts = np.bincount(blame, minlength=n)
    # each chart's extent in full-size texels (its longer side): the box
    # flattens only charts a few texels across; larger ones keep their
    # gradient at half size, and their seams' steps are halving's ordinary blur
    px = st * np.array([w, h], np.float64)
    lo = np.full((n, 2), np.inf)
    hi = np.full((n, 2), -np.inf)
    np.minimum.at(lo, chart, px.min(1))
    np.maximum.at(hi, chart, px.max(1))
    side = (hi - lo).max(1)
    blamed = np.nonzero(counts >= spots)[0]
    stats['sizes'] = [int((side[blamed] <= t).sum()) for t in CHART_SIZES_LOGGED] + [len(blamed)]
    treated = blamed[side[blamed] <= chart_max] if chart_max else blamed
    if not len(treated):
        return half, None, stats
    free = np.zeros(h2 * w2, bool)
    for c in treated:
        uv, _ = _samples(st[chart == c], w, h)
        idx, wt = _taps(uv, w2, h2)
        free[idx[wt > 1e-6]] = True
    # free texels in groups (8-connected): each group is one fit
    fidx = np.nonzero(free)[0]
    fy, fx = fidx // w2, fidx % w2
    pos = {int(i): k for k, i in enumerate(fidx)}
    pairs = [(k, pos[int(y2 * w2 + x2)]) for k, (y, x) in enumerate(zip(fy, fx))
             for y2, x2 in ((y, x + 1), (y + 1, x - 1), (y + 1, x), (y + 1, x + 1))
             if 0 <= x2 < w2 and y2 < h2 and free[y2 * w2 + x2]]
    group = _components(np.array(pairs, np.int64).reshape(-1, 2), len(fidx)) if pairs else np.arange(len(fidx))
    tri_px = st * np.array([w2, h2], np.float64)
    tlo, thi = tri_px.min(1) - 1.5, tri_px.max(1) + 1.5
    flat = half.reshape(-1, half.shape[2]).astype(np.float64)
    out = flat.copy()
    for g in range(int(group.max()) + 1):
        mine = fidx[group == g]
        if len(mine) > CHART_GROUP_MAX:
            stats['skipped'] += 1
            continue
        gx, gy = mine % w2, mine // w2
        near = (thi[:, 0] >= gx.min()) & (tlo[:, 0] <= gx.max() + 1) & (thi[:, 1] >= gy.min()) & (tlo[:, 1] <= gy.max() + 1)
        uv, sw = _samples(st[near], w, h)
        idx, wt = _taps(uv, w2, h2)
        col = np.full(h2 * w2, -1, np.int64)
        col[mine] = np.arange(len(mine))
        use = (col[idx] >= 0).any(1)
        uv, sw, idx, wt = uv[use], sw[use], idx[use], wt[use]
        target = sample(f3, uv)
        a = np.zeros((len(uv), len(mine)))
        rhs = target.copy()
        for k in range(4):
            c = col[idx[:, k]]
            fr = c >= 0
            np.add.at(a, (np.nonzero(fr)[0], c[fr]), wt[fr, k])
            rhs[~fr] -= wt[~fr, k][:, None] * flat[idx[~fr, k], :3]
        aw = a * sw[:, None]
        lhs = aw.T @ a + CHART_PULL * np.eye(len(mine))
        sol = np.linalg.solve(lhs, aw.T @ rhs + CHART_PULL * flat[mine, :3])
        out[mine, :3] = sol
    page = np.clip(out + 0.5, 0, 255).astype(np.uint8).reshape(half.shape)
    page[:, :, 3:] = half[:, :, 3:]
    changed = (page != half).any(2)
    stats.update(charts=len(treated), texels=int(changed.sum()),
                 after=float((seam_steps(page, seams) - seam_steps(full, seams)).max()))
    return page, changed, stats
