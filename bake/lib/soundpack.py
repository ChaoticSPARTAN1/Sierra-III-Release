"""soundpack.py - the sound bake (S9, S10): extracted sound tags re-encoded
smaller for the PSP, before invader-build builds the map from them.

The disc stores effects and dialogue at 22,050 Hz (some stereo) and music
and some ambience at 44,100 Hz stereo, all Xbox ADPCM. The port mixes at
22,050 Hz (psp_dsound.c), so everything above 11 kHz is lost on the PSP
anyway, and an effect played in 3D is mixed as mono. streaming.md S9
(2026-10-11: "a reduction in resource usage with near zero quality cost"):

    s9    every sound at 22,050 Hz at most; effects mono; music stereo
    s10   s9, and footsteps, impacts and casings at 11,025 Hz (S10: a
          listening A/B first)

The codec stays Xbox ADPCM, so the mixer is untouched: each permutation is
decoded, downmixed and low-passed to half rate as its profile says, and
encoded again. Blocks are encoded independently (each header carries the
true sample before the block and a step index fitted to the block), which
lets numpy encode every block of a permutation at once; a second
generation costs a little (median 29 dB SNR in audio.md's prototype).

S10's 11 kHz isn't a rate the sound tag can say (22,050 or 44,100 only), so
it isn't applied yet: the tag needs the rate per permutation, which the
runtime doesn't read (audio.md 3b). s10 is accepted and reported as s9.

    python3 soundpack.py TAGS_DIR PROFILE [CACHE_DIR]

With CACHE_DIR, each tag's re-encode is kept there by a hash of the tag's
bytes, the profile, this file and numpy's version, and read back by a later
bake instead of made again (the same bytes either way).

Reads Invader's tag definitions from HALO_PSP_INVADER_DEFINITIONS. Writes
the sound tags in place and prints a summary (counts and bytes: no tag
data)."""
import glob
import hashlib
import json
import os
import struct
import sys

import numpy as np

STEPS = np.array([
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45,
    50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130, 143, 157, 173, 190, 209, 230,
    253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796, 876, 963,
    1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327,
    3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442,
    11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794,
    32767], np.int32)
INDEX_STEP = np.array([-1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8], np.int32)
BLOCK_SAMPLES = 64
BLOCK_BYTES = 36           # per channel
FORMAT_XBOX_ADPCM = 1
RATE_22050, RATE_44100 = 0, 1
MONO, STEREO = 0, 1
CLASS_MUSIC = 32
# S10's classes (kept for when the rate can be per permutation)
CLASSES_NOISY = {13, 14, 15, 18}   # object, particle and slow particle impacts; unit footsteps

# ---------------------------------------------------------------- the codec


def _expand(nibble, predictor, index):
    """the decoder's step (psp_dsound.c ima_expand), on arrays"""
    step = STEPS[index]
    diff = step >> 3
    diff = diff + np.where(nibble & 1, step >> 2, 0)
    diff = diff + np.where(nibble & 2, step >> 1, 0)
    diff = diff + np.where(nibble & 4, step, 0)
    diff = np.where(nibble & 8, -diff, diff)
    predictor = np.clip(predictor + diff, -32768, 32767)
    index = np.clip(index + INDEX_STEP[nibble], 0, 88)
    return predictor, index


def decode(data, channels):
    """Xbox ADPCM bytes -> int16 array (samples, channels), as psp_dsound.c
    decodes: per block, a 4-byte header a channel (the block's first sample,
    step index), then 8 groups of 4 bytes a channel, eight nibbles each, low
    first, coding the 63 samples after the header's (the 64th pads)"""
    block_bytes = BLOCK_BYTES * channels
    blocks = len(data) // block_bytes
    if blocks == 0:
        return np.zeros((0, channels), np.int16)
    raw = np.frombuffer(data[:blocks * block_bytes], np.uint8).reshape(blocks, block_bytes)
    out = np.zeros((blocks, BLOCK_SAMPLES, channels), np.int16)
    for c in range(channels):
        header = raw[:, 4 * c:4 * c + 4].astype(np.int32)
        predictor = (header[:, 0] | (header[:, 1] << 8)).astype(np.int32)
        predictor = np.where(predictor >= 32768, predictor - 65536, predictor)
        index = np.minimum(header[:, 2], 88)
        out[:, 0, c] = predictor
        for group in range(8):
            at = 4 * channels + (group * channels + c) * 4
            for byte in range(4):
                b = raw[:, at + byte].astype(np.int32)
                for half, nibble in enumerate((b & 15, b >> 4)):
                    sample = group * 8 + byte * 2 + half + 1
                    if sample >= BLOCK_SAMPLES:
                        break
                    predictor, index = _expand(nibble, predictor, index)
                    out[:, sample, c] = predictor
    return out.reshape(blocks * BLOCK_SAMPLES, channels)


