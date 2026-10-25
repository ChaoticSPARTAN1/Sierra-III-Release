# Research: Halo 64 (the N64 port), read for Sierra-III

A tour of JackTulli's [halo-64](https://github.com/JackTulli/halo-64) (commit `264da17`, 2026-10-14) for tricks Sierra-III could use. Halo 64 runs Halo CE's multiplayer on an N64 with the Expansion Pak: all 13 maps, 4-player split screen, bots and Firefight. It is built on halo-ce-universal (now OpenCE), so its engine code is close to ChupathingyCE's.

Labels: [M] measured on a PSP, [E] emulator or estimate, [D] read in code or data, [U] inference. Halo 64's own numbers are from the ares emulator and are marked as theirs.

**Licence and credit.** Halo 64's code is CC0, as the decomp's is, so we may adapt it freely. Anything we take still names Halo 64 in its commit message and review section.

**Where to read it.** `port/n64/HISTORY.md` (the development log, newest first) and `port/n64/NOTES.md` (the notebook) are the best entry points. Engine changes are all under `#ifdef HALO_N64`, so `grep -rn HALO_N64 source/` finds every one.

## The short answer

Sierra-III is already ahead of Halo 64 on most CPU work: the double-maths narrowing, tag lookups by name, the keep-word cache, the sort, object sleeping and posing are all done here, several of them more exactly. **Five things are new to us** and fit the rules as exact or restoring changes (§1). A sixth is design input for the biped collision plan (§2). The rest are behaviour changes for the A/B shelf (§3) or N64-only (§5).

## 1. Worth taking (exact or restoring)

| # | Trick | Kind | Expected gain | Next step |
|---|---|---|---|---|
| H1 | `aiming_change` gives x86's answer for units with no aiming limit | **Restoration** | Correctness, not speed | Take it |
| H2 | `path_state_new` clears only the header, not the whole state | Exact | One 47 KB clear per path search [D] | Count searches a tick, then take it with a check word |
| H3 | Prisms facing away from the move skip their test | Exact | One divide per skipped prism [D] | Take it |
| H4 | `add_feature` finds duplicates by a stamped hash | Exact | Unknown on the PSP | Count its compares in the emulator first |
| H5 | gp-relative small data (`-G8`) | Exact (code generation) | Halo 64: 2.3% fewer instructions, 28 KB less code (static counts, theirs) | One flag A/B on the rig, if the PRX tools accept it |

### H1: `aiming_change` for units with no aiming limit (restoration)

- **The divergence we already know about.** `halo-psp-feasibility.md` (line 415) records it as known and unfixed. A unit whose `aiming_velocity_maximum` is 0 computes `angle / 0`.
  - When the unit is still, that's 0/0, a NaN.
  - x86 converts a NaN to the integer 0x80000000, so the byte comes out **0**. The PSP's FPU converts it to 0x7FFFFFFF, so the byte comes out **255**.
- **Why it matters.** `aiming_change` feeds the unit's exported "aiming change" function (`units.c`, `_unit_function_aiming_change`) [D]. Any tag that drives a shader or animation from it sees full aiming change on a unit standing still, where the Xbox and the desktop servers see none.
- **Halo 64's fix** (`source/units/units.c`, `#ifdef HALO_N64`): it spells out x86's answers.

  ```c
  real maximum = unit_definition->unit.aiming_velocity_maximum / TICKS_PER_SECOND;
  real change = maximum > 0.f ? angle / maximum : angle > 0.f ? 1.f : 0.f;
  ```

  A turn over a zero maximum is +infinity on x86, which pins to 1. No turn is the NaN, which ends as 0.
- **For us:** a restoration, so on by default behind a keep word. It also brings the PSP in line with the desktop server for multiplayer.
- **Not taken with it:** Halo 64 also replaces the arc cosine with a chord approximation (3% of their game with bots). It isn't bit-exact and can flip a byte value, so it belongs on the A/B shelf (§3) if it's ever wanted. Their 2–3% cost from the NaN was the N64's FPU trapping on it; our threads mask those traps (`psp_fpu_mask_traps`), so we pay nothing there [D].

### H2: the path search's 47 KB clear

- **What.** `path_state_new` (`source/ai/path.c`) does `csmemset(state, 0, sizeof(*state))` before every search. Halo 64 clears only up to `node_list`:

  ```c
  csmemset(state, 0, offsetof(struct path_state, node_list));
  ```

