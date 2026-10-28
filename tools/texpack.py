"""texpack.py - bakes a map's bitmaps into a PSP texture pack (.ptex).

The first slice of the asset baker (build 20): every 2D bitmap with power-of-
two sides becomes a swizzled CLUT8 texture with a mip chain, at the
resolution policy of research/baker/bitmap_bake.py (top mip dropped for
textures of 64 px and more, 512 px at most; lightmaps at full size). The
game finds a texture by the address of its bitmap_data in the tag window,
which is fixed for a map built for the PSP (invader-build -X 0x09600000).

usage: python3 texpack.py map.map out.ptex [--limit N] [--tints-from-means] [--no-glow-maps]
       [--premultiplied-masks] [--showcase [--showcase-32 | --showcase-palettes]]
       [--quality 1|2] [--quality-report]

File layout (little endian):
  header: 'PTX1', u32 count, u32 directory size
  directory, sorted by key: count x { u32 key (bitmap_data address),
    u16 width, u16 height, u8 levels, u8 psm (5 = T8, 4 = T4; in its own colours,
    --showcase: 3 = 8888, 0 = 5650), u8 clut (0 = 5650, 3 = 8888; 0xFF none),
    u8 flags (1 = has alpha; 2 = an exact CLUT4: 8-bit texels in 16
    colours, its average sampled as its CLUT8 layout's; 8 = the change
    mask's CLUT, 16 or 256 x u32, is the data's last bytes; 16 = the CPU reads
    its palette, so it's kept in main RAM), u32 offset, u32 size }
  tints, right after the directory: 'TNT1', u32 count, then count x
    { u32 shader tag address, u32 tint (ABGR) }, sorted by address: the
    see-through shaders' tints for the one map the GE draws of each
    (seethrough.py). Bit 0 of the address set: the tint is half the colour,
    drawn with the GE's colour doubling; bit 1 alone: the tint is added to
    the map, not multiplied; both bits: the tint alone is drawn, the map
    giving only its alpha (seethrough.py's models). The textures' data is
    read by offset, so a build that doesn't know a table never looks at it
  drawn maps, right after the tints: 'TND1', u32 count, then count x
    { u32 shader tag address (bits 0 and 1 as above, for the third word),
    u32 the map the GE draws (its index in the shader's maps), u32 the tint
    for the map the runtime's own rule would draw (keep word drawnmapscale) },
    sorted by address: only the shaders whose drawn map isn't the rule's.
    Only when there are any, after a 'TNT1' (of count 0 when no shader is
    tinted)
  tints by angle, right after the drawn maps: 'TNA1', u32 count, then count x
    { u32 shader tag address (bits 0 and 1 as its 'TNT1' entry), u32 the tint
    face on (ABGR), u32 the tint edge on }, sorted by address: the generic
    shaders whose combiners read the angle the surface is seen at (Cortana),
    blended per vertex by that angle. Only when there are any, after a 'TND1'
    (of count 0 when no drawn map is chosen)
  composites, right after the tints by angle: 'TNC1', u32 count, then count x
    { u32 shader tag address, u32 the map the composite is drawn in place of
    (its index in the shader's maps) }, sorted by address: the see-through
    shaders drawn from a texture of their own (seethrough.composite), whose
    directory entries are keyed by the shader's tag address | 1. Only when
    there are any, after a 'TNA1' (of count 0 when no shader is angled)
  averages, right after the composites: 'TAV1', u32 count (the textures'),
    then count x u32: each texture's average colour (ABGR; 0 for one in its
    own colours), in directory order, as psp_tex_average used to sample it.
    Since bake format 2 every table above is written, empty or not
  glow maps: directory entries keyed by a self-illuminated model shader's
    tag address | 2 (glow_looks), beside the bitmaps' keys; entries with the
    same texels share their data
  data: per texture, 16-byte aligned: the palette (256 x 2 or 4 bytes), then
    each level swizzled, rows padded to 16 bytes and to 8 rows; then a
    multipurpose map's change mask (flag 8), 64-byte aligned
"""
import sys, os, struct, argparse, hashlib, io, time
import numpy as np
sys.path.insert(0, os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), 'bake', 'lib'))  # the bake's readers (halomap.py, ...)
from halomap import Defs, CacheMap
from assets import fo
import refs
import bitmap_bake
from bitmap_bake import decode, decode_bump, downscale, quantize, kmeans, swizzle_psp, target, psnr, decode_dxt, decode_plain, BPP
from assets import FORMATS, TYPES, BTYPES, USAGES
import seethrough


def bitmap_list(m, d):
    bf = fo(d, 'Bitmap')
    out = []
    for t in m.tags:
        if t.group != 'bitm' or not t.address:
            continue
        o = m.off(t.address)
        btype, bfmt, usage, bflags = struct.unpack_from('<hhhH', m.data, o)
        n, p = struct.unpack_from('<iI', m.data, o + bf['bitmap data'])
        for i in range(n):
            address = p + 48 * i
            bo = m.off(address)
            (cls, w, h, depth, typ, fmt, flags, rx, ry, mips, _, poff, psize) = \
                struct.unpack_from('<IhhhhhHhhhhIi', m.data, bo)
            out.append(dict(tag=t.name, index=i, address=address,
                            group_type=BTYPES[btype] if btype < 5 else btype,
                            usage=USAGES[usage] if usage < 6 else usage,
                            w=w, h=h, depth=depth, type=TYPES[typ] if typ < 4 else typ,
                            format=FORMATS[fmt] if fmt < len(FORMATS) else '?', fmt=fmt,
                            flags=flags, mips=mips, pixels_offset=poff, pixels_size=psize))
    return out


def pow2(v):
    return v > 0 and (v & (v - 1)) == 0


