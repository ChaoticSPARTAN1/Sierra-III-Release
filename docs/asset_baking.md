# Asset baking for the PSP port: what has to shrink, and how

Scope: Halo CE (Xbox) → PSP-1000. Every number is tagged **(M)** for measured from `bloodgulch.map` / `ui.map` with the scripts in `/home/claude/research/baker/`, or **(E)** for estimated (the reasoning is given next to it). "wu" means world unit (1 wu = 3.048 m).

---

## 0. Summary

| # | Finding |
|---|---|
| 1 | Blood Gulch already fits: **7.31 MB tags + 1.29 MB BSP = 8.60 MB of the 9.0 MB window** (M). The census accounts for every byte of the window. 98.3% is walked tag data, and the rest is the tag index, the names and 4-byte alignment. |
| 2 | **Model render geometry is the largest class in the window: 2.90 MB, which is 40% of the tags** (M). Only the renderer reads it, so it can leave the tag window completely. Baked to 14-byte GE vertices it is **1.42 MB**, a 51% cut (M), with an error of at most 0.19 mm on any non-sky part. |
| 3 | **Animations come second at 1.79 MB** (M). The engine already contains Bungie's keyframe-compressed animation decoder (`animation_get_keyframe_rotation` and related functions). Re-encoding into that format brings the frame data to **67% at 0.02°** (about the int16 source precision), **56% at 0.1°** or **47% at 0.25°** (M, greedy keyframing). |
| 4 | **Textures: 23.1 MB of Xbox pixel data become 8.1 MB as CLUT8** with one mip dropped and bump maps dropped, or **6.3 MB** when CLUT4 is chosen wherever it is no worse than the 16-bit framebuffer's own quantization (M, prototype). In 95% of textures the CLUT8 error is below the 5650 framebuffer quantization error (M). Resolution, not palette depth, is what shrinks the data: CLUT8 at full resolution would be *larger* than the Xbox DXT data (27.5 MB, M). |
| 5 | **Sound: 12.2 MB of Xbox ADPCM (827 s) → 12.45 MB of VAG** (M, +1.8%). The size does not drop, but sceSas can then mix VAG in hardware. Median permutation 8 KB, p90 32 KB (M), so a single-read stream costs about 2–4 ms (E). |
| 6 | **Collision with 16-bit indices: −38.0% on object collision, −40.2% on the level's collision** (M). This matches the known 35–38%. |
| 7 | **Campaign (E):** after in-place baking, b30 and a10 still **do not fit** the 9 MB window. With render geometry moved out of the window and the BSP split into a resident collision part and a volatile render part, the estimate is **about 10–11.3 MB vs 9 MB**. Struct repacking, a looser animation tolerance and dropping names could close the gap, but whether they do depends on the real class mix. Measuring a10/b30 with `census.py` takes seconds once the maps are uploaded. |
| 8 | Pre-splitting large ground triangles **does not remove** the need for the CPU near-plane clipper. Clipping at the near plane throws vertices outside the guard band whatever the triangle size. Splitting to 8 wu costs +9% triangles (M) and only bounds the per-triangle clipping work. |

---

## 1. Method and tools

* **Parser:** `halomap.py` is a plain-Python reader for Xbox-format cache files. It follows `cache_files.c` (`cache_file_header`, `cache_file_tag_header`, the 32-byte `cache_file_tag_instance`). Struct layouts come from **Invader's tag definition JSON** (`/home/claude/invader/src/tag/hek/definition/*.json`; 361 structs, each size checked against the declared size at load). A generic walker follows every reflexive (tag block), data blob and dependency.
* **Coverage check (M):** the walk reaches 7,186,429 unique bytes out of a 7,313,912-byte window (98.3%). What it doesn't reach is the 126,028-byte table at the start of the window (tag header, 1,806 × 32 B instances, about 68 KB of names) and about 1.4 KB of 4-byte padding. The BSP blob walk reaches 1,289,803 of 1,290,240 bytes. 48 KB of blocks are shared between tags (Invader deduplicates them).
* **Invader:** prebuilt binaries already sit in `/home/claude/invader/build`, so I didn't rebuild. `invader-info` rejects both `bloodgulch.map` and the original upload ("invalid map"; the two files are byte-identical, so the upload is already the PSP-relocated build). It probably expects compressed Xbox maps. Using Invader's *definitions* in my own parser turned out to be the better route because it avoids that dependency. It's also what the baker needs anyway: the same walker can rewrite pointers.
* **Map facts (M):** version 5 (Xbox), build 01.10.12.2276, 44,034,048 bytes, uncompressed. Tag window base 0x09600000, 1,806 tags, 359 model vertex buffers and 359 index buffers registered in the tag header. One BSP at file offset 0x800, loaded at 0x09DC5000 (directly after the tags).

---

## 2. Blood Gulch census (all measured)

### 2.1 File layout

