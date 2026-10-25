# PIC1 compositing (work in progress, pinned 2026-10-12)

Scripts that build the XMB wallpaper from our own gameplay captures. **The captures and the
cutouts made from them stay on the maintainer's machine.** The exported PIC1 is the one exception to the
no-game-data rule (CLAUDE.md, 2026-10-12): it's committed as `../campaign/PIC1.PNG`.

| Script | Input | Output |
|---|---|---|
| `key_ring.py` | a capture of the ring against black space | `ring.png` (cut by its fitted ellipse, so gaps and debris don't matter) |
| `cut_ship.py` | a large capture of the ship | `hull.png` (GrabCut from a rough hand outline) + `plume_glow.png` (added as light) |
| `compose.py` | `../backdrop.py`'s approved backdrop (seed 7), the cutouts, the title layer | PIC1 (480 x 272) |

The hand-drawn outlines and the planet-glow colour rules in these scripts are tied to the
specific captures they were written for; a new capture needs them redrawn.

Status: placement settled (ring centre, ship right, titles top and bottom, all clear of the
dead zones). On hardware it reads as lower quality than commercial PIC1s: too much fine detail at
480 x 272 and mixed sources. Waiting on a larger ring capture.
