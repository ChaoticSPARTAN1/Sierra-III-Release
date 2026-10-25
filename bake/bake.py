#!/usr/bin/env python3
"""halo-psp-bake: your own copy of Halo (Xbox, NTSC, 01.10.12.2276) turned
into the map files the PSP build reads. One command:

    ./bake-halo.sh <your Halo disc image (.iso) or its maps folder> <output folder>
    "Bake Halo.command" ... (macOS), "Bake Halo.cmd" ... (Windows)
(Double-clicked, with no arguments, the same file runs the guided bake,
guided.py, from the bundle's two input folders.)

What it does, map by map (the ten campaign levels and the menus' ui.map; with
--multiplayer, the thirteen multiplayer maps too):
  1. reads the map out of the disc image (xiso.py: no other tool needed)
  2. checks it is the retail map this build was made for (hashes.json)
  3. extracts its tags and rebuilds the map for the PSP's memory with the
     port's Invader (-X 0x09600000 -T 0x900000, or 0x600000 for the
     multiplayer maps and ui; model and level geometry and the animations'
     frame data to a .pgeo pack, animations keyframed, the script node table
     sized to the map, the BSPs' grass and the bytes the port never reads
     left out)
  4. bakes the textures for the PSP's graphics chip (.ptex), and the asset
     track's version (_track.ptex: half-size lightmaps, and alpha-tested
     plants that keep their leaves at a distance; --no-track skips it)
  5. bakes the level's wall decals into its walls (.pstn)
  6. reads the loading screen's chant from the disc's default.xbe into
     halo/chant.adp (lib/chant.py; the EBOOT carries no copy)
  7. with --eboot: cuts the XMB menu's sound from the music on your disc and
     writes a copy of the release EBOOT with it (lib/snd0.py; the bundle's
     atracdenc encodes it)

The output folder then holds halo/maps/..., halo/chant.adp and halo/bake.txt
(the stamp the game checks: BAKE_FORMAT below) (and, with --eboot,
PSP/GAME/halo/EBOOT.PBP): copy its contents to the root of the Memory Stick.

Options:
    --maps a10,c20,bloodgulch   only these maps (default: the campaign's and ui)
    --multiplayer               the multiplayer maps too (for the multiplayer
                                build)
    --no-track                  skip the asset track's _track.ptex
    --keep-work                 keep the extracted tags (several GB)
    --jobs N                    bake N maps at once (default: one per CPU, at
                                most 4, fewer with under 8 GB of memory free;
                                1: one after another). The maps come out the
                                same whatever N is; each map's own log is in
                                <output>/work/<map>.log
    --force                     bake maps whose hashes don't match
    --detail fp,vehicles        also bake _detail.ptex with these classes at
                                full size (fp, weapons, vehicles, characters):
                                the launch menu's "assetdetail" uses them
    --sound s9                  the sound bake (the default; streaming.md S9):
                                every sound at 22,050 Hz at most, effects mono,
                                music stereo, still Xbox ADPCM (lib/soundpack.py);
                                --sound none keeps the disc's sounds as they are
    --eboot PATH                the release's EBOOT.PBP: a copy with the menu
                                sound goes to <output>/PSP/GAME/<its folder>/
    --music [MAP:]TAG[@START][+LENGTH]
                                which clip (default MUSIC below; MAP: ui)
    --list-music                list the maps' music tags and lengths, and stop
    --tints-from-means          the see-through shaders' tints from every
                                map's mean, on the runtime's own map, as
                                the packs were before (lib/seethrough.py)
    --no-cache                  make every palette afresh: by default a
                                palette made once is kept (<output>/work/
                                palettes) for the textures that repeat in
                                the asset track's pack and in other maps.
                                The maps come out the same either way
    --fast-palettes             palettes found sooner, not the same ones
                                (texpack.py --fast-palettes; on trial)
    --profile                   log each texture's class, size and seconds
                                (texpack.py; no names), and a summary
    --compare-palettes          make each palette both ways, the bake's own
                                and the fast one, and log their times and
                                errors (the fast palettes' proof)
    --no-clut4-exact            leave at 8 bits the textures of 16 colours or
                                fewer that 4 bits would draw exactly the same
                                (texpack.py). Alias --no-exact-clut
    --no-clut4-look             leave at 8 bits the lightmaps and HUD sheets
                                that 16 colours would draw within the CLUT4W
                                strict bar (texpack.py; by default they go to
                                16 colours, docs/review_clut4look.md; ui.map
                                never does). Alias --no-clut-look
    --alpha-audit               log how many texels of each texture with
                                alpha the GE's alpha test hides or draws
                                otherwise than the source does
    --no-alpha-split            alpha-tested maps' palettes fitted as one
                                (as before): an entry may stand for hidden
                                and drawn texels
    --effects-clut4             effects and see-through maps may take 16
                                colours (as before)
    --drawn-by-shape            a see-through shader blended by its alpha
                                draws the map that shapes what the frame
                                shows (its mask), not only its colour

Nothing from the game is in this tool; it only converts the copy you own."""
import argparse, concurrent.futures, hashlib, json, os, re, shutil, subprocess, sys, threading, time

