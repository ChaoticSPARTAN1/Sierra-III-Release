#!/usr/bin/env python3
"""Filter PPSSPP strict-interpreter output (Interpreter.cpp patch).
Kinds: range ("load of X from pc P": kernel/low/>=0x0A000000 access; the
volatile 0x08400000-0x08800000 range and its mirrors are legal on our layout
and dropped), unaligned ("unaligned load of N bytes at X from pc P"), FPU
traps ("STRICT FPU: what at pc P"). Prints one line per (kind, pc)."""
import sys, re, collections
rng = re.compile(r'STRICT: (load|store|fpu load/store) of ([0-9a-f]{8}) from pc ([0-9a-f]{8})')
una = re.compile(r'STRICT: unaligned (.*?) at ([0-9a-f]{8}) from pc ([0-9a-f]{8})')
fpu = re.compile(r'STRICT FPU: (.*) at pc ([0-9a-f]{8})')
seen = collections.OrderedDict()
def add(kind, pc, addr):
    k = (kind, pc)
    if k not in seen:
        seen[k] = [0, addr]
    seen[k][0] += 1
for line in open(sys.argv[1], errors='replace'):
    m = una.search(line)
    if m:
        add('unaligned ' + m.group(1), int(m.group(3), 16), int(m.group(2), 16)); continue
    m = rng.search(line)
    if m:
        addr = int(m.group(2), 16); a = addr & 0x0FFFFFFF
        if 0x08400000 <= a < 0x08800000:
            continue
        add('range ' + m.group(1), int(m.group(3), 16), addr); continue
    m = fpu.search(line)
    if m:
        add('fpu ' + m.group(1), int(m.group(2), 16), 0)
for (kind, pc), (n, addr) in seen.items():
    print(f"{n:6d}  {kind:32s} pc {pc:08x}  e.g. {addr:08x}")
print(f"{len(seen)} distinct sites", file=sys.stderr)
