#!/usr/bin/env python3
"""tagindex.py - a map's tag order as an Invader index file

usage: tagindex.py <map> <index file to write>

One line per tag, "<path>.<group>" as invader-index writes it, then a tab
and the tag's ID (its handle) in hex, for invader-build -w. The bake
rebuilds the multiplayer maps with it, every tag at the disc map's index and
with its salt: the netcode names tags by handle (an object's type, a damage
effect, a sound), so a PSP playing on a PC host needs every tag named as the
host's map names it (docs/multiplayer_plan.md, section 3.2). The disc maps'
salts don't all follow Invader's formula (Blood Gulch: the last 114 tags'
are 1 or 2 more), so the IDs are carried over; the port's Invader fork reads
them (upstream Invader takes only the path). Reads a retail map compressed
or not. The group names are halomap.py's table, spelled as Invader spells them."""
import os, struct, sys, tempfile, zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from halomap import CacheMap, FOURCC_GROUP  # noqa: E402


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    source, out = sys.argv[1], sys.argv[2]
    data = open(source, 'rb').read()
    length, = struct.unpack_from('<I', data, 8)
    with tempfile.TemporaryDirectory() as folder:
        path = source
        if len(data) != length:
            path = os.path.join(folder, 'map')
            with open(path, 'wb') as f:
                f.write(data[:2048] + zlib.decompressobj().decompress(data[2048:]))
        tags = CacheMap(path).tags
    lines = []
    for position, tag in enumerate(tags):
        if tag.group not in FOURCC_GROUP:
            sys.exit('tagindex: tag %d has a group Invader doesn\'t name (%s)' % (tag.index, tag.group))
        if tag.index != position or tag.tag_id & 0xFFFF != position:
            sys.exit('tagindex: tag %d is listed at %d' % (tag.index, position))
        lines.append('%s.%s\t%08X\n' % (tag.name, FOURCC_GROUP[tag.group], tag.tag_id & 0xFFFFFFFF))  # (CacheMap's is signed)
    # '\n' on every system: the fork's index reader takes the ID to the end of the line
    with open(out, 'w', encoding='latin1', newline='\n') as f:
        f.writelines(lines)
    print('tagindex: %d tags in %s' % (len(lines), out))


if __name__ == '__main__':
    main()