| Region | Bytes | Where |
|---|---:|---|
| Header | 2,048 | 0x0 |
| BSP blob (`sbsp`, loaded on BSP switch) | 1,290,240 | 0x800 |
| Bitmap pixel data (516 bitmaps) | 23,116,800 | interleaved with sounds, 1.3–35.5 MB |
| Sound sample data (916 permutations) | 12,229,380 | 3.8–35.6 MB |
| Tag data (window) | 7,313,912 | 0x2304E00 |
| **Total** | **44,034,048** | |

### 2.2 Tag window by class

| Class | Tags | Bytes | Share | Main content |
|---|---:|---:|---:|---|
| `mode` models | 63 | 3,031,524 | 41.9% | vertices 2,626,080 (82,065 × 32 B); strip indices 270,764; structs 134,680 |
| `antr` animations | 19 | 1,786,494 | 24.7% | frame data 1,440,988; default data 179,360; animation structs 62,280 |
| `effe` effects | 318 | 578,768 | 8.0% | `events/particles` 447,528 (1,929 × 232 B), of which 214,778 is `pad` |
| `scnr` scenario | 1 | 426,434 | 5.9% | script syntax data 380,076: the full 19,001-node array, **0 nodes used** on this MP map |
| `coll` collision | 36 | 355,916 | 4.9% | 91 collision BSPs (306,184 B) |
| `snd!` sound tags | 315 | 190,863 | 2.6% | 916 permutations × 124 B |
| `font` | 4 | 157,346 | 2.2% | pixels 116,274 |
| `bitm` headers | 368 | 137,632 | 1.9% | bitmap_data, sprite sequences |
| `proj` | 16 | 96,540 | 1.3% | material responses 84,160 |
| 47 other classes | | 361,850 | 5.0% | `soso` 46,640, `jpt!` 44,352, `weap` 39,944, `DeLa` 35,154, `part` 34,176, `matg` 32,492… |
| tag index + names | | 126,028 | 1.7% | |
| vb/ib headers | | 8,616 | 0.1% | |

`pad` fields inside tag structs add up to **608,462 B (8.3%)** (M). That is the ceiling on what struct repacking could save. Repacking needs engine struct changes; the biggest share is `effe` at 214,778 B.

### 2.3 The BSP (`levels\test\bloodgulch\bloodgulch`, 1,290,240 B)

| Part | Bytes | Share | Contents |
|---|---:|---:|---|
| Collision BSP | 666,668 | 51.7% | 12,071 bsp3d nodes, 4,616 planes, 5,999 leaves, 13,182 bsp2d refs, 4,916 surfaces, 7,650 edges, 2,704 vertices |
| Render geometry | 283,306 | 22.0% | 5,790 rendered vertices × 32 B + 5,534 lightmap vertices × 8 B = 229,552; 5,503 triangles × 6 B = 33,018; 79 materials |
| Render BSP tree | 280,466 | 21.7% | nodes 72,426; leaves 95,984; leaf surfaces 112,056 |
| Clusters, portals, subclusters | 50,816 | 3.9% | |
| Pathfinding | 4,916 | 0.4% | |
| Detail objects / decals / weather / sound | < 2,000 | | detail objects: 64 B (none on this map) |

### 2.4 Bitmaps: 516 bitmap_data in 368 tags, 23,116,800 B

| Format | n | Bytes | | Type | n | Bytes |
|---|---:|---:|---|---|---:|---:|
| DXT1 | 228 | 9,227,392 | | 2D | 489 | 22,090,368 |
| DXT3 | 112 | 6,240,384 | | cube | 21 | 912,384 |
| A8Y8 | 28 | 1,510,144 | | 3D | 6 | 114,048 |
| A8R8G8B8 | 14 | 1,279,872 | | | | |
| P8 (bump) | 6 | 1,136,256 | | **Max side** | | |
| DXT5 | 5 | 988,672 | | 1024 | 6 | 2,009,984 |
| R5G6B5 | 19 | 821,632 | | 512 | 47 | 8,750,848 |
| A1R5G5B5 | 7 | 817,152 | | 256 | 129 | 8,383,616 |
| X8R8G8B8 | 35 | 441,216 | | 128 | 148 | 2,743,040 |
| AY8 / Y8 / A8 / A4R4G4B4 | 62 | 654,080 | | ≤ 64 | 186 | 1,229,312 |

110 bitmaps have no mips. Lightmaps are 15 R5G6B5 pages, all without mips, 252,032 B in total. The 1024-wide bitmaps (postgame carnage report, video mask, xdemos) are larger than the GE's 512 maximum.

Bytes by role (`refs.py` works out the role from the shader field that references each bitmap):

| Role | n | Xbox bytes |
|---|---:|---:|
| object shaders (`soso`) | 114 | 7,249,408 |
| effects/sprites | 152 | 3,807,744 |
| UI/HUD | 98 | 2,654,848 |
| level shaders (`senv`) | 14 | 2,228,992 |
| transparent shaders | 57 | 1,627,776 |
| bump maps | 9 | 1,617,920 |
| sky | 8 | 1,246,336 |
| detail maps | 14 | 918,656 |
| cube maps | 19 | 780,288 |
| rasterizer/other | 10 | 618,752 |
| lightmaps | 15 | 252,032 |
| 3D | 6 | 114,048 |

