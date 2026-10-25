#!/usr/bin/env python3
"""guided_check.py - checks a built bundle's double-click path (guided.py)
without any game data: the player's two inputs in their folders, the file a
player double-clicks run with nobody to answer it, and what comes out.

    python guided_check.py <unpacked bundle> [--no-python]

It copies the bundle, puts stand-ins in place of Invader's three tools and
the bake's per-map scripts (jobs_check.py's, which write what they make from
their inputs alone; on Windows, made into .exe files by pip's own launcher
maker), and makes a made-up disc image and release zip, with hashes.json
holding the made-up maps' hashes. Then:

  - each input mistake (nothing there, two things, not a disc image, an
    archive, a map that isn't the retail one, no zip) ends in its plain
    sentence, before anything is set up;
  - a bake from a disc image and the zip (bake.py's MUSIC as shipped: no
    clip chosen, so the menu sound's soft warning) puts the zip's files, the
    EBOOT and every map in COPY-TO-MEMORY-STICK;
  - a bake from a folder of the disc's files and the unpacked release, with
    a clip chosen, puts in the EBOOT with the menu sound;
  - nothing is written outside the bundle's folder.

--no-python runs the bake from the image with the system's Python out of
sight (PATH cut down to links to the shell tools the double-click file uses;
on Windows, to System32 and PowerShell's folder), so the
double-click file downloads the pinned portable Python and the bake runs on
it; on macOS, with the bundle's tools quarantined as a browser's download
leaves them, which the double-click file clears. Exits non-zero at the first
thing that fails."""
import hashlib
import os
import random
import re
import shutil
import stat
import struct
import subprocess
import sys
import tempfile
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from jobs_check import STUB      # noqa: E402

WINDOWS = os.name == 'nt'
MACOS = sys.platform == 'darwin'
START = 'Bake Halo.cmd' if WINDOWS else 'Bake Halo.command' if MACOS else 'bake-halo.sh'
CAMPAIGN = ['a10', 'a30', 'a50', 'b30', 'b40', 'c10', 'c20', 'c40', 'd20', 'd40', 'ui']
DISC = '1-PUT-YOUR-DISC-IMAGE-HERE'
GAME = '2-PUT-THE-HALO-ZIP-HERE'
STICK = 'COPY-TO-MEMORY-STICK'
# the stand-ins for the per-map scripts jobs_check.py has none of: the
# sound bake leaves the tags as they are; the menu sound marks the EBOOT
SOUNDPACK = 'print("soundpack 0 sounds re-encoded (a stand-in)")\n'
SND0 = r'''import shutil, sys
args = sys.argv[1:]
out = args[args.index('--eboot-out') + 1]
shutil.copyfile(args[args.index('--eboot') + 1], out)
with open(out, 'ab') as f:
    f.write(b'SND0 stand-in')
'''
CHANT = '''def write(xbe, out):
    import os
    os.makedirs(os.path.dirname(out), exist_ok=True)
    with open(out, 'wb') as f:
        f.write(b'chant from ' + open(xbe, 'rb').read()[:8])
    return True
'''


def ok(text):
    print('guided_check: ok  ' + text, flush=True)


def eboot_bytes():
    """a made-up EBOOT: the header, eight offsets, then eight sections (smoke.py's)"""
    sections = [b'PARAM', b'', b'', b'', b'', b'', b'DATA', b'']
    offsets, at = [], 40
    for s in sections:
        offsets.append(at)
        at += len(s)
    return b'\0PBP' + struct.pack('<I', 0x10000) + struct.pack('<8I', *offsets) + b''.join(sections)


