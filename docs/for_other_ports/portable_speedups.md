# Speed-ups and savings that don't need a PSP

What made Sierra-III's campaign faster or smaller without leaning on the PSP's GE, Media Engine, VRAM or cache quirks. Labels: [M] measured on a PSP, [E] emulator (PPSSPP with our cache model), [D] read in code or data, [U] inference. Figures name their build and level; the PSP-1000 is a 333 MHz MIPS with 16 KB instruction and data caches and no hardware `double`.

**Exact** means the game plays the same: the whole game state hashed after every tick matches the old code's, on several levels [E] (§C). **Same look** means it draws or sounds the same but isn't bit-identical.

**The ranking** is our judgement of value to an N64, Dreamcast or 3DS-class target (small caches, slow RAM, a few MB to spare) [U]. What helped us most was cutting memory traffic, not instructions: on b30 the tick spends 37% on instructions, 39% on instruction-cache misses and 24% on data misses [E, b131, `order_file.md`].

## A. In the engine

| Rank | What | Why it helps any small machine | Gain (ours) | Exact? | Where |
|---|---|---|---|---|---|
| 1 | **Objects sleep when their update would change nothing.** An object whose update leaves its datum byte-identical twice running is skipped; a checksum of the datum (minus words that tick for nothing: the marker stamp, a corpse's aim, timers) wakes it the moment anything else changes it. Extended to vehicles, corpses, machines, ground weapons and lit items. Then **wake-on-write**: writers from outside an object's update (damage, scripts, garbage, visibility, control) mark it, so unmarked sleepers skip even the checksum | Most of a level's objects (scenery, parked vehicles, dropped weapons, bodies) do nothing most ticks, and visiting them walks the data cache | Parked vehicles: 8 cost 4.7 ms a tick before [M, b39]. Ground weapons 5.1 ms a tick before b124 [M]. Machines on c20: tick −0.7 to −0.8 ms (−2.6 to −3.0%) [M, b121]. Wake-on-write, b30 PSP-1000: tick −2.3 to −2.8 ms, frame −5.4 to −8.7 ms [M] | Exact by construction; a check word re-sums every sleeper and logs any unmarked change (`wakecheck`) | `psp_fixups.py`: `PSP_SLEEP_CODE` (`psp_sleepy_update`, the rule since b19, with the vehicle, corpse, machine and item rules), `FIXES_106` (corpses' aim), `FIXES_121` (machines), `FIXES_123L` (items, weapons), `FIXES_WAKE`; `wake_on_write.md`, `review_b121_machines.md`, `review_b123_lititems.md`. our notes on halo-ce-vita (at the commit read) found its nearest to be a non-exact "scenery a quarter as often" (`research_vita.md` §6) |
| 2 | **No big clears or debug work in hot code.** The path search cleared its whole 82 KB state before every search; lightning cleared a 147 KB scratch array per chain per frame; Bungie's collision log cleared and copied 2,952 bytes ~73 times a second for a display nobody sees; `data_verify` ran on every array step; a debug-vertex call followed every model part; our own profiling timed every object's update with two clock reads | A clear bigger than your data cache evicts everything the tick was using, every time | Path clear: a quarter of each firing-position choice's cycles on b30 (halo-ce-vita's measurement on our map); the RV levers together ~1% of b30's tick [M, b126]. Lightning: The Library's `widgets_render` 6.4 → 0.5 ms [M, b106]. Collision log: 0.2 ms a frame [M, b127 profile]. Debug-vertex call: 318 I-misses and 4.6 k instructions a tick on b30 [E, b138]. Our object timers, now one tick in eight: ~1 ms a tick [M, b138] | Exact (each says why in its group) | `FIXES_126` (`path.c#clear`, **from halo-ce-vita, after Halo 64's H2**), `FIXES_106` (`lightning.c`), `FIXES_129` (`collision_usage.c#periods`), `FIXES_106` (`data.c#verify`), `FIXES_DRAWCOLD`; `review_b126.md`, `review_b129.md`, `review_b138.md` |
| 3 | **Collision and ray walks that read less.** Each object's bounding sphere copied into one dense array, so rays and sphere queries reject without touching the datum; a deleted object's fix-up walks only the types that have a handler; a flare's visibility ray asks the level first and skips objects when a wall already hides it; a surface reached from two leaves is tested once | The walks visit every object in every cluster they touch (~300 on b30's beach), mostly sleeping scenery, and each visit is a cache miss | Dense spheres: 95% of tests reject without the datum [E]; with b107's other items the tick 72.3 → 61.7 ms [M]. Deleted-object walk: ~1,000 objects per projectile hit before (halo-ce-vita's count). Flares: c20 frame −1.9%, the flare step 5.35 → 1.72 ms [M, b129] | Exact | `FIXES_107` (`objects.h#spheres`), `FIXES_126` (`objects.c#deleted`, **from halo-ce-vita**), `FIXES_129` (`#walls`); `psp_surface_seen_before` in `psp_fixups.py` |
| 4 | **Single precision where the game used `double`.** A header narrows `sqrt`/`floor`/`ceil`/`fmod` on floats to their float forms; the animation and periodic-function "fraction of" done in float | On the PSP `double` is software: a double `sqrt` 2,390 cycles against 31 for `sqrtf` [M]. On targets with hardware doubles the gain is smaller [U] | c20 tick −3.4% from the fraction alone [M, b129]; the narrowing ~0.3–1.5 ms a tick (estimate, `engine_performance.md` §3) | Exact: `sqrt` rounded twice equals `sqrtf`; `floor` and `fmod` are exact; the fraction proven on ~4.8 M calls, 0 differ [E] | `FIXES_129` (`halo_math.h#fraction`), the narrowing header (`engine_performance.md` §3, E16). Halo 64 narrows the same calls (`research_halo64.md` §4) |
| 5 | **Render-only work done once a frame, not once a tick.** Attached lights are re-linked to the map's clusters on the last tick before a frame, in the original order | Any port that runs 30 ticks a second at under 30 fps runs several ticks a frame; work only drawing reads is wasted on all but one | `object_connect_lights` 2.57 ms a tick on b30 [M]; with the debug-vertex change, −171 k instructions and −1,617 D-misses a tick [E, b138] | Exact (the one simulation reader writes fields only a debug line reads) [D] | `FIXES_LD1`; `review_b138.md`. Next on this line: node matrices made only when read (`levers_next_b138.md` #2) |
| 6 | **Lookups by name remembered or made cheap.** Marker lookups (a binary search by case-blind compare, every tick per weapon, aim and attachment) kept per model and name pointer; `_stricmp` without a `tolower` call per character; `verify_tag_reference` answered from the reference instead of searching every tag of its group by name; launch switches read once and cached | String compares in per-tick paths are pure overhead | Markers: 2.5% of b30's tick misses before [E, b109]. `_stricmp`: ~1% of b30's beach [E]. Tag references: HUD 1.75 → 0.93 ms [E, b66] | Exact (`tolower` maps A–Z only in the C locale the game never changes) | `FIXES_109` (`models.c#markers`), `FIXES_107` (`msvc_crt.c`), `FIXES_36` (`tag_groups.c#verify`) |
| 7 | **Sorts without a call per comparison.** The light pass sorted surface indices with `qsort_4byte` and a function pointer; now the same algorithm with the compare written in, and a bitset when no index repeats | 9% of d20's drawing instructions went to it [E, b119] | ~80,000 instructions a list on d20 → a few thousand [E] | Exact (same swaps; check word `surfacesortcheck`) | `FIXES_119`; Halo 64 does the same (`research_halo64.md` §4) |
| 8 | **Data-array allocation that doesn't re-search a full array.** A full array was scanned to its end on every failed `datum_new`, and again on the next | Effects and particles at their caps fail allocation every frame | 1.6% of b30's beach tick D-misses [E, b108] | Exact | `FIXES_108` (`data.c#full`); **ChupathingyCE's own fix**: ports on older bases may not have it [U] |
| 9 | **The AI's path work trimmed.** The nearby-firing-position search made only when a position is within reach; path smoothing scans from the last step back | The AI's path code is the hot code of big fights | Part of the RV group's ~1% of b30's tick [M, b126] | Exact (check word `aipathcheck`) | `FIXES_126` (`actor_firing_position.c#nearby`, `path_smoothing`), **from halo-ce-vita** (its V5, V6) |
| 10 | **Map checks off for known-good maps.** ChupathingyCE checks map data on every read (keyframes, collision nodes) | 1–2% of b30's tick instructions, and bigger hot code [E] | ~45 k instructions a tick [E, b110] | Exact for retail-derived maps only | `FIXES_CHUPA` (`collision_bsp.c#trusted`), `PSP_MAP_CHECKS=1` restores them |

