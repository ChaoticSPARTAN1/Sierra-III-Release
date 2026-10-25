#!/usr/bin/env python3
"""jobs_check.py - checks that bake.py's --jobs changes only how long a bake
takes: the same maps come out byte for byte whatever the number of maps
baked at once.

    python3 jobs_check.py <bundle or this checkout's bake/ folder>

It copies the bundle's bake.py and xiso.py beside stand-ins for every tool a
map's bake runs (Invader's three, and the scripts in lib/). Each stand-in
waits a random moment, so the maps finish in a different order every time,
then writes what it makes from its inputs alone. Made-up maps are baked with
--jobs 1, 2 and 4; every output must match, and each map's lines in the log
must carry its name. No game data is read or made. Linux and macOS only (the
stand-ins are scripts, where Windows wants .exe files)."""
import hashlib
import os
import random
import shutil
import stat
import subprocess
import sys
import tempfile

STUB = r'''#!%(python)s
import hashlib, os, random, sys, time
time.sleep(random.random() * 0.3)
TOOL = %(tool)r
args = sys.argv[1:]


def digest(*parts):
    h = hashlib.sha256(TOOL.encode())
    for part in parts:
        h.update(part if isinstance(part, bytes) else str(part).encode())
    return h.digest() * 64


def read(path):
    with open(path, 'rb') as f:
        return f.read()


def tree(folder):
    return b''.join(name.encode() + read(os.path.join(folder, name)) for name in sorted(os.listdir(folder)))


def value(flag):
    return args[args.index(flag) + 1]


if TOOL == 'invader-extract':
    with open(os.path.join(value('-t'), 'scenario.tag'), 'wb') as f:
        f.write(digest(read(args[-1]), '-n' in args))
elif TOOL == 'invader-info':
    print('levels\\test\\' + os.path.basename(args[-1])[:-4])
elif TOOL == 'invader-build':
    made = digest(tree(value('-t')), [a for a in args if not os.path.isabs(a)],
                  read(value('-w')) if '-w' in args else b'')
    for path in (value('-o'), value('-G')):
        with open(path, 'wb') as f:
            f.write(made + path.endswith('.pgeo') * b'pgeo')
    print('Tag space: 1.0 MiB')
elif TOOL == 'tagindex':
    with open(args[1], 'wb') as f:
        f.write(digest(read(args[0])))
elif TOOL == 'dez':
    with open(args[1], 'wb') as f:
        f.write(read(args[0]))
elif TOOL == 'texpack':
    with open(args[1], 'wb') as f:
        f.write(digest(read(args[0]), [a for a in args[2:] if not os.path.isabs(a)],
                       read(value('--pgeo')) if '--pgeo' in args else b''))
    print('quality ' + os.path.basename(args[1]))
elif TOOL == 'soundpack':
    # the sound bake rewrites the extracted tags in place (bake.py's --sound)
    path = os.path.join(args[0], 'scenario.tag')
    data = read(path)
    with open(path, 'wb') as f:
        f.write(digest(data, args[1]))
    print('soundpack ' + args[1] + ': stand-in')
elif TOOL == 'stencilbake':
    with open(args[2], 'wb') as f:
        f.write(digest(read(args[0]), read(args[1])))
    print('compare: ' + os.path.basename(args[2]))
'''
CHANT = 'def write(xbe, out):\n    return False\n'
MAPS = ['a10', 'a30', 'b30', 'bloodgulch', 'putput', 'ui']


def make_bundle(source, folder):
    os.makedirs(os.path.join(folder, 'lib'))
    os.makedirs(os.path.join(folder, 'invader', 'bin'))
    for name in ('bake.py', 'xiso.py', 'hashes.json'):
        shutil.copy(os.path.join(source, name), folder)
    stubs = [(os.path.join(folder, 'invader', 'bin', t), t) for t in ('invader-extract', 'invader-info', 'invader-build')]
    stubs += [(os.path.join(folder, 'lib', t + '.py'), t) for t in ('tagindex', 'dez', 'texpack', 'stencilbake', 'soundpack')]
    for path, tool in stubs:
        with open(path, 'w') as f:
            f.write(STUB % {'python': sys.executable, 'tool': tool})
        os.chmod(path, os.stat(path).st_mode | stat.S_IXUSR)
    with open(os.path.join(folder, 'lib', 'chant.py'), 'w') as f:
        f.write(CHANT)


def outputs(folder):
    maps = os.path.join(folder, 'halo', 'maps')
    return {name: hashlib.sha256(open(os.path.join(maps, name), 'rb').read()).hexdigest()
            for name in sorted(os.listdir(maps))}


def main():
    if os.name == 'nt':
        print('jobs_check: skipped on Windows (the stand-ins are scripts)')
        return
    source = os.path.abspath(sys.argv[1])
    with tempfile.TemporaryDirectory() as work:
        bundle = os.path.join(work, 'bundle')
        make_bundle(source, bundle)
        discs = os.path.join(work, 'disc')
        os.makedirs(discs)
        rng = random.Random(1)
        for name in MAPS:
            with open(os.path.join(discs, name + '.map'), 'wb') as f:
                f.write(bytes(rng.randrange(256) for _ in range(4096)))
        made = {}
        for jobs in (1, 2, 4):
            out = os.path.join(work, 'out%d' % jobs)
            r = subprocess.run([sys.executable, os.path.join(bundle, 'bake.py'), discs, out, '--force',
                                '--maps', ','.join(MAPS), '--jobs', str(jobs)], capture_output=True, text=True)
            assert r.returncode == 0, r.stdout[-2000:] + r.stderr[-2000:]
            made[jobs] = outputs(out)
            # every line a map's bake logs names the map
            for line in r.stdout.splitlines():
                text = line[9:]
                if any(word in text for word in ('quality', 'compare:', 'Tag space', 'textures', 'done in')):
                    assert text.split(':')[0] in MAPS, line
            assert ('%d at a time' % jobs) in r.stdout, r.stdout[:500]
            print('jobs_check: --jobs %d made %d files' % (jobs, len(made[jobs])))
        # five files a map: the ui has two showcase packs where a level has its track pack and decals
        assert len(made[1]) == len(MAPS) * 5, sorted(made[1])
        for jobs in (2, 4):
            assert made[jobs] == made[1], [n for n in made[1] if made[jobs].get(n) != made[1][n]]
    print('jobs_check: --jobs 1, 2 and 4 made the same %d files, byte for byte' % len(made[1]))


if __name__ == '__main__':
    main()