HERE = os.path.dirname(os.path.abspath(__file__))
LIB = os.path.join(HERE, 'lib')
INVADER = os.path.join(HERE, 'invader')
EXE = '.exe' if os.name == 'nt' else ''
COMPARE_OLD = False
PROFILE = False             # --profile
COMPARE_PALETTES = False    # --compare-palettes
# options every texpack.py run takes (--tints-from-means)
TEXPACK_OPTIONS = []
# --slim-level: the level's rendered vertices without the binormal and tangent
# the port never reads (tools/levelslim.py)
LEVEL_SLIM = False
ATRACDENC = os.path.join(HERE, 'atracdenc')    # the bundled encoder (else atracdenc on PATH)
sys.path.insert(0, HERE)
from xiso import XIso

CAMPAIGN = ['a10', 'a30', 'a50', 'b30', 'b40', 'c10', 'c20', 'c40', 'd20', 'd40']
MULTIPLAYER = ['beavercreek', 'bloodgulch', 'boardingaction', 'carousel', 'chillout', 'damnation', 'hangemhigh',
               'longest', 'prisoner', 'putput', 'ratrace', 'sidewinder', 'wizard']
ALL = CAMPAIGN + MULTIPLAYER + ['ui']
# what the bake makes unless --maps or --multiplayer says otherwise: the
# release is the campaign build
DEFAULT = CAMPAIGN + ['ui']
# -M (build 106): animation frame data to the .pgeo pack, read through the
# game's animation cache; -Y 512: the script node table sized to the nodes in
# use plus 512 spare. Maps built with either need build 106 or later.
# -D (build 108): the BSPs' detail objects (grass) left out, which the port
# never draws (a30: 1.2 MiB); -Z (build 108): four blocks without the bytes
# the port never reads (sound permutations' and pitch ranges' names and
# subtitle data, AI squads' unused fields, firing positions' padding: ~0.5 MiB
# a campaign map, and what lets b40 fit). Maps built with -Z need build 108 or
# later; build 108 still loads older ones (psp_tag_layouts.c)
# The XMB menu's sound: [MAP:]TAG[@START][+LENGTH] (snd0.py). Not chosen yet;
# `--list-music` shows the candidates.
MUSIC = ''
BUILD_FLAGS = ['-g', 'xbox-ntsc', '-X', '0x09600000', '-U', '-l', '0', '-H', '-K', '0.25,0.003',
               '-M', '-Y', '512', '-D', '-Z']
# the tag window a map is built for (-T; psp_capacity.h's PSP_TAG_CACHE_SIZE):
# the campaign build's 9 MB, and the multiplayer build's 6 MB for the
# multiplayer maps and ui (they take 2.3-3.3 MiB of tag space as baked). A
# structure BSP loads at the end of the window it was built for, so a map
# built for 6 MB loads in both builds
TAG_SPACE = '0x900000'
MULTIPLAYER_TAG_SPACE = '0x600000'
# The bake's format: what a build needs of the maps it reads. The bake writes
# it into halo/bake.txt, and the game compares it with its own at start
# (psp_build.py reads this line; psp/src/psp_bakecheck.c), so a player whose
# maps came from another format is told to bake again, rather than seeing
# the game draw wrong or crash. Raise it whenever a build would read the
# maps from the bundle before it wrongly: a pack's layout, Invader's flags or
# tag space, or anything a release says needs a re-bake. Builds that read the
# same format share a bake.
BAKE_FORMAT = 1


# maps bake side by side (--jobs): one line at a time, whole
LOG_LOCK = threading.Lock()


def log(text):
    with LOG_LOCK:
        print(time.strftime('%H:%M:%S ') + text, flush=True)


def invader(tool):
    path = os.path.join(INVADER, 'bin', tool + EXE)
    if not os.path.exists(path):
        sys.exit('halo-psp-bake: %s is missing (the bundle\'s invader/bin)' % path)
    return path


def run(command, log_path, cwd=None, env=None):
    with open(log_path, 'a') as out:
        out.write('$ ' + ' '.join(command) + '\n')
        out.flush()
        r = subprocess.run(command, cwd=cwd, env=env, stdout=out, stderr=subprocess.STDOUT)
    if r.returncode:
        tail = open(log_path, errors='replace').read()[-1500:]
        raise RuntimeError('%s failed (%d); the end of %s:\n%s' % (os.path.basename(command[0]), r.returncode, log_path, tail))


# the lines --profile and --compare-palettes add to a map's log, and each
# pack's exact-CLUT4 count, into the bake's (texture classes, sizes and pixel
# hashes only, no names)
MEASURES = ('texprofile', 'palcompare', 'clut4 exact', 'clut4 look')
# every pack's lines on its textures' alpha (--alpha-audit's name each
# texture: tag names and counts, never texels)
ALPHA_LINES = ('alpha shapes', 'effects kept at 8 bits', 'alpha audit', 'cut-outs')
ALPHA_AUDIT = False         # --alpha-audit


