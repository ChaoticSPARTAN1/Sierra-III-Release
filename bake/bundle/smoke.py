#!/usr/bin/env python3
"""smoke.py - checks a built bundle without any game data: every script
imports, the pure-Python parts work on made-up data, Invader's three tools
and atracdenc start, and atracdenc turns a made-up tone into the RIFF ATRAC3
the XMB plays.

    <bundle>/.venv/bin/python smoke.py <bundle>    (after halo-psp-bake --setup)

Exits non-zero at the first thing that fails."""
import importlib
import math
import os
import struct
import subprocess
import sys
import tempfile

BUNDLE = os.path.abspath(sys.argv[1])
EXE = '.exe' if os.name == 'nt' else ''
sys.path[:0] = [os.path.join(BUNDLE, 'lib'), BUNDLE]
os.environ['HALO_PSP_INVADER_DEFINITIONS'] = os.path.join(BUNDLE, 'invader', 'definitions')


def ok(text):
    print('smoke: ok  ' + text, flush=True)


def tools():
    for tool in ('invader-build', 'invader-extract', 'invader-info'):
        path = os.path.join(BUNDLE, 'invader', 'bin', tool + EXE)
        r = subprocess.run([path, '--help'], capture_output=True, text=True, errors='replace')
        assert r.returncode == 0, (tool, r.returncode, r.stdout[-500:], r.stderr[-500:])
        ok('%s starts (%s)' % (tool, (r.stdout.strip().splitlines() or ['?'])[0][:60]))
    build_help = subprocess.run([os.path.join(BUNDLE, 'invader', 'bin', 'invader-build' + EXE), '--help'],
                                capture_output=True, text=True, errors='replace').stdout
    for option in ('psp-geometry', 'with-index'):
        assert option in build_help, 'invader-build has no --%s: not the port\'s fork' % option
    ok('invader-build is the port\'s fork (--psp-geometry)')


def imports():
    import numpy, PIL, shapely
    ok('numpy %s, Pillow %s, shapely %s on Python %s' % (numpy.__version__, PIL.__version__, shapely.__version__,
                                                         sys.version.split()[0]))
    # dez.py is a bare script (it runs when imported)
    names = sorted(f[:-3] for f in os.listdir(os.path.join(BUNDLE, 'lib')) if f.endswith('.py') and f != 'dez.py')
    for name in names + ['xiso', 'bake']:
        importlib.import_module(name)
    ok('every script imports: %s' % ', '.join(names))
    import halomap
    d = halomap.Defs()
    assert 'Sound' in d.structs and 'ScenarioStructureBSP' in d.structs, 'Invader definitions incomplete'
    assert halomap.FOURCC_GROUP.get('scnr') == 'scenario', 'the tag group table'
    ok('Invader definitions: %d structs, %d groups' % (len(d.structs), len(halomap.FOURCC_GROUP)))


