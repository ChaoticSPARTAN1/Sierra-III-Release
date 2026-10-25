"""assets.py - enumerate raw asset data of an Xbox-format map: every bitmap
(bitmap_data) and every sound permutation, with formats and sizes.

Used by census reports and by bitmap_bake.py.
"""
import struct, collections
from halomap import Defs, CacheMap

FORMATS = ['A8', 'Y8', 'AY8', 'A8Y8', '?4', '?5', 'R5G6B5', '?7', 'A1R5G5B5', 'A4R4G4B4',
           'X8R8G8B8', 'A8R8G8B8', '?12', '?13', 'DXT1', 'DXT3', 'DXT5', 'P8']
BPP = [8, 8, 8, 16, 0, 0, 16, 0, 16, 16, 32, 32, 0, 0, 4, 8, 8, 8]
TYPES = ['2d', '3d', 'cube', 'white']
USAGES = ['alpha blend', 'default', 'height map', 'detail map', 'light map', 'vector map']
BTYPES = ['2d textures', '3d textures', 'cube maps', 'sprites', 'interface bitmaps']
# bitmap_data flags (bitmap_group.c / xbox_texture_cache.c)
F_POW2, F_COMPRESSED, F_PALETTIZED, F_SWIZZLED, F_LINEAR, F_V16U16 = 1, 2, 4, 8, 16, 32


def fo(d, struct_name):
    return {f.get('name'): x for x, f, s in d.fields(struct_name)}


def mip_sizes(fmt, w, h, depth, mips, cube):
    """bytes of each mip level (DXT blocks are at least 4x4)"""
    out = []
    for i in range(mips + 1):
        mw, mh, md = max(1, w >> i), max(1, h >> i), max(1, depth >> i)
        if FORMATS[fmt].startswith('DXT'):
            bw, bh = (mw + 3) // 4, (mh + 3) // 4
            b = bw * bh * (8 if FORMATS[fmt] == 'DXT1' else 16)
        else:
            b = mw * mh * BPP[fmt] // 8
        b *= md * (6 if cube else 1)
        out.append(b)
    return out


def bitmaps(m, d):
    bf = fo(d, 'Bitmap')
    bdf = fo(d, 'BitmapData')
    out = []
    for t in m.tags:
        if t.group != 'bitm' or not t.address:
            continue
        o = m.off(t.address)
        btype, bfmt, usage, bflags = struct.unpack_from('<hhhH', m.data, o)
        pd_size, _, pd_off = struct.unpack_from('<iIi', m.data, o + bf['processed pixel data'])
        n, p = struct.unpack_from('<iI', m.data, o + bf['bitmap data'])
        for i in range(n):
            bo = m.off(p) + 48 * i
            (cls, w, h, depth, typ, fmt, flags, rx, ry, mips, _, poff, psize) = \
                struct.unpack_from('<IhhhhhHhhhhIi', m.data, bo)
            out.append(dict(tag=t.name, index=i, group_type=BTYPES[btype] if btype < 5 else btype,
                            usage=USAGES[usage] if usage < 6 else usage,
                            w=w, h=h, depth=depth, type=TYPES[typ], format=FORMATS[fmt], fmt=fmt,
                            flags=flags, mips=mips, pixels_offset=poff, pixels_size=psize,
                            group_file_offset=pd_off, group_size=pd_size))
    return out


def sounds(m, d):
    sf = fo(d, 'Sound')
    prf = fo(d, 'SoundPitchRange')
    pmf = fo(d, 'SoundPermutation')
    out = []
    for t in m.tags:
        if t.group != 'snd!' or not t.address:
            continue
        o = m.off(t.address)
        flags, cls, rate = struct.unpack_from('<Ihh', m.data, o)
        chans, fmt = struct.unpack_from('<hh', m.data, o + sf['channel count'])
        n, p = struct.unpack_from('<iI', m.data, o + sf['pitch ranges'])
        prsize = d.structs['SoundPitchRange']['size']
        pmsize = d.structs['SoundPermutation']['size']
        for i in range(n):
            po = m.off(p) + prsize * i
            pn, pp = struct.unpack_from('<iI', m.data, po + prf['permutations'])
            for j in range(pn):
                qo = m.off(pp) + pmsize * j
                pfmt, = struct.unpack_from('<h', m.data, qo + pmf['format'])
                ssize, ext, soff = struct.unpack_from('<iIi', m.data, qo + pmf['samples'])
                bufsize, = struct.unpack_from('<I', m.data, qo + pmf['buffer size'])
                out.append(dict(tag=t.name, cls=cls, rate=(22050, 44100)[rate] if rate in (0, 1) else rate,
                                channels=(1, 2)[chans] if chans in (0, 1) else chans,
                                format=pfmt, sound_format=fmt, size=ssize, offset=soff,
                                buffer_size=bufsize, ext=ext))
    return out


SOUND_FORMATS = {0: '16-bit PCM', 1: 'Xbox ADPCM', 2: 'IMA ADPCM', 3: 'Ogg Vorbis'}


def sound_seconds(s):
    """duration from the sample data size"""
    if s['format'] == 1:            # Xbox ADPCM: 36 bytes / 64 samples / channel
        samples = s['size'] // (36 * s['channels']) * 64
    elif s['format'] == 0:
        samples = s['size'] // (2 * s['channels'])
    else:
        return None
    return samples / float(s['rate'])