def encode(pcm):
    """int16 array (samples, channels) -> Xbox ADPCM bytes, whole blocks
    (the last padded with silence). Each block on its own: the header holds
    the block's first sample exactly and a step index fitted to its first
    differences; nibbles code the 63 samples after it, each the one whose
    decoded value is nearest (all sixteen tried); the 64th nibble pads (0),
    as the Xbox's own encoder leaves it"""
    samples, channels = pcm.shape
    blocks = max(1, (samples + BLOCK_SAMPLES - 1) // BLOCK_SAMPLES)
    padded = np.zeros((blocks * BLOCK_SAMPLES, channels), np.int32)
    padded[:samples] = pcm
    x = padded.reshape(blocks, BLOCK_SAMPLES, channels)
    out = np.zeros((blocks, BLOCK_BYTES * channels), np.uint8)
    nibbles_all = np.arange(16, dtype=np.int32)
    for c in range(channels):
        signal = x[:, :, c]
        first = np.abs(np.diff(signal[:, :5], axis=1)).mean(1)
        index = np.clip(np.searchsorted(STEPS, (first * 2).astype(np.int32)), 0, 88).astype(np.int32)
        predictor = np.clip(signal[:, 0], -32768, 32767).astype(np.int32)
        out[:, 4 * c] = (predictor & 0xFF).astype(np.uint8)
        out[:, 4 * c + 1] = ((predictor >> 8) & 0xFF).astype(np.uint8)
        out[:, 4 * c + 2] = index.astype(np.uint8)
        codes = np.zeros((blocks, BLOCK_SAMPLES), np.int32)
        for k in range(BLOCK_SAMPLES - 1):
            # every nibble's outcome, the nearest kept
            p = np.repeat(predictor[:, None], 16, 1)
            i = np.repeat(index[:, None], 16, 1)
            n = np.broadcast_to(nibbles_all, p.shape)
            p2, i2 = _expand(n, p, i)
            best = np.abs(p2 - signal[:, k + 1:k + 2]).argmin(1)
            rows = np.arange(blocks)
            codes[:, k] = best
            predictor = p2[rows, best]
            index = i2[rows, best]
        for group in range(8):
            at = 4 * channels + (group * channels + c) * 4
            for byte in range(4):
                lo = codes[:, group * 8 + byte * 2]
                hi = codes[:, group * 8 + byte * 2 + 1]
                out[:, at + byte] = (lo | (hi << 4)).astype(np.uint8)
    return out.tobytes()


def half_rate(pcm):
    """44,100 -> 22,050 Hz: a windowed-sinc low-pass at a little under the
    new Nyquist, then every second sample"""
    taps = 63
    n = np.arange(taps) - (taps - 1) / 2
    h = np.sinc(2 * 0.225 * n) * np.hamming(taps)    # cut at 0.225 of the old rate: 9.9 kHz
    h /= h.sum()
    out = np.empty(((pcm.shape[0] + 1) // 2, pcm.shape[1]), np.float64)
    for c in range(pcm.shape[1]):
        filtered = np.convolve(pcm[:, c].astype(np.float64), h, mode='same')
        out[:, c] = filtered[::2]
    return np.clip(np.round(out), -32768, 32767).astype(np.int16)


def mono(pcm):
    if pcm.shape[1] == 1:
        return pcm
    return np.clip(np.round(pcm.astype(np.float64).mean(1)), -32768, 32767).astype(np.int16)[:, None]


def snr(reference, test):
    n = min(len(reference), len(test))
    if n == 0:
        return float('inf')
    r = reference[:n].astype(np.float64)
    e = r - test[:n].astype(np.float64)
    power, noise = (r * r).mean(), (e * e).mean()
    return float('inf') if noise == 0 else 10 * np.log10(max(power, 1e-9) / noise)

# ---------------------------------------------------------------- the tags

PRIMITIVES = {
    'int8': 1, 'uint8': 1, 'int16': 2, 'uint16': 2, 'int32': 4, 'uint32': 4,
    'float': 4, 'Angle': 4, 'Fraction': 4, 'Index': 2, 'Pointer': 4, 'TagID': 4,
    'TagFourCC': 4, 'ColorARGBInt': 4, 'Point2DInt': 4, 'Rectangle2D': 8,
    'Point2D': 8, 'Vector2D': 8, 'Euler2D': 8, 'Plane2D': 12,
    'Point3D': 12, 'Vector3D': 12, 'Euler3D': 12, 'ColorRGB': 12,
    'Plane3D': 16, 'Quaternion': 16, 'ColorARGB': 16, 'Matrix': 36,
    'TagString': 32, 'TagDependency': 16, 'TagReflexive': 12, 'TagDataOffset': 20,
    'ScenarioScriptNodeValue': 4,
}


class Definitions:
    """Invader's tag definitions: struct layouts (as tools/hek_census.py)"""

    def __init__(self, folder):
        self.structs, self.enums, self.bitfields = {}, {}, {}
        for path in glob.glob(os.path.join(folder, '*.json')):
            for item in json.load(open(path, encoding='utf-8')):
                kind = item.get('type')
                if kind == 'struct':
                    self.structs[item['name']] = item
                elif kind == 'enum':
                    self.enums[item['name']] = item
                elif kind == 'bitfield':
                    self.bitfields[item['name']] = item
        self.cache = {}

    def field_size(self, field):
        kind = field['type']
        if kind == 'pad':
            return field['size']
        if kind in PRIMITIVES:
            size = PRIMITIVES[kind]
        elif kind in self.enums:
            size = 2
        elif kind in self.bitfields:
            size = self.bitfields[kind]['width'] // 8
        elif kind in self.structs:
            size = self.size(kind)
        else:
            raise KeyError('unknown field type %s' % kind)
        if field.get('bounds'):
            size *= 2
        return size * field.get('count', 1)

    def layout(self, name):
        """[(offset, field)], inherited fields first"""
        if name in self.cache:
            return self.cache[name]
        s = self.structs[name]
        fields, offset = [], 0
        if 'inherits' in s:
            fields = list(self.layout(s['inherits']))
            offset = self.size(s['inherits'])
        for field in s['fields']:
            fields.append((offset, field))
            offset += self.field_size(field)
        if 'size' in s and s['size'] != offset:
            raise ValueError('%s: fields sum to %d, the definition says %d' % (name, offset, s['size']))
        self.cache[name] = fields
        self.cache[name + '#size'] = offset
        return fields

    def size(self, name):
        if name + '#size' not in self.cache:
            self.layout(name)
        return self.cache[name + '#size']

    def offset(self, name, field_name):
        for offset, field in self.layout(name):
            if field.get('name') == field_name:
                return offset
        raise KeyError('%s has no field %s' % (name, field_name))


class Rewriter:
    """a HEK tag file re-emitted in order (the 64-byte header, the main
    struct, then each struct's children after its array: dependency paths,
    data blobs, child arrays, in field order). replace(path, element) gives
    a data blob's new bytes, or None to keep it; each struct is handed to
    patch(name, bytearray, element path) to change its fields"""

    def __init__(self, definitions, data):
        self.d = definitions
        self.data = data
        self.at = 64

    def run(self, top, replace, patch):
        out = bytearray(self.data[:64])
        size = self.d.size(top)
        main = bytearray(self.data[64:64 + size])
        self.at = 64 + size
        at_out = len(out)
        out += main
        self._children(top, at_out, out, (), replace, patch)
        patch(top, out, at_out, ())
        if self.at != len(self.data):
            raise ValueError('%d bytes after the tag (read %d of %d)' % (len(self.data) - self.at, self.at, len(self.data)))
        return bytes(out)

    def _children(self, name, base, out, path, replace, patch):
        for offset, field in self.d.layout(name):
            kind = field['type']
            at = base + offset
            if kind == 'TagDependency':
                length = struct.unpack_from('>I', out, at + 8)[0]
                if length:
                    out += self.data[self.at:self.at + length + 1]
                    self.at += length + 1
            elif kind == 'TagDataOffset':
                size = struct.unpack_from('>I', out, at)[0]
                blob = self.data[self.at:self.at + size]
                self.at += size
                new = replace(name, field.get('name'), path, blob)
                if new is not None:
                    blob = new
                    struct.pack_into('>I', out, at, len(blob))
                out += blob
            elif kind == 'TagReflexive':
                count = struct.unpack_from('>I', out, at)[0]
                child = field['struct']
                child_size = self.d.size(child)
                start = len(out)
                out += self.data[self.at:self.at + count * child_size]
                self.at += count * child_size
                for i in range(count):
                    element = path + ((field.get('name'), i),)
                    self._children(child, start + i * child_size, out, element, replace, patch)
                    patch(child, out, start + i * child_size, element)
            elif kind in self.d.structs:
                for i in range(field.get('count', 1)):
                    self._children(kind, at + i * self.d.size(kind), out, path, replace, patch)

# ---------------------------------------------------------------- one sound


def bake_sound(d, data, profile, stats):
    """the sound tag's new bytes, or None to leave it"""
    sound = 'Sound'
    o_class = 64 + d.offset(sound, 'sound class')
    o_rate = 64 + d.offset(sound, 'sample rate')
    o_channels = 64 + d.offset(sound, 'channel count')
    o_format = 64 + d.offset(sound, 'format')
    o_longest = 64 + d.offset(sound, 'longest permutation length')
    sound_class, = struct.unpack_from('>H', data, o_class)
    rate, = struct.unpack_from('>H', data, o_rate)
    channels_enum, = struct.unpack_from('>H', data, o_channels)
    fmt, = struct.unpack_from('>H', data, o_format)
    if fmt != FORMAT_XBOX_ADPCM:
        stats['other format'] += 1
        return None
    channels = 2 if channels_enum == STEREO else 1
    keep_stereo = sound_class == CLASS_MUSIC
    to_mono = channels == 2 and not keep_stereo
    to_half = rate == RATE_44100
    if not to_mono and not to_half:
        stats['kept'] += 1
        return None
    p_format = d.offset('SoundPermutation', 'format')
    p_buffer = d.offset('SoundPermutation', 'buffer size')
    p_next = d.offset('SoundPermutation', 'next permutation index')
    stats['changed'] += 1
    stats['mono'] += to_mono
    stats['half rate'] += to_half

    # first pass: each permutation's samples and its next, by (pitch range,
    # permutation). A long sound is a chain of permutations played back to
    # back (next permutation index), so a chain is one signal: encoded
    # chunk by chunk, each chunk's padded last block would put a moment of
    # silence at every join
    blobs, nexts = {}, {}

    def collect(name, field, path, blob):
        if name == 'SoundPermutation' and field == 'samples':
            blobs[(path[0][1], path[1][1])] = blob
        return None

    def note(name, out, at, path):
        if name == 'SoundPermutation':
            nexts[(path[0][1], path[1][1])] = struct.unpack_from('>h', out, at + p_next)[0]

    Rewriter(d, data).run(sound, collect, note)
    new_blobs = {}
    for key in sorted(blobs):
        pitch, index = key
        if key in new_blobs or any(nexts.get((pitch, j)) == index for (q, j) in blobs if q == pitch):
            continue   # done, or a chain's later chunk (its head does the chain)
        chain, seen = [], set()
        at = index
        while at is not None and at >= 0 and (pitch, at) in blobs and at not in seen:
            seen.add(at)
            chain.append(at)
            at = nexts.get((pitch, at))
        pieces = [decode(blobs[(pitch, j)], channels) for j in chain]
        lengths = [len(piece) for piece in pieces]
        pcm = np.concatenate(pieces) if pieces else np.zeros((0, channels), np.int16)
        if to_mono:
            pcm = mono(pcm)
        if to_half:
            pcm = half_rate(pcm)
        encoded = encode(pcm) if len(pcm) else b''
        block = BLOCK_BYTES * pcm.shape[1]
        total_blocks = len(encoded) // block
        if len(stats['snr']) < 400 and len(pcm):
            # the re-encode's own loss: what was encoded against what decodes
            stats['snr'].append(snr(pcm[:, 0], decode(encoded, pcm.shape[1])[:, 0]))
        # the chunks keep their share of the chain, in whole blocks
        whole = sum(lengths) or 1
        cut, done = 0, 0
        for n, (j, length) in enumerate(zip(chain, lengths)):
            done += length
            end_block = total_blocks if n == len(chain) - 1 else int(round(total_blocks * done / whole))
            new_blobs[(pitch, j)] = encoded[cut * block:end_block * block]
            cut = end_block
        for j in chain:
            stats['bytes before'] += len(blobs[(pitch, j)])
            stats['bytes after'] += len(new_blobs[(pitch, j)])

    def replace(name, field, path, blob):
        if name != 'SoundPermutation' or field != 'samples':
            return None
        return new_blobs.get((path[0][1], path[1][1]))

    def patch(name, out, at, path):
        if name == 'SoundPermutation':
            fmt_here, = struct.unpack_from('>H', out, at + p_format)
            if fmt_here == FORMAT_XBOX_ADPCM:
                struct.pack_into('>I', out, at + p_buffer, 0)   # invader-build sets 0 for ADPCM
        elif name == sound:
            if to_half:
                struct.pack_into('>H', out, o_rate, RATE_22050)
            if to_mono:
                struct.pack_into('>H', out, o_channels, MONO)
            struct.pack_into('>I', out, o_longest, 0)            # invader-build works it out again

    return Rewriter(d, data).run(sound, replace, patch)


with open(os.path.abspath(__file__), 'rb') as _f:
    CODE = hashlib.sha256(_f.read() + np.__version__.encode()).hexdigest()
del _f


def cached_bake(d, data, profile, stats, cache):
    """bake_sound(), or what an earlier bake kept for the same input: the
    new bytes (or none) and what it added to the summary's counts"""
    if not cache:
        return bake_sound(d, data, profile, stats)
    key = hashlib.sha256(CODE.encode() + profile.encode() + data).hexdigest()
    path = os.path.join(cache, key)
    try:
        with open(path + '.json', encoding='utf-8') as f:
            kept = json.load(f)
        new = None
        if kept['changed']:
            with open(path + '.bin', 'rb') as f:
                new = f.read()
        for k, v in kept['stats'].items():
            stats[k] = stats[k] + v
        return new
    except (OSError, ValueError, KeyError):
        pass
    before = {k: (list(v) if isinstance(v, list) else v) for k, v in stats.items()}
    new = bake_sound(d, data, profile, stats)
    delta = {k: (stats[k][len(before[k]):] if isinstance(v, list) else stats[k] - before[k])
             for k, v in before.items()}
    delta['snr'] = [float(x) for x in delta['snr']]
    try:   # written whole, then renamed, so maps baking side by side read all or nothing
        if new is not None:
            with open(path + '.bin.tmp%d' % os.getpid(), 'wb') as f:
                f.write(new)
            os.replace(path + '.bin.tmp%d' % os.getpid(), path + '.bin')
        with open(path + '.json.tmp%d' % os.getpid(), 'w', encoding='utf-8') as f:
            json.dump({'changed': new is not None, 'stats': delta}, f)
        os.replace(path + '.json.tmp%d' % os.getpid(), path + '.json')
    except OSError:
        pass
    return new


def main():
    if len(sys.argv) not in (3, 4) or sys.argv[2] not in ('s9', 's10'):
        sys.exit(__doc__)
    folder, profile = sys.argv[1], sys.argv[2]
    cache = sys.argv[3] if len(sys.argv) == 4 else ''
    d = Definitions(os.environ['HALO_PSP_INVADER_DEFINITIONS'])
    stats = {'changed': 0, 'kept': 0, 'other format': 0, 'mono': 0, 'half rate': 0, 'failed': 0,
             'bytes before': 0, 'bytes after': 0, 'snr': []}
    paths = sorted(glob.glob(os.path.join(folder, '**', '*.sound'), recursive=True))
    for path in paths:
        data = open(path, 'rb').read()
        try:
            new = cached_bake(d, data, profile, stats, cache)
        except Exception as e:  # a tag this can't read is left as it was
            stats['failed'] += 1
            print('soundpack: %s left as it was: %s' % (os.path.relpath(path, folder), e), file=sys.stderr)
            continue
        if new is not None:
            open(path, 'wb').write(new)
    before, after = stats['bytes before'], stats['bytes after']
    finite = [v for v in stats['snr'] if np.isfinite(v)]
    print('soundpack %s%s: %d sound tags, %d changed (%d to mono, %d to 22,050 Hz), %d kept, %d not Xbox ADPCM, %d failed; '
          'their samples %.1f MB -> %.1f MB (%.0f%%); the re-encode\'s SNR over %d permutations: median %.1f dB, lowest %.1f' % (
              profile, ' (as s9: the tag has no 11 kHz rate)' if profile == 's10' else '', len(paths),
              stats['changed'], stats['mono'], stats['half rate'], stats['kept'], stats['other format'],
              stats['failed'], before / 1e6, after / 1e6, 100.0 * after / before if before else 100.0,
              len(finite), float(np.median(finite)) if finite else 0.0, min(finite) if finite else 0.0))


if __name__ == '__main__':
    main()
