"""guided.py - the bake with nothing to type: the player puts two files in
two folders and double-clicks "Bake Halo" (launcher.py starts this when it is
given no arguments).

    1-PUT-YOUR-DISC-IMAGE-HERE/   the disc image (.iso), or a folder of the disc's files
    2-PUT-THE-HALO-ZIP-HERE/      the release zip (halo_psp_bNNN.zip)
    COPY-TO-MEMORY-STICK/         made by the bake: the game and its maps, laid
                                  out as the Memory Stick wants them
    bake-log.txt                  everything, for an issue report
    temp/                         the bake's work, removed when it is done

It finds the two inputs, checks every map against the retail disc's hashes
before anything is set up, sets up the bake's Python environment (the first
time), bakes, and puts the game zip's files, the EBOOT with the menu sound and
the bake together in COPY-TO-MEMORY-STICK. Every failure is one plain sentence;
the details go to bake-log.txt. It writes nowhere but the bundle's folder, and
never touches a Memory Stick.

    halo-psp-bake <disc image or folder> <output folder> --guided [--zip ZIP] [bake options]

does the same with the inputs named (automated runs): the stick's files, the
log and temp/ go into the output folder.

HALO_PSP_BAKE_YES=1 answers every question yes and never waits for Enter (the
tests)."""
import json
import os
import platform
import queue
import re
import shutil
import subprocess
import sys
import threading
import time
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import bake      # noqa: E402  the map lists (it runs nothing when imported)
from xiso import XIso      # noqa: E402

DISC_DIR = '1-PUT-YOUR-DISC-IMAGE-HERE'
ZIP_DIR = '2-PUT-THE-HALO-ZIP-HERE'
STICK_DIR = 'COPY-TO-MEMORY-STICK'
LOG_NAME = 'bake-log.txt'
WINDOWS = os.name == 'nt'
MACOS = sys.platform == 'darwin'
YES = os.environ.get('HALO_PSP_BAKE_YES') == '1'
# what a player may leave in an input folder beside the input itself
IGNORED = {'readme.txt', 'desktop.ini', 'thumbs.db'}
ARCHIVES = ('.zip', '.7z', '.rar', '.gz', '.xz', '.bz2', '.tar')
# the sound bake the release is played and tested with, as bake.py's default
SOUND = 's9'
# Each map's minutes on a desktop with four maps baking at once, the first
# guess at the time left: the maps that finish scale it to this machine
PRIOR = {'ui': 420, 'a10': 330, 'a30': 260, 'a50': 330, 'b30': 330, 'b40': 360, 'c10': 300, 'c20': 300,
         'c40': 330, 'd20': 270, 'd40': 300}
PRIOR_OTHER = 200          # a multiplayer map
# Windows refuses paths over 259 characters (unless long paths are switched
# on), and the bake writes each map's extracted tags under temp/: the longest
# path it writes inside temp/, measured on a bake of the retail maps
LONGEST_INSIDE = 180
# what the bake needs free on the drive, against the maps' own size: their
# copy out of the image, each map's work and the output
DISK_FACTOR = 3.0
DISK_EXTRA = 1 << 30


class Stop(Exception):
    """a plain sentence for the player (args[0]), and anything more for the
    log (args[1:])"""


class Log:
    """bake-log.txt: every line the bake writes, with the home folder written
    as ~ (a log is meant to be attached to an issue)"""

    def __init__(self, path):
        self.path = path
        self.f = open(path, 'w', encoding='utf-8', errors='replace')
        self.home = os.path.expanduser('~')

    def clean(self, text):
        return text.replace(self.home, '~') if len(self.home) > 3 else text

    def write(self, text):
        self.f.write(self.clean(text))
        self.f.flush()

    def line(self, text=''):
        self.write(time.strftime('%H:%M:%S ') + text.rstrip('\n') + '\n')

    def fileno(self):
        return self.f.fileno()

    def close(self):
        self.f.close()


