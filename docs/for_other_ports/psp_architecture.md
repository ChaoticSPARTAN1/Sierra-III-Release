# The PSP-specific work: a map

How Sierra-III fits Halo CE onto a PSP-1000 (333 MHz MIPS, 24 MB of user RAM, 2 MB of VRAM, 16 KB caches, a second CPU called the Media Engine, a fixed-function GPU called the GE), and which ideas might carry to another console. Labels: [M] PSP, [E] emulator, [D] code, [U] inference. "Carries" is our guess [U] unless it cites another port.

## 1. The engine is patched at build time, never forked

| Piece | What it does | Where |
|---|---|---|
| Mirror | `psp_build.py` copies ChupathingyCE (pinned at a release tag) into `<out>/tree` and fixes the GCC-vs-clang textual differences | `psp_build.py` `mirror()`; pin in `UPSTREAM.md` |
| Fixups | Anchored `(old, new)` text pairs per file, grouped by build or subject (`FIXES_126`, `FIXES_WAKE` …). Each `old` must match exactly once or the build stops. Most `new` text is `#ifdef HALO_PSP` with the original kept under `#else`. A group's comment says what and why | `psp_fixups.py` (the apply loop at its end) |
| Moving the pin | `PSP_FIXUPS_CHECK=1` lists every anchor that no longer matches, building nothing | `UPSTREAM.md`, "Moving the engine pin"; `chupa_rebase.md` |
| Keep words | Nearly every change reads a word from `ms0:/halo/keep.txt` (or the launch menu) that switches it off or into a check mode, cached in a static after the first ask | `psp_keep()` in `psp/src/psp_platform.c`; App. B of `optimization_ranking.md` |
| Shelf | Experiments whose hooks cost even when off are built only with `PSP_SHELF=1` | `SHELVED_TAGS` in `psp_fixups.py`; `shelf.md` |
| Desktop units left out | ChupathingyCE's PC menus, GL renderer and so on are not compiled; the calls into them get the answers a desktop with nothing to do would give | `psp/src/psp_chupa.c`, `psp_chupa_platform.c`, `psp_campaign.c` |
| Compile and link | Hot engine `-O2`, cold `-Os`, PGO from per-build profiles; the heap sized from the program's end | `psp_build.py`; `review_b123_flags.md`, `order_file.md` |

**Carries:** fully. Any port that wants to track upstream without a fork can do this; the cost is anchors to fix at each pin move.

## 2. The renderer: the GE

The game's Direct3D calls go to a null device; `psp/src/psp_ge.c` (~19 k lines) draws the frame itself from the game's own render lists. The d3d8 path took 25.7–28 ms a frame drawing nothing [M], so its passes are skipped.

- **Level and models:** the GE reads vertices from the map's own buffers where it can, or from converted copies (§4). Triangles near the camera are clipped on the CPU against the near plane and a guard band, since the GE drops triangles that cross either (header of `psp_ge.c`; `asset_baking.md` finding 8).
- **Skinning** on the GE in batches of up to 8 bones; lightmaps as a second pass; 16-bit colour and depth; blob shadows; the HUD as GE quads (`optimization_ranking.md` App. A, "Drawing").
- **Decorations on walls** drawn on the wall's own baked triangles with depth EQUAL (`stencil_bake.md`).
- **Where its time goes:** on b136's ladder the CPU almost never waits for the GE (0.2–1.5 ms a frame); the frame is the main CPU's, in series, and the GE's reads from main RAM slow the CPU about one for one [M] (`levers_next_b138.md`).

**PSP-only:** the display-list format, swizzling, CLUT textures, the guard band's numbers. **Carries:** working from the game's render lists rather than emulating Direct3D (Halo 64's offline evaluation of the Xbox's shader stages is the other model: `research_halo64.md`); CPU near-plane clipping for any GPU without it; depth-EQUAL decals on any 16-bit depth buffer.

## 3. The Media Engine (the second CPU)

`psp/src/psp_me.c` runs jobs on the ME through mcidclan's me-core-mapper (`UPSTREAM.md`). A job: the main CPU writes its cache back, fills a slot in uncached memory, and bumps a request number; the ME polls, works, and answers the same way. The ME can't make system calls or see the main CPU's cache.