def stand_in(bundle, folder):
    """a copy of the bundle with the stand-in tools, and the made-up maps' hashes"""
    shutil.copytree(bundle, folder, symlinks=True)
    for f in os.listdir(os.path.join(folder, 'invader', 'bin')):
        os.remove(os.path.join(folder, 'invader', 'bin', f))
    for tool in ('invader-extract', 'invader-info', 'invader-build'):
        text = STUB % {'python': sys.executable, 'tool': tool}
        if WINDOWS:
            # a .exe that runs the script with this Python: what pip makes for
            # a package's commands
            from pip._vendor.distlib.scripts import ScriptMaker
            source = tempfile.mkdtemp()
            with open(os.path.join(source, tool + '.py'), 'w') as f:
                f.write('#!python\n' + text.split('\n', 1)[1])
            maker = ScriptMaker(source, os.path.join(folder, 'invader', 'bin'))
            maker.executable = sys.executable
            maker.variants = {''}
            made = maker.make(tool + '.py')
            assert any(m.endswith(tool + '.exe') for m in made), made
        else:
            path = os.path.join(folder, 'invader', 'bin', tool)
            with open(path, 'w') as f:
                f.write(text)
            os.chmod(path, os.stat(path).st_mode | stat.S_IXUSR)
    for tool in ('tagindex', 'dez', 'texpack', 'stencilbake'):
        with open(os.path.join(folder, 'lib', tool + '.py'), 'w') as f:
            f.write(STUB % {'python': sys.executable, 'tool': tool})
    for name, text in (('soundpack', SOUNDPACK), ('snd0', SND0), ('chant', CHANT)):
        with open(os.path.join(folder, 'lib', name + '.py'), 'w') as f:
            f.write(text)
    rng = random.Random(1)
    maps = {name + '.map': bytes(rng.randrange(256) for _ in range(rng.randrange(4096, 65536))) for name in CAMPAIGN}
    with open(os.path.join(folder, 'hashes.json'), 'w') as f:
        f.write('{%s}' % ', '.join('"%s": "%s"' % (n[:-4], hashlib.sha256(b).hexdigest()) for n, b in maps.items()))
    return maps


def clear(folder):
    for name in os.listdir(folder):
        if name != 'README.txt':
            path = os.path.join(folder, name)
            shutil.rmtree(path) if os.path.isdir(path) else os.remove(path)


def image(bundle, path, maps):
    sys.path.insert(0, bundle)
    from xiso import make_test_image
    make_test_image(path, maps, {'default.xbe': b'XBEH made-up'})


def release(path, as_folder=False):
    files = {'PSP/GAME/halo/EBOOT.PBP': eboot_bytes(), 'PSP/GAME/halo/halo_crash.prx': b'prx made-up',
             'halo/presets.txt': b'presets made-up\n'}
    if as_folder:
        for name, data in files.items():
            os.makedirs(os.path.dirname(os.path.join(path, name)), exist_ok=True)
            with open(os.path.join(path, name), 'wb') as f:
                f.write(data)
    else:
        with zipfile.ZipFile(path, 'w') as z:
            for name, data in files.items():
                z.writestr(name, data)
    return files


# what the double-click file runs besides Python (POSIX)
SHELL_TOOLS = ['uname', 'dirname', 'sed', 'cut', 'tar', 'gzip', 'mkdir', 'rm', 'mv', 'curl', 'wget', 'sha256sum',
               'shasum', 'perl', 'xattr', 'xcode-select']


def env(hide_python=False, tools=None):
    e = dict(os.environ)
    e['HALO_PSP_BAKE_YES'] = '1'
    e.pop('PYTHONPATH', None)
    if hide_python:
        if WINDOWS:
            root = os.environ.get('SystemRoot', r'C:\Windows')
            e['PATH'] = os.pathsep.join([os.path.join(root, 'System32'),
                                         os.path.join(root, 'System32', 'WindowsPowerShell', 'v1.0')])
        else:
            # a PATH of the shell tools alone: no system folder, so no Python
            os.makedirs(tools, exist_ok=True)
            for name in SHELL_TOOLS:
                found = shutil.which(name)
                if found and not os.path.exists(os.path.join(tools, name)):
                    os.symlink(found, os.path.join(tools, name))
            e['PATH'] = tools
    return e


