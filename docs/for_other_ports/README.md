# For other ports of the decomp

Sierra-III runs Halo: Combat Evolved's campaign on a PSP-1000 (333 MHz MIPS, 24 MB of user RAM, 16 KB caches), built on ChupathingyCE. These pages are for the people porting the same decomp to other small machines: Halo 64 (N64), the Vita port (halo-ce-vita), and the Dreamcast, 3DS and Android efforts. They collect what we found that you may want, without making you read our build notes.

| Page | What's in it |
|---|---|
| [`game_bugs.md`](game_bugs.md) | Bugs in the original game or the decomp that every port inherits, and the ones that bite only off x86: symptom, cause, our fix, where it is |
| [`portable_speedups.md`](portable_speedups.md) | Speed-ups and memory savings that don't depend on PSP hardware, ranked for an N64, Dreamcast or 3DS-class target, with what each measured and how exact it is |
| [`psp_architecture.md`](psp_architecture.md) | A map of the PSP-specific work (the renderer, the Media Engine, VRAM, memory, the bake), and which ideas might carry to another console |

**How to read the evidence.** Every figure says where it came from: **[M]** measured on a PSP, **[E]** our emulator (PPSSPP with a cache model), **[D]** read in the code or data, **[U]** our inference. A figure names its build (`bNNN`) and its level (`a10` … `d40`, the campaign's map names; `b30` is The Silent Cartographer). Each item points to the doc, commit or `psp_fixups.py` group that holds the detail.

**Where the sources are.** A bare doc name (`review_b138.md`) is in `docs/`; research notes are in `docs/research/`; other paths are from the repository's root. Most of the docs cited are development notes that stay with the development repository, and run numbers refer to our own test runs: neither is public.

**Where the code is.** We never edit the engine. Every engine change is an anchored text replacement in `psp_fixups.py`, applied to a copy of ChupathingyCE at build time (`psp_architecture.md` §1). So "FIXES_126" means a group in that file, and each group's comment says what the change does and why. Nearly every change also has a keep word that turns it off at launch.

**Borrowed work.** Several items here came from other ports first, and say so: halo-ce-vita by BirchWoodGod (used with his permission; `docs/research/research_vita.md`), Halo 64 by JackTulli (CC0; `docs/research/research_halo64.md`) and DamnationCE (CC0). The README's Credits lists them. If you take something from here that came from them, credit them, not us.

**What isn't here.** No game data: no maps, tags, textures, sounds or strings from the disc. Our maps are baked from each player's own disc (`bake/README.md`).

**How this was made.** Like the rest of Sierra-III, these pages were written with Claude; the maintainer reviews them before they are published (README, "How it's made").