class Screen:
    """what the player sees: on a terminal, a status line rewritten in place
    and a line per finished stage; elsewhere (a log, a test run), every line"""

    def __init__(self, log):
        self.log = log
        self.tty = sys.stdout.isatty()
        self.shown = 0

    def _clear(self):
        if self.tty and self.shown:
            sys.stdout.write('\r' + ' ' * self.shown + '\r')
            self.shown = 0

    def say(self, text=''):
        """a line for the player, and the log"""
        self._clear()
        print(text, flush=True)
        self.log.line(text)

    def status(self, text):
        """the current stage's progress: rewritten in place on a terminal"""
        if not self.tty:
            return
        width = shutil.get_terminal_size((80, 20)).columns - 1
        text = text[:width]
        sys.stdout.write('\r' + text + ' ' * max(0, self.shown - len(text)))
        sys.stdout.flush()
        self.shown = len(text)

    def detail(self, text):
        """a line of the bake's own output: the log always, the screen only
        when it isn't a terminal"""
        self.log.line(text)
        if not self.tty:
            self._clear()
            print(text, flush=True)


def minutes(seconds):
    if seconds < 60:
        return 'under a minute'
    if seconds < 3600:
        return '%d min' % round(seconds / 60)
    return '%d h %02d min' % (seconds // 3600, round(seconds % 3600 / 60))


def left_text(seconds):
    return 'less than a minute left' if seconds < 60 else 'about %s left' % minutes(seconds)


def size_text(n):
    return '%.1f GB' % (n / 1e9) if n >= 1e9 else '%d MB' % max(1, round(n / 1e6))


# --- the inputs -------------------------------------------------------------

def entries(folder):
    """what the player put in an input folder (not hidden files, not our README)"""
    try:
        names = os.listdir(folder)
    except OSError:
        return []
    return sorted(n for n in names if not n.startswith('.') and n.lower() not in IGNORED)


def disc_folder(path):
    """a folder holding the disc's files: maps/ (and default.xbe), or the maps themselves"""
    return os.path.isdir(os.path.join(path, 'maps')) or any(n.lower().endswith('.map') for n in entries(path))


def find_disc(folder):
    """(the disc image or disc folder, its kind) from 1-PUT-YOUR-DISC-IMAGE-HERE"""
    where = os.path.basename(folder)
    if disc_folder(folder):
        return folder, 'folder'
    found = entries(folder)
    if not found:
        raise Stop('Put your Halo disc image (a .iso file) in the folder %s, then start this again.' % where)
    if len(found) > 1:
        raise Stop('There are %d things in %s (%s): leave only your Halo disc image there, then start this '
                   'again.' % (len(found), where, ', '.join(found)))
    path = os.path.join(folder, found[0])
    if os.path.isdir(path):
        if disc_folder(path):
            return path, 'folder'
        inside = entries(path)
        if len(inside) == 1 and os.path.isfile(os.path.join(path, inside[0])):
            return image(os.path.join(path, inside[0])), 'image'
        raise Stop('The folder %s in %s holds neither a disc image nor the disc\'s maps folder: put the disc image '
                   '(a .iso file) itself in %s, then start this again.' % (found[0], where, where))
    return image(path), 'image'


def image(path):
    name = os.path.basename(path)
    if name.lower().endswith(ARCHIVES):
        raise Stop('%s is a compressed archive: unpack it first, and put the disc image inside it (a .iso file) '
                   'in %s instead.' % (name, os.path.basename(os.path.dirname(path))))
    try:
        with XIso(path) as iso:
            names = [p.lower() for p, n in iso.files()]
    except (ValueError, OSError, IndexError) as e:
        raise Stop('%s isn\'t an Xbox disc image this bake can read. It needs an image of your Halo disc, as an '
                   'Xbox dumping tool or Redump makes it.' % name, str(e))
    if not any(n.startswith('maps/') for n in names):
        raise Stop('%s is an Xbox disc image, but it has no maps folder: it isn\'t Halo: Combat Evolved.' % name)
    return path


GAME_PATH = re.compile(r'^(.*?)PSP/GAME/([^/]+)/EBOOT\.PBP$', re.IGNORECASE)


def find_game(folder):
    """the release zip (or its unpacked folder) in 2-PUT-THE-HALO-ZIP-HERE:
    see game_in"""
    where = os.path.basename(folder)
    found = entries(folder)
    if not found:
        raise Stop('Put the Halo PSP release zip (halo_psp_bNNN.zip, from the release page) in the folder %s, '
                   'then start this again.' % where)
    if len(found) > 1:
        raise Stop('There are %d things in %s (%s): leave only the Halo PSP release zip there, then start this '
                   'again.' % (len(found), where, ', '.join(found)))
    return game_in(os.path.join(folder, found[0]))


def game_in(path):
    """(the release zip or its unpacked folder, the prefix the stick's files
    sit under in it, the game's folder: halo, or halomp for the multiplayer
    build)"""
    name = os.path.basename(path)
    if os.path.isdir(path):
        names = [os.path.relpath(os.path.join(root, f), path).replace(os.sep, '/')
                 for root, _, files in os.walk(path) for f in files]
    else:
        try:
            with zipfile.ZipFile(path) as z:
                names = z.namelist()
        except (zipfile.BadZipFile, OSError):
            raise Stop('%s isn\'t a zip file that can be read (an unfinished download?): download the release zip '
                       'again.' % name)
    games = [m.groups() for m in (GAME_PATH.match(n) for n in names) if m]
    if len(games) != 1:
        raise Stop('%s doesn\'t hold the game (one PSP/GAME/.../EBOOT.PBP): it needs the Halo PSP release zip, '
                   'halo_psp_bNNN.zip.' % name)
    prefix, game = games[0]
    return path, prefix, game


def inside(base, relative):
    """base/relative, or None when relative would land outside base"""
    parts = relative.replace('\\', '/').split('/')
    if not relative or relative.startswith('/') or ':' in relative or any(p in ('', '.', '..') for p in parts):
        return None
    return os.path.join(base, *parts)


def unpack_game(source, prefix, destination, only=None):
    """the release's files under prefix into destination (only the one file
    named by only, when given): never outside it"""
    if os.path.isdir(source):
        for root, _, files in os.walk(source):
            for f in files:
                full = os.path.join(root, f)
                relative = os.path.relpath(full, source).replace(os.sep, '/')
                if relative.startswith(prefix) and (only is None or relative == prefix + only):
                    target = inside(destination, relative[len(prefix):])
                    if target:
                        os.makedirs(os.path.dirname(target), exist_ok=True)
                        shutil.copyfile(full, target)
        return
    with zipfile.ZipFile(source) as z:
        for info in z.infolist():
            name = info.filename
            if info.is_dir() or not name.startswith(prefix) or (only is not None and name != prefix + only):
                continue
            target = inside(destination, name[len(prefix):])
            if target is None:
                continue
            os.makedirs(os.path.dirname(target), exist_ok=True)
            with z.open(info) as src, open(target, 'wb') as dst:
                shutil.copyfileobj(src, dst, 1 << 20)


# --- the disc ---------------------------------------------------------------

def read_disc(screen, source, kind, wanted, temp):
    """Each wanted map (and default.xbe) checked against hashes.json, and,
    from an image, copied out into temp/disc as it is read. Returns the
    folder bake.py bakes from."""
    import hashlib
    with open(os.path.join(HERE, 'hashes.json'), encoding='utf-8') as f:
        hashes = json.load(f)
    log = screen.log
    if kind == 'image':
        out = os.path.join(temp, 'disc')
        os.makedirs(os.path.join(out, 'maps'), exist_ok=True)
        iso = XIso(source)
        listing = {p.lower(): (p, n) for p, n in iso.files()}
        files = [('maps/%s.map' % m, listing.get('maps/%s.map' % m)) for m in wanted]
        xbe = listing.get('default.xbe')
    else:
        iso = None
        out = source
        files = []
        for m in wanted:
            path = next((p for p in (os.path.join(source, 'maps', m + '.map'), os.path.join(source, m + '.map'))
                         if os.path.isfile(p)), None)
            files.append(('maps/%s.map' % m, (path, os.path.getsize(path)) if path else None))
        xbe = next(((p, 0) for p in (os.path.join(source, 'default.xbe'), os.path.join(source, '..', 'default.xbe'))
                    if os.path.isfile(p)), None)
    missing = [name for name, entry in files if entry is None]
    if missing:
        log.line('not on the disc: ' + ', '.join(missing))
        raise Stop('Your disc %s has no %s: it isn\'t a whole image of Halo: Combat Evolved\'s disc.'
                   % ('image' if iso else 'folder', ', '.join(missing)))
    total = sum(n for _, (p, n) in files)
    done = 0
    started = time.time()
    wrong = []
    try:
        for name, (path, size) in files:
            map_name = os.path.basename(name)[:-4]
            h = hashlib.sha256()

            def each(chunk):
                nonlocal done
                h.update(chunk)
                done += len(chunk)
                elapsed = time.time() - started
                left = (total - done) * elapsed / done if done and elapsed > 2 else None
                screen.status('[1/4] Reading your disc: %s of %s%s' % (
                    size_text(done), size_text(total), ', ' + left_text(left) if left is not None else ''))

            if iso:
                iso.extract(path, os.path.join(out, 'maps', map_name + '.map'), each)
            else:
                with open(path, 'rb') as f:
                    for chunk in iter(lambda: f.read(4 << 20), b''):
                        each(chunk)
            digest = h.hexdigest()
            ok = not hashes.get(map_name) or hashes[map_name] == digest
            log.line('%-15s %11d bytes  sha256 %s  %s' % (map_name, size, digest, 'ok' if ok else 'NOT THE RETAIL MAP'))
            if not ok:
                wrong.append(map_name)
        if iso and xbe:
            iso.extract(xbe[0], os.path.join(out, 'default.xbe'))
    finally:
        if iso:
            iso.f.close()
    if wrong:
        if len(wrong) == len(files):
            raise Stop('This disc is another version of Halo than the one this build needs: none of its maps '
                       'match. It needs Halo: Combat Evolved for the Xbox, NTSC (USA), version 01.10.12.2276.')
        raise Stop('%d of the disc\'s maps (%s) %s the retail %s this build needs, so the image is damaged or the '
                   'maps were changed: make the image again from your disc.'
                   % (len(wrong), ', '.join(wrong), 'isn\'t' if len(wrong) == 1 else 'aren\'t',
                      'one' if len(wrong) == 1 else 'ones'))
    return out, bool(xbe), total


# --- the bake ---------------------------------------------------------------

LINE = re.compile(r'^\d\d:\d\d:\d\d (\S+?): (.*)$')
JOBS = re.compile(r'baking (\d+) maps, (\d+) at a time')


class Progress:
    """the maps' stage, from bake.py's lines: which maps started and
    finished, and the time left (each map's first guess, PRIOR, scaled by
    how the finished maps compared with theirs)"""

    def __init__(self, maps):
        self.maps = maps
        self.started = {}
        self.done = {}
        self.failed = []
        self.jobs = 1
        self.begin = time.time()

    def feed(self, line):
        m = JOBS.search(line)
        if m:
            self.jobs = int(m.group(2))
        m = LINE.match(line)
        if not m or m.group(1) not in self.maps:
            return None
        name, text = m.groups()
        self.started.setdefault(name, time.time())
        if text.startswith('done in'):
            self.done[name] = time.time() - self.started[name]
            return name
        if text.startswith('FAILED'):
            self.failed.append(name)
            return name
        return None

    def left(self):
        prior = lambda name: PRIOR.get(name, PRIOR_OTHER)
        scale = sum(self.done.values()) / sum(prior(n) for n in self.done) if self.done else 1.0
        now = time.time()
        work = 0.0
        for name in self.maps:
            if name in self.done or name in self.failed:
                continue
            expected = prior(name) * scale
            if name in self.started:
                expected = max(expected - (now - self.started[name]), expected * 0.1)
            work += expected
        waiting = len(self.maps) - len(self.done) - len(self.failed)
        return work / max(1, min(self.jobs, waiting))

    def text(self):
        finished = len(self.done) + len(self.failed)
        running = [n for n in self.maps if n in self.started and n not in self.done and n not in self.failed]
        guess = '' if self.done else ' (a first guess)'
        return '[3/4] Baking the maps: %d of %d done, %s%s%s' % (
            finished, len(self.maps), left_text(self.left()), guess, '; now %s' % ', '.join(running) if running else '')


def folder_size(path):
    return walk(path)[0]


def walk(path):
    """(the bytes under path, the longest path under it, from path on)"""
    total = longest = 0
    for root, dirs, files in os.walk(path):
        for name in dirs + files:
            full = os.path.join(root, name)
            longest = max(longest, len(full) - len(path) - 1)
            if name in files:
                try:
                    total += os.path.getsize(full)
                except OSError:
                    pass
    return total, longest


def run_bake(screen, launcher, command, maps, temp):
    """bake.py, its output into the log line by line, the progress on the
    screen. Returns (exit code, what it said about the menu sound and the
    chant, the most disk temp/ held)"""
    env = dict(os.environ)
    env['PYTHONUTF8'] = '1'
    screen.log.line('$ ' + ' '.join(command))
    process = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL,
                               env=env)
    lines = queue.Queue()

    def reader():
        for raw in process.stdout:
            lines.put(raw.decode('utf-8', 'replace').rstrip('\r\n'))
        lines.put(None)

    threading.Thread(target=reader, daemon=True).start()
    progress = Progress(maps)
    notes = {'sound': None, 'chant': None}
    peak = longest = 0
    measured = 0
    while True:
        try:
            line = lines.get(timeout=1)
        except queue.Empty:
            line = ''
        if line is None:
            break
        if line:
            screen.detail(line)
            finished = progress.feed(line)
            if finished and not screen.tty:
                print(progress.text(), flush=True)
            text = line[9:]
            if text.startswith('menu sound skipped') or (text.startswith('menu sound: ') and
                                                            text.rstrip().endswith('EBOOT.PBP')):
                notes['sound'] = text
            elif text.startswith('the chant:'):
                notes['chant'] = text
        if progress.done or progress.started:
            screen.status(progress.text())
        # the disk the bake takes at its most, and the longest path it
        # writes (what Windows' limit is measured against: LONGEST_INSIDE)
        if time.time() - measured > 15:
            measured = time.time()
            size, deepest = walk(temp)
            peak, longest = max(peak, size), max(longest, deepest)
    code = process.wait()
    size, deepest = walk(temp)
    peak, longest = max(peak, size), max(longest, deepest)
    screen.detail('the longest path the bake wrote: %d characters inside temp/ (%d in all)'
                  % (longest, len(temp) + 1 + longest))
    return code, progress, notes, peak