def echo_from(name, log_path, start, markers):
    """the lines a step added to a map's own log (from byte start) that
    hold one of markers, into the bake's log under the map's name (the
    texture pack's see-through tints and meter maps; names and colours only)"""
    with open(log_path, errors='replace') as f:
        f.seek(start)
        for line in f.read().splitlines():
            if any(marker in line for marker in markers):
                log('%s:   %s' % (name, line.strip()))


def sha256(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for chunk in iter(lambda: f.read(1 << 20), b''):
            h.update(chunk)
    return h.hexdigest()


def gather(source, work, wanted):
    """the retail maps into work/retail/<name>.map, from a disc image or a folder"""
    os.makedirs(os.path.join(work, 'retail'), exist_ok=True)
    found = {}
    if os.path.isdir(source):
        for name in wanted:
            for candidate in (os.path.join(source, name + '.map'), os.path.join(source, 'maps', name + '.map')):
                if os.path.exists(candidate):
                    found[name] = candidate
                    break
    else:
        with XIso(source) as iso:
            listing = {p.lower(): p for p, n in iso.files()}
            for name in wanted:
                key = 'maps/%s.map' % name
                if key in listing:
                    destination = os.path.join(work, 'retail', name + '.map')
                    if not os.path.exists(destination):
                        log('reading %s from the disc image' % listing[key])
                        iso.extract(listing[key], destination)
                    found[name] = destination
    return found


def bake_chant(source, work, output):
    """the loading screen's chant from the disc's default.xbe into halo/chant.adp
    (lib/chant.py); without it the loading screen is silent"""
    sys.path.insert(0, LIB)
    import chant
    xbe = None
    if os.path.isdir(source):
        for candidate in (os.path.join(source, 'default.xbe'), os.path.join(source, '..', 'default.xbe')):
            if os.path.exists(candidate):
                xbe = candidate
                break
    else:
        with XIso(source) as iso:
            listing = {p.lower(): p for p, n in iso.files()}
            if 'default.xbe' in listing:
                xbe = os.path.join(work, 'retail', 'default.xbe')
                if not os.path.exists(xbe):
                    iso.extract(listing['default.xbe'], xbe)
    out = os.path.join(output, 'halo', 'chant.adp')
    if not xbe:
        log('the chant: no default.xbe in %s; the loading screen will be silent' % source)
    elif chant.write(xbe, out):
        log('the chant: read from %s into %s' % (os.path.basename(xbe), out))
    else:
        log('the chant: %s holds no chant this build knows; the loading screen will be silent' % xbe)


def bundle_label():
    """this bundle's short name for the stamp: the release's tag or the CI
    run's ref (bundle.txt, which assemble.py writes), else the commit of the
    checkout it runs from"""
    name = commit = ''
    try:
        with open(os.path.join(HERE, 'bundle.txt'), encoding='utf-8') as f:
            fields = dict(line.split(':', 1) for line in f if ':' in line)
        name, commit = fields.get('name', '').strip(), fields.get('commit', '').strip()
    except OSError:
        try:
            commit = subprocess.run(['git', '-C', HERE, 'rev-parse', 'HEAD'], capture_output=True, text=True,
                                    timeout=30).stdout.strip()
        except (OSError, subprocess.SubprocessError):
            pass
    label = re.sub(r'^halo-psp-bake-?((linux|macos|windows)-)?', '', name)
    return label or ('dev-' + commit[:7] if commit else 'unknown'), commit


def write_stamp(output):
    """halo/bake.txt: the bake's format and the bundle that made it, which
    the game checks at start (BAKE_FORMAT)"""
    label, commit = bundle_label()
    path = os.path.join(output, 'halo', 'bake.txt')
    with open(path, 'w', encoding='utf-8', newline='\n') as f:
        f.write('# halo-psp-bake: the format of these maps, which the game checks at start\n'
                'format: %d\nbundle: %s\ncommit: %s\nbaked: %s\n'
                % (BAKE_FORMAT, label, commit or 'unknown', time.strftime('%Y-%m-%d %H:%M')))
    log('the stamp: %s (bake format %d, bundle %s)' % (path, BAKE_FORMAT, label))


def scenario_path(map_path, log_path):
    r = subprocess.run([invader('invader-info'), '-T', 'scenario_path', map_path], capture_output=True, text=True,
                       env=tool_env())
    path = r.stdout.strip().splitlines()[-1] if r.stdout.strip() else ''
    if r.returncode or not path:
        raise RuntimeError('invader-info could not read the scenario of %s: %s' % (map_path, r.stderr[-500:]))
    return path


def tool_env():
    """the bundle's tools are linked statically, so they need no library
    path; the scripts get the bundle's lib and Invader's definitions"""
    env = dict(os.environ)
    env['HALO_PSP_INVADER_DEFINITIONS'] = os.path.join(INVADER, 'definitions')
    env['PYTHONPATH'] = LIB + (os.pathsep + env['PYTHONPATH'] if env.get('PYTHONPATH') else '')
    # the scripts' text (logs, tag names) as UTF-8 whatever the system's code page
    env['PYTHONUTF8'] = '1'
    if COMPARE_OLD:
        env['TEXPACK_KMEANS_COMPARE'] = env['STENCIL_COMPARE'] = '1'
    if PROFILE:
        env['TEXPACK_PROFILE'] = '1'
    if COMPARE_PALETTES:
        env['TEXPACK_PALETTE_COMPARE'] = '1'
    if ALPHA_AUDIT:
        env['TEXPACK_ALPHA_AUDIT'] = '1'
    # one thread of numpy's linear algebra in each step: the bake runs its
    # maps side by side (--jobs), and each library's own threads, one per
    # CPU in every step at once, fought over the CPUs (OpenBLAS's wait by
    # spinning). The palettes' sums are the same on one thread as on many
    for name in ('OPENBLAS_NUM_THREADS', 'OMP_NUM_THREADS', 'MKL_NUM_THREADS', 'VECLIB_MAXIMUM_THREADS'):
        env[name] = '1'
    return env


def bake_map(name, retail, out_maps, work, track, keep_work, detail='', sound=''):
    started = time.time()
    folder = os.path.join(work, name)
    shutil.rmtree(folder, ignore_errors=True)
    os.makedirs(os.path.join(folder, 'maps_in'))
    os.makedirs(os.path.join(folder, 'tags'))
    log_path = os.path.join(work, name + '.log')
    open(log_path, 'w').close()
    env = tool_env()
    if env.get('STENCIL_DEBUG'):
        # with maps baking side by side, each keeps its own cut inputs
        env['STENCIL_DEBUG'] = os.path.join(env['STENCIL_DEBUG'], name)
        os.makedirs(env['STENCIL_DEBUG'], exist_ok=True)
    shutil.copyfile(retail, os.path.join(folder, 'maps_in', name + '.map'))
    log('%s: extracting its tags' % name)
    # -n: a campaign map's (and the menus') own globals tag too
    run([invader('invader-extract'), '-m', os.path.join(folder, 'maps_in'), '-t', os.path.join(folder, 'tags'), '-G']
        + ([] if name in MULTIPLAYER else ['-n']) + [os.path.join(folder, 'maps_in', name + '.map')], log_path, env=env)
    scenario = scenario_path(os.path.join(folder, 'maps_in', name + '.map'), log_path)
    if sound:
        log('%s: sounds re-encoded (%s)' % (name, sound))
        run([sys.executable, os.path.join(LIB, 'soundpack.py'), os.path.join(folder, 'tags'), sound], log_path, env=env)
        summary = [line for line in open(log_path, errors='replace') if line.startswith('soundpack ')]
        if summary:
            log('%s: %s' % (name, summary[-1].strip()))
    # a multiplayer map keeps the disc map's tag order (invader-build -w):
    # the netcode names objects' types by tag handle, so a PSP on a PC host
    # needs every tag where the host's map has it (docs/multiplayer_plan.md,
    # section 3.2; tools/tagorder.py checks a bake)
    order = []
    if name in MULTIPLAYER:
        index = os.path.join(folder, name + '_index.txt')
        run([sys.executable, os.path.join(LIB, 'tagindex.py'), os.path.join(folder, 'maps_in', name + '.map'), index],
            log_path, env=env)
        order = ['-w', index]
    log('%s: building %s for the PSP' % (name, scenario))
    built = os.path.join(folder, name + '_z.map')
    pgeo = os.path.join(out_maps, name + '.pgeo')
    space = MULTIPLAYER_TAG_SPACE if name in MULTIPLAYER or name == 'ui' else TAG_SPACE
    run([invader('invader-build')] + BUILD_FLAGS + ['-T', space] + order +
        ['-t', os.path.join(folder, 'tags'), '-G', pgeo, '-o', built, scenario], log_path, env=env)
    # the tag window's use (Invader's own count: the tags and the largest BSP),
    # into the bake's log: the window is what a map must fit
    for line in open(log_path, errors='replace'):
        if line.startswith(('Tag space:', 'BSPs:')):
            log('%s: %s' % (name, ' '.join(line.split())))
    final = os.path.join(out_maps, name + '.map')
    run([sys.executable, os.path.join(LIB, 'dez.py'), built, final], log_path, env=env)
    os.remove(built)
    # before the texture and decal steps: the decal bake keys its walls by
    # the pack addresses this moves
    if LEVEL_SLIM:
        start = os.path.getsize(log_path)
        run([sys.executable, os.path.join(LIB, 'levelslim.py'), final, pgeo], log_path, cwd=LIB, env=env)
        echo_from(name, log_path, start, ('levelslim:',))
    log('%s: textures' % name)
    start = os.path.getsize(log_path)
    run([sys.executable, os.path.join(LIB, 'texpack.py'), final, os.path.join(out_maps, name + '.ptex')] + TEXPACK_OPTIONS,
        log_path, cwd=LIB, env=env)
    echo_from(name, log_path, start, ('tint ', 'see-through shaders tinted', 'meter shader maps', 'HUD sheets', 'glow ', 'detail mask', 'quality ',
                                'kmeans compare summary', 'kmeans compare worse', 'bump relief') + MEASURES + ALPHA_LINES)
    # the main menu is the hardware's showcase (CLAUDE.md): a pack of its own
    # at the most the GE can take, beside the usual one ("noshowcase")
    if name == 'ui':
        log('%s: textures, the showcase' % name)
        start = os.path.getsize(log_path)
        run([sys.executable, os.path.join(LIB, 'texpack.py'), final, os.path.join(out_maps, name + '_showcase.ptex'),
             '--showcase'] + TEXPACK_OPTIONS, log_path, cwd=LIB, env=env)
        echo_from(name, log_path, start, ('glow maps', 'showcase:') + MEASURES)
        # the same sizes on palettes, for the comparison ("showcasepal")
        log('%s: textures, the showcase on palettes' % name)
        start = os.path.getsize(log_path)
        run([sys.executable, os.path.join(LIB, 'texpack.py'), final, os.path.join(out_maps, name + '_showcase_pal.ptex'),
             '--showcase', '--showcase-palettes'] + TEXPACK_OPTIONS, log_path, cwd=LIB, env=env)
        echo_from(name, log_path, start, ('textures,',) + MEASURES)
    if track and name != 'ui':
        log('%s: textures, asset track (half-size lightmaps, plants kept whole at a distance)' % name)
        start = os.path.getsize(log_path)
        run([sys.executable, os.path.join(LIB, 'texpack.py'), final, os.path.join(out_maps, name + '_track.ptex'),
             '--lightmap-scale', '2', '--alpha-coverage', '--pgeo', pgeo] + TEXPACK_OPTIONS, log_path, cwd=LIB, env=env)
        echo_from(name, log_path, start, ('lightmaps 1/', 'lightmaps whole', 'lightmap seams', 'lightmap charts')
                  + MEASURES + ALPHA_LINES)
    if detail and name != 'ui':
        log('%s: textures at full size for %s' % (name, detail))
        start = os.path.getsize(log_path)
        run([sys.executable, os.path.join(LIB, 'texpack.py'), final, os.path.join(out_maps, name + '_detail.ptex'),
             '--full-detail', detail] + TEXPACK_OPTIONS, log_path, cwd=LIB, env=env)
        echo_from(name, log_path, start, MEASURES)
    if name != 'ui':
        log('%s: wall decals' % name)
        start = os.path.getsize(log_path)
        run([sys.executable, os.path.join(LIB, 'stencilbake.py'), final, pgeo, os.path.join(out_maps, name + '.pstn')],
            log_path, cwd=LIB, env=env)
        echo_from(name, log_path, start, ('compare:', 'triangulation failed', 'points added', 'slivers dropped'))
    if not keep_work:
        shutil.rmtree(folder, ignore_errors=True)
    log('%s: done in %.0f s' % (name, time.time() - started))
    return time.time() - started


def free_memory():
    """bytes of memory free for new work (available, on Linux), or None
    when this system won't say"""
    try:
        if sys.platform.startswith('linux'):
            with open('/proc/meminfo') as f:
                for line in f:
                    if line.startswith('MemAvailable:'):
                        return int(line.split()[1]) * 1024
        elif sys.platform == 'darwin':
            out = subprocess.run(['vm_stat'], capture_output=True, text=True, timeout=10).stdout
            page = int(out.split('page size of')[1].split()[0])
            pages = {k.strip(): int(v.strip().rstrip('.')) for k, v in
                     (line.split(':', 1) for line in out.splitlines()[1:] if ':' in line)}
            # what macOS hands out without swapping: free, plus caches it drops
            return page * sum(pages.get('Pages ' + k, 0) for k in ('free', 'inactive', 'speculative', 'purgeable'))
        elif os.name == 'nt':
            import ctypes

            class Status(ctypes.Structure):
                _fields_ = [('dwLength', ctypes.c_ulong), ('dwMemoryLoad', ctypes.c_ulong)] + \
                           [(n, ctypes.c_ulonglong) for n in ('total', 'avail', 'totalPage', 'availPage',
                                                                'totalVirtual', 'availVirtual', 'availExtended')]
            status = Status()
            status.dwLength = ctypes.sizeof(Status)
            if ctypes.windll.kernel32.GlobalMemoryStatusEx(ctypes.byref(status)):
                return status.avail
    except Exception:
        pass
    return None


# a map's invader-build and texture packs can take 1-2 GB each; four maps
# at once is where the gain levels off on a desktop's disk and memory
MAX_DEFAULT_JOBS = 4
MEMORY_PER_JOB = 2 << 30


def default_jobs():
    """(jobs, why): min(CPUs, 4), fewer when free memory is under 8 GB"""
    jobs = min(os.cpu_count() or 1, MAX_DEFAULT_JOBS)
    free = free_memory()
    if free is not None and free < MAX_DEFAULT_JOBS * MEMORY_PER_JOB:
        by_memory = max(1, int(free // MEMORY_PER_JOB))
        if by_memory < jobs:
            return by_memory, '%.1f GB of memory free, about 2 GB a map' % (free / 2 ** 30)
    return jobs, '%d CPU%s' % (os.cpu_count() or 1, '' if os.cpu_count() == 1 else 's')


def atracdenc():
    """the menu sound's encoder: the bundle's, else one on PATH; with why
    neither works when it's None"""
    bundled = os.path.join(ATRACDENC, 'bin', 'atracdenc' + EXE)
    why = []
    for path, where, absent in ((bundled, 'the bundle\'s', 'none in the bundle\'s atracdenc/bin'),
                                (shutil.which('atracdenc'), 'the one on PATH', 'none on PATH')):
        if not path or not os.path.exists(path):
            why.append(absent)
            continue
        try:
            # it prints its usage and exits 0 when it can start at all
            subprocess.run([path, '--help'], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=60, check=True)
            return path, None
        except (OSError, subprocess.SubprocessError) as e:
            why.append('%s (%s) does not start: %s' % (where, path, e))
    return None, '; '.join(why)


def atracdenc_warning(why):
    """Loud but soft: the maps bake without it, and the menu sound is worth
    having, so say what's missing and how to get it"""
    lines = ['',
             '*' * 72,
             'WARNING: no working atracdenc (%s).' % why,
             'The XMB menu sound is made with atracdenc: without it, the game\'s entry',
             'in the PSP\'s menu stays silent. The maps bake as normal.',
             'Strongly recommended: download the bundle for your system again (it',
             'carries atracdenc in atracdenc/), or build atracdenc',
             '(https://github.com/dcherednik/atracdenc) and put it on PATH, then',
             'run the bake again with --eboot.',
             '*' * 72,
             '']
    for line in lines:
        log(line)


def music_spec(spec):
    """[MAP:]TAG[@START][+LENGTH] -> (map, the rest)"""
    head, colon, rest = spec.partition(':')
    return (head, rest) if colon and head in ALL else ('ui', spec)


def menu_eboot(eboot, spec, found, output, work):
    """Best-effort: the release EBOOT, with the clip as its SND0 when it can be
    made, goes to output/PSP/GAME/<halo or halomp>/. Sound is never why the
    bake fails: without it, the EBOOT is copied as it is."""
    folder = os.path.basename(os.path.dirname(os.path.abspath(eboot)))
    folder = folder if folder in ('halo', 'halomp') else 'halo'
    out_dir = os.path.join(output, 'PSP', 'GAME', folder)
    os.makedirs(out_dir, exist_ok=True)
    out = os.path.join(out_dir, 'EBOOT.PBP')
    source_map, clip = music_spec(spec) if spec else (None, None)
    encoder, missing = atracdenc()
    if not encoder:
        atracdenc_warning(missing)
    why = ('no clip chosen (--music)' if not spec else 'no working atracdenc' if not encoder else
           '%s.map not found' % source_map if source_map not in found else None)
    if why is None:
        log_path = os.path.join(work, 'snd0.log')
        try:
            log('menu sound: %s from %s.map' % (clip, source_map))
            run([sys.executable, os.path.join(LIB, 'snd0.py'), 'make', found[source_map], clip,
                 os.path.join(work, 'SND0.AT3'), '--atracdenc', encoder, '--eboot', eboot, '--eboot-out', out],
                log_path, env=tool_env())
            log('menu sound: %s' % out)
            return
        except Exception as e:
            why = str(e)
    shutil.copyfile(eboot, out)
    log('menu sound skipped (%s); %s is the release EBOOT as it is' % (why, out))


def bake_maps(names, found, out_maps, work, a):
    """Each map in its own work folder, its own log (work/<map>.log) and its
    own outputs, with the same commands and environment whatever --jobs is:
    no step reads another map's files, so the maps come out byte for byte
    the same however many bake at once. Returns the maps that failed."""
    if a.jobs < 0:
        sys.exit('halo-psp-bake: --jobs must be 1 or more')
    jobs, why = (a.jobs, 'as asked') if a.jobs else default_jobs()
    jobs = max(1, min(jobs, len(names) or 1))
    if len(names) > 1:
        log('baking %d maps, %d at a time (%s); each map\'s own log is in %s' % (len(names), jobs, why, work))
    results = {}

    def one(name):
        try:
            results[name] = ('ok', bake_map(name, found[name], out_maps, work, not a.no_track, a.keep_work,
                                            a.detail, a.sound))
        except Exception as e:
            results[name] = ('FAILED', None)
            log('%s: FAILED: %s' % (name, e))

    if jobs == 1:
        for name in names:
            one(name)
    else:
        # threads are enough: every step is a separate process. The biggest
        # maps start first, so the last to finish isn't a long one started
        # late while the others sit idle (on the campaign, 4 at a time: ~8%
        # sooner). The maps come out the same in any order
        pool = concurrent.futures.ThreadPoolExecutor(max_workers=jobs)
        try:
            by_size = sorted(names, key=lambda name: -os.path.getsize(found[name]))
            for future in [pool.submit(one, name) for name in by_size]:
                future.result()
        except KeyboardInterrupt:
            # the steps under way got the same interrupt; start no more
            pool.shutdown(wait=False, cancel_futures=True)
            raise
        pool.shutdown()
    if len(names) > 1:
        log('the maps:')
        for name in names:
            status, seconds = results.get(name, ('not run', None))
            log('  %-15s %-7s %6s   %s' % (name, status, '%.0f s' % seconds if seconds is not None else '',
                                          os.path.join(work, name + '.log')))
    return [name for name in names if results.get(name, ('',))[0] != 'ok']


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('source', help='your Halo disc image, or a folder holding its maps')
    ap.add_argument('output', help='where the halo/maps folder is written')
    ap.add_argument('--maps', default='', help='comma-separated map names (default: the campaign\'s and ui)')
    ap.add_argument('--multiplayer', action='store_true', help='the multiplayer maps too')
    ap.add_argument('--no-cache', action='store_true',
                    help='make every palette afresh (the maps come out the same either way)')
    ap.add_argument('--fast-palettes', action='store_true', help='palettes found sooner, not the same ones (on trial)')
    ap.add_argument('--profile', action='store_true', help="log each texture's class, size and seconds, and a summary")
    ap.add_argument('--compare-palettes', action='store_true',
                    help='make each palette both ways, the bake\'s own and the fast one, and log how they compare')
    ap.add_argument('--no-track', action='store_true')
    ap.add_argument('--keep-work', action='store_true')
    ap.add_argument('--force', action='store_true')
    ap.add_argument('--detail', default='')
    ap.add_argument('--sound', default='s9', choices=['none', 's9', 's10'])
    ap.add_argument('--eboot', default='')
    ap.add_argument('--music', default=MUSIC)
    ap.add_argument('--list-music', action='store_true')
    ap.add_argument('--tints-from-means', action='store_true')
    # --no-clut-look: the same, for option checks that take letters only
    ap.add_argument('--no-clut4-look', '--no-clut-look', dest='no_clut4_look', action='store_true',
                    help='leave at 8 bits the lightmaps and HUD sheets that 16 colours would draw within the strict bar')
    # the look set's switch before it was the default: older run specs still pass it
    ap.add_argument('--clut-look', action='store_true', help=argparse.SUPPRESS)
    # --no-exact-clut: the same, for option checks that take letters only
    ap.add_argument('--no-clut4-exact', '--no-exact-clut', dest='no_clut4_exact', action='store_true',
                    help='leave at 8 bits the textures of 16 colours or fewer that 4 bits would draw the same')
    ap.add_argument('--slim-level', action='store_true',
                    help="the level's rendered vertices without the binormal and tangent the port never reads "
                         '(24 bytes a vertex in the geometry pack instead of 32; exact)')
    # on by default: it restores the original's smooth lighting where plain
    # halving flattens a small chart against its neighbour (a hard-edged step)
    ap.add_argument('--lm-chart-halve', action='store_true', default=True,
                    help="the asset track's lightmaps halved chart by chart where plain halving breaks a seam "
                         "(texpack.py --lm-chart-halve; the default)")
    ap.add_argument('--no-cutout-fine', action='store_true',
                    help='texpack.py --no-cutout-fine: cut-outs finer than their baked base map at the usual size')
    ap.add_argument('--base-as-bump', action='store_true',
                    help='texpack.py --base-as-bump: a map named as both base and bump map dropped, as before')
    ap.add_argument('--no-lm-chart-halve', dest='lm_chart_halve', action='store_false',
                    help="the asset track's lightmaps halved plainly, as before")
    ap.add_argument('--chart-max', type=int, default=-1,
                    help="texpack.py --chart-max: --lm-chart-halve's largest chart, texels across (-1: texpack's own)")
    ap.add_argument('--lightmap-seam-whole', action='store_true',
                    help="texpack.py --lightmap-seam-whole: seam-breaking pages also baked whole (too many: off)")
    ap.add_argument('--seam-step', type=int, default=0,
                    help="the asset track's seam test (texpack.py --seam-step; 0: texpack's own)")
    ap.add_argument('--seam-spots', type=int, default=0, help="texpack.py --seam-spots (0: texpack's own)")
    ap.add_argument('--jobs', type=int, default=0,
                    help='maps baked at once (default: as many as the CPUs, at most 4, fewer when under 8 GB of '
                         'memory is free; 1 bakes them one after another). The maps come out the same whatever it is')
    ap.add_argument('--alpha-audit', action='store_true',
                    help='log, texture by texture, how many texels the alpha test treats otherwise than the source '
                         'does, packed and with the palettes as fitted before (names and counts only)')
    ap.add_argument('--no-alpha-split', action='store_true',
                    help='alpha-tested maps\' palettes fitted as one, as before')
    ap.add_argument('--drawn-by-shape', action='store_true',
                    help='see-through shaders blended by their alpha draw the map that shapes what the frame shows')
    ap.add_argument('--effects-clut4', action='store_true',
                    help='effects and see-through maps may take 16 colours, as before')
    ap.add_argument('--compare-old', action='store_true',
                    help="also make each palette and decal cut as the bake did before this one (scikit-learn's k-means, "
                         'Triangle), and log how they compare: the new bake\'s proof (docs/bake_changes.md)')
    a = ap.parse_args()
    if a.sound == 'none':
        a.sound = ''
    global COMPARE_OLD, PROFILE, COMPARE_PALETTES, LEVEL_SLIM, ALPHA_AUDIT
    LEVEL_SLIM = a.slim_level
    COMPARE_OLD, PROFILE, COMPARE_PALETTES = a.compare_old, a.profile, a.compare_palettes
    ALPHA_AUDIT = a.alpha_audit
    if a.no_alpha_split:
        TEXPACK_OPTIONS.append('--no-alpha-split')
    if a.effects_clut4:
        TEXPACK_OPTIONS.append('--effects-clut4')
    if a.drawn_by_shape:
        TEXPACK_OPTIONS.append('--drawn-by-shape')
    if a.tints_from_means:
        TEXPACK_OPTIONS.append('--tints-from-means')
    if a.fast_palettes:
        TEXPACK_OPTIONS.append('--fast-palettes')
    if a.no_clut4_exact:
        TEXPACK_OPTIONS.append('--no-clut4-exact')
    if a.no_clut4_look:
        TEXPACK_OPTIONS.append('--no-clut4-look')
    if a.lm_chart_halve:
        TEXPACK_OPTIONS.append('--lm-chart-halve')
    if a.no_cutout_fine:
        TEXPACK_OPTIONS.append('--no-cutout-fine')
    if a.base_as_bump:
        TEXPACK_OPTIONS.append('--base-as-bump')
    if a.chart_max >= 0:
        TEXPACK_OPTIONS.extend(['--chart-max', str(a.chart_max)])
    if a.lightmap_seam_whole:
        TEXPACK_OPTIONS.append('--lightmap-seam-whole')
    if a.seam_step:
        TEXPACK_OPTIONS.extend(['--seam-step', str(a.seam_step)])
    if a.seam_spots:
        TEXPACK_OPTIONS.extend(['--seam-spots', str(a.seam_spots)])
    wanted = [m.strip() for m in a.maps.split(',') if m.strip()] or \
        CAMPAIGN + (MULTIPLAYER if a.multiplayer else []) + ['ui']
    unknown = [m for m in wanted if m not in ALL]
    if unknown:
        sys.exit('halo-psp-bake: unknown map(s) %s; the maps are %s' % (', '.join(unknown), ', '.join(ALL)))
    out_maps = os.path.join(a.output, 'halo', 'maps')
    work = os.path.join(a.output, 'work')
    os.makedirs(out_maps, exist_ok=True)
    # one palette cache for every pack of every map in this bake, emptied
    # first: a bake never reads palettes another bake left
    palettes = os.path.abspath(os.path.join(work, 'palettes'))
    shutil.rmtree(palettes, ignore_errors=True)
    if not a.no_cache and not a.list_music:
        os.makedirs(palettes)
        TEXPACK_OPTIONS.extend(['--palette-cache', palettes])
    if a.eboot and not os.path.isfile(a.eboot):
        sys.exit('halo-psp-bake: --eboot %s: no such file' % a.eboot)
    # said at the start, so a long bake doesn't end in a surprise (and again
    # at the end, with --eboot, where the sound is made)
    encoder, missing = atracdenc()
    if not encoder and not a.list_music:
        atracdenc_warning(missing)
    elif not a.eboot and not a.list_music:
        log('the XMB menu sound: add --eboot <the release\'s EBOOT.PBP> to give the game\'s entry its sound')
    music_map = music_spec(a.music)[0] if a.eboot and a.music else None
    found = gather(a.source, work, wanted + ([music_map] if music_map and music_map not in wanted else []))
    if a.list_music:
        r = subprocess.run([sys.executable, os.path.join(LIB, 'snd0.py'), 'list'] + [found[m] for m in wanted if m in found],
                           env=tool_env())
        if not a.keep_work:
            shutil.rmtree(os.path.join(work, 'retail'), ignore_errors=True)
        sys.exit(r.returncode)
    music_source = {music_map: found[music_map]} if music_map in found else {}
    found = {m: found[m] for m in found if m in wanted}
    missing = [m for m in wanted if m not in found]
    if missing:
        log('not found in %s: %s' % (a.source, ', '.join(missing)))
    hashes = json.load(open(os.path.join(HERE, 'hashes.json')))
    for name in list(found):
        digest = sha256(found[name])
        if hashes.get(name) and hashes[name] != digest:
            log('%s: not the retail map this build expects (sha256 %s...)%s' % (name, digest[:16],
                '; baking anyway (--force)' if a.force else '; skipped (--force bakes it anyway)'))
            if not a.force:
                del found[name]
    failures = bake_maps([m for m in wanted if m in found], found, out_maps, work, a)
    bake_chant(a.source, work, a.output)
    write_stamp(a.output)
    if a.eboot:
        menu_eboot(a.eboot, a.music, music_source, a.output, work)
    if not a.keep_work:
        shutil.rmtree(os.path.join(work, 'retail'), ignore_errors=True)
        shutil.rmtree(palettes, ignore_errors=True)
    log('baked %d of %d maps into %s%s' % (len(found) - len(failures), len(wanted), out_maps,
        '; failed: ' + ', '.join(failures) if failures else ''))
    log('copy %s to the root of the Memory Stick (it holds maps/)' % os.path.join(a.output, 'halo')
        if not a.eboot else 'copy the contents of %s (halo/ and PSP/) to the root of the Memory Stick' % a.output)
    sys.exit(1 if failures else 0)


if __name__ == '__main__':
    main()