def level_bytes(w, h, bits=8):
    """a swizzled level: rows of at least 16 bytes, padded to 16; 8-row blocks"""
    row = max(16, (w * bits + 7) // 8)
    row = (row + 15) // 16 * 16
    return row, (h + 7) // 8 * 8


def nearest(img, pal, alpha, split=False):
    """each texel's nearest palette entry. split (a palette from
    quantize_split): only among the entries on the texel's own side of the
    alpha test, so a smaller level keeps the shape the top level's palette
    was made for"""
    ch = 4 if alpha else 3
    pts = img.reshape(-1, 4)[:, :ch].astype(np.int32)
    p = pal[:, :ch].astype(np.int32)
    best = np.zeros(len(pts), np.int64)
    bestd = np.full(len(pts), 1 << 60, np.int64)
    sides = None
    if split and alpha:
        pal_on = p[:, 3] >= bitmap_bake.ALPHA_SIDE
        if pal_on.any() and (~pal_on).any():
            sides = (pts[:, 3] >= bitmap_bake.ALPHA_SIDE)[:, None]
    for start in range(0, len(p), 64):
        d = ((pts[:, None, :] - p[None, start:start + 64, :]) ** 2).sum(2)
        if sides is not None:
            d = np.where(sides == pal_on[None, start:start + 64], d, 1 << 50)
        j = d.argmin(1)
        dj = d[np.arange(len(pts)), j]
        better = dj < bestd
        best[better] = j[better] + start
        bestd[better] = dj[better]
    return best.reshape(img.shape[:2])


def detail_colour(m, d, B_by_tag, tag_id):
    """a detail map's average colour, 0-1 (a tiled detail map averages out at
    the distances the PSP's 272 lines resolve it)"""
    t = m.by_id.get(tag_id)
    if not t or t.group != 'bitm':
        return None
    for b in B_by_tag.get(t.name, ())[:1]:
        img = decode(m, b)
        if img is not None:
            return img[:, :, :3].reshape(-1, 3).mean(0) / 255.0
    return None


def level_looks(m, d, B):
    """senv shaders: base bitmap address -> how to bake its look. Halo's level
    shaders blend two tiled detail maps by the base map's alpha and combine
    them with the base map (double/biased multiply, multiply or double/biased
    add); the GE draws one texture at a time, so the base map is baked with
    the detail maps' average colours folded in (build 20)"""
    by_tag = {}
    for b in B:
        by_tag.setdefault(b['tag'], []).append(b)
    looks = {}
    for t in m.tags:
        if t.group != 'senv' or not t.address:
            continue
        o = m.off(t.address)
        flags, = struct.unpack_from('<H', m.data, o + 40)
        function, = struct.unpack_from('<h', m.data, o + 176)
        base_id, = struct.unpack_from('<i', m.data, o + 148)
        primary_id, = struct.unpack_from('<i', m.data, o + 196)
        secondary_id, = struct.unpack_from('<i', m.data, o + 216)
        micro_function, = struct.unpack_from('<h', m.data, o + 244)
        micro_id, = struct.unpack_from('<i', m.data, o + 264)
        base = m.by_id.get(base_id)
        if not base or base.name not in by_tag:
            continue
        primary = detail_colour(m, d, by_tag, primary_id)
        secondary = detail_colour(m, d, by_tag, secondary_id)
        micro = detail_colour(m, d, by_tag, micro_id)
        key = by_tag[base.name][0]['address']
        look = looks.setdefault(key, dict(function=function, primary=primary, secondary=secondary,
                                          micro_function=micro_function, micro=micro,
                                          alpha_tested=bool(flags & 1), shader=t.name))
        # the map keeps its alpha when any shader that draws it tests it (a30's
        # tree line: drawn first by a shader that doesn't, it lost its cut-outs
        # for the one that does); a shader that doesn't test it ignores it
        if flags & 1 and LOOK_ALPHA_ANY:
            look['alpha_tested'] = True
        # the Xbox alpha-tests a level shader on its bump map's alpha, not the
        # base map's (rasterizer_xbox_environment.c: stage 0 is the bump map
        # unless it is the specular mask); the GE tests the base map, so the
        # bump map's alpha goes into it
        bump_id, = struct.unpack_from('<i', m.data, o + 308)
        bump = m.by_id.get(bump_id)
        if flags & 1 and not flags & 2 and CUTOUT_FROM_BUMP and bump and bump.name in by_tag and 'cutout' not in look:
            bump_scale, = struct.unpack_from('<f', m.data, o + 292)
            su, sv = struct.unpack_from('<2f', m.data, o + 312)
            fallback = bump_scale if bump_scale else 1.0
            look['cutout'] = dict(bump=by_tag[bump.name][0], u=su if su else fallback, v=sv if sv else fallback)
    return looks


# --no-cutout-bump: alpha-tested level shaders cut out by their base map's
# alpha (as before), not their bump map's
CUTOUT_FROM_BUMP = True
CUTOUTS = []   # (base map, bump map) whose alpha came from the bump map
# a cut-out whose bump map is finer than the baked base map keeps up to 2
# bump texels a baked texel (to 512 a side): halved to 256 x 64, a10's
# grating repeats every 3.2 texels, and the GE's filtered alpha test turns
# its wires into rows of blobs; at 512 x 128 (6.4 texels) the wire holds.
# --no-cutout-fine: the usual size, as before
CUTOUT_FINE = True
CUTOUT_FINE_MOST = 2.0
CUTOUTS_ENLARGED = []   # (base map, baked size before, after)


def cutout_alpha(img, cutout, m):
    """the alpha the Xbox tests: the bump map's, at each of the base's texels
    on the bump map tiled by its scale (nearest), or None"""
    bump = decode_bump(m, cutout['bump'])
    if bump is None:
        return None
    bh, bw = bump.shape[:2]
    h, w = img.shape[:2]
    u = np.floor((np.arange(w) + 0.5) / w * cutout['u'] * bw).astype(np.int64) % bw
    v = np.floor((np.arange(h) + 0.5) / h * cutout['v'] * bh).astype(np.int64) % bh
    return bump[v[:, None], u[None, :], 3:4]


# --look-alpha-first: a base map's alpha kept or dropped by the first level
# shader that draws it alone (as before)
LOOK_ALPHA_ANY = True


def model_looks(m, d, B):
    """soso shaders with a detail map (build 36): base bitmap address -> how
    to fold it in. The Xbox's model shader blends the detail map toward
    neutral by a mask from the multipurpose map (red: reflection; green:
    self-illumination; blue: change colour; alpha) and combines it with the
    base (double/biased multiply, multiply, double/biased add); the chief's
    armour and the vehicles' hulls take a dark corroded-metal detail map
    there (average 0.25, so x 0.5 under the mask), which the PSP never drew:
    they came out twice as bright. The GE takes the folded base under the
    key address | 1 (psp_fixups.py), the plain one stays for other shaders"""
    by_tag = {}
    for b in B:
        by_tag.setdefault(b['tag'], []).append(b)
    looks = {}
    for t in m.tags:
        if t.group != 'soso' or not t.address:
            continue
        o = m.off(t.address)
        base_id, = struct.unpack_from('<i', m.data, o + 176)
        multipurpose_id, = struct.unpack_from('<i', m.data, o + 200)
        function, mask = struct.unpack_from('<hh', m.data, o + 212)
        detail_id, = struct.unpack_from('<i', m.data, o + 232)
        base = m.by_id.get(base_id)
        if not base or base.name not in by_tag:
            continue
        detail = detail_colour(m, d, by_tag, detail_id)
        if detail is None:
            continue
        mp = m.by_id.get(multipurpose_id)
        multipurpose = None
        if mp and mp.name in by_tag:
            multipurpose = decode(m, by_tag[mp.name][0])
        key = by_tag[base.name][0]['address']
        looks.setdefault(key, dict(function=function, mask=mask, detail=detail, multipurpose=multipurpose,
                                   shader=t.name))
    return looks


def glow_looks(m, B):
    """soso shaders that light themselves: shader tag address -> the base
    bitmap and multipurpose map to bake a glow map from. On the Xbox a model
    shader's self-illumination (the multipurpose map's green times the
    shader's colour) is added to the lighting, and the lighting multiplies the
    base map (rasterizer_xbox_models.c, set_environment_shader_pixel_shader:
    stage 1 adds R0 x C0 to V0, the later stages multiply by T0), so a lit-up
    area shows its own base texture at full brightness. The GE adds the
    colour in a pass of its own (psp_ge.c, multipurpose_pass), so it takes a
    map of what that pass must add: the base x the green. Only shaders the
    runtime gives a colour (a period, and a colour at either end); the key is
    the shader's address | 2, which no bitmap's key can be"""
    by_tag = {}
    for b in B:
        by_tag.setdefault(b['tag'], []).append(b)
    glows = {}
    for t in m.tags:
        if t.group != 'soso' or not t.address:
            continue
        o = m.off(t.address)
        period, = struct.unpack_from('<f', m.data, o + 116)
        colours = struct.unpack_from('<6f', m.data, o + 120)
        base_id, = struct.unpack_from('<i', m.data, o + 176)
        multipurpose_id, = struct.unpack_from('<i', m.data, o + 200)
        base, mp = m.by_id.get(base_id), m.by_id.get(multipurpose_id)
        if period == 0.0 or not any(colours) or not base or not mp or base.name not in by_tag or mp.name not in by_tag:
            continue
        glows[t.address | 2] = dict(shader=t.name, base=by_tag[base.name][0], multipurpose=by_tag[mp.name][0])
    return glows


def level_glow_looks(m, d, B):
    """senv shaders that light themselves: the shader's address | 2 (the
    primary colour) or | 3 (the secondary) -> the base bitmap (None where the
    shader has none) and the illumination map to bake a glow map from. On the
    Xbox a level shader's self-illumination is added to the lightmap, and the
    base map then multiplies the frame (rasterizer_xbox_environment.c: the
    lightmap pass's final combiner adds the illumination; the base pass is the
    product), so a lit-up area shows its own texture at full brightness, as a
    model's does (glow_looks). The map's red weighs the primary colour and its
    green the secondary, at the level's coordinates times the map scale; its
    alpha (the plasma colour) isn't drawn. The self-illumination properties are
    at +384 (the diffuse properties' 276 bytes after +108): the primary colours
    at +412 (on) and +424 (off), its period at +440, the secondary's 60 bytes
    on, the scale at +592 and the map's index at +608"""
    by_tag = {}
    for b in B:
        by_tag.setdefault(b['tag'], []).append(b)
    glows = {}
    for t in m.tags:
        if t.group != 'senv' or not t.address:
            continue
        o = m.off(t.address)
        scale, = struct.unpack_from('<f', m.data, o + 592)
        mask_id, = struct.unpack_from('<i', m.data, o + 608)
        base_id, = struct.unpack_from('<i', m.data, o + 148)
        mask, base = m.by_id.get(mask_id), m.by_id.get(base_id)
        if not mask or mask.name not in by_tag:
            continue
        for which in range(2):
            colours = struct.unpack_from('<6f', m.data, o + 412 + 60 * which)
            period, = struct.unpack_from('<f', m.data, o + 440 + 60 * which)
            if period == 0.0 or not any(colours):
                continue
            glows[t.address | (2 + which)] = dict(
                shader=t.name, mask=by_tag[mask.name][0], channel=which, scale=scale or 1.0,
                base=by_tag[base.name][0] if base and base.name in by_tag else None)
    return glows


def relief_looks(m, d, B):
    """senv shaders with a bump map: base bitmap address -> (the bump map's
    entry, its scale on the base's coordinates). shader_environment's diffuse
    properties are at +108: the bump map scale at +292, the bump map at +296
    (its index at +308), the runtime scale (u, v) at +312, as
    rasterizer_xbox_environment.c lays them out"""
    by_tag = {}
    for b in B:
        by_tag.setdefault(b['tag'], []).append(b)
    reliefs = {}
    for t in m.tags:
        if t.group != 'senv' or not t.address:
            continue
        o = m.off(t.address)
        base_id, = struct.unpack_from('<i', m.data, o + 148)
        bump_scale, = struct.unpack_from('<f', m.data, o + 292)
        bump_id, = struct.unpack_from('<i', m.data, o + 308)
        su, sv = struct.unpack_from('<2f', m.data, o + 312)
        base, bump = m.by_id.get(base_id), m.by_id.get(bump_id)
        if not base or not bump or base.name not in by_tag or bump.name not in by_tag:
            continue
        # a colour map named as its own bump map holds no normals to light
        if bump.name == base.name:
            continue
        fallback = bump_scale if bump_scale else 1.0
        reliefs.setdefault(by_tag[base.name][0]['address'], dict(
            bump=by_tag[bump.name][0], u=su if su else fallback, v=sv if sv else fallback, shader=t.name))
    return reliefs


# the bump relief's light, in the bump map's own space (x along u, y along
# v, z out of the surface): from up and to the side, as an emboss is lit.
# The Xbox lights the bump per pixel from the lightmap's incident direction,
# which differs surface by surface; a texture shared by many surfaces can
# only take one direction
RELIEF_LIGHT = np.array([-0.4, 0.4, 0.82], np.float32) / np.float32(np.linalg.norm([-0.4, 0.4, 0.82]))
RELIEF_STRENGTH = 1.0
NO_RELIEF = False   # --no-relief: the level's base maps without their bump relief
OLD_RELIEF = False  # --relief-unfiltered: the bump sampled at the base's texels unfiltered, as build 139
# a bump map tiled so finely that one base texel spans more than this many
# of its texels along either axis is left out: averaged down to the base's
# texels it is nearly flat, and unfiltered it aliased into streaks (a30's
# cliff walls, whose stretched coordinates drew them out into vertical lines)
RELIEF_MAX_FOOTPRINT = 4.0
RELIEF_SKIPPED = []   # (shader, bump texels per base texel along u, along v)


def box_wrapped(a, kx, ky):
    """each texel the mean of the kx x ky block of texels round it, the map
    tiled (kx, ky at most RELIEF_MAX_FOOTPRINT rounded up, so a few shifts)"""
    out = np.zeros_like(a)
    for dy in range(ky):
        for dx in range(kx):
            out += np.roll(np.roll(a, dy - ky // 2, 0), dx - kx // 2, 1)
    return out / float(kx * ky)


def apply_relief(img, relief, m):
    """the base map x its bump map's relief: dot(normal, light) over the flat
    surface's, sampled where the base's texels fall on the tiled bump map
    (bilinear, wrapped); a flat area keeps its colour. The bump is first
    averaged over what one base texel covers of it, so relief finer than the
    base can show averages out instead of aliasing; None when it is so fine
    that nothing would be left (RELIEF_MAX_FOOTPRINT)"""
    bump = decode_bump(m, relief['bump'])
    if bump is None:
        return img
    normals = bump[:, :, :3].astype(np.float32) / 127.5 - 1.0
    bh, bw = normals.shape[:2]
    h, w = img.shape[:2]
    if not OLD_RELIEF:
        across_u, across_v = abs(relief['u']) * bw / w, abs(relief['v']) * bh / h
        if max(across_u, across_v) > RELIEF_MAX_FOOTPRINT:
            RELIEF_SKIPPED.append((relief['shader'], across_u, across_v))
            return None
        kx, ky = max(1, int(np.ceil(across_u - 0.25))), max(1, int(np.ceil(across_v - 0.25)))
        if kx > 1 or ky > 1:
            normals = box_wrapped(normals, kx, ky)
    u = ((np.arange(w, dtype=np.float32) + 0.5) / w * relief['u'] * bw - 0.5)[None, :].repeat(h, 0)
    v = ((np.arange(h, dtype=np.float32) + 0.5) / h * relief['v'] * bh - 0.5)[:, None].repeat(w, 1)
    u0, v0 = np.floor(u), np.floor(v)
    fu, fv = (u - u0)[:, :, None], (v - v0)[:, :, None]
    u0 = u0.astype(np.int64) % bw
    v0 = v0.astype(np.int64) % bh
    u1, v1 = (u0 + 1) % bw, (v0 + 1) % bh
    n = (normals[v0, u0] * (1 - fu) * (1 - fv) + normals[v0, u1] * fu * (1 - fv) +
         normals[v1, u0] * (1 - fu) * fv + normals[v1, u1] * fu * fv)
    n /= np.maximum(np.linalg.norm(n, axis=2, keepdims=True), 1e-6)
    shade = (n @ RELIEF_LIGHT) / RELIEF_LIGHT[2]
    factor = np.clip(1.0 + RELIEF_STRENGTH * (shade - 1.0), 0.35, 1.65)[:, :, None]
    rgb = np.clip(img[:, :, :3].astype(np.float32) * factor + 0.5, 0, 255).astype(np.uint8)
    return np.concatenate([rgb, img[:, :, 3:4]], 2)


def gel_average(m, b):
    """a light's gel cube map's mean colour over all six faces, or None. The
    Xbox turns the gel over game time, so a wall near the light sees its beams
    sweep past; the six faces' mean is that light averaged over a turn
    (rasterizer.c's light begin multiplies a light without a cone by it). One
    face won't do: a beacon's beam may sit on one face and miss another.
    Faces padded to an alignment hold each face's whole mip chain in turn, so
    each face's top level is read; unpadded data is averaged whole (top levels
    weigh three quarters, every face alike), whichever way its faces and
    levels interleave"""
    fmt, w, h = b['fmt'], b['w'], b['h']
    if b['type'] != 'cube' or fmt in (17,) or BPP[fmt] == 0:
        return None
    dxt = FORMATS[fmt].startswith('DXT')
    block = 8 if FORMATS[fmt] == 'DXT1' else 16
    chain, lw, lh = 0, w, h
    for _ in range(b['mips'] + 1):
        chain += max(1, (lw + 3) // 4) * max(1, (lh + 3) // 4) * block if dxt else lw * lh * BPP[fmt] // 8
        lw, lh = max(1, lw // 2), max(1, lh // 2)
    size = b['pixels_size']
    if size < 6 * chain:
        return None
    data = m.data[b['pixels_offset']:b['pixels_offset'] + size]
    if size > 6 * chain:
        stride = size // 6
        top = max(1, (w + 3) // 4) * max(1, (h + 3) // 4) * block if dxt else w * h * BPP[fmt] // 8
        data = b''.join(data[k * stride:k * stride + top] for k in range(6))
    else:
        data = data[:6 * chain]
    if dxt:
        img = decode_dxt(data, 4, len(data) // block * 4, FORMATS[fmt])
    else:
        img = decode_plain(data, len(data) * 8 // BPP[fmt], 1, fmt, False)
    if img is None:
        return None
    return [int(round(x)) for x in img[:, :, :3].reshape(-1, 3).astype(np.float64).mean(0)]


def gel_face_images(m, b):
    """a light's gel cube map's six faces' top levels as RGBA arrays, in the
    cube's face order (+X, -X, +Y, -Y, +Z, -Z), or None. Each face's mip
    chain is held in turn as gel_average reads the padded layout; unpadded
    data is taken in that order too"""
    fmt, w, h = b['fmt'], b['w'], b['h']
    if b['type'] != 'cube' or fmt in (17,) or BPP[fmt] == 0:
        return None
    dxt = FORMATS[fmt].startswith('DXT')
    block = 8 if FORMATS[fmt] == 'DXT1' else 16
    chain, lw, lh = 0, w, h
    for _ in range(b['mips'] + 1):
        chain += max(1, (lw + 3) // 4) * max(1, (lh + 3) // 4) * block if dxt else lw * lh * BPP[fmt] // 8
        lw, lh = max(1, lw // 2), max(1, lh // 2)
    size = b['pixels_size']
    if size < 6 * chain:
        return None
    stride = size // 6 if size > 6 * chain else chain
    top = max(1, (w + 3) // 4) * max(1, (h + 3) // 4) * block if dxt else w * h * BPP[fmt] // 8
    data = m.data[b['pixels_offset']:b['pixels_offset'] + size]
    faces = []
    for k in range(6):
        face = data[k * stride:k * stride + top]
        if dxt:
            img = decode_dxt(face, w, h, FORMATS[fmt])
        else:
            img = decode_plain(face, w, h, fmt, bool(b['flags'] & 8))
        if img is None:
            return None
        faces.append(img)
    return faces


def gel_faces(m, b):
    """a light's gel cube map's six faces' mean colours, in the cube's face
    order (+X, -X, +Y, -Y, +Z, -Z), or None. The light pass looks the gel up
    by the direction from the light in the gel's frame, +X its forward, +Y
    its left, +Z its up (the engine's environment light vertex shader), so a
    wall above the light sees the +Z face and one ahead of it the +X face"""
    images = gel_face_images(m, b)
    if images is None:
        return None
    return [[int(round(x)) for x in img[:, :, :3].reshape(-1, 3).astype(np.float64).mean(0)] for img in images]


GEL_CUBE_SIDE = 16             # texels a side of each face of a gel's kept cube (psp_tex.c, GEL_CUBE_SIDE)


def gel_cube(m, b):
    """a light's gel cube map at GEL_CUBE_SIDE texels a side, its six faces
    stacked top to bottom in the cube's face order, as RGBA, or None. The
    Xbox looks the gel up per pixel, so a beacon's narrow turning beam sweeps
    the walls at full strength; a face's mean spreads it over the face at a
    fraction of that. The PSP samples this at each lit vertex (psp_tex.c,
    psp_tex_gel_cube_at)"""
    images = gel_face_images(m, b)
    if images is None:
        return None
    return np.concatenate([downscale(img, GEL_CUBE_SIDE, GEL_CUBE_SIDE) for img in images], 0)


def gel_bitmaps(roles):
    """bitmap tag names lights use as their gel and nothing else uses"""
    return set(tag for tag, rs in roles.items()
               if rs and all(r.startswith('ligh:') and r.endswith('cube map') for r in rs))


GEL_FACES_MARK = 0xFACE        # palette entry 15 of a gel patch that carries its six faces (psp_tex.c)
GEL_CUBE_MARK = 0xC0BE         # palette entry 255 of a gel patch that carries its cube as well (psp_tex.c)
GEL_CUBE_FIRST = 7             # the cube's colours from this palette entry on (0 the mean, 1-6 the faces)


def cube_patch(m, b, rgb=None, faces=None, cube=None, sphere=None):
    """a cube map's average colour as an 8 x 8 patch of that one colour,
    under the cube map's own key: the GE draws no cube maps, and a model's
    reflection is folded into its lighting by this average (psp_ge.c,
    psp_ge_model_reflection). The first face stands for the six (the bitmap's
    data starts with it), unless rgb gives the colour (a light's gel). A
    gel's six face colours, when given, sit in palette entries 1-6 (every
    texel is entry 0, so the patch still draws and averages as one colour),
    entry 15 marking them. A gel's cube, when given, makes the patch CLUT8
    instead: the six faces stacked (gel_cube), its texels indexing entries
    7-254, entries 0-6 as above and entry 255 marking it; its average is
    still entry 0's"""
    if rgb is not None:
        r, g, bl = rgb
    else:
        img = decode(m, b)
        if img is None:
            return None
        r, g, bl = [int(round(x)) for x in img[:, :, :3].reshape(-1, 3).astype(np.float64).mean(0)]
    def entry_of(r, g, bl):
        return (r >> 3) | ((g >> 2) << 5) | ((bl >> 3) << 11)    # GU_PSM_5650, as the packed palettes
    if cube is not None and faces is not None:
        colours = 255 - GEL_CUBE_FIRST
        opaque = cube.copy()
        opaque[:, :, 3] = 255
        idx, pal, _, _ = quantize(opaque, colours)
        entries = [entry_of(r, g, bl)] + [entry_of(*face) for face in faces]
        entries += [entry_of(int(p[0]), int(p[1]), int(p[2])) for p in pal[:colours]]
        entries += [0] * (255 - len(entries)) + [GEL_CUBE_MARK]
        palette = np.array(entries, '<u2').tobytes()
        w, h = cube.shape[1], cube.shape[0]
        row, rows = level_bytes(w, h, 8)
        level = np.zeros((rows, row), np.uint8)
        level[:h, :w] = idx + GEL_CUBE_FIRST
        expanded = (entries[0] & 31) * 255 // 31, ((entries[0] >> 5) & 63) * 255 // 63, (entries[0] >> 11) * 255 // 31
        return dict(w=w, h=h, levels=1, clut=0, alpha=False, psm=5, average=expanded,
                    data=palette + swizzle_psp(level.tobytes(), row, rows))
    if sphere is not None:
        # a model reflection's sphere map, CLUT8; its average stays the
        # patch's (entry 0 expanded), so the reflection folded into the
        # part's ambient (psp_ge.c, ambient_with_reflection) is as before
        mean = entry_of(r, g, bl)
        idx, pal, _, _ = quantize(sphere, 256)
        palette = np.array([entry_of(int(p[0]), int(p[1]), int(p[2])) for p in pal[:256]], '<u2').tobytes()
        row, rows = level_bytes(SPHERE_SIDE, SPHERE_SIDE, 8)
        level = np.zeros((rows, row), np.uint8)
        level[:SPHERE_SIDE, :SPHERE_SIDE] = idx
        expanded = (mean & 31) * 255 // 31, ((mean >> 5) & 63) * 255 // 63, (mean >> 11) * 255 // 31
        return dict(w=SPHERE_SIDE, h=SPHERE_SIDE, levels=1, clut=0, alpha=False, psm=5, average=expanded,
                    data=palette + swizzle_psp(level.tobytes(), row, rows))
    entries = [entry_of(r, g, bl)] + [0] * 15
    if faces is not None:
        entries[1:7] = [entry_of(*face) for face in faces]
        entries[15] = GEL_FACES_MARK
    palette = np.array(entries, '<u2').tobytes()
    row, rows = level_bytes(8, 8, 4)
    level = swizzle_psp(bytes(row * rows), row, rows)                # every texel entry 0
    return dict(w=8, h=8, levels=1, clut=0, alpha=False, data=palette + level, psm=4)


SPHERE_SIDE = 32               # texels a side of a model reflection's sphere map (psp_ge.c, reflection_env_pass)


def sphere_map(m, b):
    """a model reflection's cube map as a sphere map, SPHERE_SIDE a side, as
    RGBA, or None: the colour reflected toward the eye by a surface whose
    normal, seen from the eye, points at that texel (s right, t up, the
    middle facing the eye), for an eye looking along +X with +Z up. The GE
    has no cube maps; its environment mapping makes a texel from the
    normal's two components along the camera's right and up
    (sceGuTexMapMode GU_ENVIRONMENT_MAP), so a shiny part's reflection moves
    as it turns, though not as the camera does. The cube is looked up as
    Direct3D's faces (psp_tex.c, psp_tex_gel_cube_at)"""
    faces = gel_face_images(m, b)
    if faces is None:
        return None
    side = faces[0].shape[0]
    n = SPHERE_SIDE
    c = (np.arange(n) + 0.5) / n * 2.0 - 1.0
    s, t = np.meshgrid(c, -c)                                         # row 0 is up
    z = np.sqrt(np.clip(1.0 - s * s - t * t, 0.0, 1.0))
    norm = np.sqrt(np.maximum(s * s + t * t + z * z, 1e-12))
    s, t, z = s / norm, t / norm, z / norm
    rx, ry, rz = 2.0 * z * s, 2.0 * z * t, 2.0 * z * z - 1.0          # the view reflected (view space)
    x, y, zz = -rz, -rx, ry                                           # into the world: forward +X, left +Y, up +Z
    ax, ay, az = np.abs(x), np.abs(y), np.abs(zz)
    out = np.zeros((n, n, 4), np.float64)
    major = np.maximum(np.maximum(ax, ay), az)
    pick_x = (ax >= ay) & (ax >= az)
    pick_y = ~pick_x & (ay >= az)
    pick_z = ~pick_x & ~pick_y
    face = np.where(pick_x, np.where(x >= 0, 0, 1), np.where(pick_y, np.where(y >= 0, 2, 3), np.where(zz >= 0, 4, 5)))
    sc = np.where(pick_x, np.where(x >= 0, -zz, zz), np.where(pick_y, x, np.where(zz >= 0, x, -x)))
    tc = np.where(pick_x, -y, np.where(pick_y, np.where(y >= 0, zz, -zz), -y))
    u = np.clip(((sc / major + 1.0) * 0.5 * side).astype(int), 0, side - 1)
    v = np.clip(((tc / major + 1.0) * 0.5 * side).astype(int), 0, side - 1)
    for k in range(6):
        where = face == k
        out[where] = faces[k][v[where], u[where]]
    out[:, :, 3] = 255
    return np.clip(np.rint(out), 0, 255).astype(np.uint8)


NO_SPHERE_MAPS = False         # --no-sphere-maps: a model reflection's cube as its 8 x 8 average patch only, as before

NO_CUBE_PATCHES = False        # --no-cube-patches: no cube map averages in the pack (as build 139)
NO_GEL_FACES = False           # --no-gel-faces: a gel patch with the single gel average only, the behaviour before per-face averages
NO_GEL_CUBE = False            # --no-gel-cube: a gel patch with its face means only, the behaviour before the kept cube
CUBE_PATCHES = []


PREMULTIPLIED_MASKS = False    # --premultiplied-masks: the detail masks read as before
MASKS_MENDED = []              # (shader, share of texels) whose detail mask the fix changed


def channel_at(image, channel, w, h):
    """one channel of an image at w x h, 0-1. Resized on its own: PIL resizes
    RGBA premultiplied, so a multipurpose map's red, green or blue under its
    alpha (the detail mask, not a transparency) would come out darkened, and
    black where that alpha is 0"""
    one = np.ascontiguousarray(image[:, :, channel])
    if one.shape != (h, w):
        one = np.array(__import__('PIL.Image', fromlist=['Image']).fromarray(one).resize((w, h)))
    return one[:, :, None].astype(np.float32) / 255.0


def apply_glow(img, multipurpose):
    """what the self-illumination pass adds, at full colour: the base x the
    multipurpose map's green, opaque (the pass draws its colour only)"""
    h, w = img.shape[:2]
    green = channel_at(multipurpose, 1, w, h)
    rgb = np.clip(img[:, :, :3].astype(np.float32) * green + 0.5, 0, 255).astype(np.uint8)
    return np.concatenate([rgb, np.full((h, w, 1), 255, np.uint8)], 2)


def apply_level_glow(img, glow):
    """what a level shader's self-illumination pass adds, at full colour: the
    base x one channel of the illumination map, which the Xbox samples at the
    level's coordinates times its scale, wrapped (here at each of the base's
    texels); opaque"""
    h, w = img.shape[:2]
    mask = glow['mask']
    mh, mw = mask.shape[:2]
    u = np.floor((np.arange(w) + 0.5) / w * glow['scale'] * mw).astype(np.int64) % mw
    v = np.floor((np.arange(h) + 0.5) / h * glow['scale'] * mh).astype(np.int64) % mh
    one = mask[v[:, None], u[None, :], glow['channel']:glow['channel'] + 1].astype(np.float32) / 255.0
    rgb = np.clip(img[:, :, :3].astype(np.float32) * one + 0.5, 0, 255).astype(np.uint8)
    return np.concatenate([rgb, np.full((h, w, 1), 255, np.uint8)], 2)


def apply_model_look(img, look):
    rgb = img[:, :, :3].astype(np.float32) / 255.0
    h, w = rgb.shape[:2]
    mask = np.ones((h, w, 1), np.float32)
    k = look['mask']
    mp = look['multipurpose']
    if k and mp is not None:
        channel = [None, 0, 0, 1, 1, 2, 2, 3, 3][k] if k < 9 else None
        if channel is not None:
            mask = channel_at(mp, channel, w, h)
            if mp.shape[:2] != (h, w) and channel < 3:
                old = np.array(__import__('PIL.Image', fromlist=['Image']).fromarray(mp).resize((w, h)))
                old = old[:, :, channel:channel + 1].astype(np.float32) / 255.0
                changed = float((np.abs(old - mask) > 2.0 / 255.0).mean())
                if changed:
                    MASKS_MENDED.append((look['shader'], changed))
                if PREMULTIPLIED_MASKS:
                    mask = old
            if k % 2 == 1:
                mask = 1.0 - mask
    detail = look['detail'][None, None, :]
    if look['function'] == 1:
        out = rgb * ((1.0 - mask) + mask * detail)
    elif look['function'] == 2:
        out = rgb + mask * (2.0 * detail - 1.0)
    else:
        out = rgb * ((1.0 - mask) + mask * 2.0 * detail)
    out = np.clip(out * 255.0 + 0.5, 0, 255).astype(np.uint8)
    return np.concatenate([out, img[:, :, 3:4]], 2)


def combine(rgb, detail, function):
    if function == 1:                   # multiply
        return rgb * detail
    if function == 2:                   # double/biased add
        return rgb + 2.0 * detail - 1.0
    return rgb * 2.0 * detail           # double/biased multiply


def neutral_detail(img):
    """a detail map the GE draws over the base map (build 31): scaled so its
    average is 0.5 in each channel, so that doubled (the GE's 2 x src x dst
    blend) it multiplies by detail / average. The base map already holds the
    average (apply_look), so the two together give the Xbox's base x 2 x
    detail, and where the map mips or fades out to its average it changes
    nothing"""
    rgb = img[:, :, :3].astype(np.float32)
    mean = np.maximum(rgb.reshape(-1, 3).mean(0), 1.0)
    out = np.clip(rgb * (127.5 / mean)[None, None, :] + 0.5, 0, 255).astype(np.uint8)
    return np.concatenate([out, np.full_like(img[:, :, 3:4], 255)], 2)


def apply_look(img, look, m=None):
    if look.get('cutout') is not None and m is not None:
        alpha = cutout_alpha(img, look['cutout'], m)
        if alpha is not None:
            img = np.concatenate([img[:, :, :3], alpha], 2)
            CUTOUTS.append((look['shader'], look['cutout']['bump']['tag']))
    rgb = img[:, :, :3].astype(np.float32) / 255.0
    a = img[:, :, 3:4].astype(np.float32) / 255.0
    p, q = look['primary'], look['secondary']
    if p is None and q is None and look.get('micro') is None:
        return img
    if p is None:
        p = q
    if q is None:
        q = p
    out = rgb
    if p is not None:
        out = combine(out, q[None, None, :] * (1.0 - a) + p[None, None, :] * a, look['function'])
    # the micro detail map combines with the result the same way (build 31)
    if look.get('micro') is not None:
        out = combine(out, look['micro'][None, None, :], look['micro_function'])
    out = np.clip(out * 255.0 + 0.5, 0, 255).astype(np.uint8)
    alpha = img[:, :, 3:4] if look['alpha_tested'] else np.full_like(img[:, :, 3:4], 255)
    return np.concatenate([out, alpha], 2)


MAXIMUM_SIDE = 256     # a 480 x 272 screen; lightmaps keep their size
OBJECT_SIDE = 128      # objects rarely cover more of the screen; first-person ones do


# The HUD's meters (build 23): a grey bitmap whose grey is the position along
# the meter and whose alpha is its shape, with more grey-alpha pairs than a
# palette holds. The game works a meter out per palette entry (psp_ge.c), so
# the palette is a fixed grid instead: 64 greys x 4 alphas, every grey within
# 2 of the original (64 steps along a meter; the assault rifle's 60 rounds
# each keep their own).
GRID_PALETTE = np.array([[min(255, (i >> 2) * 4 + 2)] * 3 + [(i & 3) * 85] for i in range(256)], np.uint8)


# build 51: the grid's greys sit on the bitmap's own step. The assault rifle's
# 60 rounds are greys 4, 8 ... 240, and its meter (alpha multiplier 4, bias 2)
# fills a round while its grey is below 4 x rounds + 2. The grid's greys were
# 4k + 2, so round 60's 240 became 242, equal to the full magazine's 242: the
# last round never lit (every build). The offset is now the bitmap's
# commonest grey modulo 4 (0 here), so greys on the 4-step stay exact
def grid_offset(img):
    a = img[:, :, 3] > 0
    if not a.any():
        return 2
    return int(np.bincount((img[:, :, 2][a].astype(np.int32) & 3), minlength=4).argmax())


GRID_ALPHAS = (0, 85, 170, 255)
METER_ALPHAS = True       # --no-meter-alphas: the fixed four, as before
METER_LOG = []


def grid_palette(offset, alphas=GRID_ALPHAS):
    return np.array([[min(255, (i >> 2) * 4 + offset)] * 3 + [alphas[i & 3]] for i in range(256)], np.uint8)


# B3: the grid kept four alpha levels, 0, 85, 170 and 255, and the GE
# multiplies each meter colour by its entry's alpha (psp_ge.c, meter_palette),
# so a shape drawn at 127 came out at 85 and the bars drew dimmer than the
# PC's. The four levels are now the bitmap's own: 0 and 255 always, the other
# two the centres of a two-way split of its partial alphas, weighted by pixel
# count. The greys are untouched (the rifle's rounds need all 64)
def meter_alphas(img):
    a = img[:, :, 3].astype(np.int64).ravel()
    inner = a[(a > 0) & (a < 255)]
    if not inner.size:
        return GRID_ALPHAS
    v = np.arange(1, 255)
    w = np.bincount(inner, minlength=256)[1:255].astype(np.float64)
    c = np.array([85.0, 170.0])
    for _ in range(32):
        near = np.abs(v[:, None] - c[None, :]).argmin(1)
        new = c.copy()
        for k in (0, 1):
            s = w[near == k].sum()
            if s:
                new[k] = (v[near == k] * w[near == k]).sum() / s
        if np.allclose(new, c):
            break
        c = new
    mid = sorted(min(254, max(1, int(round(x)))) for x in c)
    if mid[0] == mid[1]:
        return GRID_ALPHAS
    return (0, mid[0], mid[1], 255)


def meter_alpha_note(tag, img, alphas):
    """one log line a meter: its commonest alphas, the levels chosen, and the
    mean alpha error over drawn pixels with the old levels and the new"""
    a = img[:, :, 3].astype(np.int64).ravel()
    drawn = a[a > 0]
    count = np.bincount(a, minlength=256)
    top = np.argsort(-count)[:6]
    old_err = np.abs(drawn - (drawn + 42) // 85 * 85)
    new_err = np.abs(drawn[:, None] - np.array(alphas)[None, :]).min(1) if drawn.size else old_err
    METER_LOG.append('%s: alphas %s; commonest alpha:pixels %s; %d of %d drawn pixels more than 8 off the old levels; '
                     'mean error old %.2f new %.2f' % (
                         tag, alphas, ' '.join('%d:%d' % (t, count[t]) for t in top if count[t]),
                         int((old_err > 8).sum()), drawn.size,
                         old_err.mean() if drawn.size else 0.0, new_err.mean() if drawn.size else 0.0))


def grey_meter(img):
    rgba = img.reshape(-1, 4).astype(np.int32)
    if rgba[:, 3].min() == 255 or (rgba[:, 0] != rgba[:, 1]).any() or (rgba[:, 1] != rgba[:, 2]).any():
        return False
    return len(np.unique(rgba[:, [2, 3]], axis=0)) > 256


def grid_index(img, offset=2, alphas=None):
    y = np.clip((img[:, :, 2].astype(np.int32) - offset + 2) >> 2, 0, 63)
    if alphas is None:
        a = (img[:, :, 3].astype(np.int32) + 42) // 85
    else:
        a = np.abs(img[:, :, 3].astype(np.int32)[:, :, None] - np.array(alphas)[None, None, :]).argmin(2)
    return y * 4 + a


def decode_level(m, b, k):
    """the Xbox bitmap's own mip level k as RGBA (build 59), or None"""
    fmt, w, h = b['fmt'], b['w'], b['h']
    if b['type'] != '2d' or fmt in (17,) or BPP[fmt] == 0 or k > b['mips']:
        return None
    dxt = FORMATS[fmt].startswith('DXT')
    off = 0
    for _ in range(k):
        if dxt:
            off += max(1, (w + 3) // 4) * max(1, (h + 3) // 4) * (8 if FORMATS[fmt] == 'DXT1' else 16)
        else:
            off += w * h * BPP[fmt] // 8
        w, h = max(1, w // 2), max(1, h // 2)
    buf = m.data[b['pixels_offset'] + off:b['pixels_offset'] + b['pixels_size']]
    if dxt:
        return decode_dxt(buf, w, h, FORMATS[fmt])
    return decode_plain(buf, w, h, fmt, bool(b['flags'] & 8))


def detail_levels(m, b, tw, th):
    """a detail map's levels from the Xbox's own mip chain (build 59). Halo's
    detail maps fade toward their average colour level by level (the bitmap's
    detail fade, applied when the tag was built): measured on Blood Gulch,
    "detail rock sandstone" keeps half the contrast of a plain downscale at
    its sixth level, "detail grass" a third. Downscaled from the top level as
    before, the small levels kept the full pattern, and at a middle distance
    the canyon walls showed it as bands (b56-b59). All levels are scaled
    by the top level's average, so a faded level fades to the neutral grey"""
    dropped = 0
    while (b['w'] >> dropped) > tw and dropped < 12:
        dropped += 1
    top = decode_level(m, b, dropped)
    if top is None:
        return None
    scale = 127.5 / np.maximum(top[:, :, :3].astype(np.float32).reshape(-1, 3).mean(0), 1.0)
    out, w, h, k = [], tw, th, dropped
    while k <= b['mips'] and len(out) < 8:
        img = decode_level(m, b, k)
        if img is None:
            break
        if img.shape[1] != w or img.shape[0] != h:
            img = downscale(img, w, h)
        rgb = np.clip(img[:, :, :3].astype(np.float32) * scale[None, None, :] + 0.5, 0, 255).astype(np.uint8)
        out.append(np.concatenate([rgb, np.full_like(img[:, :, 3:4], 255)], 2))
        if w <= 1 or h <= 1:
            break
        w, h, k = max(1, w // 2), max(1, h // 2), k + 1
    return out


# build 102 (the asset track): lightmaps shrunk by this factor (texpack.py
# --lightmap-scale 2), their charts' colours first grown into the gutters
# (lightmap_masks.py) so the shrinking mixes nothing dark into the seams
LIGHTMAP_SCALE = 1
LIGHTMAP_MASKS = {}
# pages whose halving breaks a seam (lightmap_masks.py) are baked at full
# size too, under the key | 1, which psp_tex.c draws instead ("lmhalfall":
# not). A seam spot is broken when the halved page puts more than
# SEAM_STEP (0-255) between its two sides than the full-size page does.
# At 16, b30 kept 48 of its 50 seamed pages whole (1.8 MB: the halving undone);
# the doorway's step is about 48, so 32 keeps it with room and leaves the
# ordinary blur of halving alone. --seam-step and --seam-spots set them
# That keeps too much (b30: 48 of 50 seamed pages, the halving undone, and
# play follows, object lighting reading the lightmap): --lightmap-seam-whole.
LIGHTMAP_SEAMS = {}
LIGHTMAP_WHOLE = False         # --lightmap-seam-whole
# --lm-chart-halve: chart-aware halving instead (lightmap_masks.chart_halve).
# A chart whose halving widens a seam's step by more than SEAM_STEP, at
# CHART_SPOTS spots or more, gets the half-size texels it reads refitted on
# its own reads; the page keeps its half size, and every other texel, its
# palette and its dithering stay as the plain halving made them
LIGHTMAP_CHARTS = {}
CHART_SPOTS = 1
# only charts at most CHART_MAX texels across (their longer side, full size):
# b30 has 1,699 charts whose seams halving widens by more than 32, nearly
# all larger ones whose step is halving's ordinary blur; refitting them all
# changed 40,839 texels (--chart-max 0)
CHART_MAX = 8
LIGHTMAP_CHART_LOG = []        # (tag, page, charts, texels, worst step before, after, fits left plain, sizes)
SEAM_STEP = 32
SEAM_SPOTS_BROKEN = 3          # three spots: one edge's worth
SEAM_STEPS_LOGGED = (16, 32, 48, 64)
LIGHTMAPS_KEPT_WHOLE = []      # (tag, page, spots broken, worst step, bytes)
LIGHTMAP_SEAM_LOG = []         # (tag, page, spots, broken at each SEAM_STEPS_LOGGED, worst, kept whole)
# build 102 (the asset track): alpha-tested textures' smaller levels keep the
# top level's coverage (texpack.py --alpha-coverage). A box filter averages a
# leaf's edge with the air around it, so at each smaller level fewer texels
# pass the GE's alpha test (alpha > 127) and a far plant thins out and
# shimmers as the level changes (the flickering plants, b62 shot 11).
# Each level's alpha is scaled so the same share of it passes as at the top
# (Castano's coverage-preserving mipmaps)
ALPHA_COVERAGE = False
ALPHA_TESTED = set()

# HUD sheets stay at 8 bits, so the HUD stops looking soft. The
# 16-colour test below compares a bitmap's error with the 16-bit frame
# buffer's own, which a glyph's anti-aliased edge or a meter's soft ramp can
# pass while its steps show (the same test banded effect gradients,
# hardware_b114.md). HUD sheets are few and small, and drawn near their own
# size, so every step is seen. --hud-clut4 puts them back under the test.
# HUD_KEPT_8: the sheets the old rule would have put at 4 bits (the log's
# 'HUD sheets' line, so a bake says whether this changed anything)
HUD_CLUT4 = False
HUD_KEPT_8 = []

# Cut-outs, the textures the GE alpha-tests (a sign's letters, a grating, a
# tree line): their palettes are fitted each side of the alpha test apart
# (bitmap_bake.quantize_split), and their smaller levels mapped onto entries
# of their own side, so no entry stands for both hidden and drawn texels;
# --no-alpha-split fits them as one, as before. Effects and the see-through
# shaders' maps stay at 8 bits (--effects-clut4: the 16-colour test may take
# them, as before); EFFECTS_KEPT_8 lists the ones it would have taken, for
# the log's 'effects kept at 8 bits' line
ALPHA_SPLIT = True
SHAPE_CLASSES = ('effects/sprites', 'transparent shaders')
EFFECTS_CLUT4 = False
EFFECTS_KEPT_8 = []
# TEXPACK_ALPHA_AUDIT=1 (halo-psp-bake --alpha-audit): a line per texture
# with alpha: how many of its texels the alpha test hides or draws otherwise
# than the source does, packed, and with the palette as fitted before
# (one 4-D k-means; scikit-learn's too, where it is installed); a summary
ALPHA_AUDIT = bool(os.environ.get('TEXPACK_ALPHA_AUDIT'))
ALPHA_AUDIT_ROWS = []


def level_bytes_total(w, h, bits):
    row, rows = level_bytes(w, h, bits)
    return row * rows


def alpha_shape(b, cls, look):
    """'cutout' or None: whether this texture's palette is fitted each side
    of the alpha test (see ALPHA_SPLIT). Only where the GE tests the alpha:
    a blended map's alpha is a gradient on both sides of the test, and a
    split would only take entries from its colour"""
    if ALPHA_SPLIT and b['tag'] in ALPHA_TESTED and (look is None or look['alpha_tested']):
        return 'cutout'
    return None


def test_flips(src, packed):
    """the share of texels the GE's alpha test treats otherwise than the
    source's alpha, and the mean alpha error (0-1)"""
    a, p = src[:, :, 3].astype(np.int32), packed[:, :, 3].astype(np.int32)
    side = bitmap_bake.ALPHA_SIDE
    return float(((a >= side) != (p >= side)).mean()), float(np.abs(a - p).mean() / 255.0)


def alpha_audit(b, cls, small, packed, bits, shape):
    colours = 16 if bits == 4 else 256
    flips, error = test_flips(small, packed)
    before = quantize(small, colours)[2] if shape else packed
    flips_before, error_before = test_flips(small, before)
    old = ''
    try:
        from sklearn.cluster import KMeans
        pts = small.reshape(-1, 4).astype(np.float32)
        if len(np.unique(pts, axis=0)) > colours:
            sample = pts if len(pts) <= 40000 else pts[np.random.RandomState(0).choice(len(pts), 40000, replace=False)]
            km = KMeans(n_clusters=colours, n_init=1, max_iter=25, random_state=0).fit(sample)
            fitted = np.clip(np.rint(km.cluster_centers_), 0, 255)[km.predict(pts)].reshape(small.shape)
            old_flips, old_error = test_flips(small, fitted)
            old = '; scikit-learn %.4f (%.4f)' % (old_flips, old_error)
            ALPHA_AUDIT_ROWS.append((cls, shape, flips, flips_before, old_flips))
        else:
            ALPHA_AUDIT_ROWS.append((cls, shape, flips, flips_before, flips_before))
    except ImportError:
        ALPHA_AUDIT_ROWS.append((cls, shape, flips, flips_before, None))
    covered = float((small[:, :, 3] >= bitmap_bake.ALPHA_SIDE).mean())
    print('alpha audit: %s#%d %s %dx%d %d-bit %s: drawn %.3f; alpha-test flips (mean alpha error) packed %.4f (%.4f), '
          'one 4-D fit %.4f (%.4f)%s' % (b['tag'], b['index'], cls.replace(' ', '_'), small.shape[1], small.shape[0], bits,
                                         shape or 'plain', covered, flips, error, flips_before, error_before, old),
          file=sys.stderr)


def alpha_audit_summary():
    if not ALPHA_AUDIT_ROWS:
        return
    for cls in sorted(set(r[0] for r in ALPHA_AUDIT_ROWS)):
        rows = [r for r in ALPHA_AUDIT_ROWS if r[0] == cls]
        def count(k, limit):
            return sum(1 for r in rows if r[k] is not None and r[k] > limit)
        print('alpha audit summary: %s: %d textures with alpha; alpha-test flips over 1%% / 10%% of texels: packed %d / %d, '
              'one 4-D fit %d / %d, scikit-learn %d / %d' % (cls.replace(' ', '_'), len(rows), count(2, 0.01), count(2, 0.10),
                                                        count(3, 0.01), count(3, 0.10), count(4, 0.01), count(4, 0.10)),
              file=sys.stderr)
# build 102: per-class detail (texpack.py --full-detail fp,vehicles,...):
# these classes keep their top level (the bake otherwise halves every texture
# of 64 px and more) and the 256 px cap rises to 512. A lever to try, not the
# default: each doubles its textures' bytes four times over
DETAIL_CLASSES = {
    'fp': lambda tag: '\\fp\\' in tag,
    'weapons': lambda tag: tag.startswith('weapons\\') and '\\fp\\' not in tag,
    'vehicles': lambda tag: tag.startswith('vehicles\\'),
    'characters': lambda tag: tag.startswith('characters\\') and '\\fp\\' not in tag,
    # Chief, a centerpiece as the main menu's ring is: his
    # armour, visor and first-person hands at their own size
    'chief': lambda tag: tag.startswith('characters\\cyborg\\'),
}
FULL_DETAIL = set()


def full_detail(tag):
    return any(DETAIL_CLASSES[c](tag) for c in FULL_DETAIL)


def coverage(alpha):
    return float((alpha > 127).mean()) if alpha.size else 0.0


def keep_coverage(img, target):
    """img's alpha scaled so coverage(alpha) is as close to target as it gets"""
    a = img[:, :, 3].astype(np.float32)
    if a.size == 0 or target <= 0.0 or target >= 1.0:
        return img
    low, high = 0.25, 8.0
    for _ in range(24):
        mid = (low + high) / 2
        if coverage(np.clip(a * mid, 0, 255)) < target:
            low = mid
        else:
            high = mid
    out = img.copy()
    out[:, :, 3] = np.clip(a * high + 0.5, 0, 255).astype(np.uint8)
    return out


def alpha_tested_bitmaps(m, d):
    """bitmap tag names used as an alpha-tested shader's base map: model
    shaders (soso) unless "not alpha tested", environment shaders (senv) with
    "alpha tested" """
    names = set()
    for t in m.tags:
        if t.group not in ('soso', 'senv') or not t.address or not m.in_tags(t.address):
            continue
        o = m.off(t.address)
        flags, = struct.unpack_from('<H', m.data, o + 40)
        base_offset = 164 if t.group == 'soso' else 136
        tid, = struct.unpack_from('<i', m.data, o + base_offset + 12)
        tested = not (flags & 4) if t.group == 'soso' else bool(flags & 1)
        if tested and m.by_id.get(tid):
            names.add(m.by_id[tid].name)
    return names


SHOWCASE = False    # --showcase: the main menu's pack at the most the GE can take
# the classes a showcase pack keeps in their own colours; the rest keep
# their palettes, which the runtime reads (meters, the change colour's mask,
# the lightmaps' samples, the detail pass's grey)
DIRECT_CLASSES = ('objects (soso)', 'sky', 'level (senv)')


SHOWCASE_32 = False  # --showcase-32: opaque maps in 32 bits too (for a 32-bit screen)
SHOWCASE_PALETTES = False  # --showcase-palettes: full size, but every map on its palette
BAYER = np.array([[0, 8, 2, 10], [12, 4, 14, 6], [3, 11, 1, 9], [15, 7, 13, 5]], np.float32) / 16.0 - 0.5


def to_5650(img):
    """16-bit texels (GU_PSM_5650, red in the low bits), with a 4 x 4 ordered
    dither: the GE's filter averages it back into the steps 5 and 6 bits
    lose, where rounding alone would band a slow gradient (the sky's)"""
    h, w = img.shape[:2]
    noise = np.tile(BAYER, ((h + 3) // 4, (w + 3) // 4))[:h, :w]
    def channel(c, bits):
        steps = (1 << bits) - 1
        return np.clip(np.floor(img[:, :, c].astype(np.float32) * steps / 255.0 + 0.5 + noise), 0, steps).astype(np.uint16)
    return channel(0, 5) | (channel(1, 6) << 5) | (channel(2, 5) << 11)


def direct_levels(small, tw, th, levels_wanted, wide):
    """a texture in its own colours: 32 bits a texel (GU_PSM_8888: bytes
    r g b a) when wide, else 16 (GU_PSM_5650); its levels swizzled as the
    T8 ones are, 16-byte rows"""
    levels, w, h, img = [], tw, th, small
    size = 4 if wide else 2
    while len(levels) < levels_wanted:
        row, rows = level_bytes(w * size, h)
        padded = np.zeros((rows, row), np.uint8)
        texels = img if wide else to_5650(img).astype('<u2').view(np.uint8)
        padded[:h, :w * size] = texels.reshape(h, w * size)
        levels.append(swizzle_psp(padded.tobytes(), row, rows))
        if w <= 8 or h <= 8:
            break
        w, h = max(1, w // 2), max(1, h // 2)
        img = downscale(img, w, h)
    return levels


# ---- how texels become palette indices (2026-10-16: the textures'
# blockiness and "oily" bands, everywhere). Three steps every 8- and 4-bit
# texture goes through, with --quality 2. Measured closer to the source by
# 2-9 dB a class, but on the PSP-1000 the two bakes were indistinguishable
# (2026-10-17), so the default stays 1: it bakes in half the time.
#  - the palette is fitted in Oklab, a space where equal distances look
#    equally different, so dark tones get entries of their own rather than
#    sharing a few (RGB distance spends them on bright differences);
#  - texels are mapped onto it with error diffusion (Floyd-Steinberg,
#    serpentine, at 3/4 strength): each texel's rounding is carried to its
#    neighbours, so a gradient between two entries comes out as fine grain
#    the GE's filter blends, not as a flat step with a hard contour. Against
#    the palette as drawn (a 16-bit palette's 5650 rounding included). Not
#    the alpha: cut-outs and masks keep their edges;
#  - sizes are made with a Lanczos filter, which keeps edges sharp where a
#    box blurs them and steps them; alpha and lightmaps keep the box (a
#    cut-out's coverage, a chart's dark gutter, would ring).
QUALITY = 1
QUALITY_REPORT = False
QUALITY_STATS = {}
DIFFUSE_STRENGTH = 0.75


def srgb_to_oklab(rgb):
    c = rgb / 255.0
    c = np.where(c <= 0.04045, c / 12.92, ((c + 0.055) / 1.055) ** 2.4)
    l = 0.4122214708 * c[:, 0] + 0.5363325363 * c[:, 1] + 0.0514459929 * c[:, 2]
    m_ = 0.2119034982 * c[:, 0] + 0.6806995451 * c[:, 1] + 0.1073969566 * c[:, 2]
    s_ = 0.0883024619 * c[:, 0] + 0.2817188376 * c[:, 1] + 0.6299787005 * c[:, 2]
    l, m_, s_ = np.cbrt(l), np.cbrt(m_), np.cbrt(s_)
    return np.stack([0.2104542553 * l + 0.7936177850 * m_ - 0.0040720468 * s_,
                     1.9779984951 * l - 2.4285922050 * m_ + 0.4505937099 * s_,
                     0.0259040371 * l + 0.7827717662 * m_ - 0.8086757660 * s_], 1)


def oklab_to_srgb(lab):
    l = lab[:, 0] + 0.3963377774 * lab[:, 1] + 0.2158037573 * lab[:, 2]
    m_ = lab[:, 0] - 0.1055613458 * lab[:, 1] - 0.0638541728 * lab[:, 2]
    s_ = lab[:, 0] - 0.0894841775 * lab[:, 1] - 1.2914855480 * lab[:, 2]
    l, m_, s_ = l ** 3, m_ ** 3, s_ ** 3
    c = np.stack([4.0767416621 * l - 3.3077115913 * m_ + 0.2309699292 * s_,
                  -1.2684380046 * l + 2.6097574011 * m_ - 0.3413193965 * s_,
                  -0.0041960863 * l - 0.7034186147 * m_ + 1.7076147010 * s_], 1)
    c = np.clip(c, 0, 1)
    c = np.where(c <= 0.0031308, c * 12.92, 1.055 * c ** (1 / 2.4) - 0.055)
    return c * 255.0


# TEXPACK_KMEANS_COMPARE=1 (the new bake's proof, docs/bake_changes.md): each
# palette also made by scikit-learn's KMeans as the bake did before, where it
# is installed, and both errors logged in Oklab, so a bake shows texture by
# texture whether the numpy k-means is ever worse
KMEANS_COMPARE = bool(os.environ.get('TEXPACK_KMEANS_COMPARE'))
if KMEANS_COMPARE:
    import atexit
    atexit.register(lambda: kmeans_summary())


def kmeans_compare(tag, img, colors, new_rec):
    try:
        from sklearn.cluster import KMeans
    except ImportError:
        return
    has_alpha = img[:, :, 3].min() < 255
    ch = 4 if has_alpha else 3
    pts = img.reshape(-1, 4)[:, :ch].astype(np.float32)
    if len(np.unique(pts, axis=0)) <= colors:
        return
    sample = pts if len(pts) <= 40000 else pts[np.random.RandomState(0).choice(len(pts), 40000, replace=False)]
    km = KMeans(n_clusters=colors, n_init=1, max_iter=25, random_state=0).fit(sample)
    old = np.clip(np.rint(km.cluster_centers_), 0, 255)[km.predict(pts)]
    want = srgb_to_oklab(pts[:, :3])

    def error(rec):
        e = ((srgb_to_oklab(rec[:, :3].astype(np.float32)) - want) ** 2).sum(1)
        if has_alpha:
            e = e + ((rec[:, 3].astype(np.float32) - pts[:, 3]) / 255.0) ** 2
        return float(np.sqrt(e).mean())
    e_old, e_new = error(old), error(new_rec.reshape(-1, 4)[:, :ch])
    # what k-means itself minimises: the summed squared distance in the
    # clustered space (RGB or RGBA, 0-255), a mean per texel
    sse_old = float(((old - pts) ** 2).sum(1).mean())
    sse_new = float(((new_rec.reshape(-1, 4)[:, :ch].astype(np.float32) - pts) ** 2).sum(1).mean())
    KMEANS_RESULTS.append((tag, colors, e_old, e_new, sse_old, sse_new))
    print('kmeans compare: %s, %d colours, Oklab error old %.5f new %.5f; squared distance old %.2f new %.2f' % (
        tag, colors, e_old, e_new, sse_old, sse_new))


KMEANS_RESULTS = []


def kmeans_summary():
    """the compare's verdict for the bake's log: the means, how many
    palettes came out worse and by how much, and the worst by name"""
    if not KMEANS_RESULTS:
        return
    old = np.array([r[2] for r in KMEANS_RESULTS])
    new = np.array([r[3] for r in KMEANS_RESULTS])
    change = new / np.maximum(old, 1e-12) - 1
    print('kmeans compare summary: %d palettes, mean Oklab error old %.5f new %.5f (%+.2f%%); worse by over 1%%: %d, '
          'over 3%%: %d; better by over 1%%: %d' % (len(old), old.mean(), new.mean(), 100 * (new.mean() / old.mean() - 1),
                                                  int((change > 0.01).sum()), int((change > 0.03).sum()),
                                                  int((change < -0.01).sum())))
    sse_change = np.array([r[5] / max(r[4], 1e-12) - 1 for r in KMEANS_RESULTS])
    print('kmeans compare summary: the squared distance k-means minimises: worse by over 1%%: %d, over 3%%: %d; '
          'better by over 1%%: %d' % (int((sse_change > 0.01).sum()), int((sse_change > 0.03).sum()), int((sse_change < -0.01).sum())))
    for k in np.argsort(-change)[:8]:
        if change[k] > 0.01:
            tag, colors, e_old, e_new, s_old, s_new = KMEANS_RESULTS[k]
            print('kmeans compare worse: %s, %d colours, %.5f -> %.5f (%+.1f%%); squared distance %.2f -> %.2f' % (
                tag, colors, e_old, e_new, 100 * change[k], s_old, s_new))


# The palette step (quantize) is most of a pack's time, and much of it
# repeats: the asset track's pack makes every texture but the lightmaps again
# from the same pixels, and maps share textures. --palette-cache DIR keeps
# each palette, keyed by its input's pixels, the palette count and the
# palette code itself, so a repeat reads it back instead (the bake points
# every pack of every map at one folder). quantize() is seeded, so what comes
# back is what it would have made: the same bytes in the pack.
PALETTE_CACHE = None
FAST_PALETTES = False     # --fast-palettes: bitmap_bake.fast_kmeans()
# what the cache's keys hold besides the pixels: the code that makes the
# palettes, so a changed bitmap_bake.py never reads an older one's
with open(bitmap_bake.__file__, 'rb') as _f:
    PALETTE_CODE = hashlib.sha256(_f.read()).hexdigest()
del _f


def pixels_hash(img):
    h = hashlib.sha256(str((img.shape, img.dtype.str)).encode())
    h.update(np.ascontiguousarray(img).tobytes())
    return h.hexdigest()


def cache_read(path):
    try:
        with open(path, 'rb') as f:
            z = np.load(io.BytesIO(f.read()), allow_pickle=False)
            idx, pal, alpha = z['idx'], z['pal'], bool(z['alpha'])
    except (OSError, ValueError, KeyError):
        return None
    idx = idx.astype(np.int64)
    return idx, pal, pal[idx], alpha


def cache_write(path, idx, pal, alpha):
    """written whole to a file of this process's own, then renamed into
    place: a pack running beside this one (--jobs) reads either nothing or
    the whole palette"""
    buffer = io.BytesIO()
    np.savez(buffer, idx=idx.astype(np.uint8), pal=pal, alpha=np.array(alpha))
    part = '%s.%d.part' % (path, os.getpid())
    try:
        with open(part, 'wb') as f:
            f.write(buffer.getvalue())
        os.replace(part, path)
    except OSError:
        # another pack holds it open (Windows) or wrote it first: it is the same palette
        try:
            os.remove(part)
        except OSError:
            pass


def make_palette(img, colors, cls, shape=None):
    """quantize(img, colors), through the palette cache when there is one,
    timed for the profile. shape: None, or 'cutout' (alpha_shape): fitted
    each side of the alpha test apart (quantize_split)"""
    started = time.perf_counter()
    pixels = pixels_hash(img) if PALETTE_CACHE or PROFILE or PALETTE_COMPARE else None
    path = None
    if PALETTE_CACHE and not PALETTE_COMPARE:
        # a split palette is another palette of the same pixels
        mode = '' if shape is None else ' ' + shape
        key = hashlib.sha256(('%s %s %d %s%s' % (PALETTE_CODE, FAST_PALETTES, colors, pixels, mode)).encode()).hexdigest()
        path = os.path.join(PALETTE_CACHE, key + '.npz')
        r = cache_read(path)
        if r is not None:
            profile_palette(pixels, colors, time.perf_counter() - started, 'hit')
            return r
    r = quantize(img, colors, fast=FAST_PALETTES, split=shape is not None, cutout=shape == 'cutout')
    seconds = time.perf_counter() - started
    if path:
        cache_write(path, r[0], r[1], r[3])
    if PALETTE_COMPARE:
        compared = time.perf_counter()
        palette_compare(img, colors, cls, r, seconds, pixels)
        PROFILE_NOW['compare'] = PROFILE_NOW.get('compare', 0.0) + time.perf_counter() - compared
    profile_palette(pixels, colors, seconds, 'made')
    return r


# TEXPACK_PROFILE=1 (halo-psp-bake --profile): a line per texture with its
# class, its sizes, the seconds in the palette step and in the rest, and a
# short hash of the palette step's input pixels (never a name: these lines may
# be shared as logs); a summary at the end
PROFILE = bool(os.environ.get('TEXPACK_PROFILE'))


def note(text):
    """a profile or compare line, on the pack's other lines' stream, so the
    two never interleave in the log"""
    print(text, file=sys.stderr)
PROFILE_ROWS = []
PROFILE_NOW = {}


def profile_palette(pixels, colors, seconds, how):
    if PROFILE:
        PROFILE_NOW.setdefault('palettes', []).append((pixels, colors, seconds, how))


def profiled(bake):
    """bake_one, timed: the palettes it made and the rest"""
    def timed(m, b, cls, *args, **kwargs):
        if not PROFILE:
            return bake(m, b, cls, *args, **kwargs)
        PROFILE_NOW.clear()
        started = time.perf_counter()
        r = bake(m, b, cls, *args, **kwargs)
        # the compare's second palettes are no part of a bake's time
        total = time.perf_counter() - started - PROFILE_NOW.get('compare', 0.0)
        palettes = PROFILE_NOW.get('palettes', [])
        in_palettes = sum(p[2] for p in palettes)
        row = dict(cls=cls.replace(' ', '_'), size='%dx%d' % (b['w'], b['h']),
                   made='%dx%d/%d' % (r['w'], r['h'], 4 if r['psm'] == 4 else 8 if r['psm'] == 5 else 16)
                   if r else 'skipped', palette=in_palettes, rest=total - in_palettes, palettes=palettes)
        PROFILE_ROWS.append(row)
        note('texprofile: %s %s %s -> %s palette %.3f s rest %.3f s in %s' % (
            PROFILE_PACK, row['cls'], row['size'], row['made'], in_palettes, row['rest'],
            ' '.join('%s/%d/%s/%.3f' % (p[0][:12], p[1], p[3], p[2]) for p in palettes) or '-'))
        return r
    return timed


def profile_summary(total):
    """the pack's time by class, and how much of the palette step repeats"""
    if not PROFILE:
        return
    rows = PROFILE_ROWS
    baked = sum(r['palette'] + r['rest'] for r in rows)
    note('texprofile summary: %s: %d textures in %.1f s (palettes %.1f s, the rest %.1f s); %.1f s outside them '
          '(reading the map, tints, writing)' % (PROFILE_PACK, len(rows), baked, sum(r['palette'] for r in rows),
                                                 sum(r['rest'] for r in rows), total - baked))
    by_class = {}
    for r in rows:
        c = by_class.setdefault(r['cls'], [0, 0.0, 0.0])
        c[0] += 1
        c[1] += r['palette']
        c[2] += r['rest']
    for cls in sorted(by_class, key=lambda c: -(by_class[c][1] + by_class[c][2])):
        n, pal, rest = by_class[cls]
        note('texprofile class: %s: %s %d textures, palettes %.1f s, the rest %.1f s' % (PROFILE_PACK, cls, n, pal, rest))
    palettes = [p for r in rows for p in r['palettes']]
    seen = {}
    for p in palettes:
        seen.setdefault((p[0], p[1]), []).append(p)
    repeats = [p for group in seen.values() for p in group[1:]]
    hits = [p for p in palettes if p[3] == 'hit']
    note('texprofile palettes: %s: %d made or read, %d distinct inputs; %d repeat one already in this pack '
          '(%.1f s); %d read from the cache in %.2f s' % (PROFILE_PACK, len(palettes), len(seen), len(repeats),
                                                          sum(p[2] for p in repeats), len(hits), sum(p[2] for p in hits)))


# TEXPACK_PALETTE_COMPARE=1 (halo-psp-bake --compare-palettes; the fast
# palettes' proof): each palette also made the other way (fast if the pack's
# are the bake's own, and the other way round), timed, and both scored as
# kmeans_compare scores them (Oklab error, and the squared distance k-means
# minimises); a line per palette by class, size and pixel hash, a summary
PALETTE_COMPARE = bool(os.environ.get('TEXPACK_PALETTE_COMPARE'))
PALETTE_RESULTS = []


def palette_compare(img, colors, cls, made, seconds, pixels):
    has_alpha = img[:, :, 3].min() < 255
    ch = 4 if has_alpha else 3
    pts = img.reshape(-1, 4)[:, :ch].astype(np.float32)
    if len(np.unique(pts, axis=0)) <= colors:
        return
    started = time.perf_counter()
    other = quantize(img, colors, fast=not FAST_PALETTES)
    other_seconds = time.perf_counter() - started
    own, fast = (other, made) if FAST_PALETTES else (made, other)
    own_s, fast_s = (other_seconds, seconds) if FAST_PALETTES else (seconds, other_seconds)
    want = srgb_to_oklab(pts[:, :3])

    def score(rec):
        rec = rec.reshape(-1, 4)[:, :ch].astype(np.float32)
        e = ((srgb_to_oklab(rec[:, :3]) - want) ** 2).sum(1)
        if has_alpha:
            e = e + ((rec[:, 3] - pts[:, 3]) / 255.0) ** 2
        return float(np.sqrt(e).mean()), float(((rec - pts) ** 2).sum(1).mean())
    (e_own, d_own), (e_fast, d_fast) = score(own[2]), score(fast[2])
    row = (cls.replace(' ', '_'), colors, '%dx%d' % img.shape[1::-1], pixels[:12], own_s, fast_s, e_own, e_fast,
           d_own, d_fast)
    PALETTE_RESULTS.append(row)
    note('palcompare: %s %d colours %s in %s: time %.3f -> %.3f s; Oklab error %.5f -> %.5f; squared distance '
          '%.2f -> %.2f' % row)


def palette_compare_summary():
    if not PALETTE_RESULTS:
        return
    r = PALETTE_RESULTS
    own_s, fast_s = sum(x[4] for x in r), sum(x[5] for x in r)
    e_own, e_fast = np.array([x[6] for x in r]), np.array([x[7] for x in r])
    d_own, d_fast = np.array([x[8] for x in r]), np.array([x[9] for x in r])
    change = e_fast / np.maximum(e_own, 1e-12) - 1
    d_change = d_fast / np.maximum(d_own, 1e-12) - 1
    note('palcompare summary: %s: %d palettes, %.1f s -> %.1f s (x%.2f); mean Oklab error %.5f -> %.5f (%+.2f%%); '
          'worse by over 1%%: %d, over 3%%: %d, over 10%%: %d; better by over 1%%: %d' % (
              PROFILE_PACK, len(r), own_s, fast_s, own_s / max(fast_s, 1e-9), e_own.mean(), e_fast.mean(),
              100 * (e_fast.mean() / e_own.mean() - 1), int((change > 0.01).sum()), int((change > 0.03).sum()),
              int((change > 0.10).sum()), int((change < -0.01).sum())))
    note('palcompare summary: %s: squared distance %.2f -> %.2f (%+.2f%%); worse by over 1%%: %d, over 3%%: %d, '
          'over 10%%: %d; better by over 1%%: %d' % (
              PROFILE_PACK, d_own.mean(), d_fast.mean(), 100 * (d_fast.mean() / d_own.mean() - 1),
              int((d_change > 0.01).sum()), int((d_change > 0.03).sum()), int((d_change > 0.10).sum()),
              int((d_change < -0.01).sum())))
    for k in np.argsort(-change)[:8]:
        if change[k] > 0.01:
            note('palcompare worse: %s %d colours %s in %s: Oklab error %.5f -> %.5f (%+.1f%%); squared distance '
                  '%.2f -> %.2f' % (r[k][0], r[k][1], r[k][2], r[k][3], r[k][6], r[k][7], 100 * change[k], r[k][8],
                                    r[k][9]))


PROFILE_PACK = 'pack'


def fit_palette(img, colors):
    """(palette Nx4 uint8, has_alpha): k-means in Oklab (and alpha), as
    quantize does in RGB; a texture with no more colours than entries keeps
    its own"""
    has_alpha = bool(img[:, :, 3].min() < 255)
    pts = img.reshape(-1, 4).astype(np.float32)
    if not has_alpha:
        pts = pts[:, :3]
    uniq = np.unique(pts, axis=0)
    if len(uniq) <= colors:
        pal = uniq
    else:
        sample = pts if len(pts) <= 100000 else pts[np.random.RandomState(0).choice(len(pts), 100000, replace=False)]
        feats = srgb_to_oklab(sample[:, :3])
        if has_alpha:
            feats = np.concatenate([feats, sample[:, 3:4] / 255.0], 1)
        centres, _ = kmeans(feats, colors, 60)
        rgb = oklab_to_srgb(centres[:, :3])
        pal = np.concatenate([rgb, centres[:, 3:4] * 255.0], 1) if has_alpha else rgb
    pal = np.clip(np.rint(pal), 0, 255).astype(np.uint8)
    if not has_alpha:
        pal = np.concatenate([pal, np.full((len(pal), 1), 255, np.uint8)], 1)
    if len(pal) < colors:
        pal = np.concatenate([pal, np.zeros((colors - len(pal), 4), np.uint8)])
    return pal, has_alpha


def as_drawn(pal, alpha):
    """the palette's colours as the GE draws them: a 16-bit palette's 5650
    rounding, widened back to 8 bits"""
    shown = pal.astype(np.float32).copy()
    if not alpha:
        for c, bits in ((0, 5), (1, 6), (2, 5)):
            v = pal[:, c].astype(np.int32) >> (8 - bits)
            shown[:, c] = (v * 255 + ((1 << bits) - 1) // 2) // ((1 << bits) - 1)
    return shown


def diffuse(img, pal, alpha, count):
    """indices into the palette's first count entries, by error diffusion of
    the colour (the alpha matched, not diffused)"""
    h, w = img.shape[:2]
    ch = 4 if alpha else 3
    P = as_drawn(pal[:count], alpha)[:, :ch]
    work = img[:, :, :ch].astype(np.float32).copy()
    out = np.zeros((h, w), np.int32)
    k = DIFFUSE_STRENGTH / 16.0
    for y in range(h):
        forward = y % 2 == 0
        d = 1 if forward else -1
        row = work[y]
        below = work[y + 1] if y + 1 < h else None
        for x in (range(w) if forward else range(w - 1, -1, -1)):
            v = row[x]
            i = int(((P - v) ** 2).sum(1).argmin())
            out[y, x] = i
            e = v - P[i]
            if ch == 4:
                e[3] = 0.0
            e *= k
            if 0 <= x + d < w:
                row[x + d] += 7 * e
            if below is not None:
                if 0 <= x - d < w:
                    below[x - d] += 3 * e
                below[x] += 5 * e
                if 0 <= x + d < w:
                    below[x + d] += e
    return out


def resample(img, tw, th, cls):
    """a smaller size: Lanczos on the colour, the box on the alpha and on
    lightmaps (see above); downscale as before with --quality 1"""
    if QUALITY < 2 or cls == 'lightmap' or (img.shape[1] == tw and img.shape[0] == th):
        return downscale(img, tw, th)
    from PIL import Image
    rgb = np.array(Image.fromarray(np.ascontiguousarray(img[:, :, :3]), 'RGB').resize((tw, th), Image.Resampling.LANCZOS))
    a = np.array(Image.fromarray(np.ascontiguousarray(img[:, :, 3]), 'L').resize((tw, th), Image.Resampling.BOX))
    return np.concatenate([rgb, a[:, :, None]], 2)


def blurred_psnr(a, b):
    """PSNR of the colour after a 3 x 3 box blur of both, as the screen's
    filtering would see them (dither grain counts as the average it makes)"""
    def blur(x):
        x = x[:, :, :3].astype(np.float32)
        p = np.pad(x, ((1, 1), (1, 1), (0, 0)), mode='edge')
        return sum(p[dy:dy + x.shape[0], dx:dx + x.shape[1]] for dy in range(3) for dx in range(3)) / 9.0
    return psnr(blur(a), blur(b))


def quality_note(cls, old, new):
    st = QUALITY_STATS.setdefault(cls, [0, 0.0, 0.0])
    st[0] += 1
    st[1] += old
    st[2] += new


# ---- The exact set (docs/review_clut4exact.md). A CLUT8 texture whose
# texels, over every level, use 16 palette words or fewer draws the same
# texel for texel at 4 bits, and the GE samples CLUT4 faster from half the
# bytes (design_clut4w.md). The 16-colour rule in bake_one leaves many such
# textures at 8 bits: it skips lightmaps, level, sky and detail maps, keeps
# HUD sheets at 8 and narrow maps whole, and its error test isn't needed
# where nothing is lost. Each texture switched is proved in the bake: its
# levels read back from both packings give the same words, and the runtime's
# average (psp_tex_average) the same colour; any that don't stay CLUT8.
# --no-clut4-exact keeps them all at 8 bits.
CLUT4_EXACT = True
# the pack flag that tells psp_tex_average to sample such a texture where its
# CLUT8 layout's bytes were: object lighting and the flashlight then take
# the same average colour from it as before
CLUT4_EXACT_FLAG = 2
CLUT4_EXACT_MADE = []     # (class, CLUT8 bytes, CLUT4 bytes) per texture switched
CLUT4_EXACT_DIFFER = []   # the class of each that read back differently (kept at 8 bits)


def palette_words(pal, alpha):
    """each palette entry as the pack stores it: 8888 (r g b a bytes, one
    little-endian word) or GU_PSM_5650 (r in the low bits)"""
    if alpha:
        return np.ascontiguousarray(pal.astype(np.uint8)).view('<u4').ravel()
    r, g, bl = pal[:, 0].astype(np.uint16), pal[:, 1].astype(np.uint16), pal[:, 2].astype(np.uint16)
    return ((r >> 3) | ((g >> 2) << 5) | ((bl >> 3) << 11)).astype('<u2')


def pack_level(ix, w, h, bits):
    """one level's indices, swizzled (4 bits: the low nibble first)"""
    row, rows = level_bytes(w, h, bits)
    if bits == 4:
        pixels = np.zeros((rows, row * 2), np.uint8)
        pixels[:h, :w] = ix
        packed = (pixels[:, 0::2] & 15) | ((pixels[:, 1::2] & 15) << 4)
        return swizzle_psp(packed.astype(np.uint8).tobytes(), row, rows)
    padded = np.zeros((rows, row), np.uint8)
    padded[:h, :w] = ix
    return swizzle_psp(padded.tobytes(), row, rows)


def level_sizes(w, h, levels):
    """each level's size, halved as the runtime halves them (psp_tex_bind)"""
    sizes = []
    for _ in range(levels):
        sizes.append((w, h))
        w, h = max(1, w // 2), max(1, h // 2)
    return sizes


def read_levels(t):
    """(each level's indices, the palette's words) read back from a texture's
    bytes as the GE reads them"""
    four = t['psm'] == 4
    words = np.frombuffer(t['data'], '<u4' if t['alpha'] else '<u2', 16 if four else 256)
    at = len(words) * words.itemsize
    out = []
    for w, h in level_sizes(t['w'], t['h'], t['levels']):
        row, rows = level_bytes(w, h, 4 if four else 8)
        a = np.frombuffer(t['data'], np.uint8, row * rows, at).reshape(rows // 8, row // 16, 8, 16)
        a = a.transpose(0, 2, 1, 3).reshape(rows, row)
        if four:
            a = np.stack([a & 15, a >> 4], 2).reshape(rows, row * 2)
        out.append(a)
        at += row * rows
    return out, words


def runtime_average(t):
    """psp_tex_average's colour (psp_tex.c), worked out as it works it out: a
    sample of the top level's bytes, padding included, through the palette.
    An exact CLUT4 texture is sampled where its CLUT8 layout's bytes were"""
    levels, words = read_levels(t)
    top, (w, h) = levels[0], (t['w'], t['h'])
    clut8_order = t['psm'] == 5 or t.get('flags', 0) & CLUT4_EXACT_FLAG
    row, rows = level_bytes(w, h, 8 if clut8_order else 4)
    total = row * rows
    at = np.arange(0, total, total // 2048 + 1)
    block, inside = at >> 7, at & 127
    y = block // (row // 16) * 8 + (inside >> 4)
    x = block % (row // 16) * 16 + (inside & 15)
    if clut8_order:
        # the texel each CLUT8 byte held; a padding byte reads entry 0
        ix = np.where((x < w) & (y < h), top[np.minimum(y, top.shape[0] - 1), np.minimum(x, top.shape[1] - 1)], 0)
    else:
        ix = np.stack([top[y, 2 * x], top[y, 2 * x + 1]], 1).ravel()
    c = words[ix].astype(np.int64)
    if t['alpha']:
        rgb = np.stack([c & 0xFF, (c >> 8) & 0xFF, (c >> 16) & 0xFF], 1)
    else:
        rgb = np.stack([(c & 31) * 255 // 31, ((c >> 5) & 63) * 255 // 63, (c >> 11) * 255 // 31], 1)
    return tuple(int(v) for v in rgb.sum(0) // len(ix))


def clut4_exact(made, cls, words, level_ix):
    """made at 4 bits when its levels use 16 palette words or fewer and read
    back the same, else made as it was"""
    w, h = made['w'], made['h']
    row, rows = level_bytes(w, h)
    used = np.unique(np.concatenate([ix.ravel() for ix in level_ix]))
    # a CLUT8 top level's padding bytes are index 0, and psp_tex_average
    # samples them: where there are any, entry 0's word comes too, first
    order = ([0] if row > w or rows > h else []) + [int(i) for i in used]
    at = {}
    for i in order:
        at.setdefault(int(words[i]), len(at))
    if len(at) > 16:
        return made
    remap = np.zeros(len(words), np.int64)
    for i in used:
        remap[i] = at[int(words[i])]
    four = np.zeros(16, words.dtype)
    four[list(at.values())] = list(at.keys())
    levels = [pack_level(remap[ix], lw, lh, 4) for ix, (lw, lh) in zip(level_ix, level_sizes(w, h, len(level_ix)))]
    made4 = dict(made, psm=4, flags=CLUT4_EXACT_FLAG, data=four.tobytes() + b''.join(levels))
    (as8, words8), (as4, words4) = read_levels(made), read_levels(made4)
    same = all(np.array_equal(words8[a[:lh, :lw]], words4[b[:lh, :lw]])
               for a, b, (lw, lh) in zip(as8, as4, level_sizes(w, h, made['levels'])))
    if not same or runtime_average(made) != runtime_average(made4):
        CLUT4_EXACT_DIFFER.append(cls)
        return made
    CLUT4_EXACT_MADE.append((cls, len(made['data']), len(made4['data'])))
    return made4


def clut4_exact_report():
    """the pack's 'clut4 exact' lines (the bake's log echoes them): classes,
    counts and bytes, no names. Shared data is counted once per texture
    baked, as the textures were made"""
    before = sum(b for _, b, _ in CLUT4_EXACT_MADE)
    after = sum(a for _, _, a in CLUT4_EXACT_MADE)
    by_class = {}
    for cls, b, a in CLUT4_EXACT_MADE:
        n, saved = by_class.get(cls, (0, 0))
        by_class[cls] = (n + 1, saved + b - a)
    print('clut4 exact, %s pack: %d textures to 16 colours, %d -> %d bytes (saves %d KB); read back: %d the same, '
          '%d differ (kept at 8 bits)%s' % (PROFILE_PACK, len(CLUT4_EXACT_MADE), before, after, (before - after) // 1024,
                                            len(CLUT4_EXACT_MADE), len(CLUT4_EXACT_DIFFER),
                                            '' if CLUT4_EXACT else ' (--no-clut4-exact: off)'), file=sys.stderr)
    if by_class:
        print('clut4 exact, %s pack, by class: %s' % (PROFILE_PACK, '; '.join(
            '%s %d (%d KB)' % (cls, n, saved // 1024) for cls, (n, saved) in sorted(by_class.items()))), file=sys.stderr)


# ---- The CLUT4 look set (docs/review_clut4look.md, the census in
# design_clut4w.md). Lightmaps and HUD sheets that hold CLUT8's look at 16
# colours, for half the bytes in the VRAM texture cache and the GE's fastest
# sampling. Unlike the exact set it is lossy, so each texture must pass the
# census's strict bar: one palette for every level, each level within 1 dB of
# CLUT8's screen PSNR (or over 45 dB) and no texel more than one 5-bit step
# past CLUT8's worst; anything that misses stays CLUT8. The chain is CLUT8's
# own, so nothing changes with distance but the colours. --no-clut4-look
# leaves them all at 8 bits.
CLUT4_LOOK = True
CLUT4_LOOK_CLASSES = ('lightmap', 'ui/hud')
CLUT4_LOOK_MADE = []     # (class, CLUT8 bytes, CLUT4 bytes, lowest dB at 8, lowest dB at 4)
CLUT4_LOOK_FAILED = []   # the class of each tried that missed the bar
SCREEN_SHIFT = np.array([3, 2, 3], np.uint8)


def look_errors(orig, made, alpha, tested):
    """one level's error at the screen's 5/6/5 bits, as the CLUT4W census
    measures it: the colour where it can be seen, and the alpha (a cut-out
    by the texels whose test turns over, any other by its own dB and worst)"""
    o = ((orig[:, :, :3] >> SCREEN_SHIFT) << SCREEN_SHIFT).astype(np.int16)
    m = ((made[:, :, :3] >> SCREEN_SHIFT) << SCREEN_SHIFT).astype(np.int16)
    seen = orig[:, :, 3] > 127 if tested else np.ones(orig.shape[:2], bool)
    if not seen.any():
        seen = np.ones(orig.shape[:2], bool)
    r = dict(db=psnr(o[seen], m[seen]), worst=int(np.abs(o - m)[seen].max(1).max()))
    if alpha:
        oa, ma = orig[:, :, 3], made[:, :, 3]
        if tested:
            r['flips'] = float(((oa > 127) != (ma > 127)).mean())
        else:
            r.update(adb=psnr(oa, ma), aworst=int(np.abs(oa.astype(np.int16) - ma.astype(np.int16)).max()))
    return r


def look_passes(e4, e8):
    """the census's strict bar (CLUT4W_BARS['strict'] on the clut4w branch)"""
    if e4['db'] < min(e8['db'] - 1.0, 45.0) or e4['worst'] > e8['worst'] + 8:
        return False
    if 'adb' in e4 and (e4['adb'] < min(e8['adb'] - 1.0, 45.0) or e4['aworst'] > e8['aworst'] + 8):
        return False
    return not ('flips' in e4 and e4['flips'] > e8['flips'] + 0.001)


def clut4_look(made, b, cls, pal, alpha, level_imgs, level_ix):
    """made at 4 bits with one 16-colour palette when every level passes the
    strict bar against CLUT8's own error, else made as it was"""
    every = np.concatenate([i.reshape(-1, 1, 4) for i in level_imgs], 0)
    _, pal16, _, _ = make_palette(every, 16, cls)
    pal16 = pal16[:16]
    tested = alpha and b['tag'] in ALPHA_TESTED
    rows, four_ix = [], []
    for img, ix in zip(level_imgs, level_ix):
        ix4 = nearest(img, pal16, alpha)
        four_ix.append(ix4)
        rows.append((look_errors(img, pal[ix], alpha, tested), look_errors(img, pal16[ix4], alpha, tested)))
    if not all(look_passes(e4, e8) for e8, e4 in rows):
        CLUT4_LOOK_FAILED.append(cls)
        return made
    w, h = made['w'], made['h']
    levels = [pack_level(ix4, lw, lh, 4) for ix4, (lw, lh) in zip(four_ix, level_sizes(w, h, len(four_ix)))]
    data = palette_words(pal16, alpha).tobytes() + b''.join(levels)
    CLUT4_LOOK_MADE.append((cls, len(made['data']), len(data), min(r[0]['db'] for r in rows),
                            min(r[1]['db'] for r in rows)))
    return dict(made, psm=4, flags=0, data=data)


def clut4_look_report():
    """the pack's 'clut4 look' lines (the bake's log echoes them): classes,
    counts, bytes and the lowest level dB, no names"""
    if not CLUT4_LOOK:
        return
    before = sum(r[1] for r in CLUT4_LOOK_MADE)
    after = sum(r[2] for r in CLUT4_LOOK_MADE)
    print('clut4 look, %s pack: %d textures to 16 colours (lossy, strict bar), %d -> %d bytes (saves %d KB); '
          '%d tried missed the bar (kept at 8 bits)' % (PROFILE_PACK, len(CLUT4_LOOK_MADE), before, after,
                                                        (before - after) // 1024, len(CLUT4_LOOK_FAILED)), file=sys.stderr)
    for cls in CLUT4_LOOK_CLASSES:
        rows = [r for r in CLUT4_LOOK_MADE if r[0] == cls]
        if rows:
            print('clut4 look, %s pack, %s: %d textures, saves %d KB; lowest level dB 8 bits median %.1f, 4 bits median '
                  '%.1f, lowest %.1f; missed %d' % (
                      PROFILE_PACK, cls, len(rows), sum(r[1] - r[2] for r in rows) // 1024,
                      float(np.median([r[3] for r in rows])), float(np.median([r[4] for r in rows])),
                      min(r[4] for r in rows), CLUT4_LOOK_FAILED.count(cls)), file=sys.stderr)


def seams_broken(m, b, cls):
    """(spots broken, worst step) when the page b is halved as bake_one
    halves it, against the page at full size; (0, 0) for a page without seams.
    Notes the page's spots broken at each of SEAM_STEPS_LOGGED for the log,
    so one bake shows what any SEAM_STEP would keep"""
    page = (b['tag'], b['index'])
    if page not in LIGHTMAP_SEAMS:
        return 0, 0.0
    from lightmap_masks import dilate, seam_steps
    act, tw, th = target(b, cls)
    tw, th = max(8, tw // LIGHTMAP_SCALE), max(8, th // LIGHTMAP_SCALE)
    img = decode(m, b)
    if img is None or act != 'clut' or b['type'] != '2d' or not pow2(tw) or not pow2(th):
        return 0, 0.0
    half = img
    if page in LIGHTMAP_MASKS:
        half = dilate(img, LIGHTMAP_MASKS[page], 2 * LIGHTMAP_SCALE + 2)
    half = resample(half, tw, th, cls)
    seams = LIGHTMAP_SEAMS[page]
    worse = seam_steps(half, seams) - seam_steps(img, seams)
    broken = int((worse > SEAM_STEP).sum())
    LIGHTMAP_SEAM_LOG.append([b['tag'], b['index'], len(worse), [int((worse > t).sum()) for t in SEAM_STEPS_LOGGED],
                              float(worse.max()), broken >= SEAM_SPOTS_BROKEN])
    return broken, float(worse.max())


@profiled
def bake_one(m, b, cls, look=None, model_look=None, full_hud=True, meter=False, glow=None, own_colours=True,
             relief=None, image=None, whole=False, level_glow=None):
    act, tw, th = target(b, cls)
    if cls == 'lightmap' and LIGHTMAP_SCALE > 1 and not whole:
        tw, th = max(8, tw // LIGHTMAP_SCALE), max(8, th // LIGHTMAP_SCALE)
    # HUD sheets (meters, numbers, reticles) at their full size: halved, the
    # digits and meter cells fall below a texel each (build 39). The big
    # menu screens keep the halving, and so do the split-screen scope masks
    # (one player on a PSP: never drawn)
    if cls == 'ui/hud' and full_hud and act == 'clut' and max(b['w'], b['h']) <= 256:
        tw, th = b['w'], b['h']
    limit = MAXIMUM_SIDE
    if cls == 'objects (soso)' and '\\fp\\' not in b['tag']:
        limit = OBJECT_SIDE
    if FULL_DETAIL and act == 'clut' and full_detail(b['tag']):
        tw, th, limit = b['w'], b['h'], 512
    # the showcase: every map at its own size, up to the GE's 512 a side
    if SHOWCASE and act == 'clut' and cls != 'lightmap':
        tw, th, limit = b['w'], b['h'], 512
    while cls != 'lightmap' and max(tw, th) > limit:
        tw, th = max(1, tw // 2), max(1, th // 2)
    cutout = look.get('cutout') if look and CUTOUT_FROM_BUMP and CUTOUT_FINE and cls == 'level (senv)' else None
    if cutout and act == 'clut' and tw > 0 and th > 0:
        before = (tw, th)
        across = lambda: min(abs(cutout['u']) * cutout['bump']['w'] / tw, abs(cutout['v']) * cutout['bump']['h'] / th)
        while max(tw, th) * 2 <= 512 and across() > CUTOUT_FINE_MOST:
            tw, th = tw * 2, th * 2
        if (tw, th) != before:
            CUTOUTS_ENLARGED.append((b['tag'], before, (tw, th)))
    if act != 'clut' or b['type'] != '2d' or not pow2(tw) or not pow2(th):
        return None
    img = decode(m, b) if image is None else image
    if img is None:
        return None
    source = img
    if cls == 'lightmap' and LIGHTMAP_SCALE > 1 and not whole and (b['tag'], b['index']) in LIGHTMAP_MASKS:
        from lightmap_masks import dilate
        img = dilate(img, LIGHTMAP_MASKS[(b['tag'], b['index'])], 2 * LIGHTMAP_SCALE + 2)
    if look:
        img = apply_look(img, look, m)
    if relief:
        img = apply_relief(img, relief, m)
        if img is None:
            return None
    if model_look:
        img = apply_model_look(img, model_look)
    if glow is not None:
        img = apply_glow(img, glow)
    if level_glow is not None:
        img = apply_level_glow(img, level_glow)
    own_levels = None
    if cls == 'detail':
        # the Xbox's own mip chain belongs to the bitmap, not to an image given
        # in its place (whose size b now carries, so the chain's offsets
        # would run past the bitmap's pixels)
        own_levels = None if os.environ.get('TEXPACK_DETAIL_DOWNSCALE') or image is not None \
            else detail_levels(m, b, tw, th)
        if not own_levels:
            img = neutral_detail(img)
    small = own_levels[0] if own_levels else resample(img, tw, th, cls)
    refit = None
    if cls == 'lightmap' and LIGHTMAP_SCALE > 1 and not whole and (b['tag'], b['index']) in LIGHTMAP_CHARTS:
        from lightmap_masks import chart_halve
        fitted, changed, stats = chart_halve(source, small, LIGHTMAP_CHARTS[(b['tag'], b['index'])],
                                             SEAM_STEP, CHART_SPOTS, CHART_MAX)
        LIGHTMAP_CHART_LOG.append((b['tag'], b['index'], stats['charts'], stats['texels'], stats['before'],
                                   stats['after'], stats['skipped'], stats['sizes']))
        if changed is not None and changed.any():
            refit = (fitted, changed)
    # build 116: a meter shader's map (the plasma weapons' gauges, the
    # Warthog's meters) takes the grid too, always: the GE works its colours
    # out per palette entry from the blue (the position along the meter) and
    # the alpha (the shape), as the HUD's (psp_ge.c, meter_palette), and an
    # ordinary palette's entries are averages of both (b113_candidates.md #16)
    grid = (cls == 'ui/hud' and grey_meter(small)) or meter
    shape = alpha_shape(b, cls, look) if not grid and not own_levels else None
    if SHOWCASE and not SHOWCASE_PALETTES and own_colours and cls in DIRECT_CLASSES and not grid and not own_levels:
        dropped = 0
        while (b['w'] >> dropped) > tw and dropped < 12:
            dropped += 1
        alpha = bool(small[:, :, 3].min() < 255)
        wide = alpha or SHOWCASE_32
        levels = direct_levels(small, tw, th, min(8, max(1, b['mips'] + 1 - dropped)), wide)
        # clut 0xFF: no palette, so the runtime's palette reads pass it by
        return dict(w=tw, h=th, levels=len(levels), clut=0xFF, alpha=alpha, data=b''.join(levels),
                    psm=3 if wide else 0)
    if grid:
        grid_off = grid_offset(small)
        grid_a = meter_alphas(small) if METER_ALPHAS else None
        if grid_a is not None:
            meter_alpha_note(b['tag'], small, grid_a)
        idx = grid_index(small, grid_off, grid_a)
        pal, alpha = grid_palette(grid_off, grid_a or GRID_ALPHAS), True
    elif own_levels:
        # one palette for every level: the faded levels' near-greys need
        # entries of their own, which the top level alone wouldn't earn
        every = np.concatenate([l.reshape(-1, 1, 4) for l in own_levels], 0)
        _, pal, _, alpha = make_palette(every, 256, cls)
        idx = nearest(small, pal, alpha)
    # the quality steps (above): not for the HUD and menus (drawn at their
    # own size, where grain would show), the grid's exact entries, or a
    # detail map's own levels
    diffusing = QUALITY >= 2 and not grid and not own_levels and cls not in ('ui/hud', 'detail')
    if diffusing:
        pal, alpha = fit_palette(small, 256)
        idx = diffuse(small, pal, alpha, 256)
        rec = as_drawn(pal, alpha)[idx]
        if QUALITY_REPORT:
            old_idx, old_pal, _, _ = make_palette(small, 256, cls)
            quality_note(cls, blurred_psnr(small, as_drawn(old_pal, alpha)[old_idx]), blurred_psnr(small, rec))
    elif not grid and not own_levels:
        idx, pal, rec, alpha = make_palette(small, 256, cls, shape)
        if KMEANS_COMPARE:
            kmeans_compare(b['tag'], small, 256, rec)
    if refit is not None:
        # only the refitted texels change: the palette and the dithering
        # elsewhere are the plain halving's, so the rest of the page is too
        fitted, changed = refit
        idx = idx.copy()
        idx[changed] = nearest(fitted[changed][None], pal, alpha)[0]
    # 16 colours where that is no worse than the 16-bit frame buffer itself
    # (within 1 dB of its 5650 rounding): half the bytes (research/baker)
    bits = 8
    if not grid and tw >= 32 and th >= 8 and cls not in ('level (senv)', 'lightmap', 'sky', 'detail'):
        idx4, pal4, rec4, _ = make_palette(small, 16, cls, shape)
        if KMEANS_COMPARE:
            kmeans_compare(b['tag'], small, 16, rec4)
        reference = small.copy()
        reference[:, :, 0] &= 0xF8
        reference[:, :, 1] &= 0xFC
        reference[:, :, 2] &= 0xF8
        if psnr(small, rec4) >= psnr(small[:, :, :3], reference[:, :, :3]) - 1.0:
            if cls == 'ui/hud' and not HUD_CLUT4:
                HUD_KEPT_8.append(b['tag'])
            elif cls in SHAPE_CLASSES and not EFFECTS_CLUT4:
                # an effect's soft glow or a see-through layer's fade is a
                # gradient in its colour and alpha that the test above lets
                # through while 16 colours step it: the hard-edged thrusters,
                # the solid plasma glows, the water layers' borders
                EFFECTS_KEPT_8.append((b['tag'], b['index'], level_bytes_total(tw, th, 8) - level_bytes_total(tw, th, 4)))
            else:
                idx, pal, rec, bits = idx4, pal4[:16], rec4, 4
                if diffusing:
                    pal16, _ = fit_palette(small, 16)
                    pal = pal16[:16]
                    idx = diffuse(small, pal, alpha, 16)
    words = palette_words(pal, alpha)
    palette = words.tobytes()
    clut = 3 if alpha else 0
    levels = []
    level_ix = []
    level_imgs = []    # each level's image, for the look set's bar
    w, h, level_img = tw, th, small
    chain_img = small
    fix_coverage = ALPHA_COVERAGE and alpha and b['tag'] in ALPHA_TESTED and not own_levels and not grid
    top_coverage = coverage(small[:, :, 3]) if fix_coverage else 0.0
    # build 49: no more levels than the Xbox bitmap has (its extra mip count
    # + 1, less the levels dropped above the baked size). Blood Gulch's 15
    # lightmaps have none on the Xbox; baked with a chain, their small levels
    # mixed each lightmap page's charts and dark gutters together, and
    # surfaces seen edge-on or far off took that dark mix (the dark lines,
    # gone with "nomip", b47)
    dropped = 0
    while (b['w'] >> dropped) > tw and dropped < 12:
        dropped += 1
    source_levels = max(1, b['mips'] + 1 - dropped)
    if own_levels:
        source_levels = len(own_levels)
    if ALPHA_AUDIT and alpha and not grid:
        alpha_audit(b, cls, small, pal[idx], bits, shape)
    while len(levels) < min(8, source_levels):
        ix = idx if not levels else (grid_index(level_img, grid_off, grid_a) if grid else
                                     diffuse(level_img, pal, alpha, len(pal)) if diffusing else
                                     nearest(level_img, pal, alpha, split=shape is not None))
        levels.append(pack_level(ix, w, h, bits))
        level_ix.append(ix)
        level_imgs.append(level_img)
        # a detail map tiles up to a hundred times across a surface, so it
        # mips down to a single texel: its average, the neutral grey (build 31)
        if cls == 'detail':
            if w <= 1 or h <= 1:
                break
        elif w <= 8 or h <= 8 or (bits == 4 and w < 32):
            break
        # the HUD is drawn flat on the screen at near its own size: no mip
        # levels (a quarter of its bytes, build 39)
        if cls == 'ui/hud':
            break
        w, h = max(1, w // 2), max(1, h // 2)
        if own_levels and len(levels) < len(own_levels):
            level_img = own_levels[len(levels)]
        else:
            chain_img = resample(chain_img, w, h, cls)
            level_img = keep_coverage(chain_img, top_coverage) if fix_coverage else chain_img
    data = palette + b''.join(levels)
    made = dict(w=tw, h=th, levels=len(levels), clut=clut, alpha=alpha, data=data, psm=4 if bits == 4 else 5)
    # not the grid (its entries are positions along a meter) or a multipurpose
    # map (the change colour reads 256 entries); the showcase stays as it is
    if CLUT4_EXACT and bits == 8 and not grid and own_colours and not SHOWCASE:
        made = clut4_exact(made, cls, words, level_ix)
    # the meters' grid and the multipurpose maps read their palettes; the
    # showcase keeps every colour
    if CLUT4_LOOK and made['psm'] == 5 and cls in CLUT4_LOOK_CLASSES and not grid and own_colours and not SHOWCASE:
        made = clut4_look(made, b, cls, pal, alpha, level_imgs, level_ix)
    return made


CHANGE_MASK_FLAG = 8  # the change mask's CLUT follows the levels
CPU_READS_FLAG = 16   # the CPU reads this texture's palette: kept in main RAM


def change_mask(e):
    """a multipurpose map's change mask, as the GE's CLUT for the change pass
    (psp_ge.c, multipurpose_pass): the palette's blue (the Xbox's change
    channel) as a grey 8888 word, alpha full. The Media Engine replays the
    pass and can't read a texture in VRAM, so the bake works it out"""
    _, words = read_levels(e)
    w = words.astype(np.uint32)
    blue = (w >> 16) & 0xFF if e['alpha'] else (w >> 11) * 255 // 31
    return (0xFF000000 | (blue << 16) | (blue << 8) | blue).astype('<u4').tobytes()


def runtime_reads(entries):
    """what the runtime would otherwise read out of the textures themselves,
    made here (design_b25_bake.md): each multipurpose map's change mask after
    its levels (64-byte aligned, the last bytes of its data), the meter maps
    marked for main RAM, and every texture's average colour as the 'TAV1'
    table's bytes, in directory order (0: none, a texture in its own colours)"""
    masks = meters = 0
    for e in entries:
        reads = e.get('reads', '')
        if 'multipurpose' in reads and e['psm'] in (4, 5):
            mask = change_mask(e)
            data = bytes(e['data'])
            e['data'] = data + b'\0' * (-len(data) % 64) + mask
            e['flags'] = e.get('flags', 0) | CHANGE_MASK_FLAG
            masks += len(e['data']) - len(data)
        if 'meter' in reads:
            e['flags'] = e.get('flags', 0) | CPU_READS_FLAG
            meters += len(e['data'])
    averages = []
    for e in entries:
        if 'average' in e:
            r, g, b = e['average']
            averages.append(0xFF000000 | (b << 16) | (g << 8) | r)
        elif e['psm'] in (4, 5):
            r, g, b = runtime_average(e)
            averages.append(0xFF000000 | (b << 16) | (g << 8) | r)
        else:
            averages.append(0)
    print('runtime reads: %d change masks (+%d KB), %d meter maps kept in RAM (%d KB), averages for %d of %d textures'
          % (sum(1 for e in entries if e.get('flags', 0) & CHANGE_MASK_FLAG), masks // 1024,
             sum(1 for e in entries if e.get('flags', 0) & CPU_READS_FLAG), meters // 1024,
             sum(1 for v in averages if v), len(entries)), file=sys.stderr)
    return struct.pack('<4sI', b'TAV1', len(averages)) + struct.pack('<%dI' % len(averages), *averages)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('map')
    ap.add_argument('out')
    ap.add_argument('--limit', type=int, default=0)
    ap.add_argument('--lightmap-scale', type=int, default=1, help='asset track: lightmaps shrunk by this (2: half)')
    ap.add_argument('--lightmap-seam-whole', action='store_true',
                    help='asset track: pages whose halving breaks a seam also baked whole, under the key | 1')
    ap.add_argument('--lm-chart-halve', action='store_true',
                    help='asset track: chart-aware halving (charts whose halving breaks a seam refitted on their own reads)')
    ap.add_argument('--chart-max', type=int, default=-1,
                    help='asset track, --lm-chart-halve: only charts at most this many texels across (0: any; '
                         '-1: CHART_MAX)')
    ap.add_argument('--seam-step', type=int, default=0,
                    help='asset track: a seam spot is broken when halving widens its step by more than this (1-255; 0: SEAM_STEP)')
    ap.add_argument('--seam-spots', type=int, default=0,
                    help='asset track: a page (--lightmap-seam-whole) or chart (--lm-chart-halve) with this many '
                         'broken seam spots is treated (0: SEAM_SPOTS_BROKEN, CHART_SPOTS)')
    ap.add_argument('--pgeo', default=None, help='the map\'s geometry pack (default: beside the map) for the lightmap masks')
    ap.add_argument('--alpha-coverage', action='store_true', help='asset track: alpha-tested textures keep their coverage at every level')
    ap.add_argument('--full-detail', default='', help='classes kept at full size: fp, weapons, vehicles, characters')
    ap.add_argument('--hud-clut4', action='store_true', help='HUD sheets may take 16 colours again (as before build 120)')
    ap.add_argument('--no-clut4-look', action='store_true',
                    help='lightmaps and HUD sheets stay at 8 bits even where 16 colours pass the CLUT4W strict bar '
                         '(docs/review_clut4look.md)')
    ap.add_argument('--no-clut4-exact', action='store_true',
                    help='textures of 16 colours or fewer that the 16-colour rule leaves at 8 bits stay at 8 bits')
    ap.add_argument('--no-meter-alphas', action='store_true',
                    help="the meters' grid keeps the fixed alphas 0, 85, 170, 255 (as before)")
    ap.add_argument('--no-alpha-split', action='store_true',
                    help='alpha-tested maps\' palettes fitted as one 4-D k-means, the alpha test\'s sides '
                         'mixed in an entry (as before)')
    ap.add_argument('--drawn-by-shape', action='store_true',
                    help='see-through shaders that blend by their alpha draw the map that carries most of what the '
                         'frame shows over dark and light backgrounds alike (seethrough.DRAWN_BY_SHAPE)')
    ap.add_argument('--no-composites', action='store_true',
                    help='no see-through composites: each shader drawn as one of its maps and a tint, as before '
                         '(seethrough.COMPOSITES)')
    ap.add_argument('--no-cutout-fine', action='store_true',
                    help='cut-outs finer than their baked base map baked at the usual size, as before (CUTOUT_FINE)')
    ap.add_argument('--base-as-bump', action='store_true',
                    help='a map a shader names as both its base and its bump map dropped as a bump map, as before '
                         '(refs.BASE_OVER_BUMP)')
    ap.add_argument('--no-cutout-bump', action='store_true',
                    help='alpha-tested level shaders cut out by their base map\'s alpha, as before, not their bump '
                         'map\'s (CUTOUT_FROM_BUMP)')
    ap.add_argument('--look-alpha-first', action='store_true',
                    help='a level base map keeps its alpha only when the first shader that draws it tests it (as '
                         'before; LOOK_ALPHA_ANY)')
    ap.add_argument('--effects-clut4', action='store_true',
                    help='effects and see-through shaders\' maps may take 16 colours (as before)')
    ap.add_argument('--tints-from-means', action='store_true',
                    help='see-through tints from every map\'s mean, drawn on the runtime\'s own map (as before build 121)')
    ap.add_argument('--no-glow-maps', action='store_true',
                    help='no glow maps: the self-illumination pass adds its colour by the mask alone')
    ap.add_argument('--no-level-glow-maps', action='store_true',
                    help='no level glow maps: a level shader\'s self-illumination isn\'t drawn (as before)')
    ap.add_argument('--premultiplied-masks', action='store_true',
                    help='detail masks read from the RGBA-resized multipurpose map, darkened under its alpha (as before)')
    ap.add_argument('--no-relief', action='store_true',
                    help="the level's base maps without their bump maps' relief (as before build 139)")
    ap.add_argument('--no-cube-patches', action='store_true',
                    help="no cube map averages in the pack (as build 139)")
    ap.add_argument('--no-gel-faces', action='store_true',
                    help="a light's gel patch with the single gel average only, the behaviour before per-face averages")
    ap.add_argument('--no-sphere-maps', action='store_true',
                    help="a model reflection's cube map as its average alone, without the sphere map the reflection pass draws")
    ap.add_argument('--no-gel-cube', action='store_true',
                    help="a light's gel patch with its face means only, without the cube the light pass samples per vertex")
    ap.add_argument('--relief-unfiltered', action='store_true',
                    help="the bump relief sampled at the base's texels without averaging (as build 139)")
    ap.add_argument('--showcase', action='store_true',
                    help='the main menu\'s pack: every map at its own size up to 512, objects, sky and level maps '
                         'in their own colours (ui.map; the levels never)')
    ap.add_argument('--showcase-32', action='store_true',
                    help='with --showcase: opaque maps in 32-bit colour too (by default 16-bit, dithered; '
                         'the screen is 16-bit)')
    ap.add_argument('--quality', type=int, default=1, choices=(1, 2),
                    help='1: palettes fitted in RGB, texels mapped to the nearest entry, sizes by a box filter '
                         '(the default); 2: Oklab fit, error diffusion, Lanczos (closer to the source by 2-9 dB, '
                         'but indistinguishable on the PSP-1000, and twice the palette step\'s bake time)')
    ap.add_argument('--quality-report', action='store_true',
                    help='also fit each texture the old way and log both against the source, per class')
    ap.add_argument('--showcase-palettes', action='store_true',
                    help='with --showcase: every map on a palette (256 colours), at its own size')
    ap.add_argument('--palette-cache', default=None,
                    help='a folder where palettes are kept and read back by their input pixels (the bake passes one '
                         'for all its packs); the pack comes out the same with it or without')
    ap.add_argument('--fast-palettes', action='store_true',
                    help='each k-means settled on a subsample before its last rounds on the whole sample '
                         '(bitmap_bake.fast_kmeans): sooner, not the same palettes')
    a = ap.parse_args()
    started = time.perf_counter()
    global PALETTE_CACHE, FAST_PALETTES, PROFILE_PACK
    PALETTE_CACHE, FAST_PALETTES = a.palette_cache, a.fast_palettes
    if PALETTE_CACHE:
        os.makedirs(PALETTE_CACHE, exist_ok=True)
    stem = os.path.splitext(os.path.basename(a.out))[0]
    PROFILE_PACK = stem.split('_', 1)[1] if '_' in stem else 'main'
    global FULL_DETAIL, HUD_CLUT4, PREMULTIPLIED_MASKS, SHOWCASE, SHOWCASE_32, SHOWCASE_PALETTES
    SHOWCASE_PALETTES = a.showcase_palettes
    global QUALITY, QUALITY_REPORT
    QUALITY, QUALITY_REPORT = a.quality, a.quality_report
    PREMULTIPLIED_MASKS = a.premultiplied_masks
    SHOWCASE = a.showcase
    global NO_RELIEF, OLD_RELIEF, NO_CUBE_PATCHES, NO_GEL_FACES, NO_GEL_CUBE, NO_SPHERE_MAPS
    NO_GEL_FACES = a.no_gel_faces
    NO_SPHERE_MAPS = a.no_sphere_maps
    NO_GEL_CUBE = a.no_gel_cube
    NO_RELIEF = a.no_relief
    OLD_RELIEF = a.relief_unfiltered
    NO_CUBE_PATCHES = a.no_cube_patches
    SHOWCASE_32 = a.showcase_32
    HUD_CLUT4 = a.hud_clut4
    global CLUT4_EXACT
    CLUT4_EXACT = not a.no_clut4_exact
    global METER_ALPHAS
    METER_ALPHAS = not a.no_meter_alphas
    global CLUT4_LOOK
    # ui.map is the showcase (CLAUDE.md): its packs keep every colour
    CLUT4_LOOK = not a.no_clut4_look and os.path.splitext(os.path.basename(a.map))[0] != 'ui'
    FULL_DETAIL = set(c for c in a.full_detail.split(',') if c)
    if FULL_DETAIL - set(DETAIL_CLASSES):
        sys.exit('texpack: unknown --full-detail class(es): %s' % ', '.join(sorted(FULL_DETAIL - set(DETAIL_CLASSES))))
    d = Defs()
    m = CacheMap(a.map)
    global ALPHA_COVERAGE, ALPHA_TESTED, ALPHA_SPLIT, EFFECTS_CLUT4
    global LOOK_ALPHA_ANY, CUTOUT_FROM_BUMP, CUTOUT_FINE
    LOOK_ALPHA_ANY = not a.look_alpha_first
    CUTOUT_FROM_BUMP = not a.no_cutout_bump
    CUTOUT_FINE = not a.no_cutout_fine
    refs.BASE_OVER_BUMP = not a.base_as_bump
    ALPHA_SPLIT, EFFECTS_CLUT4 = not a.no_alpha_split, a.effects_clut4
    ALPHA_TESTED = alpha_tested_bitmaps(m, d)
    if a.alpha_coverage:
        ALPHA_COVERAGE = True
        print('alpha coverage kept for %d alpha-tested bitmaps' % len(ALPHA_TESTED), file=sys.stderr)
    roles = refs.bitmap_roles(m, d)
    # build 116: the bitmaps meter shaders draw (bake_one gives them the grid)
    meter_maps = set(tag for tag, tag_roles in roles.items() if any(r.startswith('smet:') for r in tag_roles))
    print('%d meter shader maps: the grid palette' % len(meter_maps), file=sys.stderr)
    B = bitmap_list(m, d)
    if a.limit:
        B = B[:a.limit]
    global LIGHTMAP_SCALE, LIGHTMAP_MASKS, LIGHTMAP_SEAMS, LIGHTMAP_WHOLE, SEAM_STEP, SEAM_SPOTS_BROKEN
    global LIGHTMAP_CHARTS, CHART_SPOTS, CHART_MAX
    LIGHTMAP_SCALE = max(1, a.lightmap_scale)
    SEAM_STEP = a.seam_step or SEAM_STEP
    SEAM_SPOTS_BROKEN = a.seam_spots or SEAM_SPOTS_BROKEN
    CHART_SPOTS = a.seam_spots or CHART_SPOTS
    CHART_MAX = a.chart_max if a.chart_max >= 0 else CHART_MAX
    if LIGHTMAP_SCALE > 1:
        from lightmap_masks import page_masks
        sizes = {(b['tag'], b['index']): (b['w'], b['h']) for b in B
                 if refs.classify(roles.get(b['tag'], ()), b) == 'lightmap'}
        pgeo = a.pgeo or os.path.splitext(a.map)[0] + '.pgeo'
        LIGHTMAP_MASKS = page_masks(a.map, pgeo, sizes)
        if a.lightmap_seam_whole:
            from lightmap_masks import page_seams
            LIGHTMAP_SEAMS = page_seams(a.map, pgeo, sizes)
            LIGHTMAP_WHOLE = True
        if a.lm_chart_halve:
            from lightmap_masks import page_charts
            LIGHTMAP_CHARTS = page_charts(a.map, pgeo, sizes)
        covered = sum(int(v.sum()) for v in LIGHTMAP_MASKS.values())
        total = sum(v.size for v in LIGHTMAP_MASKS.values())
        print('lightmaps 1/%d: %d of %d pages masked, %.0f%% of their texels in charts' % (
            LIGHTMAP_SCALE, len(LIGHTMAP_MASKS), len(sizes), 100.0 * covered / max(1, total)), file=sys.stderr)
    entries = []
    skipped = {}
    looks = level_looks(m, d, B)
    mlooks = model_looks(m, d, B)
    reliefs = relief_looks(m, d, B)
    relief_count = 0
    gels = gel_bitmaps(roles)
    print('%d level shaders: base maps baked with their detail maps' % len(looks), file=sys.stderr)
    for i, b in enumerate(B):
        cls = refs.classify(roles.get(b['tag'], ()), b)
        if cls == 'detail' and any('detail map' not in role for role in roles.get(b['tag'], ())):
            cls = 'detail (also drawn plain)'
        tag_roles = roles.get(b['tag'], ())
        full_hud = not tag_roles or any('splitscreen' not in role for role in tag_roles)
        meter = b['tag'] in meter_maps
        # a multipurpose map's palette is read for the change colour's mask
        own_colours = not any('multipurpose' in role for role in tag_roles)
        r = bake_one(m, b, cls, looks.get(b['address']), full_hud=full_hud, meter=meter, own_colours=own_colours)
        if r is None and b['type'] == 'cube' and b['index'] == 0 and not NO_CUBE_PATCHES:
            rgb = gel_average(m, b) if b['tag'] in gels else None
            faces = gel_faces(m, b) if rgb is not None and not NO_GEL_FACES else None
            cube = gel_cube(m, b) if faces is not None and not NO_GEL_CUBE else None
            sphere = sphere_map(m, b) if rgb is None and not NO_SPHERE_MAPS else None
            r = cube_patch(m, b, rgb, faces, cube, sphere)
            if r is not None:
                CUBE_PATCHES.append(b['tag'])
                if rgb is not None:
                    print('  light gel %s: six faces\' mean %d %d %d' % (b['tag'], rgb[0], rgb[1], rgb[2]),
                          file=sys.stderr)
                if faces is not None:
                    print('  light gel %s: faces %s' % (b['tag'], ' / '.join('%d %d %d' % tuple(f) for f in faces)),
                          file=sys.stderr)
                if cube is not None:
                    print('  light gel %s: its cube kept at %d a side (%dx%d source, brightest texel %d)'
                          % (b['tag'], GEL_CUBE_SIDE, b['w'], b['h'], int(cube[:, :, :3].max())), file=sys.stderr)
        if r is None:
            skipped[cls] = skipped.get(cls, 0) + 1
            continue
        r['key'] = b['address']
        r['name'] = b['tag']
        # what the runtime reads out of this texture (design_b25_bake.md):
        # the HUD's meters (a unit's shield and health panels, a weapon's
        # ammunition: 'meter bitmap' fields) go through meter_palette as the
        # meter shaders' maps do, without that shader's grid palette
        hud_meter = any('meter bitmap' in role for role in tag_roles)
        r['reads'] = ('multipurpose' if not own_colours else '') + (' meter' if meter or hud_meter else '')
        entries.append(r)
        if cls == 'lightmap' and LIGHTMAP_SCALE > 1 and LIGHTMAP_WHOLE:
            broken, worst = seams_broken(m, b, cls)
            if broken >= SEAM_SPOTS_BROKEN:
                v = bake_one(m, b, cls, whole=True)
                if v is not None:
                    v['key'] = b['address'] | 1
                    v['name'] = b['tag'] + ' (whole: its seams)'
                    entries.append(v)
                    LIGHTMAPS_KEPT_WHOLE.append((b['tag'], b['index'], broken, worst, len(v['data'])))
        if b['address'] in reliefs and not NO_RELIEF:
            v = bake_one(m, b, cls, looks.get(b['address']), full_hud=full_hud, meter=meter, own_colours=own_colours,
                         relief=reliefs[b['address']])
            if v is not None:
                v['key'] = b['address'] | 2
                v['name'] = b['tag'] + ' (with its bump relief)'
                entries.append(v)
                relief_count += 1
        if b['address'] in mlooks:
            v = bake_one(m, b, cls, looks.get(b['address']), mlooks[b['address']], full_hud=full_hud, meter=meter,
                         own_colours=own_colours)
            if v is not None:
                v['key'] = b['address'] | 1
                v['name'] = b['tag'] + ' (with its detail map)'
                entries.append(v)
                print('  %s: detail map folded in for %s' % (b['tag'], mlooks[b['address']]['shader']), file=sys.stderr)
        if i % 50 == 0:
            print('  %d/%d' % (i, len(B)), file=sys.stderr)
    print('cut-outs: %d level base maps alpha-tested by their bump maps\' alpha%s' % (
        len(set(CUTOUTS)), ': ' + ', '.join('%s (%s)' % c for c in sorted(set(CUTOUTS))[:12]) if CUTOUTS else ''),
        file=sys.stderr)
    print('cut-outs kept fine%s: %d base maps%s' % (
        '' if CUTOUT_FINE else ' (off: --no-cutout-fine)', len(set(CUTOUTS_ENLARGED)),
        ': ' + ', '.join('%s %dx%d -> %dx%d' % (t, w0, h0, w1, h1) for t, (w0, h0), (w1, h1) in sorted(set(CUTOUTS_ENLARGED)))
        if CUTOUTS_ENLARGED else ''), file=sys.stderr)
    if LIGHTMAP_SCALE > 1 and LIGHTMAP_CHARTS:
        fixed = [r for r in LIGHTMAP_CHART_LOG if r[3] or r[6]]
        print('lightmap charts (--lm-chart-halve, step %d, %d spots, at most %s texels across): %d charts refitted '
              'on %d of %d pages, %d texels' % (
            SEAM_STEP, CHART_SPOTS, CHART_MAX or 'any', sum(r[2] for r in fixed), len(fixed), len(LIGHTMAP_CHART_LOG),
            sum(r[3] for r in fixed)), file=sys.stderr)
        from lightmap_masks import CHART_SIZES_LOGGED
        sizes = np.sum([r[7] for r in LIGHTMAP_CHART_LOG], 0) if LIGHTMAP_CHART_LOG else []
        print('lightmap charts: charts whose halving breaks a seam, by size: %s' % ', '.join(
            '%s %d' % ('<= %d texels' % t if t else 'all', int(c))
            for t, c in zip(list(CHART_SIZES_LOGGED) + [0], sizes)), file=sys.stderr)
        for tag, page, charts, texels, before, after, skipped, _ in fixed:
            print('lightmap charts: %s page %d: %d charts, %d texels, worst seam step %.0f -> %.0f%s' % (
                tag, page, charts, texels, before, after, ', %d fits too large, left plain' % skipped if skipped else ''),
                file=sys.stderr)
    if LIGHTMAP_SCALE > 1 and LIGHTMAP_WHOLE:
        print('lightmaps whole for their seams: %d of %d pages with seams (under the key | 1, %d KB)' % (
            len(LIGHTMAPS_KEPT_WHOLE), len(LIGHTMAP_SEAMS), sum(k[4] for k in LIGHTMAPS_KEPT_WHOLE) // 1024),
            file=sys.stderr)
        print('lightmap seams (step %d, %d spots): spots broken by halving at more than %s' % (
            SEAM_STEP, SEAM_SPOTS_BROKEN, '/'.join(str(t) for t in SEAM_STEPS_LOGGED)), file=sys.stderr)
        for tag, page, spots, counts, worst, whole in LIGHTMAP_SEAM_LOG:
            print('lightmap seams: %s page %d: %d spots, broken %s, worst %.0f%s' % (
                tag, page, spots, '/'.join(str(c) for c in counts), worst, ', kept whole' if whole else ''),
                file=sys.stderr)
    print('bump relief: %d level base maps baked with their bump maps\' relief (under the key | 2)%s' % (
        relief_count, ' (--no-relief)' if NO_RELIEF else ' (unfiltered)' if OLD_RELIEF else ''), file=sys.stderr)
    print('cube maps: %d averaged into 8 x 8 patches (model reflections)' % len(CUBE_PATCHES), file=sys.stderr)
    for shader, across_u, across_v in RELIEF_SKIPPED:
        print('  bump relief left out for %s: %.1f x %.1f bump texels a base texel' % (shader, across_u, across_v),
              file=sys.stderr)
    for shader, changed in MASKS_MENDED:
        print('detail mask read on its own for %s: %.0f%% of its texels changed%s' % (
            shader, 100.0 * changed, ' (--premultiplied-masks: kept as before)' if PREMULTIPLIED_MASKS else ''),
            file=sys.stderr)
    print('detail masks: %d shaders whose mask the resize had darkened' % len(set(s for s, _ in MASKS_MENDED)),
          file=sys.stderr)
    # the self-illuminated shaders' glow maps, at their base map's size and
    # from the base the GE draws (with its detail map folded in, if it has
    # one); shaders with the same two maps share one copy of the texels
    glows = glow_looks(m, B) if not a.no_glow_maps else {}
    made = {}
    glow_bytes = 0
    for key in sorted(glows):
        g = glows[key]
        base, mp = g['base'], g['multipurpose']
        pair = (base['address'], mp['address'])
        if pair not in made:
            mp_img = decode(m, mp)
            cls = refs.classify(roles.get(base['tag'], ()), base)
            made[pair] = None if mp_img is None else bake_one(
                m, base, cls, looks.get(base['address']), mlooks.get(base['address']), glow=mp_img)
            if made[pair] is not None:
                glow_bytes += len(made[pair]['data'])
        r = made[pair]
        if r is None:
            print('glow map for %s: none (%s or %s not baked)' % (g['shader'], base['tag'], mp['tag']), file=sys.stderr)
            continue
        entries.append(dict(r, key=key, name=g['shader'] + ' (glow map)'))
        print('glow map for %s: %s x the green of %s, %dx%d, %d bytes%s' % (
            g['shader'], base['tag'], mp['tag'], r['w'], r['h'], len(r['data']),
            '' if sum(1 for k in glows if (glows[k]['base']['address'], glows[k]['multipurpose']['address']) == pair) == 1
            else ' (shared)'), file=sys.stderr)
    print('glow maps: %d self-illuminated model shaders, %d maps, %d bytes%s' % (
        len(glows), sum(1 for v in made.values() if v is not None), glow_bytes,
        ' (--no-glow-maps)' if a.no_glow_maps else ''), file=sys.stderr)
    # the self-illuminated level shaders' glow maps the same way, one a colour:
    # the shader's address | 2 (the illumination map's red, the primary colour)
    # and | 3 (its green, the secondary), at the base map's size; a shader with
    # no base map takes the illumination map's own, tiled as many times as its
    # scale tiles it (to 512), over white
    lglows = level_glow_looks(m, d, B) if not a.no_level_glow_maps else {}
    lmade = {}
    lglow_bytes = 0
    for key in sorted(lglows):
        g = lglows[key]
        base, mask = g['base'], g['mask']
        pair = (base['address'] if base else None, mask['address'], g['channel'], g['scale'])
        if pair not in lmade:
            mask_img = decode(m, mask)
            lmade[pair] = None
            if mask_img is not None and mask_img[:, :, g['channel']].any():
                glow = dict(mask=mask_img, channel=g['channel'], scale=g['scale'])
                if base:
                    cls = refs.classify(roles.get(base['tag'], ()), base)
                    lmade[pair] = bake_one(m, base, cls, looks.get(base['address']), level_glow=glow)
                else:
                    tile = 1
                    while tile < g['scale']:
                        tile *= 2
                    side = (min(512, mask['w'] * tile), min(512, mask['h'] * tile))
                    lmade[pair] = bake_one(m, dict(mask, w=side[0], h=side[1]), 'level (senv)', level_glow=glow,
                                           image=np.full((side[1], side[0], 4), 255, np.uint8))
            if lmade[pair] is not None:
                lglow_bytes += len(lmade[pair]['data'])
        r = lmade[pair]
        if r is None:
            print('level glow map for %s: none (%s empty or not baked)' % (g['shader'], mask['tag']), file=sys.stderr)
            continue
        entries.append(dict(r, key=key, name='%s (level glow map, %s)' % (g['shader'], ('primary', 'secondary')[g['channel']])))
        print('level glow map for %s: %s x the %s of %s, scale %g, %dx%d, %d bytes' % (
            g['shader'], base['tag'] if base else 'white', ('red', 'green')[g['channel']], mask['tag'], g['scale'],
            r['w'], r['h'], len(r['data'])), file=sys.stderr)
    print('level glow maps: %d colours of self-illuminated level shaders, %d maps, %d bytes%s' % (
        len(lglows), sum(1 for v in lmade.values() if v is not None), lglow_bytes,
        ' (--no-level-glow-maps)' if a.no_level_glow_maps else ''), file=sys.stderr)
    print('alpha shapes: %s; %d alpha-tested bitmaps' % (
        'each side of the alpha test fitted apart' if ALPHA_SPLIT else 'one 4-D fit (--no-alpha-split)',
        len(ALPHA_TESTED)), file=sys.stderr)
    if EFFECTS_CLUT4:
        print('effects kept at 8 bits: none (--effects-clut4)', file=sys.stderr)
    else:
        kept = sorted(set(EFFECTS_KEPT_8))
        print('effects kept at 8 bits: %d maps the 16-colour test would have taken, %d bytes more at their top level' % (
            len(kept), sum(k[2] for k in kept)), file=sys.stderr)
    alpha_audit_summary()
    print('meter alphas: %s' % ('fixed 0/85/170/255 (--no-meter-alphas)' if not METER_ALPHAS
                                else '%d meter bitmaps' % len(METER_LOG)), file=sys.stderr)
    for line in METER_LOG:
        print('meter alphas: ' + line, file=sys.stderr)
    if HUD_CLUT4:
        print('HUD sheets: 16 colours allowed (--hud-clut4)', file=sys.stderr)
    else:
        kept = sorted(set(HUD_KEPT_8))
        print('HUD sheets: %d kept at 8 bits that the old rule put at 4%s' % (
            len(kept), (': ' + ', '.join(kept)) if kept else ''), file=sys.stderr)
    entries.sort(key=lambda e: e['key'])
    # the see-through shaders' tints and drawn maps (seethrough.py)
    seethrough.DRAWN_BY_SHAPE = a.drawn_by_shape
    tints, drawn = seethrough.tints(m, bitmap_list(m, d), from_means=a.tints_from_means)
    if ALPHA_AUDIT:
        for name, found in seethrough.FOUND:
            if not found.alpha_shares:
                continue
            shape = max(found.alpha_shares, key=lambda j: found.alpha_shares[j])
            print('alpha audit: see-through %s: blend %s, map %d drawn (alpha variance %.4f), the alpha\'s most from map '
                  '%d (%.4f)%s' % (name, found.blend, found.drawn, found.alpha_shares.get(found.drawn, 0.0), shape,
                                   found.alpha_shares[shape], '' if shape == found.drawn else ' (not drawn)'),
                  file=sys.stderr)
    names = {t.address: t.name for t in m.tags}
    # the see-through composites (seethrough.composite): under the shader's
    # address | 1, which no bitmap's key can be, with the map each is drawn in
    # place of ('TNC1'); the menu's up to the GE's 512 a side
    composites = {}
    composite_bytes = 0
    seethrough.COMPOSITES = not a.no_composites
    if SHOWCASE:
        seethrough.COMPOSITE_SIDE = 512
    by_tag = {}
    for b in B:
        by_tag.setdefault(b['tag'], b)
    composite_means = seethrough.Means(m, B)
    for name, found in (seethrough.FOUND if seethrough.COMPOSITES else []):
        made = seethrough.composite(m, found.tag, composite_means, found)
        if made is None:
            continue
        basis, img, why = made
        if img is None:
            print('composite for %s: none (%s)' % (name, why), file=sys.stderr)
            continue
        maps = seethrough._maps(m, m.off(found.tag.address), struct.unpack_from('<H', m.data, m.off(found.tag.address) + 36)[0])
        t = m.by_id.get(maps[basis][0])
        b = by_tag.get(t.name) if t is not None else None
        if b is None:
            continue
        cls = refs.classify(roles.get(b['tag'], ()), b)
        if cls == 'detail':
            # a detail bake levels the colours to grey and drops the alpha,
            # which would undo the composite: the shader keeps its own maps
            print('composite for %s: none (%s is baked as a detail map)' % (name, b['tag']), file=sys.stderr)
            continue
        r = bake_one(m, dict(b, w=img.shape[1], h=img.shape[0]), cls, image=img)
        if r is None:
            print('composite for %s: none (%s not baked as %s)' % (name, b['tag'], cls), file=sys.stderr)
            continue
        entries.append(dict(r, key=found.tag.address | 1, name=name + ' (composite)'))
        composites[found.tag.address] = basis
        composite_bytes += len(r['data'])
        print('composite for %s: %s; %dx%d baked, %d bytes' % (name, why, r['w'], r['h'], len(r['data'])),
              file=sys.stderr)
    print('composites: %d see-through shaders drawn from a texture of their own, %d bytes%s' % (
        len(composites), composite_bytes, ' (--no-composites)' if a.no_composites else ''), file=sys.stderr)
    entries.sort(key=lambda e: e['key'])
    for address in sorted(set(k & ~3 for k in tints) | set(drawn)):
        found = next((tints[k][2] for k in tints if k & ~3 == address), None) or drawn[address]
        alt = '' if found.alt is None else ' (the scale rule\'s map %d: tint %s)' % (found.scale_drawn, found.alt.name())
        print('  tint %s for %s: map %d drawn%s; the shader averages %s over its map\'s %s' % (
            found.name(), names[address], found.drawn, alt, ' '.join('%.2f' % v for v in found.result),
            ' '.join('%.2f' % v for v in found.mean)), file=sys.stderr)
    print('%d see-through shaders tinted (%d doubled, %d added, %d flat); %d drawn from another map than the scale rule\'s%s' % (
        len(tints), sum(1 for k in tints if k & 3 == seethrough.DOUBLED), sum(1 for k in tints if k & 3 == seethrough.ADD),
        sum(1 for k in tints if k & 3 == seethrough.FLAT), len(drawn),
        ' (tints from the maps\' means: --tints-from-means)' if a.tints_from_means else ''), file=sys.stderr)
    angled = sorted(key for key in tints if tints[key][2].angle is not None)
    for key in angled:
        face, edge = tints[key][2].angle
        print('  tints by angle for %s: face on %08x, edge on %08x' % (
            names[key & ~3], seethrough.abgr(face), seethrough.abgr(edge)), file=sys.stderr)
    # every table is written, empty or not, so the averages can follow them
    tint_bytes = struct.pack('<4sI', b'TNT1', len(tints)) + b''.join(
        struct.pack('<II', key, tints[key][0]) for key in sorted(tints))
    tint_bytes += struct.pack('<4sI', b'TND1', len(drawn)) + b''.join(
        struct.pack('<III', address | drawn[address].alt.flags(), drawn[address].drawn,
                    drawn[address].alt.word()) for address in sorted(drawn))
    tint_bytes += struct.pack('<4sI', b'TNA1', len(angled)) + b''.join(
        struct.pack('<III', key, seethrough.abgr(tints[key][2].angle[0]),
                    seethrough.abgr(tints[key][2].angle[1])) for key in angled)
    tint_bytes += struct.pack('<4sI', b'TNC1', len(composites)) + b''.join(
        struct.pack('<II', address, composites[address]) for address in sorted(composites))
    tint_bytes += runtime_reads(entries)
    header = 12
    directory = 20 * len(entries)
    offset = (header + directory + len(tint_bytes) + 15) // 16 * 16
    blob = bytearray()
    dir_bytes = bytearray()
    placed = {}
    for e in entries:
        if id(e['data']) in placed:
            start = placed[id(e['data'])]
        else:
            start = offset + len(blob)
            placed[id(e['data'])] = start
            blob += e['data']
            while len(blob) % 16:
                blob += b'\0'
        dir_bytes += struct.pack('<IHHBBBBII', e['key'], e['w'], e['h'], e['levels'], e['psm'], e['clut'],
                                 (1 if e['alpha'] else 0) | e.get('flags', 0), start, len(e['data']))
    out = bytearray(struct.pack('<4sII', b'PTX1', len(entries), directory)) + dir_bytes + tint_bytes
    while len(out) < offset:
        out += b'\0'
    out += blob
    open(a.out, 'wb').write(out)
    print('quality %d: %s' % (QUALITY, 'palettes fitted in Oklab, texels by error diffusion, sizes by Lanczos'
                              if QUALITY >= 2 else 'as before (RGB fit, nearest entry, box)'), file=sys.stderr)
    for cls in sorted(QUALITY_STATS):
        n, old, new = QUALITY_STATS[cls]
        print('quality %s: %d textures, blurred PSNR %.2f dB as before, %.2f now' % (cls, n, old / n, new / n),
              file=sys.stderr)
    if SHOWCASE:
        own = [e for e in entries if e['psm'] in (0, 3)]
        for e in sorted(own, key=lambda e: -len(e['data'])):
            print('showcase: %s, %dx%d in %d-bit colour, %d levels, %d bytes' % (
                e['name'], e['w'], e['h'], 32 if e['psm'] == 3 else 16, e['levels'], len(e['data'])), file=sys.stderr)
        print('showcase: %d textures in their own colours (%d bytes), every map at its own size up to 512' % (
            len(own), sum(len(d) for d in {id(e['data']): e['data'] for e in own}.values())), file=sys.stderr)
    clut4_exact_report()
    clut4_look_report()
    print('%s: %d textures, %d bytes; skipped %s' % (a.out, len(entries), len(out), skipped))
    if FAST_PALETTES:
        print('palettes: fast (--fast-palettes)', file=sys.stderr)
    profile_summary(time.perf_counter() - started)
    palette_compare_summary()


if __name__ == '__main__':
    main()