def pure_python():
    import numpy as np
    r = subprocess.run([sys.executable, os.path.join(BUNDLE, 'xiso.py')], capture_output=True, text=True)
    assert r.returncode == 0 and 'passed' in r.stdout, r.stdout + r.stderr
    ok('xiso self-test (a made-up disc image)')

    import bitmap_bake
    rng = np.random.RandomState(1)
    img = np.zeros((64, 64, 4), np.uint8)
    img[:, :, :3] = rng.randint(0, 256, (64, 64, 3))
    img[:, :, 3] = 255
    indices, palette, rec, alpha = bitmap_bake.quantize(img, 16)
    assert indices.shape == (64, 64) and len(palette) <= 16 and not alpha
    error = np.abs(rec[:, :, :3].astype(int) - img[:, :, :3]).mean()
    assert error < 40, error
    centres, labels = bitmap_bake.kmeans(rng.rand(500, 3), 8, 60)
    assert centres.shape == (8, 3) and len(set(labels.tolist())) == 8
    ok('palettes: 16 colours for random texels (mean error %.1f), k-means' % error)

    # the palette cache: what it reads back is what quantize made, on this
    # OS's file system (the write's rename into place included)
    import texpack
    with tempfile.TemporaryDirectory() as cache:
        texpack.PALETTE_CACHE = cache
        made = texpack.make_palette(img, 16, "smoke")
        read = texpack.make_palette(img, 16, "smoke")
        texpack.PALETTE_CACHE = None
        assert len(os.listdir(cache)) == 1, os.listdir(cache)
    assert all(np.array_equal(x, y) for x, y in zip(made[:3], read[:3])) and made[3] == read[3]
    assert np.array_equal(made[0], indices) and np.array_equal(made[1], palette)
    fast = bitmap_bake.quantize(img, 16, fast=True)
    assert np.abs(fast[2][:, :, :3].astype(int) - img[:, :, :3]).mean() < 40
    ok('palette cache: a palette read back the same as made; fast palettes')

    import stencilbake
    V = np.array([(0, 0), (4, 0), (4, 4), (0, 4), (1, 1), (3, 1), (3, 3), (1, 3)], np.float64)
    S = np.array([(0, 1), (1, 2), (2, 3), (3, 0), (4, 5), (5, 6), (6, 7), (7, 4)], np.int32)
    tv, tt = stencilbake.pslg_triangulate(V, S)
    tv = np.asarray(tv, np.float64)
    area = sum(abs((tv[b][0] - tv[a][0]) * (tv[c][1] - tv[a][1]) - (tv[c][0] - tv[a][0]) * (tv[b][1] - tv[a][1])) / 2
               for a, b, c in tt)
    assert abs(area - 16) < 1e-9, area
    ok('decal cuts: a square with a square inside, %d triangles covering it exactly' % len(tt))

    import snd0, chant
    assert chant.find(os.urandom(40000)) is None
    blocks = os.urandom(36 * 2 * 200)
    pcm = snd0.decode_xbox_adpcm(blocks, 2)
    assert pcm.shape == (200 * 64, 2)
    ok('Xbox ADPCM decode, chant search (made-up bytes)')

    import bake
    out = tempfile.mkdtemp()
    os.makedirs(os.path.join(out, 'halo'))
    bake.write_stamp(out)
    stamp = dict(line.split(': ', 1) for line in open(os.path.join(out, 'halo', 'bake.txt')).read().splitlines()
                 if ': ' in line and not line.startswith('#'))
    assert stamp['format'] == str(bake.BAKE_FORMAT) and stamp['bundle'] not in ('', 'unknown'), stamp
    ok('the stamp: bake format %s, bundle %s' % (stamp['format'], stamp['bundle']))


def menu_sound():
    import numpy as np
    import snd0
    encoder = os.path.join(BUNDLE, 'atracdenc', 'bin', 'atracdenc' + EXE)
    with tempfile.TemporaryDirectory() as work:
        t = np.arange(44100 * 3) / 44100
        tone = np.stack([np.sin(2 * math.pi * 440 * t), np.sin(2 * math.pi * 660 * t)], 1) * 20000
        clip = snd0.cut(tone.astype(np.int16), 0.5, 2.0, 0.02, 0.5, -3)
        wav, at3 = os.path.join(work, 'tone.wav'), os.path.join(work, 'SND0.AT3')
        snd0.write_wav(wav, clip)
        snd0.encode_at3(wav, at3, encoder)       # checks the RIFF ATRAC3 header itself
        size = os.path.getsize(at3)
        # a made-up EBOOT: the header, eight offsets, then eight sections
        sections = [b'PARAM', b'', b'', b'', b'', b'old sound', b'DATA', b'']
        offsets, at = [], 40
        for s in sections:
            offsets.append(at)
            at += len(s)
        pbp = os.path.join(work, 'EBOOT.PBP')
        with open(pbp, 'wb') as f:
            f.write(b'\0PBP' + struct.pack('<I', 0x10000) + struct.pack('<8I', *offsets) + b''.join(sections))
        out = os.path.join(work, 'EBOOT_out.PBP')
        snd0.pbp_with(pbp, out, snd0.SND0, open(at3, 'rb').read())
        raw = open(out, 'rb').read()
        o = struct.unpack_from('<8I', raw, 8)
        assert raw[o[5]:o[6]] == open(at3, 'rb').read() and raw[o[6]:o[7]] == b'DATA'
    ok('menu sound: a 2 s tone through atracdenc to RIFF ATRAC3 (%d bytes), into an EBOOT' % size)


if __name__ == '__main__':
    tools()
    imports()
    pure_python()
    menu_sound()
    print('smoke: all passed', flush=True)