# --- the whole of it ----------------------------------------------------------

def cache_choice(screen, named, extra):
    """[] or ['--cache-dir', DIR]: the bake keeps palettes and sounds in the
    user's cache folder (bake.py, default_cache_root), so a re-bake after a
    new release takes minutes. When that folder holds nothing yet, ask once
    whether there's a cache from another computer or folder to use instead;
    never in a named run, with HALO_PSP_BAKE_YES, or without a console"""
    if named or YES or not sys.stdin.isatty() or '--cache-dir' in extra or '--no-cache' in extra:
        return []
    sys.path.insert(0, HERE)
    try:
        import bake
        root = bake.default_cache_root()
    except Exception:
        return []
    if os.path.isdir(os.path.join(root, 'palettes')) and os.listdir(os.path.join(root, 'palettes')):
        screen.say('Reusing earlier bakes\' results from %s: a re-bake takes minutes.' % root)
        return []
    screen.say('This bake keeps what it makes in %s, so the next one takes minutes.' % root)
    try:
        answer = input('Baked before on another computer or kept a cache elsewhere? Type its folder, '
                       'or press Enter to go on: ').strip().strip('"')
    except (EOFError, KeyboardInterrupt):
        return []
    if answer and os.path.isdir(os.path.join(answer, 'palettes')):
        screen.say('Using the cache in %s.' % answer)
        return ['--cache-dir', answer]
    if answer:
        screen.say('%s holds no bake cache (no palettes folder): going on with %s.' % (answer, root))
    return []


