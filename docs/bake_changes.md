# The new bake (go_live 3.3): numpy, Pillow and shapely only

From build 139 the bake needs three Python libraries, and any Python from 3.9 to 3.14. Two it used to need are gone: scikit-learn (the palettes) and Triangle (the wall decals' cuts). Triangle has no package for Python 3.14 and its source no longer builds there, which shut out current Arch-based systems; scikit-learn was a large dependency for one function. Labels: [M] PSP, [E] emulator or measured on the bake, [D] code, [U] inference.

**Re-bake needed:** yes. The packs (`.ptex`) and the decal files (`.pstn`) come out different byte for byte, though they draw the same (below). A build from 139 on works with a bake from the new bundle; a bake from an older bundle still loads, but the release names the bundle it was checked with (go_live 3.4 adds the check).

## What changed

### Palettes: a numpy k-means (`bake/lib/bitmap_bake.py`, `kmeans()`)

Each 8-bit and 4-bit texture's palette is the centres of a k-means over its colours (RGBA where the alpha holds data, RGB otherwise); the texture quality step's (`--quality 2`, off by default) is in Oklab.

- **Before:** scikit-learn's `KMeans(n_init=1, random_state=0)`, 25 rounds for the plain palettes, 60 for the quality step.
- **Now:** `kmeans()`: the same method, written in numpy. It starts as scikit-learn does (k-means++, each next centre the best of 2 + log(k) picks weighted by squared distance), then runs Lloyd's rounds until nothing moves, at most 60. A centre left with no colours takes the farthest one, so every entry stays in use. The nearest centres are found in 32-bit chunks. Seeded, so a bake is the same on every machine.
- **Why the palettes differ at all:** two k-means runs differ by where they start, and scikit-learn's random sequence isn't numpy's. That alone moves a single texture's error by a few percent either way: scikit-learn against itself with another seed does the same.

### Wall decals: faces, then ear clipping (`tools/stencilbake.py`, `pslg_triangulate()`)

A wall triangle under a decal is cut along the decal's outline, so the part under the decal is its own triangles (`stencilbake.py`'s header says why).

- **Before:** Triangle's constrained Delaunay triangulation of the outlines and the triangle's edges.
- **Now:** the outlines are noded (shapely's `unary_union`), vertices lying a hair off a segment are snapped onto it (as Triangle's exact arithmetic split them), and `polygonize` finds the faces. Each face is cut into triangles by ear clipping, its holes joined in by cuts to the nearest outside vertex that sees them.
- Every vertex on a face's edge is a corner of its triangles, so neighbouring pieces meet without T-junctions, as Triangle's did. The triangles themselves differ (ear clipping makes thinner ones than Delaunay), but each face, and so each decal layer, covers exactly the same area.

### The launcher and the README

`halo-psp-bake` installs numpy, Pillow and shapely, and takes Python 3.14. `bake/README.md` no longer sends 3.14 users to uv.

## The proof

`--compare-old` makes each palette and each decal cut both ways in the same bake, with scikit-learn and Triangle installed for that alone, and logs:

- `kmeans compare summary`: the mean Oklab error old and new over every palette, and how many came out worse by more than 1% and 3%; `kmeans compare worse`: the worst eight by name. Outliers go to side-by-sides.
- The decals' `compare:` counts: how many source triangles were compared, how many layers covered a different area (more than a millionth of the triangle), the largest difference, and where Triangle failed and the faces didn't.

Before the disc [E, synthetic]:
- 48 test palettes (16 and 256 colours): the mean error 0.4% lower than scikit-learn's; single textures up to ~4% either way.
- 1,200 random decal layouts: every layer's area the same to a millionth of the triangle wherever Triangle succeeded, and three it failed on ("topological inconsistency") cut correctly.

