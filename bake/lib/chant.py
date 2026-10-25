#!/usr/bin/env python3
"""chant.py - the loading screen's chant, read from your own copy of the game.

The tone the loading screen's four voices play is 18,396 bytes of Xbox ADPCM
inside the game's executable (default.xbe), not in a map. The EBOOT doesn't
carry it (docs/research/chant_provenance.md): this finds it in your
default.xbe and writes it as halo/chant.adp, which psp_loading.c reads.

    chant.py XBE OUT.adp

It is found by its SHA-256 below, at the offset the retail executable keeps it
at or, failing that, anywhere in the file (a CRC of its first 64 bytes picks
the candidates). Only the two checksums are here, never the bytes.
"""
import hashlib, os, sys, zlib

CHANT_BYTES = 18396
CHANT_SHA256 = '4f61e827be9b54b71e3d37a145c9b88fb32eaf517e23746af85236ca41ba8fc9'
CHANT_HEAD_CRC32 = 0xd6be65e9      # zlib.crc32 of its first 64 bytes
RETAIL_OFFSET = 3134896            # where NTSC 01.10.12.2276's default.xbe keeps it


def find(xbe_bytes):
    """the chant's bytes, or None when this executable doesn't hold them"""
    def check(offset):
        piece = xbe_bytes[offset:offset + CHANT_BYTES]
        return piece if len(piece) == CHANT_BYTES and hashlib.sha256(piece).hexdigest() == CHANT_SHA256 else None

    found = check(RETAIL_OFFSET)
    if found:
        return found
    # another release may keep it elsewhere: every offset whose first 64
    # bytes match, then the whole hash
    for offset in range(0, len(xbe_bytes) - CHANT_BYTES + 1):
        if zlib.crc32(xbe_bytes[offset:offset + 64]) == CHANT_HEAD_CRC32:
            found = check(offset)
            if found:
                return found
    return None


def write(xbe_path, out_path):
    """True when the chant was written to out_path"""
    found = find(open(xbe_path, 'rb').read())
    if not found:
        return False
    os.makedirs(os.path.dirname(os.path.abspath(out_path)), exist_ok=True)
    with open(out_path, 'wb') as out:
        out.write(found)
    return True


if __name__ == '__main__':
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    if not write(sys.argv[1], sys.argv[2]):
        sys.exit('chant.py: %s holds no chant this build knows (the loading screen will be silent)' % sys.argv[1])
    print('chant.py: wrote %s' % sys.argv[2])
