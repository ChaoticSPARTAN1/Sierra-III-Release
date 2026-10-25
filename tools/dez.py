import sys, zlib, struct
src, dst = sys.argv[1], sys.argv[2]
d = open(src, 'rb').read()
length, = struct.unpack_from('<I', d, 8)
if len(d) == length:
    open(dst, 'wb').write(d); print(src, 'already decompressed'); sys.exit()
body = zlib.decompressobj().decompress(d[2048:])
out = d[:2048] + body
print(src, 'compressed', len(d), '->', len(out), 'header says', length)
open(dst, 'wb').write(out)
