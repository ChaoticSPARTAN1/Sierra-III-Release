"""seethrough.py - which map the PSP draws of a see-through shader, and the
tint it draws that map with.

The Xbox draws a transparent generic shader (sotr) with up to four maps run
through up to seven register-combiner stages (shader_transparent_generic_
preprocessor.c), and a transparent chicago shader (schi) with its maps folded
one into the next by each map's colour and alpha functions (shader_transparent_
chicago_preprocessor.c). The GE draws one texture at a time, so the port draws
one map of the shader (psp_fixups.py, psp_transparent_map) with a constant
colour for the rest of it (psp_ge.c).

The constant comes from running the shader the way the Xbox's combiners would
over a sample of the drawn map's texels. Another map that is the same bitmap
at the same scale takes the same texel (a map layered on itself: c40's
teleport column multiplies two copies of one streak map, where means would
give the streaks' mean squared, a quarter of their brightness); the other
maps sit at their mean colour, and so does a reflection cube the PSP skips.
What the texels can't stand for (the angle a surface is seen at, a constant
colour's animation over time) takes its middle value.

Three ways of drawing the result are fitted to the texels. Modulate is kept
unless another is much closer (MODEL_BETTER) and the shader has no fade (the
GE's fade rides on the vertex colour, which only modulate multiplies the
whole draw by):
  modulate  the map times the tint (the GE's TFX_MODULATE), so the draw
            averages to the shader's own colour and keeps its map's pattern;
            a tint over 1 is halved and drawn with the GE's colour doubling
  add       the map plus the tint (TFX_ADD): a reflection added over the map
            (a10's monitor glass)
  flat      the tint alone, whatever the texel (TFX_BLEND with the vertex
            and environment colours alike): a shader whose colour hardly
            depends on the map at all. a10's cryo glass (its bridge windows)
            is the reflection cube times a constant, its map adding a few
            per cent; folded into a multiplier of the map it scaled each
            texel by x1.2 red and green and x0.5 blue, so the map's near-white
            texels showed as the yellow splotches
In all three the alpha is the map's times the tint's.

The drawn map is the one whose texels carry most of what the shader draws
(the variance of its output when only that map varies), unless the map the
runtime's own rule picks (the smallest scale: the map that spans the
surface) carries nearly as much. The holograms' scrolling band overlay has
the smallest scale, but their planet and rings are the content.

tints(..., from_means=True) works every shader out with every map at its mean
and the runtime's map, as the packs were baked before (texpack.py
--tints-from-means). Prints nothing; texpack.py writes the tints and the
drawn maps into the pack, by the shader's tag address (psp_tex.c).
"""
import struct
import numpy as np

from bitmap_bake import decode

# the shader types as an Xbox map holds them: Invader writes the Xbox's,
# which have no chicago extended, so water is 7 (8 is glass, 9 meter)
GENERIC, CHICAGO, WATER = 5, 6, 7
# the maps the runtime's own rule looks through (psp_transparent_map)
PSP_MAPS_LOOKED_AT = 4
# texels sampled from a map, on an even spread over its top level
TEXELS = 4096
# another map is drawn in place of the runtime's only when it carries this
# many times the share of the output
DRAWN_MARGIN = 1.5
# the add or flat model is kept only when its error is under this share of
# the modulate model's
MODEL_BETTER = 0.5
# a modulate model this close (mean squared error, colours 0-1: two steps
# of 255 in each channel) is kept whatever the others' errors
MODULATE_CLOSE = 3 * (2.0 / 255.0) ** 2
# the drawing models, as the pack's two low address bits give them
MODULATE, DOUBLED, ADD, FLAT = 0, 1, 2, 3
MODEL_NAMES = {MODULATE: '', DOUBLED: ' doubled', ADD: ' added', FLAT: ' flat'}

# ---------------------------------------------------------------- the maps