On the disc [E]:
- **Decals** (bake `newbake138b`): b30's 372 cuts identical in area, layer by layer; c20's 523 cuts identical but one, which differs by a millionth of its triangle.
- **Palettes** (`newbake138b` and `newbake138c`): the mean Oklab error 2.9–3.4% lower on every map. A few 16-colour palettes come out far worse by Oklab (sparks +265%, soft flares +237%, the HUD's ammo icons +223%), the same ones on every map and whatever the start. By the measure k-means itself minimises (the squared distance in RGBA), the new palettes fit better in 505 of a10's palettes by over 1% and worse in 82, and every Oklab outlier fits *better* (sparks 32.3 → 29.8): those are textures whose colour sits under transparent texels, where the two measures disagree, not worse fits.
- **On the screen** (b138's code on both bakes, frame-locked, in the emulator): the same look. On c20, 5–18 thousand pixels a frame differ by a few levels (the new palettes), and 1–131 by more than 24 (the edge of a blue light's glow); b30's flight the same, with its decals placed and cut alike. The sparks and flares the Oklab outliers named draw as before.

## No loading picture (go_live 3.5)

The Xbox's loading screen reads `maps\loading.tga` from the disc (progress_bar.c), but the retail disc has none: its only pictures are two `xdemos/media` fonts. On the Xbox the copy fails quietly and the band draws over a black texture, so the retail loading screen is the trail alone, which the PSP already draws. The bake carries no loading picture.

## A faster bake: the same maps, sooner

The campaign and menus (10 levels and ui) baked in 9.0 minutes where they took 101 [E: one 8-core, 16-thread desktop, four maps at a time, the same disc]. All 55 output files are byte for byte the bake's before (size and SHA-256). Most of the gain is the first change below; the rest is the palette cache.

### What changed

- **One BLAS thread in each step** (`bake.py`, `tool_env()`). numpy's linear algebra runs a thread per CPU in every process by default, and the bake runs four maps at once, so each palette's small matrix products were fought over by 4 x 16 threads that wait by spinning. The bake now sets `OPENBLAS_NUM_THREADS`, `OMP_NUM_THREADS`, `MKL_NUM_THREADS` and `VECLIB_MAXIMUM_THREADS` to 1 for its steps. The sums are the same on one thread (the products split over rows, never along the 3 or 4 colour channels they add). Synthetic, two processes on two CPUs: a 256-colour palette 6.5 s -> 2.2 s, the same bytes [E].
- **A palette cache** (`texpack.py --palette-cache`; the bake's `<output>/work/palettes`, `--no-cache` without). Each palette is kept by a hash of its input pixels, the colour count and `bitmap_bake.py` itself, and read back wherever the same input comes up: the asset track's pack makes every texture but the lightmaps again from the same pixels, and maps share textures. A palette is written to a file of its own, then renamed into place, so maps baking side by side read a whole one or none. The cache is emptied at the start and removed at the end (kept with `--keep-work`).
- **The biggest maps first** (by the retail map's size): the last map to finish isn't a long one started late (on the campaign with four at a time, ~8% sooner by a schedule of earlier per-map times [U]).
- **The campaign by default**: `--multiplayer` adds the thirteen multiplayer maps (the release is the campaign build).

### The profile (`--profile`) [E, the 9-minute bake]

Per texture, its class, sizes, seconds in the palette step and the rest, and a short hash of its input pixels (no names).

- Texture packs are nearly all of a map's time; extracting, building and the wall decals take ~20 s a map together.
- 29,127 palettes asked for: 21,913 (75%) read from the cache. Made, those would have cost 2,686 CPU-seconds, 68% of the palette step's time without the cache.
- By pack: the asset track's packs read 100% of their non-lightmap palettes from the main pack's; the main packs read 76% of their non-lightmap palettes from other maps (each map's distinct inputs shared with another map: 56-84%, ui 28%). Lightmaps are each map's own (1% read back).
- What's left: 1,246 s of palettes (606 s of them lightmaps, main and track) and 306 s of the rest, of CPU time over 11 maps.

### Not measured on their own

The no-cache bake with one BLAS thread wasn't run again; from the profile, it would take about 20 minutes on that machine (the 2,686 CPU-seconds the cache saved, four maps at a time) [U]: so ~5x from the threads, ~2.2x more from the cache.

### The fast palettes (`--fast-palettes`): off, for a decision

Not exact. Each k-means run settles on a seeded 8,192-point subsample, then takes at most 10 rounds on the whole sample; both stop once no centre moves by 0.5 (`bitmap_bake.fast_kmeans()`). The same starts and tries as now. `--compare-palettes` makes every palette both ways in one bake and logs time and error by class, size and pixel hash.

Four maps (a10, b30, c20, ui), every pack, 6,763 palettes [E]:

| Palettes | n | Time now -> fast | Mean Oklab error | Worse by >3% / >10% | Better by >1% |
|---|---|---|---|---|---|
| all | 6,763 | 1,200 -> 749 s (x1.60) | 0.00687 -> 0.00695 (+1.1%) | 12.2% / 5.3% | 14.9% |
| level (senv), 256 | 260 | 132 -> 61 s (x2.15) | -0.1% | 12.3% / 5.4% | 16.2% |
| lightmaps, 256 | 724 | 162 -> 107 s (x1.51) | +0.4% | 20.4% / 11.6% | 11.9% |
| objects (soso), 16 | 1,147 | 149 -> 83 s (x1.80) | +3.0% | 13.1% / 7.0% | 17.6% |
| objects (soso), 256 | 880 | 198 -> 133 s (x1.49) | -0.3% | 12.2% / 5.7% | 23.6% |
| HUD and menus, 16 | 1,021 | 145 -> 97 s (x1.50) | +2.8% | 11.9% / 3.4% | 11.3% |
| HUD and menus, 256 | 453 | 194 -> 112 s (x1.73) | -0.5% | 27.2% / 13.7% | 27.4% |
| the rest, 16 / 256 | 1,518 / 760 | 221 -> 157 s | -0.6% / +0.2% | 2.6% / 13.9% (>3%) | 10.0% |

- The squared distance k-means minimises: +0.3% over all.
- The worst are 16-colour object and HUD sheets: up to +277% Oklab error on one object texture, +100-200% on five HUD sheets, the same textures in every map. Most of the >10% cases are textures whose error is near zero either way: of palettes with an Oklab error over 0.001, 2.2% are worse by over 10%.
- **What it would save:** with the cache, the palettes left are 1,246 CPU-seconds of the 9-minute bake (half of them lightmaps). At x1.6 that is ~470 CPU-seconds, about 2 minutes off 9 [U].
- **Recommendation:** leave it off. The exact changes above took the bake from 101 to 9 minutes; this would take ~2 more, and its 16-colour outliers would need side-by-sides on the PSP first.

## 16 colours where they are exact (clut4exact)

Textures whose 8-bit texels use 16 palette words or fewer, over every level, are written at 4 bits: the same texels from half the bytes, which the GE samples faster (`texpack.py`, `clut4_exact()`; `docs/review_clut4exact.md`, the census in `design_clut4w.md`). Each is read back from both packings in the bake and kept at 8 bits unless every level and the runtime's average colour match; each pack's log says how many were switched and that 0 differ. Pack flag 2 marks them, so `psp_tex_average` samples them as before. 28-65 textures a pack, 42-334 KB [E, bake b140x]. `--no-clut4-exact` (or `--no-exact-clut`) leaves them at 8 bits.

**Re-bake needed:** for the saving only. An older bake still loads and draws as before; an older EBOOT draws the new packs the same, but its object lighting takes a very slightly different average from the switched level maps.

## 16 colours where they look the same (the CLUT4 look set)

Lightmaps and HUD sheets the bake would make CLUT8 are written at 4 bits when one 16-colour palette over the whole chain passes the CLUT4W census's strict bar on every level: screen PSNR within 1 dB of CLUT8's own (or over 45 dB), no texel more than one 5-bit step past CLUT8's worst, and for alpha the same tests on coverage (`texpack.py`, `clut4_look()`; `docs/review_clut4look.md`). Unlike the exact set this is lossy, so a texture that misses the bar stays CLUT8. CLUT8's chain is kept (same levels and sizes); ui.map (the showcase), the meters' grid and the multipurpose maps stay out. On by default from 2026-10-21; `--no-clut4-look` (or `--no-clut-look`) bakes without it. Each pack's log has `clut4 look` lines: textures switched, KB saved, the lowest level dB per class, and how many missed the bar. 13-50 textures a pack, 30-523 KB (the asset track's packs 30-137 KB; per map in `review_clut4look.md`) [E, bake b140y].

**Re-bake needed:** for the saving only. Older bakes load and draw as before; the runtime already takes CLUT4 lightmaps and HUD sheets.

## What a player needs to do

Bake again with build 139's bundle: the palettes and decal cuts change.
