"""bspread.py - the level's render geometry from a PSP-built (-G) map and its
.pgeo pack: per material, the shader, the pack address (the renderer's key),
the rendered vertices (32 bytes, or 24 in a slim pack) and 8-byte lightmap
vertices exactly as the renderer copies them, and the triangles (surfaces).

usage (library): mats, info = load(map_path, pgeo_path, bsp_index=0)"""
import struct, sys, os
import numpy as np
sys.path.insert(0, os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), 'bake', 'lib'))  # the bake's readers (halomap.py, ...)
from halomap import Defs, CacheMap

PACK_BASE = 0x0C000000
HEADER = 64  # psp_geo.c HEADER_BYTES: the PGEO header before the data
FORMAT_LEVEL_SLIM = 0x10  # header word 10: rendered vertices without binormal and tangent (levelslim.py)


def fo(d, s):
    return {f.get('name'): x for x, f, sz in d.fields(s)}


def load(map_path, pgeo_path, which=0):
    d = Defs()
    m = CacheMap(map_path)
    pack = open(pgeo_path, 'rb').read()
    slim = len(pack) >= HEADER and struct.unpack_from('<I', pack, 40)[0] & FORMAT_LEVEL_SLIM
    # a slim pack's rendered vertex: position, normal, uv (the 32-byte one has
    # the binormal and tangent between normal and uv)
    stride, uv_at = (24, 16) if slim else (32, 24)
    scn = m.tags[m.scenario_id & 0xffff]
    sf = fo(d, 'Scenario')
    n, p = struct.unpack_from('<iI', m.data, m.off(scn.address) + sf['structure bsps'])
    start, size, addr = struct.unpack_from('<III', m.data, m.off(p) + 32 * which)
    tr = lambda a: start + (a - addr)
    base, = struct.unpack_from('<I', m.data, start)
    bf = fo(d, 'ScenarioStructureBSP')
    so = tr(base)
    sn, sp = struct.unpack_from('<iI', m.data, so + bf['surfaces'])
    # a BSP without surfaces may leave their pointer 0
    surfaces = np.frombuffer(m.data, '<u2', sn * 3, tr(sp)).reshape(sn, 3) if sn > 0 else np.zeros((0, 3), '<u2')
    ln, lp = struct.unpack_from('<iI', m.data, so + bf['lightmaps'])
    lmf = fo(d, 'ScenarioStructureBSPLightmap')
    mf = fo(d, 'ScenarioStructureBSPMaterial')
    mats = []
    for li in range(ln):
        lo = tr(lp) + 32 * li
        mn, mp = struct.unpack_from('<iI', m.data, lo + lmf['materials'])
        lbi, = struct.unpack_from('<h', m.data, lo)
        for mi in range(mn):
            mo = tr(mp) + 256 * mi
            g, _, _, tid = struct.unpack_from('<IIii', m.data, mo + mf['shader'])
            shader = m.by_id.get(tid)
            first, count = struct.unpack_from('<ii', m.data, mo + mf['surfaces'])
            rcount, = struct.unpack_from('<I', m.data, mo + mf['rendered vertices count'])
            lcount, = struct.unpack_from('<I', m.data, mo + mf['lightmap vertices count'])
            ip, = struct.unpack_from('<I', m.data, mo + mf['rendered vertices index pointer'])
            key, = struct.unpack_from('<I', m.data, tr(ip) + 4)  # MemeBSPPointer {pad, pointer, pad}
            centroid = struct.unpack_from('<3f', m.data, mo + mf['centroid'])
            plane = struct.unpack_from('<4f', m.data, mo + mf['plane'])
            if key >= PACK_BASE:
                raw = pack[HEADER + key - PACK_BASE: HEADER + key - PACK_BASE + rcount * stride + lcount * 8]
                vstride, vuv = stride, uv_at
            else:
                raw = m.data[tr(key): tr(key) + rcount * 32 + lcount * 8]
                vstride, vuv = 32, 24
            v = np.frombuffer(raw, np.uint8, rcount * vstride).reshape(rcount, vstride)
            pos = v[:, :12].copy().view('<f4').reshape(rcount, 3)
            normal = v[:, 12:16].copy().view('<u4').reshape(rcount)
            uv = v[:, vuv:vuv + 8].copy().view('<f4').reshape(rcount, 2)
            lm = np.frombuffer(raw, np.uint8, lcount * 8, rcount * vstride).reshape(lcount, 8) if lcount else None
            tris = surfaces[first:first + count].astype(np.int64)
            mats.append(dict(lightmap=lbi, lightmap_index=li, material_index=mi, key=key, rcount=rcount, lcount=lcount,
                             shader=shader.name if shader else None, group=shader.group if shader else None,
                             pos=pos.astype(np.float64), pos32=pos, uv=uv, normal=normal, lmraw=lm, tris=tris,
                             first_surface=first, centroid=centroid, plane=plane, raw=raw))
    return mats, dict(map=m, surfaces=surfaces)


if __name__ == '__main__':
    mats, info = load(sys.argv[1], sys.argv[2])
    from collections import Counter
    print('%d materials, %d triangles' % (len(mats), sum(len(x['tris']) for x in mats)))
    for x in mats:
        print('%2d/%2d %s %-60s key %08x verts %5d lm %5d tris %5d' % (x['lightmap_index'], x['material_index'], x['group'],
              x['shader'], x['key'], x['rcount'], x['lcount'], len(x['tris'])))