def wait_for_enter():
    if YES or not sys.stdin.isatty():
        return
    try:
        input('\nPress Enter to close this window.')
    except (EOFError, KeyboardInterrupt):
        pass


def lock(path):
    """held while a bake runs in this folder, so a second double-click can't
    start another over it; the system lets it go if the bake dies"""
    f = open(path, 'a+')
    try:
        if WINDOWS:
            import msvcrt
            f.seek(0)
            msvcrt.locking(f.fileno(), msvcrt.LK_NBLCK, 1)
        else:
            import fcntl
            fcntl.flock(f.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)
    except OSError:
        f.close()
        return None
    return f


def parse(args):
    """the inputs named on the command line (--guided), and the options for bake.py"""
    disc = output = game = None
    maps = None
    keep = False
    rest = []
    positional = []
    i = 0
    while i < len(args):
        a = args[i]
        if a == '--zip' and i + 1 < len(args):
            game = args[i + 1]
            i += 2
            continue
        if a == '--maps' and i + 1 < len(args):
            maps = [m.strip() for m in args[i + 1].split(',') if m.strip()]
        if a == '--keep-work':
            keep = True
        if a.startswith('-'):
            rest.append(a)
            # an option's value goes with it
            if a in ('--maps', '--jobs', '--detail', '--sound', '--music') and i + 1 < len(args):
                rest.append(args[i + 1])
                i += 1
        else:
            positional.append(a)
        i += 1
    if positional:
        disc = positional[0]
        output = positional[1] if len(positional) > 1 else None
    return disc, output, game, maps, keep, rest


