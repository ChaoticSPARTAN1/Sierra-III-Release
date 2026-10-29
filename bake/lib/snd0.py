#!/usr/bin/env python3
"""snd0.py - the XMB menu's sound (SND0.AT3), cut from the music in your own
copy of the game, and packed into the release EBOOT.

The music is never shipped: this reads it from the retail map at bake time,
the same way the maps themselves are made.

    snd0.py list MAP.map ...                     the map's music, with lengths
    snd0.py make MAP.map SPEC OUT.at3 [options]  cut, encode, and optionally pack
    snd0.py previews OUT_DIR MAP.map ...         each music tag's opening as .wav

SPEC names the clip: TAG[@START][+LENGTH][#loop[X]], in seconds, e.g.
    levels\\a10\\music\\a10_01@12+30
TAG#loop is the looping sound's own loop as it is (whole ATRAC3 frames, no
fades), and @START+LENGTH#loopX that cut with its wrap crossfaded over X
seconds (default 0.75): both repeat seamlessly where the XMB loops them.
`snd0.py loops MAP TAG OUT_DIR` writes previews of both, each three times.
TAG is a sound_looping (music) tag, by path. Its first track's start and loop
sounds are joined (the intro, then one pass of the loop), and the clip is cut
from that. A plain sound (snd!) tag works too.

What `make` does:
  1. finds the tag by path (paths are the same on every release; byte offsets
     aren't)
  2. decodes it: Xbox ADPCM, the same decoder as the port's (psp_dsound.c)
  3. cuts START..START+LENGTH, fades in and out, and sets the loudest sample
     to PEAK dBFS, because the XMB starts it the moment the entry is
     highlighted
  4. writes a 44.1 kHz stereo .wav, and encodes it to ATRAC3 (RIFF) with
     atracdenc
  5. with --eboot: writes a copy of the EBOOT with the clip as its SND0

Options:
    --fade-in S      default 0.02
    --fade-out S     default 2.0
    --peak DB        default -3
    --wav PATH       keep the .wav too (for listening)
    --atracdenc PATH the encoder (default: atracdenc on PATH)
    --eboot IN       the release EBOOT.PBP
    --eboot-out OUT  where the EBOOT with sound goes (needs --eboot)
"""
import argparse, atexit, os, struct, subprocess, sys, tempfile, wave, zlib
import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from halomap import Defs, CacheMap
from assets import fo

# Xbox ADPCM is IMA ADPCM in 36-byte blocks per channel: a 4-byte header
# (predictor, step index), then 32 bytes of nibbles, 64 samples
BLOCK_BYTES, BLOCK_SAMPLES = 36, 64
STEPS = [7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88, 97,
         107, 118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796,
         876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871,
         5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623,
         27086, 29794, 32767]
INDEX_STEP = [-1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8]
RATES = {0: 22050, 1: 44100}
FORMAT_PCM, FORMAT_XBOX_ADPCM = 0, 1
SND0 = 5      # the PBP's sections: PARAM.SFO, ICON0, ICON1, PIC0, PIC1, SND0, DATA.PSP, DATA.PSAR