| Job | Since | What it's worth |
|---|---|---|
| The sound mixer (Xbox ADPCM decoded 64 samples at a time, mixed at 22 kHz) | b86–b87 | the CPU mixer's 1.6 ms per 23 ms block back [M] (`psp/src/psp_dsound.c`) |
| Perception's queued rays | b122 | `FIXES_122` (`#merays`), `review_b122_merays.md` |
| Next tick's base poses, predicted (the frame after the last 97–99.5% of the time) | b130, on by default b135 | b30's flight −2.0% [M] (`me_poses.md`) |
| The GE's lists replayed on the ME, one frame behind | experiment | the ME runs 0.45–0.92× the main CPU's speed per record [M]; the bus is the cost (`levers_next_b138.md` #4) |

**Carries:** to any second core. The base pose is a pure function of (model, animation, frame), so it can be made ahead anywhere. The audio caution from halo-ce-vita applies: what the sound manager chooses feeds back into play; mixing doesn't (`research_vita.md` §4).

## 4. Memory

| Region | Size (campaign, b137) | Holds | Where |
|---|---|---|---|
| Tag window | 9 MB at a fixed address | the map's tags, built for that address so no pointer needs fixing; the largest BSP; the animation cache in its unused end | `psp_capacity.h`, `bake/bake.py` (`-X`, `-T`) |
| Game state | 3.4 MB at a fixed address | the Xbox's fixed-address blocks, taken from the kernel at those addresses | `psp/src/psp_memory.c` |
| Heap | 5,600 KB, sized from the program's end | sound cache 1,792; the level copies' arena 1,056; display list 768 KB | `memory_savings_b137.md` §1 |
| Volatile partition | 4 MB (lent by the system; gone while the PSP sleeps) | geometry cache 2 MB, texture RAM cache ~2 MB | `psp_geo.c`, `psp_tex.c` |
| VRAM | 2 MB | two 16-bit framebuffers and depth, then a textures room and a models room (784 KB for models since b138) | `psp_tex.c`; `review_b138.md` §VRAM |
| Scratchpad | 16 KB beside the CPU | tables placed by misses per byte in the cache model | `psp/src/psp_scratch.c` |

- **Out of the tag window, through caches:** model and level geometry (`psp_geo.c`: asynchronous reads, answers "not yet") and animation frame data (`psp_anim.c`: never "not yet", since the pose must be right this tick). The bake rewrites their pointers to `0x0C000000 +` an offset in a pack file.
- **Textures** live in two ring caches (VRAM first, then RAM); a learned set per map is saved in play (`psp_tex.c`). Textures from VRAM cost the CPU +0–2% in misses against +65–96% from RAM [M].
- **The level copies' arena** (b111): converted vertex copies get one block of their own, so their churn never fragments the heap (`build_b111.md`). Fragmentation is still the limit: b137 failed b30's 156 KB stencil block with 451 KB free (`review_b138.md` §Heap).

**Carries:** the window idea for any machine whose RAM can't hold a whole map (Halo 64 reserves a never-freed home for the map at boot for the same reason: `research_halo64.md` §6); streaming caches keyed by a rewritten pointer; a separate arena for churny render copies; placing tables in any small fast memory by measured misses per byte [U].

## 5. Loading, front end, input

- **Loading screen** (`psp/src/psp_loading.c`, `FIXES_LOADING`; `plan_loading_screen.md`): the load is one blocking chain on the main thread, so the tag read is split into pieces that report progress; the Xbox's chant comes from the player's own bake, not the program (`docs/research/chant_provenance.md`); after the load a "settle" draws the first view, with no tick, until its textures are in.
- **Front end** (`FIXES_FRONT`; `plan_front_end.md`): the Xbox's main menu runs from `ui.map`; a launch menu before it (`psp/src/psp_menu.c`, text on the debug screen) picks a map and keep-word presets for testing. The start-up's file storms are in `game_bugs.md` G6, P4.
- **Controls** (`FIXES_CONTROLS`; `review_controls.md`): one stick and no right stick, so crouch is a toggle (Select + down), the D-pad's left is reload on a tap and action on a hold, and the analog nub is calibrated per console (`ms0:/halo/nub.txt`). Button prompts draw PSP glyphs from a sheet drawn by `tools/psp_glyphs.py` (`psp/glyphs/`).
- **The clock:** the game ran at under 1 fps until the frame clock was fixed (b71–b110), then 25–29 fps [M] (`psp_clock_handoff.md`).

**Carries:** the settle (a level's first frame without pop-in), the progress by bytes, and a debug launcher in front of the real menu.

## 6. The bake

`bake/halo-psp-bake` turns a player's own Xbox disc image into the PSP's maps; nothing from the game is in the repository (`bake/README.md`). Steps: read the disc (`bake/xiso.py`), check each map's hash, decompress (`tools/dez.py`), extract and rebuild with our Invader fork at the PSP's tag address (`bake/INVADER.md`: `-X`, `-G`, `-K`, packed collision), then pack textures (`tools/texpack.py`), sounds (`bake/lib/soundpack.py`) and wall decorations (`tools/stencilbake.py`). What each step saves is in `portable_speedups.md` §B. Since b139 the Python side needs only numpy, Pillow and shapely (`bake_changes.md`).

**Carries:** all of it but the texture formats. Baking for a fixed address, moving render-only data out of the tag window, and re-keyframing animations with the engine's own codec fit any target.

## 7. How we test (in general terms)

Two loops: the emulator in bulk (where time goes, whether a change is exact), then the PSP by hand (how long). Our emulator is PPSSPP with a cache model and a strict interpreter; runs are made repeatable (fixed ticks and seeds) and compared by a hash of the game state every tick. `docs/testing_pipeline.md` describes the shape so it can be rebuilt with your own pieces; `portable_speedups.md` §C lists what we'd copy first.

Other PSP-only tools: the hardware-counter profiler (`psp/profile`), the crash handler (`psp/crash`), numbered breadcrumbs before each call in start-up and the main loop (`psp_fixups.py`, "breadcrumbs"), and an on-screen log that survives a freeze (`halo-psp-feasibility.md`).
