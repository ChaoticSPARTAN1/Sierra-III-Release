#!/usr/bin/env python3
"""Writes an Xbox Halo map uncompressed (for halo_loadtest on the PSP) and
prints what the PSP should report for it, to compare line by line.
usage: mapunpack.py IN.map OUT.map"""
import array, struct, sys, zlib

TAG_SPACE, XBOX_BASE = 0x1600000, 0x803A6000
raw = open(sys.argv[1], "rb").read()
head = raw[:0x800]
try:
    body = zlib.decompressobj().decompress(raw[0x800:])
except zlib.error:
    body = raw[0x800:]
data = head + body
open(sys.argv[2], "wb").write(data)

tag_off, tag_size = struct.unpack_from("<2I", head, 0x10)
tags = data[tag_off:tag_off + tag_size]
w = array.array("I", tags[:len(tags) // 4 * 4])
tags_ptr, scen, _, count = struct.unpack_from("<4I", tags, 0)
base = tags_ptr - 0x24

def at(address, fmt):
    return struct.unpack_from(fmt, tags, address - base)

names_out = data_in = data_zero = data_out = 0
for i in range(count):
    name, addr = at(tags_ptr + i * 0x20 + 16, "<2I")
    names_out += not (base <= name < base + tag_size)
    if addr == 0: data_zero += 1
    elif base <= addr < base + tag_size: data_in += 1
    else: data_out += 1
in_window = sum(1 for x in w if base <= x < base + TAG_SPACE)
in_xbox = sum(1 for x in w if XBOX_BASE <= x < XBOX_BASE + TAG_SPACE)

print("wrote %s (%d bytes)" % (sys.argv[2], len(data)))
print("What the PSP should report:")
print("  map '%s': tag data %d bytes at file offset 0x%X" % (head[0x20:0x40].split(b"\0")[0].decode(), tag_size, tag_off))
print("  built for 0x%08X; window 0x%08X-0x%08X" % (base, base, base + TAG_SPACE))
print("  crc32 of tag data: 0x%08X" % zlib.crc32(tags))
print("  tag names: %d inside, %d outside" % (count - names_out, names_out))
print("  tag data: %d inside, %d not loaded (BSPs), %d outside" % (data_in, data_zero, data_out))
print("  words that look like addresses: %d in the window, %d in the Xbox window" % (in_window, in_xbox))
scen_data = at(tags_ptr + (scen & 0xFFFF) * 0x20 + 20, "<I")[0]
n, ptr = at(scen_data + 0x5A4, "<2I")
for j in range(n):
    off, size, addr = at(ptr + j * 32, "<3I")
    print("  BSP %d: %d KB to 0x%08X" % (j, size // 1024, addr))