def main(args, launcher):
    # a file name the console's code page can't show mustn't stop the bake
    for stream in (sys.stdout, sys.stderr):
        try:
            stream.reconfigure(errors='replace')
        except (AttributeError, ValueError):
            pass
    disc_arg, out_arg, zip_arg, maps_arg, keep, extra = parse(args)
    named = disc_arg is not None
    if named and not out_arg:
        print('halo-psp-bake: --guided takes the disc image and the output folder')
        return 2
    if named:
        out = os.path.abspath(out_arg)
        os.makedirs(out, exist_ok=True)
        log_path, temp, stick = os.path.join(out, LOG_NAME), os.path.join(out, 'temp'), out
    else:
        out = HERE
        log_path, temp, stick = os.path.join(HERE, LOG_NAME), os.path.join(HERE, 'temp'), os.path.join(HERE, STICK_DIR)
        for folder in (DISC_DIR, ZIP_DIR):
            os.makedirs(os.path.join(HERE, folder), exist_ok=True)
    held = lock(os.path.join(out, '.bake-lock'))
    if held is None:
        print('Another bake is already running in this folder: let it finish, or close its window first.')
        wait_for_enter()
        return 1
    log = Log(log_path)
    screen = Screen(log)
    log.line('halo-psp-bake, the guided bake: Python %s on %s %s (%s); bundle %s (%d characters)' % (
        sys.version.split()[0], platform.system(), platform.release(), platform.machine(), HERE, len(HERE)))
    begin = time.time()
    code = 1
    try:
        code = bake_all(screen, launcher, named, disc_arg, zip_arg, maps_arg, keep, extra, temp, stick, log_path)
    except Stop as e:
        for detail in e.args[1:]:
            log.line(detail)
        screen.say('')
        screen.say(e.args[0])
        screen.say('(Details in %s.)' % log_path if named else '(Details in %s, in this folder.)' % LOG_NAME)
    except launcher.Failed as e:
        screen.say('')
        screen.say('Setting up failed: %s' % e)
        screen.say('(Details in %s.)' % log_path if named else '(Details in %s, in this folder.)' % LOG_NAME)
    except KeyboardInterrupt:
        screen.say('')
        screen.say('Stopped. Start it again to bake from the beginning.')
        code = 130
    except Exception as e:
        import traceback
        log.write(traceback.format_exc())
        screen.say('')
        screen.say('Something went wrong that the bake didn\'t expect (%s: %s).' % (type(e).__name__, e))
        screen.say('Please report it with %s, from this folder.' % LOG_NAME)
    log.line('finished in %s, exit %d' % (minutes(time.time() - begin), code))
    log.close()
    held.close()
    try:
        os.remove(os.path.join(out, '.bake-lock'))
    except OSError:
        pass
    if not named:
        wait_for_enter()
    return code


