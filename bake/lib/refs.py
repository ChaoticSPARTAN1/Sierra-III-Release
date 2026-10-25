"""refs.py - who references each bitmap (shader field), plus padding /
non-runtime bytes in the tag window.
usage: python3 refs.py map.map"""
import sys, struct, collections, json
from halomap import Defs, CacheMap, Walker, FOURCC_GROUP
from assets import bitmaps

FALLBACK = {'Soul': 'TagCollection'}


def bitmap_roles(m, d):
    """bitmap tag name -> set of 'group:field' that reference it (BSP included)"""
    roles = collections.defaultdict(set)
    W = Walker(d, m.data, lambda p: m.off(p) if m.in_tags(p) else None)
    for t in m.tags:
        if not t.address or not m.in_tags(t.address):
            continue
        sname = d.groups.get(FOURCC_GROUP.get(t.group)) or FALLBACK.get(t.group)

        def cb(kind, p, tid, n, g, c, t=t):
            if kind == 'dep' and g == 'bitm':
                tt = m.by_id.get(tid)
                if tt:
                    roles[tt.name].add('%s:%s' % (t.group, p))
        W.walk(sname, m.off(t.address), 1, '', cb)
    # the BSP's lightmap bitmap
    scn = m.tags[m.scenario_id & 0xffff]
    off = [x for x, f, s in d.fields('Scenario') if f.get('name') == 'structure bsps'][0]
    n, p = struct.unpack_from('<iI', m.data, m.off(scn.address) + off)
    for i in range(n):
        start, size, addr = struct.unpack_from('<III', m.data, m.off(p) + 32 * i)
        base, = struct.unpack_from('<I', m.data, start)
        g, _, _, tid = struct.unpack_from('<IIii', m.data, start + (base - addr))
        if m.by_id.get(tid):
            roles[m.by_id[tid].name].add('sbsp:/lightmaps bitmap')
    return roles


def padding(m, d):
    """bytes of 'pad' fields per group (instances x pad bytes)"""
    pad_of = {}
    for name, s in d.structs.items():
        pad_of[name] = sum(f['size'] for f in s['fields'] if f['type'] == 'pad')
    tot = collections.Counter()
    W = Walker(d, m.data, lambda p: m.off(p) if m.in_tags(p) else None)
    for t in m.tags:
        if not t.address or not m.in_tags(t.address):
            continue
        sname = d.groups.get(FOURCC_GROUP.get(t.group)) or FALLBACK.get(t.group)
        # root (including inherited parents' padding)
        chain, s = [], sname
        while s:
            chain.append(s)
            s = d.structs[s].get('inherits')
        tot[t.group] += sum(pad_of[c] for c in chain)

        def cb(kind, p, fo, n, sub, count, t=t):
            if kind == 'block':
                tot[t.group] += pad_of[sub] * count
        W.walk(sname, m.off(t.address), 1, '', cb)
    return tot


# a colour map that a shader names as both its base and its bump map (c10's
# hanging moss) is drawn, so it is baked as a base map; the bump role alone
# had it dropped and the surface drew flat in the shader's colour.
# texpack.py --base-as-bump: dropped, as before
BASE_OVER_BUMP = True


def classify(roles_for_tag, bm):
    r = ' '.join(sorted(roles_for_tag))
    if 'lightmaps bitmap' in r:
        return 'lightmap'
    bump_role = 'bump map' in r and not (BASE_OVER_BUMP and 'base map' in r)
    if bm['format'] == 'P8' or bump_role or bm['usage'] in ('height map', 'vector map'):
        return 'bump'
    if bm['type'] == 'cube' or 'reflection cube map' in r:
        return 'cube'
    if bm['type'] == '3d':
        return '3d'
    if 'detail map' in r or bm['usage'] == 'detail map':
        return 'detail'
    if any(x in r for x in ('unhi:', 'wphi:', 'grhi:', 'hudg:', 'DeLa:', 'hud#:', 'font', 'metr:', 'colo:')) or \
            bm['tag'].startswith('ui\\') or bm['group_type'] == 'interface bitmaps':
        return 'ui/hud'
    if bm['group_type'] == 'sprites' or any(x in r for x in ('part:', 'pctl:', 'cont:', 'deca:', 'lens:', 'ligh:', 'mgs2:', 'rain:', 'glw!:')):
        return 'effects/sprites'
    if 'senv:' in r:
        return 'level (senv)'
    if 'soso:' in r:
        return 'objects (soso)'
    if 'sky ' in r or bm['tag'].startswith('sky\\'):
        return 'sky'
    if any(x in r for x in ('sotr:', 'schi:', 'scex:', 'sgla:', 'smet:', 'spla:', 'swat:')):
        return 'transparent shaders'
    if not r:
        return 'unreferenced'
    return 'other'


if __name__ == '__main__':
    d = Defs()
    m = CacheMap(sys.argv[1])
    roles = bitmap_roles(m, d)
    B = bitmaps(m, d)
    c, n = collections.Counter(), collections.Counter()
    fmt = collections.defaultdict(collections.Counter)
    for b in B:
        k = classify(roles.get(b['tag'], ()), b)
        b['class'] = k
        c[k] += b['pixels_size']
        n[k] += 1
        fmt[k][b['format']] += b['pixels_size']
    print('== bitmap pixel data by role')
    for k, v in c.most_common():
        print('  %-20s %4d %10d B  %s' % (k, n[k], v, dict(fmt[k].most_common(4))))
    unref = [b['tag'] for b in B if b['class'] == 'unreferenced']
    print('  unreferenced tags:', sorted(set(unref))[:30])
    pad = padding(m, d)
    print('== pad bytes in tag structs: %d B' % sum(pad.values()))
    for g, v in pad.most_common(12):
        print('   %-6s %d' % (g, v))
    json.dump({b['tag'] + '#%d' % b['index']: b['class'] for b in B}, open('bitmap_classes_%s.json' % m.name, 'w'), indent=0)