- **Why it's exact (their reasoning, to verify on ChupathingyCE).** A node's fields are all written when the node is taken (`path_state_begin`, `path_state_traverse`). The heap is read only below its count. The hash table is cleared by `path_state_reset`.
- **Why it pays.** Clearing 47 KB runs a 16 KB data cache through three times over, evicting the AI's hot data along with it. Halo 64 measured ~2.5 ms of cache line fills per search (theirs).
- **Sierra-III status.** Not done: our fixups touch only the path *debugger's* state [D].
- **Next step.** Count `path_state_new` calls a tick on b30 and a50 in the emulator, to size the gain. Then take it behind a word, with a check word that runs each search both ways and compares the paths.

### H3: prisms facing away from the move

- **What.** In `collision_move_point`'s feature loop (`source/physics/collision_features.c`), a hit only counts on a face the move runs into: `dot(vector, plane.n) < -epsilon`. For a prism, the hit's plane normal is always the prism's own (`collision_prism_test_vector` sets `plane->n = prism->plane.n`) [D]. So the facing test can run *before* the prism test:

  ```c
  if (dot_product3d(vector, &prism->plane.n) < -_real_epsilon &&
      collision_prism_test_vector(prism, point, vector, &feature_t, &feature_plane) &&
      closest_feature_t > feature_t)
  ```

- **Why it's exact.** The same dot product on the same bits, tested earlier. Halo 64 also splits the loop by type, which keeps the original order (all spheres, then cylinders, then prisms).
- **Why it pays.** About half the prisms around a pill's edges face away, and each prism test costs a divide (Halo 64's count) [U for our maps].
- **Sierra-III status.** Not done: our fixups don't touch this loop [D].

### H4: `add_feature` by hash

- **What.** The sphere walk lists each vertex, edge and surface once. `add_feature` scans the whole list to check whether an entry is already there. Halo 64 replaces the scan with a 1,024-slot hash, stamped per walk so it never needs clearing (`collision_bsp.c`, `n64_feature_hash_begin`). It also skips re-walking a surface already listed. The lists come out the same, in the same order [D].
- **Sierra-III status.** We already skip re-walked surfaces, for walks past three leaves (`psp_surface_seen_before`, keyed by surface and designator). Vertices and edges still go through the engine's linear `add_feature` [D].
- **Next step.** A cache-model count of `add_feature`'s compares on a50, where walks reach 47 leaves. If they're a real share of the walk, take the hash for vertices and edges.

### H5: gp-relative small data (`-G8`)

- **What.** With `-G8`, globals of 8 bytes or less go in `.sdata`/`.sbss` and are read with one load from `$gp`, instead of `lui` plus a load.
- **Their result.** 2.3% fewer instructions in the program (306,788 → 299,715) and 28 KB less code. Those are static counts, not timings.
- **Sierra-III status.** Listed in `optimization_ranking.md` (Build) with the caveat that PRX relocation may forbid it. We build with `-G0` throughout, and `halo.elf` goes through `psp-prxgen`.
- **Next step.** One test link: does `psp-prxgen` accept GP-relative relocations? If it does, one rig A/B. Exact by construction: it changes only how globals are addressed.

## 2. Design input: the biped collision plan

Halo 64 shipped a per-biped level cache on the main CPU (12 October; `source/physics/collisions.c`, `n64_pill_features_in_sphere`). That's the same idea as our `plan_biped_collision_me.md`, step 2, from a different direction. Unmeasured on their side ("Unmeasured" in their log).

**What's the same:**
- A sphere 0.25 units wider than the pill's, kept while the needed sphere stays inside. Our `pillreach` numbers say that margin holds for 20–70 sweeps.
- Dropped with the map, at a BSP switch, and when breakable surfaces change.
- Objects gathered fresh each tick from the kept clusters.