### Memory (no speed change)

| What | Saves | Exact? | Where |
|---|---|---|---|
| Campaign-only engine (network units out) | heap 5,376 → 6,016 KB [D, b107] | yes | `optimization_ranking.md` App. A |
| Update rings sized to what a local game uses (client 128 → 16, host 32 → 1) | ~74 KB | yes [D] | `FIXES_CAMPAIGNNET`; `memory_savings_b137.md` S6 |
| Lens flares' per-light results for one view, not four split-screen views | 21 KB | yes (one local player) | commit `6ffb351`; `review_b138.md` |
| Thread stacks to measured use (deepest under 11 KB in 38,193 samples; floor 64 → 48 KB) | 48 KB a thread | yes | commit `c23d929` |
| A multiplayer client that never hosts: co-op and host-only records out | heap ceiling 3,668 → 5,220 KB [E] | yes, for a client | `mp_client_only.md` |
| Pools sized to measured peaks (actors peak 29 of 256, props 235 of 768, lights 244 of 896 on b137's ladder) | ~430 KB [D] | **no**: a fight past the peak fails to spawn; not shipped | `memory_savings_b137.md` R4 |
| Free particles capped (the Xbox's 1,024) | 15–39 ms a frame under fire before [M, b35] | **no**: long bursts thin (shipped at b36) | `FIXES_36`, `psp_capacity.h`, keep word `fullparticles` |

### I/O on slow storage

Batch the log (b30's opening frame −11%, start-up 7.4 → 1.8 s [M]); write checkpoints in 4 KB pieces (3.1 → 0.18 s [M]); one reader thread above the game for streamed data (a call 87 → 2.2 ms [M, b106]); and see `game_bugs.md` G6, P4 for the start-up's own file storms (`levers_next_b138.md`, "Batching").

## B. At bake time

Retail maps are zlib-compressed after a 2 KB header (`tools/dez.py`). Our bake rebuilds every map with a fork of Invader (`bake/INVADER.md`) plus Python tools. Sizes are Blood Gulch's unless named [D, `asset_baking.md` §3].

| What | Result | Kind | Where |
|---|---|---|---|
| Textures to paletted 8- or 4-bit (4-bit where it's no worse than a 16-bit framebuffer), top mip and bump maps dropped | 23.1 MB → 8.1 MB (8-bit), 6.3 MB (adaptive) | same look on a 16-bit screen [U] | `tools/texpack.py`, `bake/lib/bitmap_bake.py` (numpy k-means since b139: `bake_changes.md`) |
| Lightmaps paletted: 13 of 15 pages have ≤ 256 colours | 252 → 103–177 KB | lossless where ≤ 256 | `asset_baking.md` §3.1 |
| Model and level render geometry out of the tag window, into a pack read through a cache (Invader `-G`) | 4.5–5.4 MB of a campaign map's tags | exact: only the renderer reads it | `psp/src/psp_geo.c` |
| Animations re-keyframed in **the engine's own** keyframe codec (`-K`) and moved out of the window (`-M`) | frame data to 67% at 0.02° | near-lossless | `psp/src/psp_anim.c`; `asset_baking.md` §3.6 |
| Collision with 16-bit indices | −38% (objects), −40% (level) | lossless | the Invader fork |
| Detail objects (grass) left out (`-D`); bytes the game never reads (`-Z`); script nodes sized to use (`-Y`) | a30 1.2 MiB; ~0.5 MiB a campaign map; 380 KB on a multiplayer map | exact if you don't draw grass | `bake/bake.py` (flags' comment) |
| Sounds at 22,050 Hz at most, 3D effects mono, still Xbox ADPCM (the mixer unchanged) | size cut with "near zero quality cost" | same sound at a 22 kHz mix [U] | `bake/lib/soundpack.py` (S9) |
| Wall decorations cut into the wall's own triangles and drawn with depth EQUAL (no depth nudging) | fixes decal flicker on a 16-bit depth buffer | same look; a restoration | `tools/stencilbake.py`, `docs/stencil_bake.md` |

## C. How we measured (the part we'd copy first)

- **Make runs repeatable, then compare whole states.** Fixed ticks a frame, fixed seeds, and a hash of the game state after every tick (`fixedticks`, `fixedseed`, `statecrc`); `tools/statecmp.py` names the first tick and object that differ. Every "exact" claim above is two such logs agreeing on several levels. G1 and G5 in `game_bugs.md` were found this way.
- **Leave lip sync out of those runs.** A speaking unit's mouth aperture is read at the sound's play position, which follows the clock and the mixer rather than the ticks, and it moves the unit's nodes, its weapon's marker and so its shots: the original game's behaviour. So any change that only moves timing (heap layout, logging, a faster draw path) parts two runs on maps with dialogue, and the difference isn't the change's. Our test word `nomouth` makes `game_sound_set_mouth_aperture` return at once (mouths stay shut, so never for play): with it, four builds that differ only in timing play identically through 9,000 ticks on a30; without it they part at tick 5,942 [E]. halo-ce-vita gets there another way: its fixed-tick mode runs the sound manager on a frame clock. `FIXES_112` (`game_sound.c#nomouth`), `review_nomouth.md`.
- **Every change has an off word and a check word.** The off word restores the old code at launch, so one build A/Bs itself; the check word runs both ways and logs any difference ("0 of 4.8 M differ"). halo-ce-vita's verify modes do the same (`research_vita.md` §10).
- **A cache model in the emulator.** Our PPSSPP patch simulates the 16 KB caches on every access and charges misses to functions and data (`tools/ppsspp_local_patches.diff`, `tools/cachemap.py`). It tells you *where* time goes; only hardware tells you how long.
- **A strict interpreter** that reports what the hardware would fault on (unaligned accesses, FPU traps, bad addresses) and the JIT forgives (`game_bugs.md` X1, X3).
- **Ladders and frame traces.** Every level in one session, one line of numbers each (`tools/ladderreport.py`); every frame timed and split, with 99th-percentile frame times (`tools/frametrace.py`). The pipeline's shape: `docs/testing_pipeline.md`.
- **Profile-guided builds, and code layout.** PGO took 7% off the tick [M, b116/b117]; `-Os` on hot code was 30% slower [M, b108]; aligning functions to 64 and loops to 32 bytes −4.4% on b30 [E, b123]. A profile from older code can make things worse: b131's placed functions so the tick was 13–14% slower [E] (`order_file.md`). Halo 64's gp-relative small data (`-G8`, MIPS) is the same family (`research_halo64.md` H5).

## D. Tried, and not worth it for us

| Idea | What happened | Where |
|---|---|---|
| Node matrices for all scenery in one batch after the object loop | Faster on b30, slower on c10 and c20 [M]: each object's data arrived cold, and the hooks alone made each object's update ~2.5 KB bigger | `shelf.md` |
| Prefetch hints | On the PSP a fill blocks every load, so hints gain little without reshaping the code | `prefetch_map.md` |
| Inlining datum lookups | 48 KB more code; declined | `optimization_ranking.md` |
| Light re-link memo, render-state memo | ~0 on the PSP [M] | `review_b121_machines.md`, `levers_next_b138.md` |
| AI or posing at half rate for far actors | Not exact; behind words for an A/B, never default (`aihalf`, `posehalf`). halo-ce-vita's distance-tiered "Adaptive" AI rate is the better-tested form (`research_vita.md` §3) | `review_b122_aihalf.md` |

## E. What a wide screen costs

Halo keeps 4:3's vertical view and widens the horizontal one (`set_window_camera_values` in `main.c` sets the vertical field of view from 0.75 × the 4:3 tangent), so a 16:9 screen adds the side strips [D]. All our figures are 16:9 (480×272).

- **Objects:** on b30's Pelican ride the game draws 155.5 objects a frame at 16:9 against 120 at 4:3, +30% (counted in ChupathingyCE's desktop build) [E]. That is per-object work on the CPU, which a lower render resolution doesn't buy back [U].
- **Pixels:** 848 columns against 640 is +32.5% at the same height [D].
- **Cull with the width you draw.** Until b19 ours culled a 640×480 view while drawing a 480:272 one, so the screen's sides could show space the game had culled (`engine_performance.md` §4.3) [D].
- **Pick model detail for the lines you really draw.** The game measures an object's size on a 480-line screen; ours scales it to 272 lines (b19) [D]. It changes which detail level shows, so it is a look choice, not an exact one.
- **Wait by blocking, not polling.** An idle core that sleeps in a short loop to check for work wakes thousands of times a second [U]; a semaphore or event flag lets it rest. We have no power figures; `game_bugs.md` G8 is the game's own busy-wait.