def double_click(bundle, hide_python=False):
    """the file a player double-clicks, with nobody there: (exit code, what it said)"""
    tools = None if WINDOWS or not hide_python else os.path.join(tempfile.mkdtemp(), 'bin')
    # cmd.exe keeps the quotes round a path with a space only after "call"
    command = ['cmd.exe', '/d', '/c', 'call', os.path.join(bundle, START)] if WINDOWS else [os.path.join(bundle, START)]
    r = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                       stdin=subprocess.DEVNULL, env=env(hide_python, tools), cwd=os.path.dirname(bundle), timeout=1800)
    return r.returncode, r.stdout.decode('utf-8', 'replace')


def expect_stop(bundle, what, sentence):
    code, said = double_click(bundle)
    assert code != 0, (what, said[-3000:])
    assert re.search(sentence, said), (what, sentence, said[-3000:])
    assert 'Details in bake-log.txt' in said, (what, said[-3000:])
    assert os.path.exists(os.path.join(bundle, 'bake-log.txt')), what
    assert not os.path.exists(os.path.join(bundle, STICK)), what
    ok('%s: "%s"' % (what, [line for line in said.splitlines() if re.search(sentence, line)][0].strip()[:110]))


def check_output(bundle, maps, files, sound):
    stick = os.path.join(bundle, STICK)
    eboot = open(os.path.join(stick, 'PSP', 'GAME', 'halo', 'EBOOT.PBP'), 'rb').read()
    assert eboot == files['PSP/GAME/halo/EBOOT.PBP'] + (b'SND0 stand-in' if sound else b''), eboot[-40:]
    for name, data in files.items():
        if not name.endswith('EBOOT.PBP'):
            assert open(os.path.join(stick, *name.split('/')), 'rb').read() == data, name
    made = sorted(os.listdir(os.path.join(stick, 'halo', 'maps')))
    # five files a map: the ui has two showcase packs where a level has its track pack and decals
    assert len(made) == len(maps) * 5, made
    assert open(os.path.join(stick, 'halo', 'chant.adp'), 'rb').read() == b'chant from XBEH mad'
    assert sorted(os.listdir(stick)) == ['PSP', 'halo'], os.listdir(stick)
    assert not os.path.exists(os.path.join(bundle, 'temp')), 'temp/ left behind'
    return len(made)