def bake_all(screen, launcher, named, disc_arg, zip_arg, maps_arg, keep, extra, temp, stick, log_path):
    log = screen.log
    begin = time.time()
    screen.say('Halo PSP bake: your own Halo disc turned into the PSP\'s maps')
    screen.say('')
    if named:
        if not os.path.exists(disc_arg):
            raise Stop('%s: no such file or folder' % disc_arg)
        source, kind = (disc_arg, 'folder') if os.path.isdir(disc_arg) else (image(disc_arg), 'image')
        if zip_arg and not os.path.exists(zip_arg):
            raise Stop('%s: no such file or folder' % zip_arg)
        game = game_in(os.path.abspath(zip_arg)) if zip_arg else None
    else:
        source, kind = find_disc(os.path.join(HERE, DISC_DIR))
        game = find_game(os.path.join(HERE, ZIP_DIR))
    screen.say('  Your disc:  %s' % (os.path.basename(source) if kind == 'image' else os.path.basename(source) + ' (a folder)'))
    if game:
        number = re.search(r'b(\d+)', os.path.basename(game[0]))
        screen.say('  The game:   %s (%s build%s)' % (os.path.basename(game[0]),
                                                   'the multiplayer' if game[2] == 'halomp' else 'the campaign',
                                                   ', build %s' % number.group(1) if number else ''))
    screen.say('')
    # the campaign build bakes its levels and the menus; the multiplayer
    # build reads the multiplayer maps too
    maps = maps_arg or (bake.ALL if game and game[2] == 'halomp' else bake.CAMPAIGN + ['ui'])
    unknown = [m for m in maps if m not in bake.ALL]
    if unknown:
        raise Stop('unknown map(s) %s; the maps are %s' % (', '.join(unknown), ', '.join(bake.ALL)))
    log.line('maps: ' + ','.join(maps))

    if WINDOWS:
        room = 259 - 1 - LONGEST_INSIDE
        if len(temp) > room:
            raise Stop('This folder\'s path is too long for Windows to bake in (%d characters; at most %d): move the '
                       'whole %s folder somewhere with a short path, like C:\\halo-psp-bake, then start this again.'
                       % (len(temp), room, os.path.basename(HERE)))
    shutil.rmtree(temp, ignore_errors=True)
    if os.path.exists(temp):
        raise Stop('The folder %s, left by an earlier bake, can\'t be removed: close anything using it, or delete '
                   'it yourself, then start this again.' % temp)
    os.makedirs(temp)

    if kind == 'image':
        with XIso(source) as iso:
            sizes = {p.lower(): n for p, n in iso.files()}
        maps_size = sum(sizes.get('maps/%s.map' % m, 0) for m in maps)
    else:
        maps_size = sum(os.path.getsize(p) for m in maps for p in (os.path.join(source, 'maps', m + '.map'),
                                                                   os.path.join(source, m + '.map')) if os.path.isfile(p))
    need = int(maps_size * DISK_FACTOR + DISK_EXTRA)
    free = shutil.disk_usage(temp).free
    log.line('disk: %s free, the bake needs about %s (the maps: %s)' % (size_text(free), size_text(need),
                                                                        size_text(maps_size)))
    if free < need:
        raise Stop('There is %s free on this drive, and the bake needs about %s: free some space (or move this '
                   'folder to a drive with more), then start this again.' % (size_text(free), size_text(need)))

    # [1/4] the disc: every map checked before the long part starts
    started = time.time()
    screen.status('[1/4] Reading your disc ...')
    source, has_xbe, total = read_disc(screen, source, kind, maps, temp)
    screen.say('[1/4] Your disc checks out: every map is the retail one this build needs (%s read in %s)'
               % (size_text(total), minutes(time.time() - started)))

    # [2/4] the bake's own Python environment and tools (quick once made)
    started = time.time()
    first = not os.path.exists(launcher.venv_python())
    screen.say('[2/4] Setting up%s' % (' (the first time only: about 30 MB from the internet, a minute or two)'
                                       if first else ''))
    screen.status('[2/4] Setting up ...')
    launcher.setup(False, out=log)
    launcher.check_tools()
    screen.say('[2/4] Set up%s' % (' in %s' % minutes(time.time() - started) if first else ''))

    # [3/4] the maps
    eboot = None
    if game:
        eboot_root = os.path.join(temp, 'eboot')
        unpack_game(game[0], game[1], eboot_root, only='PSP/GAME/%s/EBOOT.PBP' % game[2])
        eboot = os.path.join(eboot_root, 'PSP', 'GAME', game[2], 'EBOOT.PBP')
    baked = os.path.join(temp, 'out')
    command = [launcher.venv_python(), os.path.join(HERE, 'bake.py'), source, baked]
    if not maps_arg:
        command += ['--maps', ','.join(maps)]
    if '--sound' not in extra:
        command += ['--sound', SOUND]
    if eboot:
        command += ['--eboot', eboot]
    command += extra + cache_choice(screen, named, extra)
    screen.say('[3/4] Baking %d maps: this is the long part. Leave this window open.' % len(maps))
    code, progress, notes, peak = run_bake(screen, launcher, command, maps, temp)
    screen.detail('the most disk the bake used: %s; per map: %s' % (size_text(peak), ', '.join(
        '%s %.0f s' % (n, s) for n, s in sorted(progress.done.items()))))
    if code or progress.failed:
        failed = progress.failed or [m for m in maps if m not in progress.done]
        for name in failed:
            path = os.path.join(baked, 'work', name + '.log')
            if os.path.exists(path):
                log.line('--- %s.log' % name)
                with open(path, encoding='utf-8', errors='replace') as f:
                    log.write(f.read())
        if not keep:
            shutil.rmtree(temp, ignore_errors=True)
        raise Stop('The bake of %s failed, so nothing was put in %s. Please report it, with %s.'
                   % (', '.join(failed) or 'the maps', os.path.basename(stick), LOG_NAME))
    screen.say('[3/4] Baked the %d maps in %s' % (len(maps), minutes(time.time() - progress.begin)))

    # [4/4] everything the stick needs, in one folder
    screen.status('[4/4] Putting it together ...')
    if named:
        for part in ('halo/maps', 'halo/chant.adp', 'halo/bake.txt'):
            target = os.path.join(stick, *part.split('/'))
            shutil.rmtree(target, ignore_errors=True) if os.path.isdir(target) else (
                os.remove(target) if os.path.exists(target) else None)
    else:
        shutil.rmtree(stick, ignore_errors=True)
        if os.path.exists(stick):
            raise Stop('The folder %s, from an earlier bake, can\'t be replaced: close anything using it, or delete '
                       'it yourself, then start this again.' % STICK_DIR)
        os.makedirs(stick)
    if game:
        unpack_game(game[0], game[1], stick)
        made = os.path.join(baked, 'PSP', 'GAME', game[2], 'EBOOT.PBP')
        if os.path.exists(made):
            os.replace(made, os.path.join(stick, 'PSP', 'GAME', game[2], 'EBOOT.PBP'))
    os.makedirs(os.path.join(stick, 'halo'), exist_ok=True)
    for name in os.listdir(os.path.join(baked, 'halo')):
        target = os.path.join(stick, 'halo', name)
        if os.path.isdir(target):
            shutil.rmtree(target)
        shutil.move(os.path.join(baked, 'halo', name), target)
    if not keep:
        shutil.rmtree(temp, ignore_errors=True)
    screen.say('[4/4] Put together in %s (%s)' % (os.path.basename(stick) if not named else stick,
                                                 size_text(folder_size(stick))))

    warnings = []
    sound = notes['sound'] or ''
    if game and not sound.startswith('menu sound: '):
        why = sound[len('menu sound skipped ('):].split(');')[0] if sound.startswith('menu sound skipped') else \
            'no word from the bake'
        for said, plain in (('no clip chosen', 'this release\'s bake has no music clip chosen for it yet'),
                            ('no working atracdenc', 'the bundle\'s sound encoder, atracdenc, doesn\'t start on this '
                                                     'computer')):
            if why.startswith(said):
                why = plain
        warnings.append('The menu sound couldn\'t be added (%s). The game plays as normal, but its entry in the '
                        'PSP\'s menu is silent.' % why)
    chant = notes['chant'] or ''
    if not has_xbe or 'silent' in chant:
        warnings.append('The loading screen\'s chant couldn\'t be read from your disc (%s): the game plays, but '
                        'its loading screen is silent.' % (chant[len('the chant: '):].split(';')[0] or
                                                          'no default.xbe'))
    screen.say('')
    screen.say('Done in %s.' % minutes(time.time() - begin))
    for warning in warnings:
        screen.say('Note: ' + warning)
    screen.say('')
    if named:
        screen.say('Copy the contents of %s (%s) to the top of the Memory Stick.' % (
            stick, ' and '.join(sorted(n for n in os.listdir(stick) if n in ('PSP', 'halo')))))
    else:
        screen.say('Now copy the two folders inside %s (PSP and halo) to the top of your PSP\'s Memory Stick, '
                   'merging them with the folders already there.' % STICK_DIR)
        if MACOS:
            screen.say('Use Terminal, not Finder (Finder can replace the stick\'s whole PSP folder, ARK-4\'s files '
                       'with it): cp -R "%s/." /Volumes/<the stick\'s name>/' % stick)
        elif WINDOWS:
            screen.say('Windows merges the folders by itself; if it asks about files with the same name, choose '
                       '"Replace the files in the destination".')
        else:
            screen.say('If your file manager asks, choose Merge (or "Write Into"), never a choice that replaces '
                       'the whole folder.')
    return 0
