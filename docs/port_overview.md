# How the Halo PSP port works (overview, build 102)

A map of the moving parts, from the source to the frame on the screen. The other project docs go deep on each; this one says how they fit together.

## 1. The code: an Xbox game rebuilt for a MIPS handheld

- **The game** is halo-ce-universal, a decompilation of Halo CE for the Xbox into C (CC0). It already builds for Linux; the port builds it for the PSP's Allegrex (MIPS32 with an FPU and the VFPU vector unit) with pspdev's GCC.
- **The decomp is barely touched.** Two commits (timers that don't assume 30 Hz, cold asserts). Everything else is `psp_fixups.py`: text replacements applied to a mirror of the source at build time (thousands of lines: memory sizes, the renderer hooks, the debug memory left out, unaligned accesses fixed, breadcrumbs). Why: the decomp moves on upstream, and one script that says what changed and why is easier to carry forward than a fork.
- **The platform layer** (`psp/src`) replaces what the Xbox kernel, Direct3D 8 and DirectSound gave the game: memory (`psp_memory.c`), files and threads (`psp_platform.c`, `psp_libc.c`), input (`psp_sdl.c`), sound (`psp_dsound.c`, mixed on the Media Engine by `psp_me.c`), and the renderer (`psp_ge.c`).
- **`psp_build.py`** mirrors, applies the fixups, compiles ~480 files, links a PRX, sizes the heap to fill the gap below the game state, and packs EBOOT.PBP. Two helper modules ride along: `halo_crash.prx` (the crash handler's kernel half) and `halo_profile.prx` (hardware counters).

## 2. Memory: 24 MB of user RAM, 4 MB volatile, 2 MB VRAM

| Region | Size | What |
|---|---|---|
| 0x08804000 program | 5.6 MB | code 2.9 MB, read-only data 0.5 MB, data 0.2 MB, zeroed data 2.1 MB (the display list 384 KB, render globals, game statics) |
| heap | 4.9 MB | sound cache 1.1 MB, the level's vertex copies (≤ 1 MB), allocations |
| 0x092B0000 game state | 3.3 MB | the Xbox's game state, at the Xbox's size: objects, AI, effects (saved whole for checkpoints) |
| 0x09600000 tag window | 9 MB | the map's tags (the Xbox gives 22 MB: maps are rebuilt to fit, below) |
| 0x09F00000 | 1 MB | thread stacks, the crash handler's block |
| volatile 0x08400000 | 4 MB | the texture RAM cache (3 MB) and, on campaign maps, the geometry cache (1 MB, or 1.5/2 with `geo15`/`geo2`) |
| VRAM | 2 MB | two 16-bit frame buffers, depth, the texture cache (1.2 MB) |

The Xbox has 64 MB; the PSP-1000 has 32 (8 of it the kernel's). The port fits by moving bulk data out of the tag window into packs streamed from the Memory Stick, and by sizing the game's own pools for one player.

## 3. Data: the maps rebuilt for the PSP (the bake)

The Xbox maps can't be used as they are: their tags expect 22 MB at the Xbox's address, and their textures and geometry are in Xbox formats. Each map goes through:

1. **Invader** (a fork): extract the tags, rebuild the map for the PSP's address (`-X 0x09600000 -T 0x900000`), writing model and level geometry to a separate `.pgeo` pack (`-G`) and keyframing animations (`-K`).
2. **texpack.py** → `.ptex`: every texture decoded and re-baked for the GE (8- or 4-bit palettes, swizzled, sized for 480×272). The asset track's `_track.ptex` has half-size lightmaps and coverage-preserving mips for alpha-tested plants.
3. **stencilbake.py** → `.pstn`: the wall decals Halo draws with the stencil buffer, cut into the walls' own triangles (the GE's stencil is too limited).
4. `halo-psp-bake` does all of it from a disc image in one command.

## 4. A frame

1. **The tick** (`game_tick`, 30 a second): objects, AI, physics, scripts, as on the Xbox. Objects that leave themselves unchanged sleep (scenery, parked vehicles).
2. **The render pass**: the game's own visibility and render code runs as on the Xbox, but every draw call lands in `psp_ge.c` instead of Direct3D:
   - **The level**: per material, vertices copied once into GE format (float or 16-bit), triangles cut at the near plane on the CPU, drawn in passes: base texture, lightmap multiplied in, detail maps, the dynamic lights.
   - **Models**: vertices converted in place in the geometry cache, skinned by the GE (up to 8 bones a batch), lit by the game's per-object lighting: an ambient colour and two directional lights.
   - **See-through and effects**: sorted and drawn last; particles, decals, the HUD as sprites.
3. **Textures** bind through `psp_tex.c`: VRAM first, then the RAM cache, else read from the `.ptex` (a budget a frame; a miss draws untextured that frame).
4. **Geometry** locks through `psp_geo.c`: a block missing from the cache is read asynchronously and the draw skipped that frame (the "flashing" at area transitions).
5. **The display list** (384 KB) is built by the CPU while the GE draws the previous one.

## 5. Sound

The game's DirectSound calls are emulated (`psp_dsound.c`); the mixing runs on the Media Engine (the second CPU, mcidclan's me-core) from the sound cache, which streams from the Memory Stick.

## 6. Finding problems

- **The log** (`halo/log/log.txt`): a heartbeat (game time, frame and tick times, memory), periodic reports from each subsystem, written in batches.
- **The launch menu** turns on test switches (keep words): benchrun, blackbox, crashplay, hwprofile, the asset track, cache sizes.
- **The crash handler** (b102): a report on the screen and in `halo/crash.txt`.
- **The emulator harness**: PPSSPP, patched to be a PSP-1000 (32 MB), with a strict interpreter that reports what a real PSP would fault on (unaligned words, out-of-range addresses, FPU traps), an I-cache model, scripted input, teleports and route replays from a hardware log.

## 7. Where the time goes (b101, The Library, worst window)

157 ms a frame (6.3 fps): ticks 44 ms (2 a frame; bipeds 8.8 ms a tick, machines 7.5, weapons 2.5), the frame's CPU 96 ms (the GE renderer's CPU 36: level 11, models 9, lights 6.5, see-through level 5.3, decals 4.6), the rest 18. The CPU waits on memory for over half its time; the GE reading main RAM at the same time makes each CPU miss about twice as long (b93). That's why the work is about bytes crossing the bus, not instructions.