def _face_bytes(b, bpp):
    """one cube face's bytes, its mip levels included (the Xbox's textures
    are in 4x4 blocks for DXT, and a face is padded to 128 bytes)"""
    w, h, total = b['w'], b['h'], 0
    for _ in range(b['mips'] + 1):
        if b['format'].startswith('DXT'):
            total += max(1, (w + 3) // 4) * max(1, (h + 3) // 4) * (8 if b['format'] == 'DXT1' else 16)
        else:
            total += w * h * bpp // 8
        w, h = max(1, w // 2), max(1, h // 2)
    return total


def bitmap_mean(m, b):
    """a bitmap's mean colour, RGBA 0-1, or None: its top level, every face
    of a cube map (a reflection averages what it reflects)"""
    from bitmap_bake import BPP
    if b['type'] != 'cube':
        img = decode(m, b)
        return None if img is None else img.reshape(-1, 4).mean(0) / 255.0
    faces = []
    bpp = BPP[b['fmt']] if b['fmt'] < len(BPP) else 0
    face = _face_bytes(b, bpp) if bpp else 0
    for stride in (face, (face + 127) // 128 * 128):
        if stride and 6 * stride == b['pixels_size']:
            for k in range(6):
                img = decode(m, dict(b, pixels_offset=b['pixels_offset'] + k * stride, pixels_size=stride))
                if img is None:
                    break
                faces.append(img.reshape(-1, 4).mean(0))
            break
    if len(faces) == 6:
        return np.mean(faces, 0) / 255.0
    img = decode(m, b)  # face 0 alone when the faces can't be placed
    return None if img is None else img.reshape(-1, 4).mean(0) / 255.0


def bitmap_texels(m, b, count=TEXELS):
    """up to count texels of a 2D bitmap's top level (count x RGBA, 0-1),
    evenly spread; None for a cube map or one that can't be read"""
    if b['type'] != '2d':
        return None
    img = decode(m, b)
    if img is None:
        return None
    flat = img.reshape(-1, 4)
    if len(flat) > count:
        flat = flat[np.linspace(0, len(flat) - 1, count).astype(np.int64)]
    return flat.astype(np.float64) / 255.0


class Means:
    """bitmap tag id -> the mean of its first bitmap, worked out once; and
    texels(tag id), a sample of its texels"""

    def __init__(self, m, bitmaps):
        self.m = m
        self.first = {}
        for b in bitmaps:
            self.first.setdefault(b['tag'], b)
        self.cache = {}
        self.texel_cache = {}

    def _bitmap(self, tag_id):
        t = self.m.by_id.get(tag_id)
        return self.first.get(t.name) if t is not None and t.group == 'bitm' else None

    def __call__(self, tag_id):
        if tag_id not in self.cache:
            b = self._bitmap(tag_id)
            self.cache[tag_id] = bitmap_mean(self.m, b) if b else None
        return self.cache[tag_id]

    def image(self, tag_id):
        """a 2D bitmap's top level (h x w x RGBA, 0-255), or None"""
        b = self._bitmap(tag_id)
        return decode(self.m, b) if b is not None and b['type'] == '2d' else None

    def texels(self, tag_id):
        if tag_id not in self.texel_cache:
            b = self._bitmap(tag_id)
            self.texel_cache[tag_id] = bitmap_texels(self.m, b) if b else None
        return self.texel_cache[tag_id]


def _rows(x, n):
    """a map's colour as n x RGBA: its texels, or its mean (black when
    unreadable) on every row"""
    if x is None:
        return np.zeros((n, 4))
    x = np.asarray(x, dtype=np.float64)
    return x.copy() if x.ndim == 2 else np.tile(x, (n, 1))


# ---------------------------------------------------------------- generic

# NV2A input mappings, by the tag's mapping index (code = index * 0x20)
def _mapped(x, mapping):
    x = np.asarray(x, dtype=np.float64)
    pos = np.maximum(x, 0.0)
    return [pos,                                 # clamp x (unsigned identity)
            1.0 - np.clip(x, 0.0, 1.0),          # 1 - clamp x (unsigned invert)
            2.0 * pos - 1.0,                     # expand normal
            1.0 - 2.0 * pos,                     # expand negate
            pos - 0.5,                           # half bias normal
            0.5 - pos,                           # half bias negate
            x,                                   # signed identity
            -x][mapping & 7]                     # signed negate


# the constant inputs (zero, one, one half, -1, -1/2) read the zero register
# through these mapping codes (shader_stage_constant_input_registers)
_CONSTANT_CODES = [
    [0x00, 0x20, 0x40, 0x20, 0x80, 0xa0, 0x00, 0x00],
    [0x20, 0x00, 0x20, 0x40, 0xa0, 0x80, 0x20, 0x40],
    [0xa0, 0xa0, 0x00, 0x00, 0x00, 0x00, 0xa0, 0x80],
    [0x00, 0x20, 0x40, 0x20, 0x80, 0xa0, 0x40, 0x20],
    [0x00, 0x20, 0x40, 0x20, 0x80, 0xa0, 0x80, 0xa0],
]
_CODE_AT_ZERO = {0x00: 0.0, 0x20: 1.0, 0x40: -1.0, 0x60: 1.0, 0x80: -0.5, 0xa0: 0.5, 0xc0: 0.0, 0xe0: 0.0}
# the registers by the tag's input index, after the five constants: map
# colours 0-3, vertex colours 0-1, scratch 0-1, constants 0-1, then the same
# again for the other half (alpha for a colour input, blue for an alpha one)
_REGISTERS = ['T0', 'T1', 'T2', 'T3', 'V0', 'V1', 'R0', 'R1', 'C0', 'C1']
# outputs: discard, scratch 0 (the result), scratch 1, vertex 0-1, maps 0-3
_OUTPUTS = [None, 'R0', 'R1', 'V0', 'V1', 'T0', 'T1', 'T2', 'T3']
_OUTPUT_MAPPINGS = [lambda x: x, lambda x: x * 0.5, lambda x: x * 2.0, lambda x: x * 4.0,
                    lambda x: x - 0.5, lambda x: (x - 0.5) * 2.0]


def _colour_input(reg, index, mapping, n):
    """n x RGB"""
    if index < 5:
        return np.full((n, 3), _CODE_AT_ZERO[_CONSTANT_CODES[index][mapping & 7]])
    index -= 5
    if index < 10:
        return _mapped(reg[_REGISTERS[index]][:, :3], mapping)
    if index < 20:
        return _mapped(np.repeat(reg[_REGISTERS[index - 10]][:, 3:4], 3, 1), mapping)
    return np.zeros((n, 3))


def _alpha_input(reg, index, mapping, n):
    """n values"""
    if index < 5:
        return np.full(n, float(_CODE_AT_ZERO[_CONSTANT_CODES[index][mapping & 7]]))
    index -= 5
    if index < 10:
        return _mapped(reg[_REGISTERS[index]][:, 3], mapping)
    if index < 20:
        return _mapped(reg[_REGISTERS[index - 10]][:, 2], mapping)
    return np.zeros(n)


def _function_mean(function):
    """a periodic function's value over time: 'one' 1, 'zero' 0, the waves
    their middle"""
    return 1.0 if function == 0 else 0.0 if function == 1 else 0.5


def _argb(data, at):
    a, r, g, b = struct.unpack_from('<4f', data, at)
    return np.array([r, g, b, a])


# the generic combiners' vertex colour 1 by the angle the surface is seen at:
# fade(perpendicular) in red, green and alpha, fade(parallel) in blue (the
# stage inputs' names). Face on, perpendicular is 0 and parallel 1
VIEWS = {None: (0.5, 0.5, 0.5, 0.5), 'face': (0.0, 0.0, 1.0, 0.0), 'edge': (1.0, 1.0, 0.0, 1.0)}


def generic_result(m, o, maps, view=None):
    """the shader's colour (n x RGBA, 0-1), each map given as its texels (n x
    RGBA) or a mean (RGBA); view: the angle (VIEWS), the middle by default"""
    n = max([len(x) for x in maps[:4] if x is not None and np.ndim(x) == 2] or [1])
    stages_n, stages_p = struct.unpack_from('<iI', m.data, o + 96)
    reg = {k: np.zeros((n, 4)) for k in _REGISTERS}
    for i, x in enumerate(maps[:4]):
        reg['T%d' % i] = _rows(x, n)
    reg['V0'] = np.ones((n, 4))        # the diffuse light: unlit, white
    reg['V1'] = np.tile(np.array(VIEWS[view]), (n, 1))   # the fades by angle
    reg['R0'][:, 3] = reg['T0'][:, 3]  # the combiners start with map 0's alpha in scratch 0
    if stages_n <= 0:
        reg['R0'] = reg['T0'].copy()
    so = m.off(stages_p) if stages_n > 0 else None
    for s in range(min(stages_n, 7)):
        at = so + 112 * s
        flags, = struct.unpack_from('<H', m.data, at)
        function, = struct.unpack_from('<h', m.data, at + 6)
        lower, upper, colour1 = _argb(m.data, at + 12), _argb(m.data, at + 28), _argb(m.data, at + 44)
        reg['C0'] = np.tile(lower + _function_mean(function) * (upper - lower), (n, 1))
        reg['C1'] = np.tile(colour1, (n, 1))
        c = struct.unpack_from('<14h', m.data, at + 60)
        a = struct.unpack_from('<12h', m.data, at + 88)
        A, B, C, D = (_colour_input(reg, c[2 * k], c[2 * k + 1], n) for k in range(4))
        ab = np.repeat((A * B).sum(1, keepdims=True), 3, 1) if c[9] == 1 else A * B
        cd = np.repeat((C * D).sum(1, keepdims=True), 3, 1) if c[11] == 1 else C * D
        mux = reg['R0'][:, 3] >= 0.5
        total = np.where(mux[:, None], cd, ab) if flags & 1 else ab + cd
        fmap = _OUTPUT_MAPPINGS[c[13]] if 0 <= c[13] < 6 else _OUTPUT_MAPPINGS[0]
        colour_out = [(c[8], ab), (c[10], cd), (c[12], total)]
        aA, aB, aC, aD = (_alpha_input(reg, a[2 * k], a[2 * k + 1], n) for k in range(4))
        aab, acd = aA * aB, aC * aD
        atotal = np.where(mux, acd, aab) if flags & 2 else aab + acd
        amap = _OUTPUT_MAPPINGS[a[11]] if 0 <= a[11] < 6 else _OUTPUT_MAPPINGS[0]
        alpha_out = [(a[8], aab), (a[9], acd), (a[10], atotal)]
        for out, value in colour_out:
            if 0 < out < len(_OUTPUTS):
                reg[_OUTPUTS[out]][:, :3] = np.clip(fmap(value), -1.0, 1.0)
        for out, value in alpha_out:
            if 0 < out < len(_OUTPUTS):
                reg[_OUTPUTS[out]][:, 3] = np.clip(amap(value), -1.0, 1.0)
    return np.clip(reg['R0'], 0.0, 1.0)


# ---------------------------------------------------------------- chicago

def _fold(c, n, function):
    """chicago's colour and alpha functions, current (c) with the next map (n)"""
    if function == 0: return c
    if function == 1: return n
    if function == 2: return c * n
    if function == 3: return 2.0 * c * n
    if function == 4: return c + n
    if function == 5: return n + 2.0 * c - 1.0    # add signed current
    if function == 6: return c + 2.0 * n - 1.0    # add signed next map
    if function == 7: return n - c                # subtract current
    if function == 8: return c - n                # subtract next map
    return None


def chicago_result(m, o, maps_p, count, maps):
    """the shader's colour (n x RGBA) with its maps as generic_result takes
    them: map 0, then each next map folded in by the previous map's
    functions"""
    rows = max([len(x) for x in maps[:4] if x is not None and np.ndim(x) == 2] or [1])
    mo = m.off(maps_p)
    current = None
    for i in range(min(count, 4)):
        at = mo + 220 * i
        flags, = struct.unpack_from('<H', m.data, at)
        n = _rows(maps[i], rows)
        if flags & 2:   # alpha replicate: the map's alpha in its colour too
            n = np.repeat(n[:, 3:4], 4, 1)
        if current is None:
            current = n
            continue
        colour_function, alpha_function = struct.unpack_from('<hh', m.data, mo + 220 * (i - 1) + 44)
        rgb, alpha = current[:, :3], current[:, 3]
        if colour_function in (9, 10, 11, 12):
            w = (alpha if colour_function in (9, 10) else n[:, 3])[:, None]
            rgb = n[:, :3] * w + rgb * (1 - w) if colour_function in (9, 11) else rgb * w + n[:, :3] * (1 - w)
        else:
            rgb = _fold(rgb, n[:, :3], colour_function)
        if alpha_function in (9, 10, 11, 12):
            w = alpha if alpha_function in (9, 10) else n[:, 3]
            alpha = n[:, 3] * w + alpha * (1 - w) if alpha_function in (9, 11) else alpha * w + n[:, 3] * (1 - w)
        else:
            alpha = _fold(alpha, n[:, 3], alpha_function)
        if rgb is None or alpha is None:
            return None
        current = np.clip(np.concatenate([rgb, alpha[:, None]], 1), -1.0, 1.0)
    return None if current is None else np.clip(current, 0.0, 1.0)


# ---------------------------------------------------------------- water

def water_tint(m, o, means):
    """water (swat): the Xbox draws the background times the base map's
    colour (flag bit 1), the frame's alpha from the base map's alpha and the
    view (bit 0), then the reflection cube times a tint between the parallel
    and perpendicular colours, added through that alpha (rasterizer_xbox_
    water.c). The GE draws the base map once: alpha-blended when its colour
    modulates the background (psp_fixups.py), so the background keeps
    1 - alpha of itself, alpha being one minus the base map's mean colour,
    and the source is what the reflection adds over that alpha; added
    otherwise. (the result over the base map's mean, unclipped; the result;
    the base map's mean) or None"""
    flags, = struct.unpack_from('<H', m.data, o + 40)
    base_id, = struct.unpack_from('<i', m.data, o + 88)
    reflection_id, = struct.unpack_from('<i', m.data, o + 168)
    perpendicular, parallel = _argb(m.data, o + 108), _argb(m.data, o + 124)
    base = means(base_id)
    if base is None:
        return None
    reflection = means(reflection_id)
    r = reflection[:3] if reflection is not None else np.zeros(3)
    view = (perpendicular + parallel) * 0.5   # the view between face on and edge on
    through = base[3] * view[3] if flags & 1 else 1.0
    added = r * view[:3] * through
    if flags & 2:
        keep = float(np.clip(base[:3] @ np.array([0.299, 0.587, 0.114]), 0.0, 1.0))
        alpha = max(1.0 - keep, 1.0 / 255.0)
        result = np.append(np.clip(added / alpha, 0.0, 1.0), alpha)
    else:
        result = np.append(np.clip(added, 0.0, 1.0), 1.0)
    floor = np.maximum(base, 1.0 / 255.0)
    quotient = result / floor
    if not flags & 2:
        quotient[3] = 1.0
    return quotient, result, base


# ---------------------------------------------------------------- the tint

# a colour more than this over its map's (the GE multiplies by at most 1)
# is drawn doubled: the tint at half, the GE's colour doubling on
DOUBLED_ABOVE = 1.02
# a shader whose colour face on and edge on differ by more than this (0-1,
# any channel) gets the two tints, blended per vertex by the angle
ANGLE_APART = 2.0 / 255.0
_LUMA = np.array([0.299, 0.587, 0.114])


def _clipped(quotient):
    """(tint RGBA 0-1, doubled): a shader's result over its drawn map's mean
    as the GE can multiply it. Where a colour channel asks for more than the
    map (Cortana, the holograms, the sniper rifle's screen: up to 3x), the
    tint is halved and drawn with the GE's colour doubling (the alpha isn't
    doubled); 2x is the most it gives"""
    quotient = np.asarray(quotient, dtype=np.float64)
    doubled = bool(quotient[:3].max() > DOUBLED_ABOVE)
    rgb = quotient[:3] * (0.5 if doubled else 1.0)
    return np.clip(np.append(rgb, quotient[3]), 0.0, 1.0), doubled


class Tint:
    """how the GE draws a shader: map `drawn` with `tint` (RGBA 0-1) by the
    drawing model (MODULATE, DOUBLED, ADD or FLAT). result and mean: the
    shader's mean colour and the drawn map's. scale_drawn: the map the
    runtime's own rule picks; alt: this shader's Tint for that map when the
    bake draws another (the runtime's keep word drawnmapscale draws it).
    errors: each model's mean squared error over the texels, by name (None
    from means). shares: each candidate map's share of the output"""

    def __init__(self, tint, drawn, result, mean, model=MODULATE):
        self.tint, self.drawn, self.result, self.mean, self.model = tint, drawn, result, mean, model
        self.scale_drawn, self.alt, self.errors, self.shares = drawn, None, None, {}
        # each candidate map's share of the alpha (its variance when only
        # that map varies), and the framebuffer blend: for texpack's audit
        self.alpha_shares, self.blend = {}, None
        # (face-on tint, edge-on tint), RGBA 0-1, for a generic shader whose
        # combiners read the angle; None when the angle changes nothing
        self.angle = None

    @property
    def doubled(self):
        return self.model == DOUBLED

    def word(self):
        return abgr(self.tint)

    def flags(self):
        """the model, as the pack keys it in the address's two low bits"""
        return self.model

    def name(self):
        """the colour word and the model, as the bake's log and tagdeps.py
        print them"""
        return '%08x%s' % (self.word(), MODEL_NAMES[self.model])

    def plain(self):
        """drawn as the map is: no entry in the pack's tints"""
        return self.word() == 0xFFFFFFFF and self.model == MODULATE


def _visible(result, blend):
    """what the frame gets of the shader's colour: through its alpha for an
    alpha blend, as it is otherwise"""
    return result[:, :3] * result[:, 3:4] if blend == 0 else result[:, :3]


# DRAWN_BY_SHAPE (texpack --drawn-by-shape): a shader whose blend lets what
# is behind it through by its alpha (alpha blend, alpha-multiply-add) draws
# the map that carries most of what the frame shows over a dark background
# and over a light one, so a map that only shapes the alpha (a mask: a
# thruster's cone, a shore layer's fading edge) counts for the part of the
# background it hides, not only for the colour it darkens
DRAWN_BY_SHAPE = False
SHAPE_BLENDS = (0, 7)


def _shown(result, blend):
    """the luminance the frame shows of the shader over black and over
    white, side by side (n x 2)"""
    lum = result[:, :3] @ _LUMA
    if blend not in SHAPE_BLENDS:
        return np.stack([lum, lum], 1)
    a = result[:, 3]
    src = lum * a if blend == 0 else lum
    return np.stack([src, src + 1.0 - a], 1)


def _share(result, blend):
    if DRAWN_BY_SHAPE:
        return float(_shown(result, blend).var(0).sum())
    return float((_visible(result, blend) @ _LUMA).var())


def _fit(result, texels, blend, fade_free):
    """(tint, model, {model name: error}) for the drawn map's texels against
    the shader's colour over them. The error is the mean squared difference
    in colour, weighted by the shader's alpha for an alpha blend (what the
    frame gets). Modulate unless add or flat is closer by MODEL_BETTER, the
    modulate model is off by more than MODULATE_CLOSE, and the shader has no
    fade"""
    mean_r, mean_t = result.mean(0), texels.mean(0)
    tint, doubled = _clipped(mean_r / np.maximum(mean_t, 1.0 / 255.0))
    weight = result[:, 3] if blend == 0 else np.ones(len(result))
    weight = weight / max(float(weight.sum()), 1e-9)

    def error(drawn):
        return float((((np.clip(drawn, 0.0, 1.0) - result[:, :3]) ** 2).sum(1) * weight).sum())
    errors = {'modulate': error(texels[:, :3] * tint[:3] * (2.0 if doubled else 1.0))}
    k = np.clip(mean_r[:3] - mean_t[:3], 0.0, 1.0)
    errors['add'] = error(texels[:, :3] + k)
    flat = np.clip(mean_r[:3], 0.0, 1.0)
    errors['flat'] = error(np.tile(flat, (len(texels), 1)))
    model = DOUBLED if doubled else MODULATE
    # a shader pinned at white (the needler's ammo glow) is the combiners'
    # clamp, not its maps: the map's shape is all a draw can keep of it
    pinned = bool((mean_r[:3] > 0.98).all())
    if fade_free and not pinned and errors['modulate'] > MODULATE_CLOSE:
        best = min(('add', 'flat'), key=lambda name: errors[name])
        if errors[best] < MODEL_BETTER * errors['modulate'] and (best == 'flat' or k.max() > 0.5 / 255.0):
            return np.append(k if best == 'add' else flat, tint[3]), ADD if best == 'add' else FLAT, errors
    return tint, model, errors


def _scales_up(m, o):
    """whether a generic shader's stages scale an output by 2 or 4"""
    stages_n, stages_p = struct.unpack_from('<iI', m.data, o + 96)
    if stages_n <= 0:
        return False
    so = m.off(stages_p)
    for s in range(min(stages_n, 7)):
        colour_mapping, = struct.unpack_from('<h', m.data, so + 112 * s + 60 + 26)
        alpha_mapping, = struct.unpack_from('<h', m.data, so + 112 * s + 88 + 22)
        if colour_mapping in (2, 3) or alpha_mapping in (2, 3):
            return True
    return False


def _shuffled(texels, i, n):
    """n of a map's texels in a fixed order of its own (seeded by its place,
    so a bake gives the same tints every time). n is the drawn map's sample
    size: a smaller map than the drawn one has fewer texels (TEXELS caps the
    sample), so its order is repeated to fill the rows, and a larger one's is
    cut short; every row of the shader's result then has one texel of each"""
    order = np.random.default_rng(1 + i).permutation(len(texels))
    return texels[np.resize(order, n)]


def _maps(m, o, kind):
    """[(bitmap tag id, (u scale, v scale))] of a generic or chicago shader"""
    count, maps_p = struct.unpack_from('<iI', m.data, o + 84)
    stride, scale_at, map_at = (220, 84, 120) if kind == CHICAGO else (100, 4, 40)
    mo = m.off(maps_p)
    return [(struct.unpack_from('<i', m.data, mo + stride * i + map_at)[0],
             struct.unpack_from('<ff', m.data, mo + stride * i + scale_at)) for i in range(count)]


def _scale_drawn(maps, skip):
    """the map the runtime's own rule draws: of those after a cube map, the
    one with the smallest scale (the map that spans the surface), a tie to
    the later one (psp_transparent_map)"""
    drawn, best = skip, maps[skip][1][0]
    for i in range(skip + 1, min(len(maps), skip + PSP_MAPS_LOOKED_AT)):
        scale = maps[i][1][0]
        if scale != 0.0 and (best == 0.0 or scale <= best):
            drawn, best = i, scale
    return drawn


def shader_tint(m, tag, means, from_means=False):
    """a Tint for a generic, chicago or water shader, or None when the PSP's
    draw needs none (a numeric shader, no maps, a map that can't be read). A
    water shader's drawn map is its base map (index 0 here). from_means:
    every map at its mean, the runtime's map, the modulate model"""
    if not tag.address or not m.in_tags(tag.address):
        return None
    o = m.off(tag.address)
    kind, = struct.unpack_from('<H', m.data, o + 36)
    if kind == WATER:
        found = water_tint(m, o, means)
        if found is None:
            return None
        tint, doubled = _clipped(found[0])
        return Tint(tint, 0, found[1], found[2], DOUBLED if doubled else MODULATE)
    if kind not in (GENERIC, CHICAGO):
        return None
    flags = m.data[o + 41]
    first_map_type, blend, fade_mode, fade_source = struct.unpack_from('<hhhh', m.data, o + 42)
    count, maps_p = struct.unpack_from('<iI', m.data, o + 84)
    skip = 1 if first_map_type != 0 else 0
    if flags & 0x80 or count <= skip:
        return None
    maps = _maps(m, o, kind)
    map_means = [means(i) for i, _ in maps]

    # a generic shader that scales a stage's output up clips wherever its
    # other maps are bright, which their means would hide (Cortana: the noise
    # mask times 4, so the PSP drew her brighter than the Xbox): for its tint,
    # the other maps come in as their own texels, in an order unrelated to the
    # drawn map's
    spread = kind == GENERIC and _scales_up(m, o)

    def other(i, spread_out, n):
        texels = means.texels(maps[i][0]) if spread_out and n else None
        return map_means[i] if texels is None else _shuffled(texels, i, n)

    def evaluate(group, view=None, spread_out=False):
        """the shader over the texels of the maps in group (one bitmap at one
        scale), every other map at its mean (or its texels, spread_out, as
        many as the group's)"""
        drawn = [means.texels(maps[i][0]) for i in group]
        n = max([len(x) for x in drawn if x is not None] or [0])
        given = [means.texels(maps[i][0]) if i in group else other(i, spread_out, n) for i in range(count)]
        return generic_result(m, o, given, view) if kind == GENERIC else chicago_result(m, o, maps_p, count, given)

    def group_of(j):
        return {i for i in range(skip, count) if maps[i] == maps[j]}

    scale_drawn = _scale_drawn(maps, skip)
    if map_means[scale_drawn] is None:
        return None
    if from_means:
        result = evaluate(set())
        if result is None:
            return None
        tint, doubled = _clipped(result[0] / np.maximum(map_means[scale_drawn], 1.0 / 255.0))
        return Tint(tint, scale_drawn, result[0], map_means[scale_drawn], DOUBLED if doubled else MODULATE)

    fade_free = fade_mode == 0 and fade_source == 0

    def tint_for(j):
        texels = means.texels(maps[j][0])
        if texels is None:
            return None
        result = evaluate(group_of(j), spread_out=spread)
        if result is None:
            return None
        tint, model, errors = _fit(result, texels, blend, fade_free)
        found = Tint(tint, j, result.mean(0), map_means[j], model)
        found.errors = errors
        return found

    # each candidate's share of the output: the variance of what the frame
    # gets when that map (with its layered copies) is the only one varying
    shares, alpha_shares = {}, {}
    for j in range(skip, count):
        if any(maps[i] == maps[j] for i in shares) or means.texels(maps[j][0]) is None:
            continue
        result = evaluate(group_of(j))
        if result is not None:
            shares[j] = _share(result, blend)
            alpha_shares[j] = float(result[:, 3].var())
    drawn = scale_drawn
    if shares:
        best = max(shares, key=lambda j: shares[j])
        own = max([shares[j] for j in shares if maps[j] == maps[scale_drawn]] or [0.0])
        if maps[best] != maps[scale_drawn] and shares[best] > DRAWN_MARGIN * own:
            drawn = best
    found = tint_for(drawn)
    if found is None:
        # a map that can't be sampled (3D, unreadable): from its mean
        return shader_tint(m, tag, means, from_means=True)
    found.scale_drawn, found.shares, found.alpha_shares, found.blend = scale_drawn, shares, alpha_shares, blend
    if drawn != scale_drawn:
        found.alt = tint_for(scale_drawn) or shader_tint(m, tag, means, from_means=True)
    if kind == GENERIC and found.model in (MODULATE, DOUBLED):
        _angle_tints(found, [evaluate(group_of(drawn), view, spread) for view in ('face', 'edge')])
    return found


def _angle_tints(found, results):
    """found.angle from the shader's colour face on and edge on (results),
    when the two differ by more than ANGLE_APART in a channel. Both are
    drawn with one doubling, so the middle tint is redone to match it"""
    if any(r is None for r in results):
        return
    face, edge = (r.mean(0) for r in results)
    if np.abs(face - edge).max() <= ANGLE_APART:
        return
    floor = np.maximum(found.mean, 1.0 / 255.0)
    quotients = [face / floor, edge / floor, found.result / floor]
    doubled = any(q[:3].max() > DOUBLED_ABOVE for q in quotients)
    half = 0.5 if doubled else 1.0
    face_tint, edge_tint, middle = (np.clip(np.append(q[:3] * half, q[3]), 0.0, 1.0) for q in quotients)
    if doubled != found.doubled:
        found.tint, found.model = middle, DOUBLED if doubled else MODULATE
    found.angle = (face_tint, edge_tint)


# ---------------------------------------------------------------- composites

# COMPOSITES (texpack.py; --no-composites leaves them out): one map and a tint
# can't stand for a shader whose look comes from two maps. A thruster's cone
# is one map's alpha and its colour another's; the Jackal's shield is a mask
# times two noise maps, added. Drawn as one of them, the cone came out a hard
# card and the shield a solid disc. Such a shader gets a texture of its own:
# the shader worked out texel by texel with every map in place, laid out on
# the map it is drawn in place of (that map's scale, offset, clamping and
# animation carry it). What a still texture can't hold: the other maps'
# scrolling (frozen where it stood at time 0), the angle (its middle value)
COMPOSITES = True
# another map earns a composite when it carries this share of the output (or,
# for a blend that lets the background through, of the alpha) against the
# map that carries most
COMPOSITE_SHARE = 0.1
# below this the alpha hardly varies at all: no shape to keep
ALPHA_FLAT = 1e-4
COMPOSITE_SIDE = 256
# how far from a whole number another map's repeats over the drawn map's may
# be before the composite would show a seam where it tiles
TILE_APART = 0.01


def _place(m, o, kind, i):
    """map i's (u scale, v scale, u offset, v offset, u clamped, v clamped),
    as psp_transparent_map reads them"""
    stride, scale_at = (220, 84) if kind == CHICAGO else (100, 4)
    maps_p, = struct.unpack_from('<I', m.data, o + 88)
    mo = m.off(maps_p) + stride * i
    flags, = struct.unpack_from('<H', m.data, mo)
    su, sv, ou, ov = struct.unpack_from('<4f', m.data, mo + scale_at)
    u_bit, v_bit = (4, 8) if kind == CHICAGO else (2, 4)
    return su or 1.0, sv or 1.0, ou, ov, bool(flags & u_bit), bool(flags & v_bit)


def _sample(img, u, v, clamp_u, clamp_v):
    """img (h x w x RGBA, 0-255) at texture coordinates u, v, filtered as the
    GE filters (bilinear), repeated or clamped as the map is: n x RGBA 0-1"""
    h, w = img.shape[:2]
    x, y = u * w - 0.5, v * h - 0.5
    x0, y0 = np.floor(x), np.floor(y)
    fx, fy = (x - x0)[:, None], (y - y0)[:, None]

    def index(i, n, clamp):
        return (np.clip(i, 0, n - 1) if clamp else np.mod(i, n)).astype(np.int64)
    xa, xb = index(x0, w, clamp_u), index(x0 + 1, w, clamp_u)
    ya, yb = index(y0, h, clamp_v), index(y0 + 1, h, clamp_v)
    f = img.astype(np.float64) / 255.0
    return (f[ya, xa] * (1 - fx) + f[ya, xb] * fx) * (1 - fy) + (f[yb, xa] * (1 - fx) + f[yb, xb] * fx) * fy


def _pow2(v):
    p = 1
    while p < v:
        p *= 2
    return p


def composite(m, tag, means, found):
    """(the map index the composite is drawn in place of, the composite as
    h x w x RGBA uint8, a note for the log) for a shader that needs one, or
    (None, None, why not) for one that would but can't have it, or None"""
    shares = getattr(found, 'shares', None)
    if not shares or len(shares) < 2:
        return None
    o = m.off(tag.address)
    kind, = struct.unpack_from('<H', m.data, o + 36)
    count, maps_p = struct.unpack_from('<iI', m.data, o + 84)
    first_map_type, = struct.unpack_from('<h', m.data, o + 42)
    skip = 1 if first_map_type != 0 else 0
    maps = _maps(m, o, kind)
    alpha, blend = found.alpha_shares, found.blend
    top, top_alpha = max(shares.values()), max(alpha.values())
    shaped = blend in SHAPE_BLENDS and top_alpha > ALPHA_FLAT
    shape = max(alpha, key=lambda j: alpha[j])
    others = [j for j in shares if maps[j] != maps[found.drawn] and (
        shares[j] >= COMPOSITE_SHARE * top or (shaped and alpha[j] >= COMPOSITE_SHARE * top_alpha))]
    if not others:
        return None
    if found.angle is not None:
        return None, None, 'its colour turns with the angle it is seen at'
    places = {i: _place(m, o, kind, i) for i in range(skip, count)}
    images = {i: means.image(maps[i][0]) for i in range(skip, count)}

    def fits(basis):
        """the composite's size laid out on basis, or None when another map
        spans more of the surface than basis does, or would seam where
        basis repeats"""
        su, sv, _, _, cu, cv = places[basis]
        w, h = images[basis].shape[1], images[basis].shape[0]
        for i in range(skip, count):
            if i == basis or images[i] is None:
                continue
            for r, clamped, side in ((places[i][0] / su, cu, 1), (places[i][1] / sv, cv, 0)):
                if not clamped and (r < 1.0 - TILE_APART or abs(r - round(r)) > TILE_APART):
                    return None
                need = images[i].shape[side] * r
                if side:
                    w = max(w, need)
                else:
                    h = max(h, need)
        w, h = _pow2(w), _pow2(h)
        base_w, base_h = images[basis].shape[1], images[basis].shape[0]
        return max(base_w, min(w, COMPOSITE_SIDE)), max(base_h, min(h, COMPOSITE_SIDE))

    # laid out on the map that shapes it where the shape is what's lost (a
    # cone's mask stays still while its colour scrolls), else on the drawn map
    order = ([shape] if shaped else []) + [found.drawn, found.scale_drawn]
    basis = next((j for j in order if images.get(j) is not None and fits(j)), None)
    if basis is None:
        return None, None, 'no map spans the others whole'
    w, h = fits(basis)
    su, sv, ou, ov, _, _ = places[basis]
    tu, tv = np.meshgrid((np.arange(w) + 0.5) / w, (np.arange(h) + 0.5) / h)
    # the surface's coordinates under each texel of the basis, then each map's
    su_, sv_ = (tu.ravel() - ou) / su, (tv.ravel() - ov) / sv
    given = []
    for i in range(count):
        if i < skip or images.get(i) is None:
            given.append(means(maps[i][0]))
            continue
        pu, pv, qu, qv, cu, cv = places[i]
        given.append(_sample(images[i], su_ * pu + qu, sv_ * pv + qv, cu, cv))
    result = generic_result(m, o, given) if kind == GENERIC else chicago_result(m, o, maps_p, count, given)
    if result is None:
        return None, None, 'its combiners could not be worked out'
    img = (np.clip(result, 0.0, 1.0) * 255.0 + 0.5).astype(np.uint8).reshape(h, w, 4)
    return basis, img, 'map %d %s, %dx%d, from maps %s' % (
        basis, 'shapes it' if basis == shape and shaped else 'drawn', w, h,
        ' '.join(str(i) for i in range(skip, count) if images.get(i) is not None))


def abgr(rgba):
    """a tint as the GE's colour word"""
    r, g, b, a = (int(round(float(v) * 255.0)) for v in rgba)
    return (a << 24) | (b << 16) | (g << 8) | r


# (shader name, Tint) for every shader the last tints() call worked out
FOUND = []


def tints(m, bitmaps, shaders=None, from_means=False):
    """({key: (ABGR, the shader's name, its Tint)}, {address: its Tint}): the
    first for every generic, chicago and water shader whose draw needs a tint
    (one that isn't white, or isn't drawn by plain modulate), keyed by its
    tag address plus its model (tag addresses are multiples of 4; psp_tex.c
    reads the two bits); the second for every shader whose drawn map isn't
    the one the runtime's own rule picks"""
    means = Means(m, bitmaps)
    out, drawn = {}, {}
    del FOUND[:]
    for t in m.tags:
        if t.group not in ('sotr', 'schi', 'swat') or (shaders is not None and t.name not in shaders):
            continue
        found = shader_tint(m, t, means, from_means)
        if found is None:
            continue
        found.tag = t
        FOUND.append((t.name, found))
        if not found.plain():
            out[t.address | found.flags()] = (found.word(), t.name, found)
        if found.drawn != found.scale_drawn:
            drawn[t.address] = found
    return out, drawn