### 2.5 Sounds: 916 permutations in 315 tags, 12,229,380 B, all Xbox ADPCM

| | Permutations | Bytes | Seconds* |
|---|---:|---:|---:|
| 22,050 Hz | 895 | 11,061,684 | 803.2 |
| 44,100 Hz | 21 | 1,167,696 | 23.5 (stereo ambience `countryday2`) |
| mono | 834 | 8,863,308 | 714.6 |
| stereo | 82 | 3,366,072 | 112.2 |
| **total** | 916 | 12,229,380 | **826.8** |

\* Seconds use 36 B per 64 samples per channel. That is about 1.5% low, because an Xbox ADPCM block actually carries 65 samples.

Largest classes: projectile impacts 1.91 MB, object impacts 1.71 MB, scripted effects 1.37 MB, footsteps 1.19 MB, ambience 1.17 MB. Median permutation 8,028 B, p90 32,616 B, maximum 66,240 B. The largest single sound tag is 1.17 MB.

`ui.map` (M), for reference: 1.62 MB of tags (`DeLa` widgets 550 KB, `scnr` 385 KB, fonts 157 KB), 19.5 MB of bitmaps (63.6% DXT3), and 12.4 MB of sound (233 s of it at 44.1 kHz: music).

---

## 3. PSP-native baked forms

Legend: **L** = lossless, **≈L** = invisible or inaudible at 480×272 / PSP output (with the evidence), **V** = visible, as a trade-off option.

### 3.1 Summary table (Blood Gulch)

| Asset | Xbox form | PSP baked form | Before | After | Kind |
|---|---|---|---:|---:|---|
| Textures | DXT1/3/5, 16/32-bit, P8 bump | swizzled CLUT8 (CLUT4 where its PSNR ≥ the 5650 reference); top mip dropped; bump dropped; cubes → 64² sphere maps | 23.12 MB (M) | 8.10 MB CLUT8 / **6.34 MB adaptive** (M) | ≈L (V for cube→sphere) |
| Model vertices | 32 B (float pos, 3 packed vectors, s16 uv, 2 nodes) | 14 B: s16 uv, s8 normal, s16 pos with per-part scale; per-node batches, GE skinning only where needed | 2.63 MB (M) | **1.15 MB** (M) | ≈L (0.19 mm max) |
| Model indices | 16-bit strips | same strips (8-bit for parts < 256 vertices: optional) | 271 KB (M) | 271 KB | L |
| Animations | uncompressed (8 B quaternions, 12 B translations) | engine's own keyframe format (6 B quaternions + keyframe indices) | 1.44 MB frame data (M) | **0.97 MB at 0.02°** / 0.81 at 0.1° / 0.68 at 0.25° (M) | ≈L at 0.02° |
| Object collision | 32-bit indices | 16-bit indices | 306 KB (M) | **190 KB** (M) | L |
| Level collision | 32-bit indices | 16-bit indices | 666 KB (M) | **399 KB** (M) | L |
| BSP render vertices | 32 B + 8 B lightmap | keep the game's 32 B stream (CPU code reads it); add a lightmap-UV stream for the GE | 230 KB (M) | 230 KB + 89 KB GE lightmap stream (E) | L |
| BSP leaf surfaces | 2 × int32 | 2 × int16 | 112 KB (M) | 56 KB (E) | L |
| Script syntax (MP) | 19,001-node array | truncate to nodes used (0 here) | 380 KB (M) | ~0 (M) | L (needs care, §3.8) |
| Tag names | 68 KB | keep (debug, `tag_loaded`) or replace with hashes | 68 KB (M) | 0–68 KB | L |
| Struct padding | in place | repacked structs (engine change) | ≤ 608 KB (M) | 0 | L, but expensive |
| Sounds | Xbox ADPCM | VAG (PS-ADPCM, 16 B / 28 samples) for sceSas | 12.23 MB (M) | **12.45 MB** (M) | ≈L (transcode) |
| Lightmaps | R5G6B5, no mips | CLUT8 at full resolution (13 of 15 pages have ≤ 256 colours: exactly lossless) | 252 KB (M) | 103–177 KB (M) | L / ≈L |

### 3.2 Textures (prototype: `bitmap_bake.py`)

Policy A, which the prototype implements and measures:

1. **Resolution:** textures with a max side of at least 64 drop their top mip, and everything is capped at 512. Why this is ≈L: the Xbox renders 480 lines and the PSP 272. That is 2^-0.82 of the texel density, so the GE would only sample the original mip 0 at magnifications the Xbox barely reached. This is **(E)**; it holds for world textures and needs an eye check on HUD bitmaps, which the HUD draws at 1:1 pixels, scaled by 272/480.
2. **Format:** swizzled CLUT8 (16-byte × 8-row blocks; rows padded to 16 bytes, levels to 8 rows). Opaque textures use a 16-bit 5650 palette (512 B), which is lossless here because the framebuffer itself is 5650. Textures with alpha use an 8888 palette (1 KB). **Alpha is quantized as data, not transparency:** level textures keep blend masks in alpha, and the colour under alpha 0 is visible. PIL's default RGBA resize and quantize blacken that colour; the prototype filters each channel separately and clusters in 4-D (k-means).
3. **Mips:** the chain is kept down to 8 px (the GE has 8 levels). Lightmaps stay at full resolution without mips, as on the Xbox.
4. **Dropped or converted:** bump maps (P8, height and vector maps: 1.62 MB) are dropped because the GE has no per-pixel lighting to use them. Cube maps become 64×64 CLUT8 sphere maps for GE environment mapping (V, an approximation). 3D textures stay at 8 bpp.

