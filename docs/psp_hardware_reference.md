# The PSP-1000 for the Halo port: a hardware reference

2026-10-03. Written for whoever comes next (the Halo 2 wave included). Everything here was read from primary sources whole, not from summaries, after the earlier mistake of accepting a digest of the Copetti article. Where a figure comes from a run, it says which run.

**Labels.** **[M]** measured, by us (a PSP-1000 or this emulator, as stated). **[D]** documented: in the firmware's own code (uofw), an SDK header, a hardware document, or a library's source we read. **[C]** claimed: by a homebrew author about their own tests, not reproduced here. **[U]** untested: our inference. Treat [C] and [U] as questions for the next hardware run.

---

## 1. The machine, as it matters to a 30 Hz game

| Part | Facts | Source |
|---|---|---|
| Main CPU ("SC", Allegrex) | MIPS32 R2-like core, 7-stage pipeline, 32-bit, no MMU/TLB (fixed mapping), 222 MHz by firmware default, 333 MHz allowed (`scePowerSetClockFrequency(333, 333, 166)`; we run 333 [M]). 16 KB instruction cache, 16 KB data cache, **64-byte lines**, write-back with a write buffer, **no snooping**. Single-precision FPU; doubles are software (`__extendsfdf2`/`__muldf3`…, the earlier audit counted 1,097 extend sites) | Copetti PSP article (full text), YAPSPD 4.x, Fitzhardinge's Cache HOWTO; our build 37 hardware counters [M] |
| VFPU | 128 single-precision registers as 8×(4×4) matrices, quad ops, `vmmul` 8× the FPU for 4×3 products [M, build 37 self-test: 553 µs vs 1,610 µs for 2,000 products in the emulator]. **PSP-1000 silicon bug: unaligned `ulv.q`/`usv.q` corrupt data**; use `lv.q` on 16-byte aligned memory or scalar `lv.s`. The Media Engine has **no VFPU**. **Layout [E, b106's `vfputest` probe in PPSSPP]:** `S` *m c r* is matrix *m*, column *c*, row *r*; `vhtfm4.q vd, M100, vs` gives element *r* = column *r* · (vs, 1), and with `E100` (the transposed view) it gives Σ *j* vs[*j*] × column *j* | YAPSPD VFPU chapter; pspsdk notes; our `psp_vfpu.c` |
| Main RAM | 32 MB DDR, 64-bit bus at 166 MHz (PSP-1000). Random cache-line miss ≈ 73 cycles at 333 MHz [M, halo_bench v7]; a sequential line ≈ 57 [M]. CPU reads of VRAM 132 ns vs 220 ns for RAM [M, v7] | halo_bench series; Copetti |
| Memory map (firmware, retail PSP-1000, uofw `sysmem.c` type 0) | kernel partition 1: 3 MB at `0x88000000`; partition "other2": 1 MB at `0x88300000` (**the Media Engine firmware image lives here**, `meimg.img`; mcidclan's `ME_CORE_KERNEL_ADDR 0x88300000`); **"vshell" 4 MB at `0x08400000`** — the VSH's partition, which a game borrows as *volatile memory* (`sceKernelVolatileMemLock`; our texture RAM cache); **user partition 2: 24 MB at `0x08800000`–`0x0A000000`**. 64 MB models add partitions above `0x0A000000` (24 + 4 + 4 MB), and `MEMSIZE=1` in the PARAM.SFO asks the firmware to grow the user partition into them | uofw `src/kd/sysmem/sysmem.c` (`MemInfo` table init) [D]; build 44's partition survey will confirm the free sizes on the test unit [M pending] |
| Scratchpad | 16 KB SRAM at `0x00010000`, reachable by the CPU and the GE; single-cycle, uncached. One owner at a time (a thread's stack there is a classic use). We use none of it yet | YAPSPD; Copetti |
| VRAM / eDRAM | 2 MB at `0x04000000` (uncached alias `0x44000000`), 512-bit internal bus to the GE. The GE reads **textures from VRAM at ~500 MB/s but from main RAM at ~50 MB/s**; RAM→VRAM copies ~150 MB/s (YAPSPD 11.7). Our frame: two 16-bit 512×272 buffers + 16-bit depth = 816 KB; the rest is the texture cache (1,216 KB) and display list | YAPSPD 11; `psp_ge.c` [M] |
| GE | Fixed-function: T&L, 8 KB on-chip texture cache (LRU), swizzled textures, CLUT (we use CLUT8 with 5650/8888 palettes), fog, per-vertex lighting (4 lights), BBOX conditional execution, no programmable shaders. Display lists in RAM: `GU_DIRECT` reads them as they're written (which is why every CPU write the GE will read must be written back from the data cache first) | YAPSPD; pspsdk `pspgu.h`; our builds 39/40 [M: missing cache write-backs showed as rubber-banding and flicker] |
| Media Engine ("ME") | A second Allegrex, same ISA, **FPU but no VFPU**, own 16 KB/16 KB caches, **2 MB of private eDRAM** at `0x00000000` in its own view (slim models: 4 MB), shares main RAM at the same addresses, **cannot see VRAM**, has its own view of the DMACplus/VME/AVC blocks at `0x04xxxxxx`. Normally runs Sony's firmware image for audio/video codecs and sceSas | mcidclan's repos (source read); Copetti; YAPSPD |
| Memory Stick | Random 4 KB read ≈ 1.75 ms [M, feasibility bench]; our texture streaming saw 64 MB/s sequential in the emulator (meaningless there) and 3 ms worst frame; hardware numbers come from the hardware runs. ARK-4's "Memory Stick Speedup" setting exists and is worth A/B-ing [U] | halo_bench; ARK-4 wiki |
| Display | 480×272, 60 Hz vblank; `sceDisplayWaitVblankStart` paces us; the game's own 15 fps floor (`CEILING(seconds_elapsed, 1/15)`) means below 15 fps the game slows rather than skipping ticks | `main.c`; `psp_ge.c` |
| Clocks | CPU 333 / bus 166 MHz is the top on retail firmware without kernel patches; ARK-4 can pin it. Overclocking isn't a lever here | ARK-4 wiki |

### 1.1 Cache coherence rules we now live by (Fitzhardinge, 2005; bit us in builds 39–40)

1. Nothing snoops. A write sits in a dirty line until eviction; the GE, the ME and DMA read main memory.
2. Before the GE reads anything the CPU wrote (vertices, index lists, palettes, textures, the display list in `GU_DIRECT`): `sceKernelDcacheWritebackRange` on the exact lines, or `WritebackAll` once per frame (ours: `frame end: cache write-back` ≈ measured per build, logged since build 44).
3. Before the CPU reads what another unit wrote: invalidate the range (`WritebackInvalidateRange` is the safe one). `InvalidateRange` alone discards dirty data — only on cache-line-aligned ranges you are sure are clean.
4. Memory shared across the cached/uncached views (`| 0x40000000`) must be 64-byte aligned and sized, `memalign(64, …)`, and write-invalidated once before first uncached use (a freshly allocated block can still have stale lines from its previous owner).
5. The instruction cache has no documented invalidate in that HOWTO; `sceKernelIcacheInvalidateAll` exists in pspsdk and the ME libraries use `cache 0x8` hits.

---

## 2. The Media Engine, in detail

### 2.1 How code gets onto it [D, from mcidclan's `me-core-custom.c`, `me-core.h`, `kernel/main.c`]

- The ME boots from `0xBFC00000` (in the SC's uncached kernel view, the ME's reset vector area). A kernel module copies a small handler there (`meLibReset`: `memcpy` of a `_me_section`, then `sceKernelDcacheWritebackInvalidateAll` + `IcacheInvalidateAll`).
- It is then reset by pulsing `0xBC10004C` (`HW_SYS_RESET_ENABLE`): write `0x14`, then `0`. (Earlier notes said 4; mcidclan writes `0x14`.)
- The handler enables bus clocks (`0xBC100050 = 0x0F`), sets `0xBC100040 |= 2`, clears NMI flags (`0xBC100004`), unlocks memory protection and the user hardware registers on the ME side, sets CP0 status `0x30000000` (CU0|CU1: FPU on), picks a stack top from the firmware image's witness word at `0x88300018` (**`0x80200000` for the PSP-1000's 2 MB eDRAM**, `0x80400000` for later models), and jumps to the user program's `meLibOnProcess()` at its **cached kernel alias** (`0x80000000 | address`). So the ME runs code that lives in the user ELF, at `0x88xxxxxx` addresses.
- CP0 register 22 tells a function which CPU it's on (`meLibGetCpuId`: 0 = SC, 1 = ME). `0xBC100048` is a hardware spinlock both CPUs can use.
- Suspend/resume: the kernel module replaces the firmware's `SceMeRpc` sysevent handler so the firmware stops trying to talk to an ME it no longer owns; mcidclan's `sleep-wake` sample shows the ME surviving a sleep when the program handles `meLibOnSleep/OnWake` [C].
- The SC-side helper (`kcall`) is a 1,006-type kernel PRX that lets user code run a function in kernel mode and installs that handler. ARK-4 on 6.61 loads such PRXs from a user-mode EBOOT (we already load `halo_profile.prx` this way [M]).

### 2.2 What the firmware's ME image contains, and the PSP-1000 gap [D]

mcidclan's `me-core-mapping.def.h` maps ~75 functions inside Sony's image: bus-clock control, DMACplus primitives, EDRAM alloc/free, a hardware mutex, cache ops, and — notably — `_meCoreProcessAudioMixer(AudioMixer *mixer, u32 *output, u32 config)` with a 32-channel mixer struct and reverb helpers: **that is the sceSas mixer, confirmed to be ME-resident code**. The table has two columns: the slim+ image (`t2img`) is fully mapped; **the PSP-1000 image (`img`) has about half its entries still `0x00000000`**, including the audio mixer and the EDRAM heap init. The hardware mutex, cache ops and DMAC primitives *are* mapped for the fat. Consequence: on the PSP-1000, mcidclan's "call the firmware's own ME functions" convenience is partial; a custom-core program that uses only `meLib*` functions (as our `halo_me` test does) is unaffected.

### 2.3 What taking the ME over costs [D/C]

- The firmware's ME services stop until reboot: sceSas, Atrac3/MP3 decoding, AVC video, the XMB music player. Our game today uses none (software mixer on the SC, `sceAudioOutputPannedBlocking` which is a DMA to the codec, not the ME) [M: the game plays after the custom-core samples ran, per mcidclan's notes, to be confirmed on the test unit].
- PPSSPP does **not** execute ME code; it HLEs the ME's services. Everything in this section is hardware-only. `sceIoDevctl("kemulator:", 3 /* IS_EMULATOR */, …)` detects PPSSPP so a build can skip the ME there [D, PPSSPP source].
- A hang on the ME can't be debugged from the SC; mcidclan's `exceptions` sample installs an ME exception handler that reports through shared memory.

### 2.4 The middle path: safe tasks [C, mcidclan `psp-media-engine-safe-task`]

Three methods run a custom function on the ME **without disrupting the firmware's own ME syscalls**: (1) redirect invalid syscall indices in the ME handler (needs the AVCODEC module loaded; tested on Phat), (2) hijack an H.264 syscall (slim-tested), (3) **MIST**: DMACplus writes one word into the ME's syscall table to add a custom entry, no core patching, XMB-compatible, **tested on Slim and Phat**. All three keep sleep/wake working and keep SC→ME syscalls (so sceSas) alive. The constraint: a custom task must **run to completion** — "ensure the loop is broken before any sleep/awake event or before making another syscall to the ME". So the ME can be shared between sceSas and our own bounded jobs (mix 512 frames; classify these vertices; cast these rays), serialised, but not a forever-loop.

This dissolves the audio design's "mutually exclusive" conflict (audio.md §3a): sceSas for output **and** bounded ME jobs for the game is a legitimate configuration, pending a hardware trial.

### 2.5 Constraints for any ME job in the game [D/U]

- No VFPU; FPU single precision; the C library's `sinf`/`sqrtf` and `memcpy` work (plain MIPS) — our `halo_me` test exercises exactly these.
- Inputs must be written back from the SC's cache before the job starts; the ME invalidates its own cache before reading; outputs are written back on the ME and invalidated on the SC before use. Shared control words go through uncached pointers (`| 0x40000000`) in 64-byte-aligned storage.
- Job data in main RAM competes with the SC and the GE for the one bus (halo_bench v8b: CPU misses +272% during GE vertex reads [M]). The ME's private 2 MB eDRAM is the place for a job's working set.
- VRAM is invisible to the ME: anything for the GE must be left in RAM for the SC/GE to pick up.
- The 2 MB eDRAM is also where the firmware's image expects its heap; a custom core owns it entirely; a safe task should stay inside what the firmware leaves free (unknown for the fat image) [U].

### 2.6 The two tests shipped for the morning (`halo_psp_me44.zip`)

- `PSP/GAME/halo_me`: our custom-core test. Proves start-up on the PSP-1000's table, runs a 16-voice fixed-point mix 200 times on the ME and on the SC for comparison, runs FPU maths, stresses 10 s, stops cleanly, writes `ms0:/halo/log/me_test.txt`.
- `PSP/GAME/halo_me_audio`: mcidclan's `audio-shared-buffer` sample unchanged: the ME synthesises a tone into a double buffer the SC plays. Uses the fat-mapped hardware mutex.
- Both need a reboot afterwards (firmware ME services are gone until then). A hang = hold power 10 s.

### 2.7 Results on a PSP-1000, 2026-10-03 [M]

- `halo_me`: firmware table recognised as the PSP-1000's (`meLibDefaultInit` 3); the ME counted 5,740 → 300,865 in 0.2 s. At the firmware default clock (222 MHz, bus 111; the test does not set 333): 16 voices × 512 frames 1,531 µs on the ME vs 1,552 µs on the SC, **bit-identical output**; 20,000 `sinf`/`sqrtf` 27,990 vs 28,031 µs, identical results; 10 s stress with the ME mixing flat out (6,480 blocks) while the SC spun freely; stopped cleanly on request.
- `halo_me_audio` (mcidclan's sample): the three generated sounds played correctly. **Pressing HOME crashed the PSP**: the sample never stops its ME loop, so when the XMB reloads into the memory the ME is executing from, the ME runs garbage. Rule for the game: stop the ME (and wait for its acknowledgement) before any exit path — HOME, quit, and the suspend power callback.
- For comparison, the game's own SC mixer on hardware: 1.6 ms per 23 ms block at 6.4 voices (-O2 build), 2.8 ms with -Os; worst block 6.8 ms. That is the time an ME mixer would hand back.

---

## 3. sceSas, the firmware mixer [D; costs measured by PPSSPP's authors on hardware]

- 32 voices per core, one core in practice; VAG (PS-ADPCM), 16-bit PCM and ATRAC3 sources; 44.1 kHz output only; grain 64–2,048 samples (256 typical); pitch `0x1000` = unity, `0x4000` max (so a 22.05 kHz source at natural pitch is `0x800`); per-voice L/R and effect-send volumes; ADSR; one global reverb; end flags refresh only after `__sceSasCore` returns. Needs `PSP_MODULE_AV_AVCODEC` then `PSP_MODULE_AV_SASCORE` loaded; the `SceSasCore` struct is 3,616 bytes, 64-byte aligned.
- ME time per grain (PPSSPP `SasAudio.cpp`, fitted to pspautotests at 222 MHz): 110 µs + 0.49 µs/sample + per voice-sample 0.445 µs + 0.0675 × pitch ratio. 32 VAG voices × 512 samples = 8.7 ms per 11.6 ms grain at 222 MHz; ≈ 48% of the ME at 333 MHz with 256-sample grains.
- Our software mixer on the SC: 724 µs per 23 ms block for 3.7 voices on Blood Gulch, 1,486 µs for 8.2 voices (15 peak) on b30's beach, in the emulator [M]; ~3–6% of the CPU. Hardware figures are in the b40–b43 hardware logs (never late, never clipped [M]).
- To use sceSas the Xbox ADPCM must be baked to VAG (audio.md §3b has the encoder and quality numbers), and long sounds need the ring-buffer trick (validated in PPSSPP only).

---

## 4. The kernel side on 6.61 ARK-4 [D/M]

- A user-mode EBOOT can `pspSdkLoadStartModule` a kernel PRX (`halo_profile.prx` reads the CPU's hardware counters and, since build 44, the partition sizes) [M, builds 37–44]. In current PPSSPP the module loads and runs, but every counter reads 0: the numbers are hardware-only.
- **Calling a kernel PRX from the game: a syscall export [M, build 46, emulator; hardware pending].** The PRX lists the function in a library with attribute `0x4001` in its `exports.exp` (`PSP_EXPORT_START(halo_profile, 0, 0x4001)`; the pattern mcidclan's mecore uses for `kcall`); the EBOOT links an import stub for it. The PRX is loaded *after* the EBOOT starts, and the firmware links the EBOOT's import then, because the stub's flags (`0x40090000`) mark it a weak import. **Trap [M]: generate the stub with `psp-build-exports -k` (the "new" format), not `-s`.** The old format puts the library entry (`.lib.stub`) in a section nothing refers to, and a link with `--gc-sections` (ours) silently drops it: the function's stub stays, the import table doesn't list the library, and the call can never be linked. The new format makes the function stub refer to the library entry, so both survive (the SDK's own libraries are built this way). Check: the library's name appears in the PRX's strings.
- **ARK-4's "Memory Stick Speedup" is a 4 KB read-ahead buffer, nothing more [D, ARK-4 `core/systemctrl/src/msstor_cache.c`, read in full].** It hooks the Memory Stick's block driver (`msstor0p`, below the FAT driver): a read of 4 KB or less fills one 4 KB buffer from that position and later small reads inside it are copied from there; a larger read goes straight through; any open, write, directory read, stat or devctl empties it. So it can help small scattered reads (the FAT's own sector reads, small files) and does nothing for 64 KB texture reads. ARK enables it at boot (`mscache` in its settings; `se_config.msspeed`). A program can read ARK's settings from user mode with `sctrlSEGetConfigEx` (SystemCtrlForUser, NID `0x8E426F09`; it copies only when the structure size matches ARK's): halo_bench v10 logs it.
- `sctrlKernelSetUserLevel(8)` and the rest of ARK's `systemctrl` are available for a user program that needs kernel access without a PRX.
- The firmware's `sceKernelPartitionTotalFreeMemSize/MaxFreeMemSize` answer the "what's left" question per partition; build 44 logs them once at start-up under `hwprofile`. **Trap [M, a PSP-1000, 2026-10-03]: those two calls do not check that the partition exists** (uofw `partition.c`: `MpidToCB(mpid)->firstCtlBlk` with no null test). Partitions that are empty on a fat (7 = ME user, 8 = the slim-only extended kernel) dereference a null control block: bus error inside sysmem (epc `0x880044ac`, just before uofw's label `44BC`), at module start. Ask `sceKernelQueryMemoryPartitionInfo` first (it checks) and skip ids it refuses or reports as size 0.
- ARK-4 settings that matter: CPU clock pinning (and the Speedup above, for small reads only). The ARK-4 wiki documents each.
- **Coprocessor 0 register 24, bit 0 [D, uofw `reboot/hw.c`]:** the firmware's test for the 04g+ boards' "L2" cache before it touches `0xA7F00000`. Build 46 logs it once under `hwprofile`; expected clear on a PSP-1000.
- **Hardware counters by phase [build 46]:** `halo_profile_phase(n)` (the syscall above) charges the counters since the last change to the phase that was running (0 other, 1 game tick, 2 drawing, 3 frame end), with interrupts off while it reads them; the sampler thread does the same every 100 ms so a long phase (a map load) survives the 13 s wrap of the cycle counter. The counters see the whole CPU: the sound thread's work lands in whatever phase it interrupted.
- **Threads [D/E, b106].** The scheduler runs the highest-priority ready thread and never shares the CPU between equals (halo_bench v10 [M]). pspdev's pthreads gives its threads low priorities: the game's cache file thread came out at 0x3C and another at 0x5C, against the game thread's 0x20 [E, b106's `threads (...)` line]. So a pthread doing I/O waits for the game thread to block after each read: b105 run 1's cache file thread took **87 ms a call** for reads of about 30 KB [M]. b106 raises it to 0x1C. **And pspdev's pthreads polls condition variables**: `pthread_cond_wait` goes to `pte_osSemaphoreCancellablePend`, which looks at the semaphore and calls `sceKernelDelayThread(100)` in a loop [D, the linked library's code]. A waiting thread wakes up to 10,000 times a second (4,000 counted [E]). That costs nothing below a busy game thread and a lot above it, so b106 replaces `pthread_cond_*` with a waiter count and a kernel semaphore (`psp_libc.c`). Mutexes are not affected: `pthread_mutex_lock` waits in `sceKernelWaitSema`.

---

## 5. Emulator versus hardware, so far [M]

| | PPSSPP (this machine, JIT) | PSP-1000 |
|---|---|---|
| Blood Gulch frame, on foot | ≈ 8–10 ms | 23–38 ms (b42 heartbeats); 40–66 ms driving |
| Blood Gulch tick, on foot | 1.8 ms (b44) | 7.4–8.1 ms (b42 heartbeats); 14–16 ms driving |
| VFPU 4×3 product | 553 µs / 2,000 | not measured in isolation |
| What PPSSPP can't tell you | ME code, cache misses, Memory Stick latency, bus contention, kernel partitions, the `ulv.q` bug |

Rule of thumb: the frame runs **2.5–4×** slower on hardware, the tick **≈4×** (b42 on foot: 7.4–8.1 ms against 1.8). The tick is pointer-chasing, cache-miss-bound code, and PPSSPP's JIT models no cache at all, which is why the tick ratio is the worse one. Use the emulator for *where* the time goes; use the hardware logs for *how much*.

---

## 6. How the levers land on the game (pointers)

| Lever | Hardware fact it rests on | Where it's written up |
|---|---|---|
| Write-backs only when data changed (HUD text, palettes, index lists) | §1.1 | builds 39–44, `psp_ge.c`, `psp_tex.c` |
| VRAM-resident textures by bind frequency | GE texture bandwidth 10× from VRAM | build 44 probe `textures: most bound…` → next pass |
| Sleep for idle objects | the CPU budget, not a hardware feature | engine_performance.md §2.2, builds 19–44 |
| Collision BSP in cache-friendly form; top levels in scratchpad | 16 KB D-cache, 73-cycle misses, 16 KB scratchpad | campaign_recon_b30.md (the shared hot path) |
| Line-of-sight rays and ground probes on the ME | §2.4–2.5 (read-only inputs, small outputs, bounded) | campaign_recon_b30.md, feasibility doc |
| sceSas output with VAG bake | §3, audio.md | audio.md |
| 64 MB recon builds | §1 memory map (`MEMSIZE=1`) | campaign_recon_b30.md |

---

### halo_bench v9 results, a PSP-1000, 2026-10-03 [M]

Clock measured 329.6 MHz (333/166 set). Source: `/home/claude/halo_bench9/main.c`; raw output `result_psp1000.txt` beside it.

| Test | Result | Reading |
|---|---|---|
| Random-line miss, used at once | 222 ns over loop overhead (≈ 74 cycles) | matches v7's ~73 cycles |
| Miss, then 32 cycles of unrelated work, then use | 285 ns net vs 317 ns if serial | the CPU stalls almost at once: only ~10 cycles of work slip under a miss. Reordering code alone hides little |
| `cache 30` (Fill), then 32 / 64 cycles of work, then the load | 240 / 239 ns net vs 317 / 414 serial | **Fill is a real, non-blocking prefetch**: 64 cycles of work disappear entirely under it, then the load hits |
| Fill alone, back to back | 252 ns each (= a miss) | **only one miss in flight**: a second fill waits for the first |
| Fill 1, 2 or 4 steps ahead with 32 cycles of work a step | 372–375 ns vs 317 without | worse: with less work than a miss between them, the next fill stalls on the previous one. Prefetch pays only with ≥ ~70 cycles of independent work between the fill and the use |
| Writing 1 MB in whole lines, plain vs `cache 24` first | 6,067 vs 3,116 µs, no wrong words | **create-dirty-exclusive halves the cost of whole-line writes** (no read-for-ownership) |
| RAM→VRAM copy, 512 KB: memcpy uncached / memcpy cached + write-back / `sceDmacMemcpy` | 4,880 / 4,148 / 1,580 µs | **DMA is 2.6–3× faster** (≈ 330 MB/s); `sceDmacTryMemcpy` also returned only when done (1.76 ms), so both block the caller |
| Dependent loads: scratchpad / cached hit / miss | 24.3 / 24.2 / 215.6 ns | the scratchpad is exactly as fast as a cache hit |
| The same while the GE textures from RAM | 25.9 / 24.4 / **424.4** ns | **misses double (+97%) when the GE pulls textures from RAM**; hits and the scratchpad are unaffected |

What it means: the scratchpad is a 16 KB region that always hits and costs the data cache nothing ("useless" only in that it is no faster than a hit). VRAM placement of hot textures matters to the CPU, not just the GE: every miss the tick takes while the GE reads RAM costs twice as much. Prefetch suits loops with a few hundred cycles of work per element (the per-object update and render loops), not the tight BSP walk.

## 8. Copetti's bibliography, chased (2026-10-03)

Resolved from the live page's Sources list; read in full where reachable. GitHub-hosted mirrors were used where the original host is blocked from the build machine.

| Source (Copetti key) | Status | What it adds |
|---|---|---|
| TyRaNiD, *The Naked PSP* (2006 slides, `cpu-naked`) | read in full (upspd gh-pages mirror, `docs/software/naked_psp.pdf`) | The "scratchpad (pretty useless)" remark is a bare aside with no reasoning or measurement. Also: the GE "can operate in a bus master mode to pull in textures/lists from main RAM"; display lists must be uncached or written back; a **general-purpose DMA channel for user applications** exists (pspsdk `sceDmacMemcpy`); threads are cooperative; the CPU has an on-board profiler (the counters our `halo_profile.prx` reads) |
| groepaz/hitmen, YAPSPD (`cpu-coherency` = §4.10.2, `cpu-overview` = ch. 2, `graphics-overview` = ch. 11) | read: the 2006 PDF in full (upspd `docs/hardware/psp_doc.pdf`, 250 pp.) plus the online chapters via the psp-doc plugin | §4.10 is Fitzhardinge's cache HOWTO (already applied). Ch. 2 bus diagram: one main system bus joining DDR, the GE (166 MHz, 2 MB eDRAM), the CPU and the ME group; a separate peripheral bus. Ch. 11.7: texture reads 50 MB/s from RAM vs ~500 MB/s from VRAM |
| Alex Marshall, PSPTEK (`cpu-marshall`) | read: overview, memory map, memory controller, profiler and CPU sections in full (upspd `docs/hardware/PSPTEK.htm`); register chapters skimmed | Profiler counters include **CNTBUSACCESS** and **CNTLOCALBUS** (GE local-bus cycles) — a way to attribute bus contention; profiler control bit 9 makes it readable from user mode. A "shared RAM" window at physical `0x1FC00000` visible to both CPUs (the ME reset-vector area mcidclan uses); PSPTEK says 2 MB, psdevwiki's Hardware Registers page says the RAM there is 4 KB (see below) |
| PPSSPP `MediaEngine.cpp`, `SasAudio.cpp` | read earlier (clone) | as §3 |
| Bryan Fletcher, *Programming the PSP*, GDC 2004 (`cpu-programming`) | **not read yet**: GDC Vault has audio only (`media.gdcvault.com/gdc04/audio/prog_playstation.mp3`, 55 min, tape-recorder quality); a transcript is in progress | pending |
| psdevwiki "Tachyon", "Modules" (`cpu-tachyon`, `cpu-modules`), plus "Baryon" | read in full (saved copies, 2026-10-03; Tachyon page last edited 2026-01-20) | Tachyon: the address map is shared by the SC, the GE and the ME **except VRAM (SC and GE only)**; `0x1FC00000`–`0x1FDFFFFF` is the reset-vector region; PSP-1000 = discrete DDR chip, 32 MB, 2 MB ME eDRAM (slims: DDR in-package, 64 MB, 4 MB eDRAM); repeats the `ulv.q` bug. The linked "Allegrex", "Media Engine", "Graphics Engine" and "Hardware Registers" pages were saved too (next row). Modules: the firmware module list (`avcodec.prx` "decoding system for ME", `dmacman.prx`, ...) — reference only. Baryon is the power/system controller (SPI): nothing for performance |
| psdevwiki "Allegrex", "Media Engine", "Graphics Engine", "Hardware Registers" | read in full (saved copies, 2026-10-03) | **Allegrex:** based on the MIPS32 4K core; **the scratchpad "is accessed directly without going through the system bus"**; the system bus runs at **half the CPU clock** (166 MHz at 333). PSP calling convention passes 8 arguments in registers ($a0–$a3, $t0–$t3). **Media Engine:** same clock and instruction set as the SC plus `mfvme`/`mtvme` (VME access, reusing the LDL/SDL opcodes); its firmware program loads at `0x08300000`–`0x0837FFFF` (512 KB); ME-side ping-pong buffers (993 KB at `0x04000000` in its view) and two DMA engines. **Graphics Engine:** 1–166 MHz (111 by default), 256-bit bus, 5.3 GB/s, 664 Mpixel/s; command list only, no bus-arbitration detail. **Hardware Registers:** CPU and bus frequency ratio registers at `0xBC200000/4`; the profiler (same counters as PSPTEK; user access through a mask of 0x300 in the register-enable word); DMACplus at `0xBC800000`, DMAC at `0xBC900000`/`0xBCA00000`; **the reset-vector RAM at `0xBFC00000` is 4 KB** (the boot scratch RAM remapped after reset), not the 2 MB window PSPTEK lists — the space mcidclan's ME handler and shared variables live in; an "L2 cache" control at `0xA7F00000` exists only on 04g+ boards (PSP-3000 and later), not the PSP-1000 |
| MIPS32 4K Software User's Manual rev 01.18 (`cpu-mips_4k`) | **not read**: no copy found either (the citation has no link); upspd mirrors MIPS32 PRA, MIPS32 IS and R4000 UM instead | low priority: PPSSPP notes the Allegrex's cache op codes "don't match regular MIPS cache codes very well" |
| pspdev GCC 4.9.3 patch (`cpu-gcc_patch`) | superseded: read the current `allegrex-v15.2.0` branch of pspdev/gcc instead | see below |
| Foremski, CHM interview, PCMag, edepot, NIL remote port, SilverSpring ICs, IdStorage, IPL pages | not performance-relevant; SilverSpring's chip list is an image | — |

### What changes our plan [D unless marked]

1. **The Allegrex has a software prefetch: `cache 30` ("Fill").** PPSSPP's interpreter notes it is used "a lot" by GTA: Liberty City Stories (and by Tony Hawk's Underground 2). There is no MIPS `pref` instruction (`__builtin_prefetch` compiles to nothing), but pspdev GCC exposes `__builtin_allegrex_cache(30, address)`. For pointer-chasing code (the collision BSP walk) a fill issued one step ahead could overlap the next miss with the current step's work — **if** the fill does not stall the pipeline until the line arrives [U: needs a hardware test].
2. **`cache 24` ("Create Dirty Exclusive")** allocates a line for writing without first reading it from RAM — for buffers that are written whole (display lists, vertex copies, mixer output, `memset`). Lines not fully written hold garbage [D: PPSSPP].
3. **GCC has no Allegrex pipeline model** (`DEFAULT_COSTS`, no `define_insn_reservation` in `allegrex.md`): instruction scheduling does not model load latency for this core. Hand-ordering loads early in hot loops is not something the compiler will do for us.
4. **A user DMA channel** (`sceDmacMemcpy`) can copy without the CPU — a candidate for texture uploads into VRAM.
5. **The scratchpad's reputation rests on one unexplained slide, and psdevwiki gives a reason to doubt it:** it sits beside the CPU, off the system bus. A cached hit is as fast, but a scratchpad read never waits for the GE's bus mastering, and never evicts anything from the 16 KB data cache. For hot, small, read-mostly tables (the top of the collision BSP, the tag-instance index) during GE-heavy frames, that is the property we want. It needs measuring (hit vs miss vs scratchpad, with and without the GE busy) before being used or dismissed.

All five go into one hardware micro-benchmark (halo_bench v9) before any engine change depends on them.

## 7. Sources (all read in full; clones under `/home/claude/research/src/`)

- Rodrigo Copetti, *PlayStation Portable architecture* — `Architecture-of-consoles/articles/playstation-portable.Rmd.md` (786 lines) plus the live bibliography.
- YAPSPD (Yet Another PSP Documentation) via the `psp-doc-claude-plugin` skills: CPU, cache, VFPU, GE, memory chapters.
- Jeremy Fitzhardinge, *PSP Cache HOWTO* (2005), in the plugin's `cache` skill.
- uofw (`src/kd/sysmem/sysmem.c`): the retail memory-partition table.
- upspd wiki: register names and the ME/DMACplus pages.
- mcidclan (m-c/d): `psp-media-engine-custom-core` (built and installed; MIT), `psp-media-engine-safe-task` + `MIST.md`, `psp-media-engine-reload`, `me-feed-graphics-engine`, `me-tiny-lib`, `me-spinlock`, `me-dmacplus-transfer`, `me-soft-interrupt`, `me-minimal-handler`, `psp-media-engine-cracking-the-unknown` (the ME syscall table dump).
- Daedalus (`Source/SysPSP/PRX/MediaEngine`, `HLEAudio/Plugin/PSP/AudioPluginPSP.cpp`): a shipped game running its audio ucode on the ME through a PRX, with the same uncached-buffer pattern.
- PPSSPP (`Core/HW/SasAudio.cpp`, `Core/HLE/sceSas.cpp`, `Core/System.cpp`, `Core/MemMap.cpp`): the sceSas cost model and the 32/64 MB memory logic.
- ARK-4 and its wiki: settings and kernel services on 6.61.
- Our own runs: halo_bench v7/v8 (cache, VRAM, bus contention), builds 18–44's hardware and emulator logs.

### halo_bench v10 results, a PSP-1000, 2026-10-03 [M] (333 MHz, bus 166; `halo_bench10.txt`)

**Memory Stick** (reading `bloodgulch.map`, 41 MB; each test on a part of the file not read before):

| Request size | Throughput | Time a call |
|---|---|---|
| 512 B | 5.2 MB/s | 96 µs |
| 4 KB | 8.5 MB/s | 473 µs |
| 16 KB | 13.7 MB/s | 1.16 ms |
| 64 KB | 15.7 MB/s | 4.1 ms |
| 256 KB – 1 MB | 16.3 MB/s | 15.7 / 62.6 ms |

- A random read costs about 1.6–2 ms more than a sequential one (4 KB anywhere: 2.1 ms; 64 KB anywhere: 6.0 ms).
- **`sceIoReadAsync` leaves the CPU free**: with the file's async priority set above the game thread's (0x10 against 0x20), a 1 MB read finished in 65 ms (as fast as a blocking read) while the CPU kept 97% of its speed. The transfer is DMA; the CPU only handles completions.
- **At the default async priority, the read never completes while the calling thread stays busy** (3 s without completing, 98% CPU to the caller); it completed 20 µs after the thread blocked. The PSP's scheduler doesn't time-slice equal priorities, so the completion handling waits for the busy thread. Rule: any streaming the game does runs at an I/O priority above the game thread's.
- The CPU's cache misses while the Memory Stick's data arrives: 219.5 → 229.6 ns (+5%). Card DMA barely competes for the bus.
- ARK-4's settings weren't readable through `sctrlSEGetConfigEx` (the test unit's ARK-4 build's structure size differs from the source read); irrelevant for 64 KB reads anyway (§4).

**Media Engine** (mecore):
- **Round trip** (the CPU writes a sequence number to uncached shared memory, the ME answers): **1.16 µs** (~390 cycles).
- **Handing over a block** (CPU write-back of its cache lines; ME invalidate, read, answer): 256 B 2.8 + 9.3 µs; 4 KB 7.3 + 39.6 µs; 16 KB 22.3 + 140 µs; 64 KB 37 + 531 µs. The ME's read of fresh data dominates (~8 µs a KB, ~120 MB/s); the CPU's write-back is cheap. All sums correct with the protocol.
- **The hazard, shown:** without the ME's invalidate, the ME summed its old cached copy (`0007fe00` against the right `0017fe00`). Coherence by hand is not optional.
- **The ME's cache miss costs the same as the CPU's**: 218.9 ns against 215.7 ns (4 MB, random lines).
- **Contention: the ME chasing its own 4 MB at full rate (4.9 M misses a second) left the CPU's misses unchanged** (211.8 ns against 215.7 alone). With one miss in flight each, the two CPUs don't slow each other: the bus isn't the limit for latency-bound work. Offloading pointer-chasing work (collision walks, raycasts) to the ME roughly doubles the misses the machine can serve.
- **Invalid, a bench bug:** the "ME streams 1 MB" rows (and "ME streaming MB/s") are meaningless. The ME's sum was unused, so the compiler removed it and the ME only counted. Bandwidth contention (sequential streaming on both CPUs) remains unmeasured.

## 9. Open research items

### 9.1 A hardware bus-priority (arbiter) setting [U] — flagged 2026-10-07

**Question.** Does the PSP-1000's main-bus arbiter have a priority setting (a register, or a GE/DMA bus-master mode) that would favour the CPU's cache-line fills over the GE's bus-master reads?

**Why it matters.** b102's benchrun on a PSP-1000 [M, `b102_run5`] measured the GE's main-RAM reads costing the CPU 8–18 ms of a 29–69 ms frame (the "GE busy on VRAM only" switch: memory wait 56% → 40%, cycles per D-miss 358 → 192). Texel reads are not the cause (stand-in texture, stand-in lightmaps and one-level-smaller textures all within ±1 ms); vertex, index and display-list reads are the likely bulk [U, by subtraction of the "GE skips models/level" rows]. Per-phase counters (since b46) show no quiet phase: tick 47%, drawing 53%, frame end 34% memory wait. So scheduling the GE around CPU phases can't hide it; reducing the GE's main-RAM bytes, or changing the arbiter's priority, can.

**What we know.** Sony documents nothing. Leads to read in full: PSPTEK's memory-controller chapter (`docs/hardware/PSPTEK.htm`, upspd: only the overview was read for §8); psdevwiki "Hardware Registers" (the `0xBC100000` system-control block: bus clock enables `0xBC100050`, the `0xBC100040` bit mcidclan's ME handler sets); uofw's `sysreg` and `ge` drivers for any write to a bus-control register; PPSSPP's register stubs (it emulates no arbiter, so at best names). The profiler's **CNTBUSACCESS** and **CNTLOCALBUS** counters (PSPTEK) could attribute bus cycles to the GE directly and are worth reading regardless.

**How to settle it.** Find a candidate register in those sources; if one exists, a halo_bench test: the CPU's dependent-miss latency (v9's "424 ns while the GE textures from RAM") with the register at each value. No engine change depends on this until a measurement does.

**Searched, 2026-10-07 (b103 day) [D].** No source we have documents a bus arbiter or a per-master priority:
- **PSPTEK** (upspd `PSPTEK.htm`, searched whole for arbitration, priority, bus master, memory controller): nothing. Its register chapters cover NAND, GE list control and KIRK; no DDR controller chapter.
- **psdevwiki "Hardware Registers"** (a saved copy): the DDR controller is at **`0xBD000000`**. Only `0xBD000004` (flush) is named; `0x20`, `0x24`, `0x2C`, `0x38` are "unknown" (some used by the IPL), and `0x30`, `0x34`, `0x40`, `0x44` are "used during `sceDdrChangePllClock()`". `0xBC100050` is per-device **bus clock enables** (ME, AW RegA/RegB/EDRAM buses, DMACplus, DMAC, …): on/off, no priority. `0xBC000040/44` are hardware-register user-access enables, not priority.
- **uofw**: the GE driver (`src/kd/ge/ge.c`) never writes a priority. Its unknown registers are `0xBD400004` (read and saved on suspend, passed to the interrupt handler, never written), `0xBD400104`, `0xBD400300` (never used), and the EDRAM block `0xBD500040/50/60/90/A0` (EDRAM refresh/init; `0x40` is set to 1 or 3 by `sceGeEdramSetRefreshParam`). The DDR driver lives in `lowio.prx`, and uofw hasn't reversed it: its exports are NIDs only.
- **Function names that could be it** (ARK-4's `PSPLibDoc_1.50.xml`): **`sceDdrWriteMaxAllocate`** (sceDdr_driver: the name suggests a limit on the DDR controller's write allocation, which sounds like a queueing/arbitration knob [U]) and **`sceSysregSetMasterPriv`** (sysreg: probably a bus master's *access privilege*, i.e. memory protection, not priority [U]). Neither turns up anywhere on the web.
- The web (searched for both names and for PSP bus arbitration): nothing.

**Next steps, in order of risk:**
1. **Read-only probe** (kernel mode through mecore's `kcall`, which the game already loads with the ME): log the documented-but-unknown registers once at start-up: DDR `0xBD000000`–`0x44`, GE `0xBD400004/104/200/300`, EDRAM `0xBD500000`–`0xA0`, `0xBC100040`–`0x5C`. Values that differ between "GE idle" and "GE busy", or that look like small fields, are the candidates. Harmless, since only addresses psdevwiki lists are read.
2. **Disassemble `sceDdrWriteMaxAllocate`** from the 6.61 `flash0:/kd/lowio.prx` (copied to the stick with ARK-4's file manager; it's Sony's code, so it stays on our machines, unshared). That shows which DDR register it writes, and with what.
3. **Only then a write test** in halo_bench, not the game: v9's dependent-miss latency with the GE texturing from RAM, before and after the write. A wrong value could hang the bus until power-off, so it needs a test that is safe to lose.

Meanwhile the levers that don't need it are clear from b102 run 5: fewer GE main-RAM fetches (b103's "level vertices in VRAM" switch measures it), and the scratchpad for the CPU's hot read-mostly tables (v9: unaffected by the GE).

**Step 1 done, b105 run 1 [M].** The probe read the registers with the GE idle and busy. The DDR controller's are identical in both (`bd000020=00008020 24=00020220 2c=80000004 30=11110410 34=00000007 38=0 40=0 44=0000000a`). `bd400004/104/300` go from 0 to an address inside our display list (the GE's list pointers), and `bc10004c`/`bc100050` change the way the Media Engine starting would (a reset released, a bus clock on) [U]. Nothing moved with the GE's load the way a priority setting would. `bd000030 = 11110410` is the only value shaped like per-master fields [U]. Step 2 (disassembling `sceDdrWriteMaxAllocate`) is next.