def main():
    bundle_in = os.path.abspath(sys.argv[1])
    no_python = '--no-python' in sys.argv[2:]
    with tempfile.TemporaryDirectory() as work:
        bundle = os.path.join(work, 'halo-psp-bake')
        maps = stand_in(bundle_in, bundle)
        before = sorted(os.listdir(work))
        disc, game = os.path.join(bundle, DISC), os.path.join(bundle, GAME)
        assert os.path.isdir(disc) and os.path.isdir(game), 'the input folders are missing'

        if no_python:
            assert not os.path.exists(os.path.join(bundle, '.python'))
            image(bundle, os.path.join(disc, 'Halo.iso'), maps)
            files = release(os.path.join(game, 'halo_psp_b999.zip'))
            tool = os.path.join(bundle, 'invader', 'bin', 'invader-info')
            if MACOS:
                subprocess.run(['xattr', '-w', 'com.apple.quarantine', '0081;00000000;Safari;', tool], check=True)
            code, said = double_click(bundle, hide_python=True)
            assert code == 0, said[-4000:]
            if MACOS:
                assert subprocess.run(['xattr', '-p', 'com.apple.quarantine', tool], capture_output=True).returncode, \
                    'the quarantine was left on the tools'
                ok('macOS: the download\'s quarantine taken off the bundle\'s own tools')
            assert 'Downloading cpython-3.12' in said, said[:3000]
            log = open(os.path.join(bundle, 'bake-log.txt'), encoding='utf-8').read()
            assert 'Python 3.12.15' in log.splitlines()[0], log.splitlines()[0]
            python = os.path.join(bundle, '.python', 'python.exe' if WINDOWS else 'bin/python3')
            assert os.path.exists(python), os.listdir(bundle)
            assert not os.path.exists(os.path.join(bundle, '.python-download')), 'the download left behind'
            n = check_output(bundle, maps, files, sound=False)
            ok('no Python on the system: the pinned Python downloaded, checked and used (%s); %d files baked'
               % (log.splitlines()[0].split(': ', 1)[1].split(';')[0], n))
        else:
            expect_stop(bundle, 'nothing in the folders', r'Put your Halo disc image \(a \.iso file\) in the folder')
            image(bundle, os.path.join(disc, 'Halo.iso'), maps)
            open(os.path.join(disc, 'other.iso'), 'wb').write(b'x')
            expect_stop(bundle, 'two things', r'There are 2 things in 1-PUT-YOUR-DISC-IMAGE-HERE')
            os.remove(os.path.join(disc, 'Halo.iso'))
            open(os.path.join(disc, 'other.iso'), 'wb').write(os.urandom(200000))
            expect_stop(bundle, 'not a disc image', r"other\.iso isn't an Xbox disc image")
            clear(disc)
            open(os.path.join(disc, 'Halo.7z'), 'wb').write(b'7z')
            expect_stop(bundle, 'an archive', r'Halo\.7z is a compressed archive')
            clear(disc)
            image(bundle, os.path.join(disc, 'Halo.iso'), maps)
            expect_stop(bundle, 'no zip', r'Put the Halo PSP release zip')
            open(os.path.join(game, 'halo_psp_b999.zip'), 'wb').write(b'not a zip')
            expect_stop(bundle, 'a broken zip', r"halo_psp_b999\.zip isn't a zip file that can be read")
            files = release(os.path.join(game, 'halo_psp_b999.zip'))
            changed = dict(maps)
            changed['b30.map'] = b'\0' + maps['b30.map'][1:]
            clear(disc)
            image(bundle, os.path.join(disc, 'Halo.iso'), changed)
            expect_stop(bundle, 'a map not the retail one', r"1 of the disc's maps \(b30\) isn't the retail one")
            assert not os.path.exists(os.path.join(bundle, '.venv')), 'set up before the disc was checked'
            ok('every input mistake stops before anything is set up')

            clear(disc)
            image(bundle, os.path.join(disc, 'Halo.iso'), maps)
            code, said = double_click(bundle)
            assert code == 0, said[-4000:]
            n = check_output(bundle, maps, files, sound=False)
            assert "The menu sound couldn't be added (this release's bake has no music clip chosen" in said, said[-3000:]
            assert 'Now copy the two folders inside COPY-TO-MEMORY-STICK' in said, said[-2000:]
            ok('a disc image and the zip: %d map files, the zip\'s files and its EBOOT in %s; the menu sound\'s '
               'soft warning (no clip chosen yet)' % (n, STICK))

            # a folder of the disc's files, the release unpacked, and a clip chosen
            clear(disc)
            clear(game)
            for name, data in maps.items():
                os.makedirs(os.path.join(disc, 'Halo', 'maps'), exist_ok=True)
                open(os.path.join(disc, 'Halo', 'maps', name), 'wb').write(data)
            open(os.path.join(disc, 'Halo', 'default.xbe'), 'wb').write(b'XBEH made-up')
            files = release(os.path.join(game, 'halo_psp_b999'), as_folder=True)
            bake_py = os.path.join(bundle, 'bake.py')
            text = open(bake_py, encoding='utf-8').read()
            open(bake_py, 'w', encoding='utf-8').write(re.sub(r"(?m)^MUSIC = ''$", "MUSIC = 'stand-in'", text))
            code, said = double_click(bundle)
            assert code == 0, said[-4000:]
            n = check_output(bundle, maps, files, sound=True)
            assert "menu sound couldn't" not in said, said[-3000:]
            ok('a folder of the disc\'s files and the unpacked release: %d map files, the EBOOT with the menu sound'
               % n)
        after = sorted(os.listdir(work))
        assert after == before, (before, after)
        ok('nothing written beside the bundle\'s folder')
    print('guided_check: all passed', flush=True)


if __name__ == '__main__':
    main()