def decode_xbox_adpcm(data, channels, frames_wanted=None):
    """Xbox ADPCM bytes -> int16 array (frames, channels). Per block, per
    channel: the header (the block's first sample), then 4-byte groups of
    eight nibbles, low nibble first, alternating between channels, coding the
    63 samples after it; the 64th nibble pads (as psp_dsound.c decode_block)."""
    block = BLOCK_BYTES * channels
    blocks = len(data) // block
    if frames_wanted is not None:
        blocks = min(blocks, -(-frames_wanted // BLOCK_SAMPLES))
    out = np.zeros((blocks * BLOCK_SAMPLES, channels), np.int16)
    for c in range(channels):
        column = []
        add = column.append
        for b in range(blocks):
            base = b * block
            predictor = struct.unpack_from('<h', data, base + 4 * c)[0]
            index = min(data[base + 4 * c + 2], 88)
            add(predictor)
            coded = 0
            for group in range(8):
                g = base + 4 * channels + (group * channels + c) * 4
                for byte in data[g:g + 4]:
                    for nibble in (byte & 15, byte >> 4):
                        coded += 1
                        if coded >= BLOCK_SAMPLES:
                            break
                        step = STEPS[index]
                        diff = step >> 3
                        if nibble & 4: diff += step
                        if nibble & 2: diff += step >> 1
                        if nibble & 1: diff += step >> 2
                        predictor += -diff if nibble & 8 else diff
                        predictor = 32767 if predictor > 32767 else -32768 if predictor < -32768 else predictor
                        index += INDEX_STEP[nibble]
                        index = 0 if index < 0 else 88 if index > 88 else index
                        add(predictor)
        out[:, c] = column
    return out


def plain_map(path):
    """A retail Xbox map is zlib-compressed after its 2 KB header (dez.py),
    which CacheMap can't read: a decompressed copy in a temporary file,
    removed at exit; a map already plain is read where it is"""
    with open(path, 'rb') as f:
        head = f.read(2048)
    length, = struct.unpack_from('<I', head, 8)
    if os.path.getsize(path) == length:
        return path
    with open(path, 'rb') as f:
        d = f.read()
    tmp = tempfile.NamedTemporaryFile(suffix='.map', delete=False)
    with tmp:
        tmp.write(d[:2048])
        tmp.write(zlib.decompressobj().decompress(d[2048:]))
    atexit.register(lambda: os.path.exists(tmp.name) and os.remove(tmp.name))
    return tmp.name


class Music:
    """The sound and sound_looping tags of one retail (Xbox) map."""

    def __init__(self, path):
        self.map, self.defs = CacheMap(plain_map(path)), Defs()
        self.by_name = {}
        for t in self.map.tags:
            if t.group in ('snd!', 'lsnd') and t.address:
                self.by_name[(t.group, t.name.lower())] = t
        self.f_sound, self.f_range = fo(self.defs, 'Sound'), fo(self.defs, 'SoundPitchRange')
        self.f_perm, self.f_track = fo(self.defs, 'SoundPermutation'), fo(self.defs, 'SoundLoopingTrack')
        self.f_looping = fo(self.defs, 'SoundLooping')

    def dependency(self, o):
        """a TagDependency at file offset o -> the tag, or None"""
        tag_id = struct.unpack_from('<i', self.map.data, o + 12)[0]
        return self.map.by_id.get(tag_id) if tag_id != -1 else None

    def sound_info(self, tag):
        d, o = self.map.data, self.map.off(tag.address)
        rate = struct.unpack_from('<h', d, o + self.f_sound['sample rate'])[0]
        channels, fmt = struct.unpack_from('<hh', d, o + self.f_sound['channel count'])
        return RATES.get(rate, rate), (1, 2)[channels] if channels in (0, 1) else channels, fmt

    def chain_starts(self, tag):
        """the permutations a chain can start from: the variations the game
        picks among (the first 'actual permutation count'), then any chunk no
        other chunk names as its next (b30_01's loop has one: 3..39, about
        49 s, while variation 0 runs 0 -> 1, into variation 1's chain)"""
        d, m = self.map.data, self.map
        o = m.off(tag.address)
        n, p = struct.unpack_from('<iI', d, o + self.f_sound['pitch ranges'])
        if n <= 0:
            return []
        r = m.off(p)
        pn, pp = struct.unpack_from('<iI', d, r + self.f_range['permutations'])
        actual = struct.unpack_from('<h', d, r + self.f_range['actual permutation count'])[0] \
            if 'actual permutation count' in self.f_range else 1
        size = self.defs.structs['SoundPermutation']['size']
        named = set()
        for i in range(max(pn, 0)):
            nxt = struct.unpack_from('<h', d, m.off(pp) + size * i + self.f_perm['next permutation index'])[0]
            named.add(nxt)
        bases = list(range(max(1, min(actual, pn))))
        return bases + [i for i in range(len(bases), pn) if i not in named]

    def chain(self, tag, first=0):
        """the sample data of a sound's permutation FIRST and every chunk
        chained after it (music is split into ~1.3 s chunks, each naming
        the next with 'next permutation index'), joined"""
        d, m = self.map.data, self.map
        o = m.off(tag.address)
        n, p = struct.unpack_from('<iI', d, o + self.f_sound['pitch ranges'])
        if n <= 0:
            return b'', None
        r = m.off(p)
        pn, pp = struct.unpack_from('<iI', d, r + self.f_range['permutations'])
        size = self.defs.structs['SoundPermutation']['size']
        parts, fmt, index, seen = [], None, first, set()
        while 0 <= index < pn and index not in seen:
            seen.add(index)
            q = m.off(pp) + size * index
            fmt = struct.unpack_from('<h', d, q + self.f_perm['format'])[0]
            length, external, offset = struct.unpack_from('<iIi', d, q + self.f_perm['samples'])
            if length > 0:
                if external & 1 or not 0 <= offset <= len(d) - length:
                    raise ValueError('%s: chunk %d is outside the map (offset %d, %d bytes)'
                                     % (tag.name, index, offset, length))
                parts.append(d[offset:offset + length])
            index = struct.unpack_from('<h', d, q + self.f_perm['next permutation index'])[0]
        return b''.join(parts), fmt

    def pcm(self, tag, frames_wanted=None, first=0):
        """a sound (from its permutation FIRST) -> (int16 (frames, 2) at 44.1 kHz)"""
        rate, channels, _ = self.sound_info(tag)
        data, fmt = self.chain(tag, first)
        if fmt == FORMAT_XBOX_ADPCM:
            want = None if frames_wanted is None else -(-frames_wanted * rate // 44100)
            a = decode_xbox_adpcm(data, channels, want)
        elif fmt == FORMAT_PCM:
            a = np.frombuffer(data[:len(data) // (2 * channels) * 2 * channels], '<i2').reshape(-1, channels)
        else:
            raise ValueError('%s: sound format %d (only Xbox ADPCM and PCM are read)' % (tag.name, fmt))
        if channels == 1:
            a = np.repeat(a, 2, axis=1)
        if rate != 44100:   # the menu wants 44.1 kHz; 22.05 kHz doubles each frame (linear)
            x = np.arange(len(a) * 44100 // rate) * (rate / 44100)
            a = np.stack([np.interp(x, np.arange(len(a)), a[:, c]) for c in (0, 1)], 1).astype(np.int16)
        return a

    def seconds(self, tag):
        rate, channels, _ = self.sound_info(tag)
        data, fmt = self.chain(tag)
        if fmt == FORMAT_XBOX_ADPCM:
            return len(data) // (BLOCK_BYTES * channels) * BLOCK_SAMPLES / rate
        return len(data) / (2 * channels) / rate

    def tracks(self, tag):
        """a sound_looping's tracks: [(start, loop, end) tags or None]"""
        d, o = self.map.data, self.map.off(tag.address)
        n, p = struct.unpack_from('<iI', d, o + self.f_looping['tracks'])
        size = self.defs.structs['SoundLoopingTrack']['size']
        return [tuple(self.dependency(self.map.off(p) + size * i + self.f_track[k]) for k in ('start', 'loop', 'end'))
                for i in range(max(n, 0))]

    def find(self, path):
        key = path.replace('/', '\\').lower()
        return self.by_name.get(('lsnd', key)) or self.by_name.get(('snd!', key))

    def source(self, tag, frames_wanted):
        """the audio a clip is cut from: a sound itself, or a looping
        sound's first track, start then one pass of the loop"""
        if tag.group == 'snd!':
            return self.pcm(tag, frames_wanted)
        tracks = self.tracks(tag)
        if not tracks:
            raise ValueError('%s has no tracks' % tag.name)
        parts, have = [], 0
        for sound in tracks[0][:2]:
            if sound is not None and (frames_wanted is None or have < frames_wanted):
                parts.append(self.pcm(sound, None if frames_wanted is None else frames_wanted - have))
                have += len(parts[-1])
        if not parts:
            raise ValueError('%s: its first track has no start or loop sound' % tag.name)
        return np.concatenate(parts)


LOOP_FRAME = 1024   # ATRAC3 codes 1,024 samples a frame: a loop of whole frames wraps without padding


def loop_sound(m, tag):
    if tag.group != 'lsnd':
        raise ValueError('%s is a plain sound, not a looping one' % tag.name)
    tracks = m.tracks(tag)
    if not tracks or tracks[0][1] is None:
        raise ValueError('%s has no loop sound' % tag.name)
    return tracks[0][1]


def loop_part(m, tag, start=0):
    """a looping sound's first track's loop sound alone, the chain from its
    permutation START: one pass of the part the game repeats"""
    return m.pcm(loop_sound(m, tag), None, start)


def frames_of(n):
    return max(LOOP_FRAME, n // LOOP_FRAME * LOOP_FRAME)


def find_loops(audio, seconds, count=3, window=2.0, slack=4.0, starts=8):
    """[(start, length, score)]: cut points where the music just after the
    clip's end sounds most like its start, so the wrap continues naturally.
    Scored by the normalised correlation of WINDOW seconds after the end
    with the WINDOW seconds at the start, lengths SECONDS +/- SLACK, at a few
    starts through the first half of the audio"""
    rate = 11025
    mono = audio.astype(np.float32).mean(axis=1)[::4]
    w, n = int(window * rate), len(mono)
    best = []
    for s in np.linspace(0, max(0, n // 2 - w), starts).astype(int):
        head = mono[s:s + w]
        if len(head) < w or not np.any(head):
            continue
        lo, hi = s + int((seconds - slack) * rate), s + int((seconds + slack) * rate)
        if hi + w > n:
            continue
        region = mono[lo:hi + w]
        size = 1 << int(np.ceil(np.log2(len(region) + w)))
        corr = np.fft.irfft(np.fft.rfft(region, size) * np.conj(np.fft.rfft(head, size)), size)[:hi - lo]
        energy = np.sqrt(np.convolve(region ** 2, np.ones(w), 'valid')[:hi - lo]) * np.sqrt((head ** 2).sum())
        score = corr / np.maximum(energy, 1e-9)
        for k in np.argsort(score)[::-1][:count]:
            best.append((s * 4 / 44100, (lo + k - s) * 4 / 44100, float(score[k])))
    best.sort(key=lambda b: -b[2])
    picked = []
    for b in best:   # distinct cuts, not the same one a few samples apart
        if all(abs(b[0] - p[0]) > 1 or abs(b[1] - p[1]) > 1 for p in picked):
            picked.append(b)
        if len(picked) == count:
            break
    return picked


def loop_clip(audio, start, length, crossfade, peak_db):
    """START..START+LENGTH, whole ATRAC3 frames, with its first CROSSFADE
    seconds blended (equal power) from what follows its end into its own
    start: the wrap from the last sample to the first plays on as the music
    did, then becomes the clip's start"""
    s, n = int(start * 44100), frames_of(int(length * 44100))
    x = min(int(crossfade * 44100), n // 2)
    if s + n + x > len(audio):
        raise ValueError('the loop runs past the end of the music (%.1f s long)' % (len(audio) / 44100))
    a = audio[s:s + n].astype(np.float32).copy()
    if x:
        t = np.linspace(0, np.pi / 2, x, dtype=np.float32)[:, None]
        a[:x] = a[:x] * np.sin(t) + audio[s + n:s + n + x].astype(np.float32) * np.cos(t)
    top = np.abs(a).max()
    if top > 0:
        a *= 32767 * 10 ** (peak_db / 20) / top
    return np.clip(np.round(a), -32768, 32767).astype(np.int16)


def own_loop(audio_loop, peak_db):
    """the game's loop as it is, trimmed to whole ATRAC3 frames (the last
    few ms of a frame's padding would be a gap at the wrap), no fades"""
    a = audio_loop[:frames_of(len(audio_loop))].astype(np.float32)
    top = np.abs(a).max()
    if top > 0:
        a *= 32767 * 10 ** (peak_db / 20) / top
    return np.clip(np.round(a), -32768, 32767).astype(np.int16)


def loops(a):
    """previews for choosing a looping menu sound: the tag's own loop, and
    the best cuts of about --seconds with a crossfade, each written looped
    three times so the wrap can be heard"""
    m = Music(a.map)
    tag = m.find(a.tag)
    if tag is None:
        raise SystemExit('snd0: no sound or sound_looping tag %s in %s' % (a.tag, a.map))
    os.makedirs(a.out_dir, exist_ok=True)
    base = tag.name.replace('\\', '_')
    done = []
    try:
        part = loop_part(m, tag)
        for start in m.chain_starts(loop_sound(m, tag)):
            clip = own_loop(loop_part(m, tag, start), a.peak)
            write_wav(os.path.join(a.out_dir, '%s__chain%d_%.1fs_x3.wav' % (base, start, len(clip) / 44100)),
                      np.concatenate([clip] * 3))
            done.append('the loop from permutation %d: %.2f s (spec %s#loopc%d)' % (start, len(clip) / 44100,
                                                                                tag.name, start))
        audio = np.concatenate([m.source(tag, None), part, part])
    except ValueError as e:
        done.append('own loop: none (%s)' % e)
        audio = m.source(tag, None)
    for i, (start, length, score) in enumerate(find_loops(audio, a.seconds), 1):
        clip = loop_clip(audio, start, length, a.crossfade, a.peak)
        name = '%s__cut%d_%.2fs_from_%.2fs_x3.wav' % (base, i, len(clip) / 44100, start)
        write_wav(os.path.join(a.out_dir, name), np.concatenate([clip] * 3))
        done.append('cut %d: %.2f s from %.2f s, match %.3f (spec %s@%.2f+%.2f#loop%.2f)' % (
            i, len(clip) / 44100, start, score, tag.name, start, length, a.crossfade))
    for line in done:
        print('snd0 loops: %s' % line)


def inspect(a):
    """a looping sound's structure, in numbers only (no audio): each track's
    start, loop and end sounds, their pitch ranges and permutations (the
    variations the game picks among, and the chunks chained after each),
    and where the loop's audio repeats its own first seconds"""
    m = Music(a.map)
    tag = m.find(a.tag)
    if tag is None:
        raise SystemExit('snd0: no tag %s in %s' % (a.tag, a.map))
    d, cm = m.map.data, m.map
    size = m.defs.structs['SoundPermutation']['size']
    rsize = m.defs.structs['SoundPitchRange']['size']
    for t, track in enumerate(m.tracks(tag) if tag.group == 'lsnd' else [(tag, None, None)]):
        for role, sound in zip(('start', 'loop', 'end'), track):
            if sound is None:
                continue
            o = cm.off(sound.address)
            n, p = struct.unpack_from('<iI', d, o + m.f_sound['pitch ranges'])
            rate, channels, fmt = m.sound_info(sound)
            print('snd0 inspect: track %d %s: %s, %d Hz, %d channel(s), format %d, %d pitch range(s)'
                  % (t, role, sound.name, rate, channels, fmt, n))
            for r in range(max(n, 0)):
                ro = cm.off(p) + rsize * r
                pn, pp = struct.unpack_from('<iI', d, ro + m.f_range['permutations'])
                actual = struct.unpack_from('<h', d, ro + m.f_range['actual permutation count'])[0] \
                    if 'actual permutation count' in m.f_range else -1
                print('snd0 inspect:   range %d: %d permutations, actual (the variations) %d' % (r, pn, actual))
                for i in range(max(pn, 0)):
                    q = cm.off(pp) + size * i
                    length = struct.unpack_from('<i', d, q + m.f_perm['samples'])[0]
                    nxt = struct.unpack_from('<h', d, q + m.f_perm['next permutation index'])[0]
                    blocks = length // (BLOCK_BYTES * channels) if fmt == FORMAT_XBOX_ADPCM else 0
                    print('snd0 inspect:     perm %d: %d bytes (%.2f s), next %d' % (
                        i, length, blocks * BLOCK_SAMPLES / rate if blocks else length / (2 * channels) / rate, nxt))
        if track[1] is not None:
            loop = m.pcm(track[1], None).astype(np.float32).mean(axis=1)[::4]
            w = int(2.0 * 11025)
            head = loop[:w]
            scores = []
            for off in range(int(0.3 * 11025), min(len(loop) - w, int(30 * 11025)), 110):
                seg = loop[off:off + w]
                den = np.sqrt((seg ** 2).sum() * (head ** 2).sum())
                scores.append((float((seg * head).sum() / den) if den else 0.0, off / 11025))
            scores.sort(reverse=True)
            print('snd0 inspect: the loop\'s first 2 s recur at: %s' % ', '.join(
                '%.2f s (%.2f)' % (t_, c) for c, t_ in scores[:6]))


def parse_spec(spec):
    """TAG[@START][+LENGTH] -> (tag, start, length)"""
    tag, length = (spec.rsplit('+', 1) + [None])[:2] if '+' in spec else (spec, None)
    tag, start = (tag.rsplit('@', 1) + [None])[:2] if '@' in tag else (tag, None)
    return tag, float(start or 0), float(length or 30)


def cut(audio, start, length, fade_in, fade_out, peak_db):
    a = audio[int(start * 44100):int((start + length) * 44100)].astype(np.float32)
    if not len(a):
        raise ValueError('the clip starts past the end of the music (%.1f s long)' % (len(audio) / 44100))
    n_in, n_out = min(int(fade_in * 44100), len(a)), min(int(fade_out * 44100), len(a))
    if n_in:
        a[:n_in] *= np.linspace(0, 1, n_in, dtype=np.float32)[:, None]
    if n_out:   # an equal-power curve: it fades evenly to the ear
        a[-n_out:] *= np.cos(np.linspace(0, np.pi / 2, n_out, dtype=np.float32))[:, None]
    top = np.abs(a).max()
    if top > 0:
        a *= 32767 * 10 ** (peak_db / 20) / top
    return np.clip(np.round(a), -32768, 32767).astype(np.int16)


def write_wav(path, a):
    with wave.open(path, 'wb') as w:
        w.setnchannels(2)
        w.setsampwidth(2)
        w.setframerate(44100)
        w.writeframes(a.astype('<i2').tobytes())


def encode_at3(wav, out, atracdenc):
    r = subprocess.run([atracdenc, '-e', 'atrac3', '--container', 'riff', '-i', wav, '-o', out],
                       capture_output=True, text=True)
    if r.returncode or not os.path.exists(out):
        raise RuntimeError('atracdenc failed (%d): %s' % (r.returncode, (r.stderr or r.stdout)[-500:]))
    head = open(out, 'rb').read(64)
    fmt = head.find(b'fmt ')
    if head[:4] != b'RIFF' or fmt < 0 or struct.unpack_from('<H', head, fmt + 8)[0] != 0x0270:
        raise RuntimeError('%s is not a RIFF ATRAC3 file (what the XMB plays)' % out)


def pbp_with(eboot_in, eboot_out, section, data):
    """a copy of a PBP with one section replaced. A PBP is 'PBP' magic, a
    version, eight offsets, then the sections back to back in that order;
    each runs to the next one's offset (the last to the end of the file)."""
    raw = open(eboot_in, 'rb').read()
    if raw[:4] != b'\0PBP':
        raise ValueError('%s is not an EBOOT.PBP' % eboot_in)
    offsets = list(struct.unpack_from('<8I', raw, 8)) + [len(raw)]
    parts = [raw[offsets[i]:offsets[i + 1]] for i in range(8)]
    parts[section] = data
    out, at = [raw[:8]], 8 + 4 * 8
    new_offsets = []
    for p in parts:
        new_offsets.append(at)
        at += len(p)
    out.append(struct.pack('<8I', *new_offsets))
    out.extend(parts)
    tmp = eboot_out + '.part'
    with open(tmp, 'wb') as f:
        f.write(b''.join(out))
    os.replace(tmp, eboot_out)


def list_music(paths):
    for path in paths:
        m = Music(path)
        print('%s:' % os.path.basename(path))
        for (group, _), tag in sorted(m.by_name.items(), key=lambda kv: kv[1].name):
            if group != 'lsnd':
                continue
            parts = []
            for i, track in enumerate(m.tracks(tag)):
                lengths = ['%s %.1fs' % (k, m.seconds(s)) for k, s in zip(('start', 'loop', 'end'), track) if s]
                parts.append('track %d: %s' % (i, ', '.join(lengths) or 'empty'))
            print('  %s\n      %s' % (tag.name, '\n      '.join(parts) or 'no tracks'))


def make(a):
    spec, _, loop = a.spec.partition('#')
    tag_path, start, length = parse_spec(spec)
    m = Music(a.map)
    tag = m.find(tag_path)
    if tag is None:
        raise SystemExit('snd0: no sound or sound_looping tag %s in %s (`snd0.py list` shows them)' % (tag_path, a.map))
    if (loop == 'loop' or loop.startswith('loopc')) and '@' not in spec and '+' not in spec:
        # TAG#loop: the game's own loop as variation 0 plays it; TAG#loopcN:
        # the chain from permutation N (a variation, or a chunk nothing names)
        clip = own_loop(loop_part(m, tag, int(loop[5:] or 0) if loop.startswith('loopc') else 0), a.peak)
    elif loop.startswith('loop'):
        # TAG@START+LENGTH#loopX: that cut, its wrap crossfaded over X s
        audio = m.source(tag, None)
        if tag.group == 'lsnd':
            part = loop_part(m, tag)
            audio = np.concatenate([audio, part, part])
        clip = loop_clip(audio, start, length, float(loop[4:] or 0.75), a.peak)
    else:
        audio = m.source(tag, int((start + length) * 44100) + 1)
        clip = cut(audio, start, length, a.fade_in, a.fade_out, a.peak)
    wav = a.wav or a.out + '.wav'
    write_wav(wav, clip)
    try:
        encode_at3(wav, a.out, a.atracdenc)
    finally:
        if not a.wav:
            os.remove(wav)
    print('snd0: %s, %.1f s from %.1f s -> %s (%d KB)' % (tag.name, len(clip) / 44100, start, a.out,
                                                         os.path.getsize(a.out) // 1024))
    if a.eboot:
        pbp_with(a.eboot, a.eboot_out, SND0, open(a.out, 'rb').read())
        print('snd0: %s, with the clip as its SND0' % a.eboot_out)


def previews(paths, out_dir, seconds):
    """The first SECONDS of each music tag's first track, as .wav files to
    listen to while choosing the menu's clip (they hold the disc's music:
    they stay on the machine that baked)"""
    os.makedirs(out_dir, exist_ok=True)
    for path in paths:
        m = Music(path)
        for (group, _), tag in sorted(m.by_name.items(), key=lambda kv: kv[1].name):
            if group != 'lsnd':
                continue
            try:
                audio = m.source(tag, int(seconds * 44100) + 1)
            except Exception as e:
                print('snd0 previews: %s: %s' % (tag.name, e))
                continue
            clip = cut(audio, 0.0, min(seconds, len(audio) / 44100), 0.02, 1.0, -3.0)
            name = '%s__%s.wav' % (os.path.splitext(os.path.basename(path))[0], tag.name.replace('\\', '_'))
            write_wav(os.path.join(out_dir, name), clip)
            print('snd0 previews: %s (%.1f s)' % (name, len(clip) / 44100))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest='command', required=True)
    sub.add_parser('list').add_argument('maps', nargs='+')
    ip = sub.add_parser('inspect')
    ip.add_argument('map')
    ip.add_argument('tag')
    lp = sub.add_parser('loops')
    lp.add_argument('map')
    lp.add_argument('tag')
    lp.add_argument('out_dir')
    lp.add_argument('--seconds', type=float, default=30.0)
    lp.add_argument('--crossfade', type=float, default=0.75)
    lp.add_argument('--peak', type=float, default=-3.0)
    pv = sub.add_parser('previews')
    pv.add_argument('out_dir')
    pv.add_argument('maps', nargs='+')
    pv.add_argument('--seconds', type=float, default=30.0)
    mk = sub.add_parser('make')
    mk.add_argument('map')
    mk.add_argument('spec')
    mk.add_argument('out')
    mk.add_argument('--fade-in', type=float, default=0.02)
    mk.add_argument('--fade-out', type=float, default=2.0)
    mk.add_argument('--peak', type=float, default=-3.0)
    mk.add_argument('--wav')
    mk.add_argument('--atracdenc', default='atracdenc')
    mk.add_argument('--eboot')
    mk.add_argument('--eboot-out')
    a = ap.parse_args()
    if a.command == 'list':
        list_music(a.maps)
    elif a.command == 'inspect':
        inspect(a)
    elif a.command == 'loops':
        loops(a)
    elif a.command == 'previews':
        previews(a.maps, a.out_dir, a.seconds)
    else:
        if a.eboot and not a.eboot_out:
            ap.error('--eboot needs --eboot-out')
        make(a)


if __name__ == '__main__':
    main()
