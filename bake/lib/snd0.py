#!/usr/bin/env python3
"""snd0.py - the XMB menu's sound (SND0.AT3), cut from the music in your own
copy of the game, and packed into the release EBOOT.

The music is never shipped: this reads it from the retail map at bake time,
the same way the maps themselves are made.

    snd0.py list MAP.map ...                     the map's music, with lengths
    snd0.py make MAP.map SPEC OUT.at3 [options]  cut, encode, and optionally pack

SPEC names the clip: TAG[@START][+LENGTH], in seconds, e.g.
    levels\\a10\\music\\a10_01@12+30
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
import argparse, os, struct, subprocess, sys, wave
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


class Music:
    """The sound and sound_looping tags of one retail (Xbox) map."""

    def __init__(self, path):
        self.map, self.defs = CacheMap(path), Defs()
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

    def chain(self, tag):
        """the sample data of a sound's first permutation and every chunk
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
        parts, fmt, index, seen = [], None, 0, set()
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

    def pcm(self, tag, frames_wanted=None):
        """a sound -> (int16 (frames, 2) at 44.1 kHz)"""
        rate, channels, _ = self.sound_info(tag)
        data, fmt = self.chain(tag)
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
    tag_path, start, length = parse_spec(a.spec)
    m = Music(a.map)
    tag = m.find(tag_path)
    if tag is None:
        raise SystemExit('snd0: no sound or sound_looping tag %s in %s (`snd0.py list` shows them)' % (tag_path, a.map))
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


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest='command', required=True)
    sub.add_parser('list').add_argument('maps', nargs='+')
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
    else:
        if a.eboot and not a.eboot_out:
            ap.error('--eboot needs --eboot-out')
        make(a)


if __name__ == '__main__':
    main()