Results (M; PSNR is the median per role, in dB, against the downscaled truecolour image; "565" is the PSNR of just the framebuffer's 5650 quantization):

| Role | n | Xbox B | CLUT8 B | CLUT4 B | PSNR CLUT8 | PSNR CLUT4 | PSNR 565 |
|---|---:|---:|---:|---:|---:|---:|---:|
| object shaders | 114 | 7,249,408 | 3,696,384 | 1,845,088 | 52.9 | 38.8 | 40.2 |
| effects/sprites | 152 | 3,807,744 | 1,150,848 | 549,216 | 99 (lossless) | 40.5 | 41.6 |
| UI/HUD | 98 | 2,654,848 | 850,176 | 418,560 | 99 | 48.6 | 42.7 |
| level shaders | 14 | 2,228,992 | 646,144 | 320,736 | 47.6 | 37.8 | 37.1 |
| transparent shaders | 57 | 1,627,776 | 535,040 | 258,688 | 99 | 44.7 | 42.3 |
| bump | 9 | 1,617,920 | 0 | 0 | dropped | | |
| sky | 8 | 1,246,336 | 275,968 | 136,608 | 48.6 | 38.6 | 36.9 |
| detail | 14 | 918,656 | 379,648 | 189,152 | 54.7 | 42.2 | 37.0 |
| cube → sphere | 19 | 780,288 | 114,304 | 56,544 | | | |
| other | 10 | 618,752 | 208,000 | 101,760 | 99 | 99 | 37.0 |
| lightmap | 15 | 252,032 | 177,152 | 88,288 | 99 | 39.5 | 37.3 |
| 3D | 6 | 114,048 | 68,864 | 33,088 | | | |
| **Total** | 516 | **23,116,800** | **8,102,528** | **3,997,728** | | | |

* **Adaptive choice (CLUT4 when its PSNR ≥ the 565 reference − 1 dB): 6,343,232 B, with CLUT4 picked for 295 of 516** (M). At a threshold of −0 dB the total is 6.71 MB; at −3 dB it is 5.47 MB (the −3 dB setting is V).
* For CLUT8, the margin over the 565 reference is p5 = +2.2 dB and median +42.5 dB. The only bitmap below the reference is `rasterizer\video noise` (−1.7 dB), which is a noise texture.
* `previews/clut_compare.png` shows the cyborg, warthog and blood ground textures as Xbox mip 0, PSP CLUT8 at half resolution, and CLUT4. CLUT8 matches the original. CLUT4 visibly loses the cyborg's blue patch, which is why CLUT4 is chosen per texture and not applied globally.
* **Residency (E):** the resident texture budget is about 2.25 MB (1 MB VRAM + 1.25 MB RAM). The level's always-visible set comes to about 1.10 MB adaptive: `senv` 0.52, lightmaps 0.10, detail maps 0.23, sky 0.24. That leaves about 1.1 MB for objects, effects and HUD in view, out of 6.3 MB on disk. That calls for an LRU texture cache streaming from the Memory Stick, as the Xbox streamed into its 22 MB cache. The tags' predicted-resource blocks (per cluster and object) are the existing prefetch hints for this.
* **Engine contract:** keep `bitmap_data.width/height/registration_point/format` as they are, because HUD, sprite and text code read them. Put the PSP dimensions, format, mip offsets and palette in a **side table indexed by bitmap** (or in the runtime-only fields `hardware_format`, `cache_block_index` and `pixels_offset/size`, which only `xbox_texture_cache.c` uses).

### 3.3 Model geometry (`model_geom.py`)

* 359 parts, 82,065 vertices, 135,382 strip indices (M). Only **1,328 vertices (1.6%) blend two nodes**. **67,031 of 70,001 triangles (95.8%) use a single node** (M): they can be drawn with one matrix and no weights.
* Nodes per part (M): 246 parts use 1 node, 41 use 2, 21 use 3, 17 use 4, 13 use 5, 3 use 6, and 18 use 9 or more. Those 18 need splitting into batches of at most 8 bones.
* **Baked vertex: 14 B** = s16 uv ×2 + s8 normal ×3 + 1 pad + s16 pos ×3. Positions are scaled per part through the model matrix: the largest non-sky part spans 4.15 wu, so the position step is 0.19 mm (M). Binormal and tangent go, because model shaders do no bump mapping and the GE couldn't use them. The 8-bit normal costs about 0.5° of lighting direction (≈L). Mixed-node triangles go into skinned batches with 8-bit weights per batch bone (+k B for those vertices only). **2.63 MB → 1.15 MB** (M), +~2% for skinned batches (E). The sky model (parts up to 111,000 wu) keeps float positions.
* **LODs:** 1.32 MB (45%) of the geometry is reachable only from the high and super-high LODs (M). LOD selection uses the object's pixel diameter on screen (`render_frustum_sphere_diameter_in_pixels`), so at 272 lines the game naturally picks lower LODs more often. But high LODs are still reached up close, and cinematic "movie stars" always force super-high. Dropping them would be V. Streaming them on demand into a geometry cache is the ≈L route.
* **Nothing CPU-side reads model vertices** (only `models.c` → rasterizer, M by code search). The whole 2.9 MB can therefore **leave the tag window** for a geometry cache or chunk. That is the biggest single window saving.

### 3.4 BSP render geometry (`bsp_geom.py`)

* The game reads BSP vertex positions on the CPU: `structure_visibility.c` (per-triangle frustum test), `object_lights.c` (object lighting from lightmap vertices) and `structures.c` (M by code search). So keep the game's 32-byte stride, which `psp_ge.c` already draws zero-copy, and add a GE stream for the lightmap pass. The GE has one UV set per vertex, so lightmapping is a second pass with lightmap UVs: s16 uv + float pos = 16 B, or 10 B with 16-bit positions on a BSP-wide grid of 6.8 mm, which shares lattice points and so leaves no cracks (M). If the CPU users are later adapted, two GE-only streams with 16-bit positions would be 181 KB against 263 KB now (M).
* **Pre-splitting (M):** triangle edges: median 1.89 wu, p99 9.85 wu, maximum 16.3 wu. Splitting to ≤ 8 wu gives 5,503 → 6,013 triangles (+9%); to ≤ 4 wu ×1.90; to ≤ 2 wu ×5.83. It doesn't make the CPU clipper unnecessary (E, geometry): a triangle that crosses the near plane at 0.06 wu produces clipped vertices more than 10× the frustum width out, beyond the ±2048 px guard band, at any triangle size. Recommendation: keep the runtime clipper, and optionally split at 8 wu to bound its per-triangle cost.
* **Display lists:** precompiled GE lists per (cluster, material), called with `sceGuCallList` for visible clusters, replace the per-frame CPU triangle-list build. Cost (E): about 32 B per (cluster, material) pair plus the index data that exists already.

### 3.5 Collision (`deep.py`)

The layout with 16-bit indices: bsp3d node 12→6 B, leaf 8→4, bsp2d reference 8→4, bsp2d node 20→16, surface 12→8, edge 24→12, vertex 16→14; planes stay 16 B. The largest counts (12,071 bsp3d nodes, 13,182 bsp2d references) fit in 15 bits with the child-is-leaf flag bit. **Objects 306,184 → 189,916 B (−38.0%); level 666,372 → 398,678 B (−40.2%)** (M). This changes the engine: the collision structs and the code that walks them. Campaign BSPs need a check that every count stays below 32,768. If one doesn't, that BSP keeps 32-bit indices: a per-BSP format flag.

### 3.6 Animations (`anim.py`, `anim_keyframe.py`)

* 346 animations, 11,166 frames, 4,152 rotation, 362 translation and 40 scale tracks; all uncompressed (M). Frame layout: per frame, node by node, rotation 4×s16, then translation 3×f32, then scale f32, for the flagged nodes. The flag bits must be masked by the node count; the bits past it are junk (M).
* Lossless candidates are small: only 26 rotation tracks are constant (M), and the default data deduplicates only 179,360 → 175,192 B (M).
* **The real lever is the format the engine already decodes:** `compressed_animation_header` with 6-byte quaternions and keyframe frame indices, interpolating between keyframes (`model_animations.c`). Greedy keyframe selection gives frame data **1,440,988 → 965,370 B at 0.02° / 0.1 mm** (≈L: the int16 source step is about 0.0035°, so this is about 6 source steps), → 808,000 B at 0.1°/0.9 mm, and → 682,298 B at 0.25°/3 mm (M). The baker writes the compressed blob and sets `_animation_compressed_bit`. Check that `hs_model_animation_compression_enabled` is on in the PSP build.
* The cyborg graph alone is 759 KB (195 animations, M). Campaign maps carry several graphs like it (§4).

### 3.7 Sounds

* **VAG** (16 B per 28 samples, plus a 16 B end frame): 12.45 MB for Blood Gulch, +1.8% (M). Transcoding Xbox ADPCM → PCM16 → VAG is a second lossy step (≈L, E; worth a listening check on impacts and dialogue). sceSas mixes VAG in hardware (32 voices), which removes the CPU ADPCM decode. Stereo sounds become two panned mono voices.
* **Sample rates:** keep 22,050 Hz (sceSas pitch handles any rate). Option V: the 21 permutations at 44.1 kHz (ambience) at 22.05 kHz saves 0.59 MB. In `ui.map`, music at 44.1 kHz halves from 12.6 to 6.7 MB.
* **Residency:** the 512 KB cache stays; the baker can help in three ways (E). (1) Lay out each sound tag's permutations contiguously, 64-byte aligned (sceSas wants aligned VAG in RAM). (2) Mark a resident "hot set" from classes that fire every second: footsteps, weapon fire, projectile impacts (about 3.6 MB on the Xbox, so only a subset fits). (3) Keep everything else on-demand. At 1.75 ms + size/16 MB/s, a median 8 KB permutation loads in about 2.3 ms and a p90 32 KB one in about 3.8 ms, both well inside a 33 ms frame, with reads issued asynchronously and the start deferred by one tick.

### 3.8 Everything else

| Item | Measured | Bake |
|---|---|---|
| Script syntax data | 380,076 B; data array "script node", maximum 19,001 × 20 B; **0 used** on Blood Gulch | Truncate to the highest used node + 1, and set the array's maximum to match. This needs checking against `hs` code that may allocate nodes at runtime (console/debug). Campaign maps use a real share of it. |
| Tag names | 68 KB | Keep for now (`tag_loaded` matches by name, and they make debugging possible); hashing later. |
| Effects | 578,768 B; 214,778 B of it `pad` | Repacked `effect_definitions` structs (engine change); later phase. |
| Fonts | 157,346 B, 116,274 B of pixels | 4-bit glyphs (≈L) −58 KB, or re-render at PSP size. |
| Editor/debug data | `editor scenario data` 438 B; source files and comments 0 | Bungie's tool already stripped them. |
| Unreferenced tags | 0 unreferenced bitmaps; every tag is reachable | Nothing to strip on this map. |

### 3.9 Blood Gulch after baking, by budget

| Budget | Now | After the baking above | Notes |
|---|---:|---:|---|
| Tag window (9.0 MB) | 8.60 MB (M) | **≈4.4 MB** (E from M) | tags 7.31 − model geometry 2.90 − animation 0.48 − collision 0.12 − scripts 0.38 ≈ 3.44; BSP 1.29 − collision 0.27 − leaf surfaces 0.06 ≈ 0.97 |
| Geometry cache (volatile) | – | 1.42 MB on disk, LRU subset resident | all LODs of every model |
| Texture cache (≈2.25 MB) | – | 6.3–8.1 MB on disk; ~1.1 MB always resident for the level | §3.2 |
| Sound cache (512 KB) | – | 12.45 MB VAG on disk | §3.7 |

---

## 4. Campaign extrapolation (all E)

Known inputs (from the brief): **b30 15.0 MB tags + 6.3 MB largest BSP; a10 13.5 MB + 8.2 MB; window 9 MB; volatile partition 4 MB.**
Assumptions: Blood Gulch's class mix (models 40%, animations 25–30%, collision ~5%, other ~30%) and BSP mix (collision 52%, render geometry 22%, render tree 22%, rest 4%). Campaign maps carry more character graphs: marine, elite, grunt, jackal and hunter next to the 759 KB cyborg, plus cinematic animations. They also carry a much bigger scenario (AI encounters, command lists, scripts actually used, recorded animations) and pathfinding in the BSP. The animation and scenario shares are probably *higher* than Blood Gulch's, which makes the estimates below optimistic.

| Step (b30) | Window MB | Notes |
|---|---:|---|
| as is | 21.3 | 15.0 + 6.3 |
| bake in place (geometry 14 B, animations 0.02°, collision 16-bit) | ≈15.4 | not enough on its own |
| + model geometry **out of the window** (to the volatile partition / geometry cache: ≈2.9 MB baked) | ≈12.4 | |
| + BSP split: collision + tree + clusters + pathfinding resident (≈3.3), render geometry (≈1.4) to volatile | **≈11.0** (≈9.9 with a geometry-heavier mix and animations at 0.25°) | still over by about 1–2 MB |
| + struct repacking (−8%), animations at 0.25°, names hashed | ≈9.0–9.7 | **borderline** |

| Step (a10) | Window MB | |
|---|---:|---|
| as is | 21.7 | 13.5 + 8.2 |
| geometry out + BSP split (collision 4.26 → 2.56, tree 1.8 → 1.44, rest 0.33) | **≈11.3** | |
| + repacking / looser animations / names | ≈10 | **over** |

**What dominates the campaign window (E):** animations (the largest resident class once geometry leaves), BSP collision, then the scenario. **Volatile partition (E):** model geometry 2.6–2.9 MB + BSP render 1.4–1.8 MB ≈ 4.0–4.7 MB against 4 MB, so it has to be an LRU cache, not a full load. **Textures (E):** campaign maps likely hold 40–60 MB of Xbox pixel data → 11–21 MB baked on disk, against the same ~2.25 MB resident budget. Interiors (a10) show many more distinct textures per view than Blood Gulch, so the texture cache's hit rate is the risk.

The conclusion is that baking alone does not make a10/b30 fit the current memory map. A layout that might fit: window = gameplay data only (tags without render geometry, the collision BSP, keyframe-compressed animations), with render data (model and BSP geometry, textures, sounds) streamed into the volatile, texture and sound caches. Whether it fits depends on the real a10/b30 class mix. **First action once the maps are uploaded:** run `python3 census.py a10.map; python3 deep.py a10.map; python3 anim_keyframe.py a10.map`; this replaces every E in this section with an M.

---

## 5. Baker pipeline design

### 5.1 Inputs

1. Xbox retail map. Retail maps are zlib-compressed, so they're decompressed first. Then **Invader** extracts the tags and rebuilds for PSP addresses (`invader-build -X 0x09600000 -T 0x900000`), as was done for Blood Gulch. Invader handles the relocation and the 9 MB check.
2. The **baker** (Python, built on `halomap.py`) reads that relocated map directly. Its job is format conversion plus re-linking the tag window. It doesn't need Invader at runtime; it reuses Invader's JSON struct definitions.

### 5.2 Output: a PSP map file (`.pmap`)

```
0x000  cache_file_header (unchanged 'head'/'foot'; version 5)
       + PSP fields in reserved68: psp_format_version, directory offset/size
       chunk directory: {kind, tag index, sub index} -> {offset, size, format, dims, flags}
       BSP n, gameplay half   -> loaded into the window after the tags (collision 16-bit, tree,
                                  clusters, pathfinding, materials minus vertex data)
       BSP n, render half     -> loaded into volatile (game 32 B vertices + GE lightmap stream
                                  + per-(cluster,material) display lists); its pointers are relocated
                                  to the volatile base
       tag data               -> re-linked: model vertex/index data removed; vb headers point at
                                  geometry chunk descriptors; animations keyframe-compressed;
                                  scripts truncated
       geometry chunks        -> per model and LOD: 14 B vertices, strips, node batches (2 KB aligned)
       texture chunks         -> per bitmap: palette + swizzled mip chain, 16 B aligned,
                                  starting at 2 KB boundaries; grouped by tag
       sound chunks           -> per permutation: VAG, 64 B aligned; a tag's permutations contiguous
```

**Re-linking** (removing data from the middle of the window) means rewriting every pointer. The walker already finds all of them: reflexive addresses, data-blob addresses, tag-instance base address and name, vb/ib header `Data`, and the model-part triangle, vertex and header pointers. Invader's definitions mark the remaining raw pointers: `ModelGeometryPart.vertex pointer`, `BitmapData.pointer`, `SoundPermutation.samples pointer`, `ScenarioStructureBSPMaterial.*vertices index pointer`, and the BSP compiled-header pointers. Verification: walk the re-linked map, check that every block's content is identical apart from the pointers, and compare per-class byte totals.

### 5.3 What changes in the engine

| Area | Function / struct | Change |
|---|---|---|
| Map loading | `scenario_tags_load`, `cache_file_open` (`cache_files.c`) | read the tag data as now, plus the chunk directory (a small RAM table) |
| BSP | `scenario_structure_bsp_load` / `_unload`, `structure_bsp_header_register_vertex_buffers` | two reads: gameplay half → window, render half → volatile |
| Model buffers | `tags_header_register_vertex_and_index_buffers` | point each part's `vertex_buffer.hardware_format` at its geometry chunk descriptor |
| Textures | `xbox_texture_cache.c`: `texture_cache_bitmap_new` (currently recomputes `pixels_size` from the format, line 380), `texture_cache_start_loading_bitmap`, `texture_cache_initialize_hardware_format`, `_texture_cache_bitmap_get_hardware_format` | PSP texture cache: size, offset and format come from the directory; load into VRAM/RAM; the hardware format becomes a GE texture descriptor (CLUT, mip pointers) |
| Sound | `xbox_sound_cache.c` (`sound_cache_start_loading_sound`, line 504; reads `samples.file_offset/size`), `sound_dsound_xbox.c` | VAG chunk from the directory; sceSas voices |
| Models | `models.c` → `psp_ge_draw_model` | baked 14 B format, per-node batches, GE skinning for mixed batches |
| Level | `psp_ge_draw_level` / `structure_render.c` | lightmap pass from the GE stream; `sceGuCallList` per visible cluster |
| Animation | `model_animations.c` | none: the compressed path already exists (keep `hs_model_animation_compression_enabled` on) |
| Collision | `collision_bsp` structs and walkers (`physics/`) | 16-bit variant behind a per-BSP flag |
| Scripts | `hs` load of `script syntax data` | data array maximum = the truncated count |

### 5.4 Incremental path

1. **Blood Gulch textures.** Append texture chunks and the directory to the current map; tag data untouched. Add the PSP texture cache and GE texturing in `psp_ge.c`, using the existing zero-copy level vertices (the uv is already inside the 32-byte vertex). `bitmap_bake.py` already produces swizzled CLUT8 level-0 blobs; it still needs to write mip chains, the directory and the cache code. Test: flat → textured Blood Gulch.
2. **Lightmap pass** for the level (GE lightmap UV stream + CLUT8 lightmaps).
3. **Sounds to VAG + sceSas.** This also removes the 512 KB thrash by making reads asynchronous.
4. **Model geometry chunks** (14 B, node batches) → the first re-link of the window. Blood Gulch's window goes from 8.6 to about 5.7 MB.
5. **Keyframe-compressed animations.** Check the cyborg in game against the uncompressed original.
6. **16-bit collision + BSP split.**
7. **Campaign.** Run the census on a10/b30, fit the window (§4), then optional struct repacking and texture-cache tuning.

---

## 6. Files (`/home/claude/research/baker/`)

| File | What it does |
|---|---|
| `halomap.py` | cache-file reader plus a JSON-driven struct walker (blocks, data, dependencies) |
| `census.py` | window census by class and field path, BSP breakdown, gaps → `bg_census.json`, `ui_census.json` |
| `assets.py`, `asset_census.py` | bitmap_data and sound permutation enumeration; format, size and seconds tables → `bg_assets.txt` |
| `refs.py` | bitmap roles from shader references; struct padding totals → `bg_refs.txt` |
| `deep.py` | models by tag and LOD, collision with 16-bit indices, script nodes → `bg_deep.txt` |
| `model_geom.py` | node and skinning statistics, 16-bit error → `bg_models.txt` |
| `bsp_geom.py` | BSP triangle sizes, pre-split cost, stream sizes → `bg_bsp.txt` |
| `anim.py`, `anim_keyframe.py` | animation layout, constant tracks, keyframe-format estimate → `bg_anim.txt` |
| `bitmap_bake.py` | **prototype texture baker**: decodes DXT1/3/5 and 16/32/8-bit formats (unswizzling Xbox Morton order), applies policy A, k-means CLUT8/CLUT4, GE swizzle, PSNR report → `bg_bake.txt`, `bitmap_bake_bloodgulch.json`; with `--out`, writes GE-ready `.clut8` blobs (palette + swizzled level 0; Blood Gulch's are in the session scratchpad, 7.1 MB) |
| `previews/clut_compare.png` | Xbox / CLUT8 / CLUT4 crops |

Each script runs in under 5 s, except the full texture bake (about 90 s with k-means on 2 cores).

## 7. Open points and risks

* Mip-drop "invisibility" is an estimate. Check it in PPSSPP at 480×272, especially HUD bitmaps drawn 1:1.
* The VAG transcode needs a listening test. A direct Xbox ADPCM → VAG requantizer could avoid the PCM round trip, but probably isn't needed.
* Truncating script nodes needs the `hs` runtime allocation paths checked.
* Cube → sphere maps (warthog headlights, shiny shaders) is the one deliberately V texture change. The alternative is to drop reflections.
* The campaign window fit (§4) is the main unknown. It is cheap to resolve once a10/b30 are available.

## 8. Addendum, 2026-10-03 (build 49): the bake now keeps each bitmap's own mip count

* **The design in §3.2 said lightmaps stay without mips, as on the Xbox; `tools/texpack.py` didn't do that.** It built every texture's chain down to 8 px, lightmaps included. Measured on `bloodgulch.map`: all 15 lightmaps have 0 extra mip levels on the Xbox, as do 11 object bitmaps and 78 HUD bitmaps.
* **The visible cost:** a lightmap page is an atlas of charts separated by dark gutters, and its small levels mix the charts and gutters together. Surfaces the GE textured from those levels (seen edge-on, or far off) came out dark. These were the thin dark lines seen on hardware: present in normal play, absent with keep word `nomip`, which forces full-size textures (b47 / b48).
* **The fix (exact, more faithful):** a baked texture gets at most the Xbox bitmap's level count (its mip count + 1, less the levels dropped above the baked size). 129 of 501 Blood Gulch textures changed. Lightmaps, and objects with none on the Xbox, now have a single level; effects sprites lost the extra levels the source never had. The pack went from 5.41 to 5.18 MB. Keys and count are unchanged, so the learned VRAM file stays valid.
* Rule for the next wave: the bake follows the source's own level count. More levels is a deviation, not a free improvement.

## 9. Addendum, 2026-10-03 (pack 51): HUD meter greys must keep their step

* **Measured from `bloodgulch.map`:** the assault rifle's ammo meter (`ui\hud\bitmaps\combined\hud_ammo_meters`, sequence 1) draws its 60 rounds with greys 4, 8 … 240. Its meter definition has alpha multiplier 4 and bias 2, so with n rounds loaded the threshold is 4n + 2, and a round lights while its grey is below that. Bungie put the threshold halfway between two rounds.
* **The bug:** the bake's 64-grey grid palette used greys 4k + 2, which moved round 60's 240 to 242: exactly the full magazine's threshold, so the last round never lit. Every hardware test showed it.
* **The fix:** the grid's offset is now the bitmap's commonest grey modulo 4 (0 here), and greys round to the nearest grid value. 9 meter bitmaps changed; nothing else in the pack did.
* Rule: a quantized bitmap that the code compares against thresholds must keep the values its thresholds were designed around. Check the meter definition's multiplier and bias against the bitmap's greys before choosing a palette.
