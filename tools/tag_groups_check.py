#!/usr/bin/env python3
"""Checks the bake's tag group table (bake/lib/halomap.py's FOURCC_GROUP).

    tools/tag_groups_check.py <engine checkout> [<Invader fourcc.hpp>]

The table was written from ChupathingyCE's source, so every group in it but
the editing kit's own four must appear there as a character literal ('scnr').
Given Invader's fourcc.hpp as well, it also proves the table names every group
as Invader does, entry for entry: the bake hands these names to Invader's
definitions and to its tag index, so a different spelling would break them.
Exits 1 on any difference.
"""
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(os.path.dirname(HERE), 'bake', 'lib'))
from halomap import FOURCC_GROUP  # noqa: E402

# groups the editing kit has and the engine never loads
KIT_ONLY = {'boom', 'devc', 'ngpr', 'tagc'}


def engine_literals(root):
    found = set()
    for sub in ('source', 'port'):
        for dirpath, _, files in os.walk(os.path.join(root, sub)):
            for f in files:
                if f.endswith(('.c', '.h')):
                    text = open(os.path.join(dirpath, f), encoding='latin1').read()
                    found.update(re.findall(r"'([^'\\\n]{4})'", text))
    return found


def invader_table(path):
    out = {}
    for m in re.finditer(r'TAG_FOURCC_(\w+) = 0x([0-9A-Fa-f]+)', open(path, encoding='utf-8').read()):
        out[int(m.group(2), 16).to_bytes(4, 'big').decode('latin1')] = m.group(1).lower()
    return out


def main():
    if len(sys.argv) not in (2, 3):
        sys.exit(__doc__)
    bad = 0
    lits = engine_literals(sys.argv[1])
    for tag in sorted(FOURCC_GROUP):
        if tag not in lits and tag not in KIT_ONLY:
            print('not in the engine: %r %s' % (tag, FOURCC_GROUP[tag]))
            bad += 1
    print('%d groups; %d found in the engine, %d the editing kit\'s own'
          % (len(FOURCC_GROUP), len(set(FOURCC_GROUP) & lits), len(KIT_ONLY - lits)))
    if len(sys.argv) == 3:
        inv = invader_table(sys.argv[2])
        for tag in sorted(set(inv) | set(FOURCC_GROUP)):
            if inv.get(tag) != FOURCC_GROUP.get(tag):
                print('differs from Invader: %r ours %s, Invader\'s %s'
                      % (tag, FOURCC_GROUP.get(tag), inv.get(tag)))
                bad += 1
        print('against Invader\'s %d: %s' % (len(inv), 'identical' if not bad else 'DIFFERENT'))
    sys.exit(1 if bad else 0)


if __name__ == '__main__':
    main()