**What differs, and why we keep our design:**
- Halo 64 *filters* the kept list (vertices and edges by the walk's sphere tests, surfaces by bounding spheres). Their claim is that the features come out the same except in order, which only decides between two at exactly the same distance.
- Our plan rejects filtering as not exact. `add_feature` keeps first occurrences, and the 256 caps cut differently. A walk of a copy gives the walk's own order.
- So their design is evidence that filtering *plays* fine, not that it's exact. Ours stays the exact one; theirs is a fallback if the copy proves too costly.

**Worth borrowing: their answer to a50's size problem** (our plan, §6):
- Fixed budgets per cache: 48 vertices, 96 edges, 64 surfaces, 8 clusters. 12 caches cost 23 KB in all.
- When a build overflows its budget, the margin halves, down to 0.1. While a cache has spare room, the margin grows back.
- If even 0.1 overflows, the biped walks the level as before and retries after 30 ticks (`N64_PILL_CACHE_RETRY_TICKS`).
- For us: a cap on each copy's bytes with the same backoff turns a50's 30–60 KB copies [U] into "no copy where the level is too busy". The cost is a miss, never a wrong answer. Step 2's log of each copy's bytes then tells us how often a50 falls back.

## 3. The A/B shelf (behaviour changes, the maintainer's call)

None of these is exact. Each would ship off by default behind a keep word, with an A/B, if the exact work runs out.

| Trick (Halo 64's) | What it changes | Their lesson |
|---|---|---|
| Holstered weapons get their full update every 4th tick | Timing of a holstered weapon's state | Keep the weapon update itself every tick: a holstered plasma weapon cooled at a quarter speed until they did |
| Vehicle physics every other tick, impulses scaled to the ticks covered (`n64_physics_step`) | Suspension and contacts settle differently | Skip the *whole* physics block on off ticks: vehicle mass points are a stack array the step fills, and the slipping, suspension and crash updates read it uninitialised (their first build crashed) |
| Bodies settle one a tick; unseen ones not at all | Ragdoll-free bodies come to rest later | |
| Path search a slice a tick (40 surfaces), weighted (distance to go ×1.5), bounded | Paths arrive later and differ | The search is Halo's own `path.c`, but its callers there are Halo 64's own bots and Firefight AI, so their search counts don't carry over to our actors |
| Aim change from the chord, not the arc cosine (H1's companion) | `aiming_change` within 1e-4, may flip a byte | |
| Effects made only of sounds, particles, decals and lights skipped beyond hearing and sight | Effects that drift into view later are missing | Exact for sounds past their maximum distance; not for particles |

## 4. Already in Sierra-III (so no one re-researches it)

| Halo 64's trick | Ours |
|---|---|
| Double-precision `sqrt`/`floor`/`fmod` narrowed to float | The type-generic narrowing header, bit-exact (`engine_performance.md` #2, E16) |
| Debug flags looked up once where they're asked | Callers cache `psp_keep()` in a static |
| `tag_loaded` remembering name lookups | `verify_tag_reference` answered from the reference (b66; HUD 1.75 → 0.93 ms [E]) |
| `qsort` replaced by a written-out sort | `qsort_4byte` replaced (b119, `surfaceqsort` restores it) |
| Idle objects every 4th tick | `weapontrackawake` and `machinefuncsheld`: exact sleeping (b124) |
| Unseen or far AI posed every other tick | `posehalf`, `aihalf` (b122, opt-in A/Bs) |
| Datum lookups inline | Measured and declined: 48 KB of code (`optimization_ranking.md`) |
| Ray tests reject object types by header | In the ME's line-of-sight path (`psp_object_test_vector_quiet`) |
| Halo's own sight rays skipped by PVS | Halo's perception already does it (`actor_perception.c`); Halo 64's version is for its own bots |
| Periodic heap walks and stack scans only with `debug_stats` | Our log's cost on the stick is a known lever (`STATE` §1, test 14) |

## 5. Not transferable (N64-only)

- The RSP command queue's buffer size: a libdragon detail. The general lesson holds: when the CPU waits on the graphics queue mid-frame, the queue is too small.
- `-mno-fix4300`: a VR4300 multiplier erratum.
- Removing 8-byte loop padding. Our b123 flag sweep found the opposite on the PSP: 64-byte function and 32-byte loop alignment wins.
- The RDRAM "ninth bits", CI4 textures, the RDP blender wrapping on addition, baked vertex lighting: hardware specifics, or detracting on our side.
- Big-endian map conversion: the PSP is little-endian like the Xbox.

## 6. Process notes

- **A heap that can't fragment.** The map image gets a home made at boot and never freed. Everything long-lived (sound, mixer rings, animation cache, HUD slots) is reserved at boot below it. They found leaks by diffing heap dumps from one game to the next, until every menu showed the same 74 blocks. Worth copying if campaign sessions ever fragment across level loads or checkpoint reloads; b120 ran all ten maps in one session without a crash [M].
- **A compiler trap.** Their "is there room?" probe did `malloc` then `free` without using the block, and GCC removed both, so every probe succeeded. Fixed by keeping the block in a `volatile`. Check any room probe of ours for the same pattern.
- **Their unattended test.** `debug_cycle=<seconds>` returns to the menu, which starts the next map: every map in turn, as a player goes. Our test harness does the same job differently.
- **The workflow.** Their log shows an AI-assisted port with the user testing every build ("built, not tested by me (the user tests)"), and the log covers about a week (8–14 October), from the first two-player game on Wizard to the release. The HISTORY/NOTES split mirrors our `STATE` plus build notes. It's a good public example of the shape we plan for the README's "How it's made" section.
