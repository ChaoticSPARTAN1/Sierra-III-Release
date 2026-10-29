# Sierra-III b148: release notes

**Sierra-III b148** is Halo: Combat Evolved's campaign on the PSP. It is built for the PSP-1000, and the PSP-2000 and 3000 work too. It contains nothing from the game: you bake the maps yourself, from your own Xbox disc, on your own PC. The full source of this release is in this repository.

**How to play:** follow [`docs/play.md`](docs/play.md) step by step. It covers custom firmware, imaging your disc, the bake, the Memory Stick, the first launch, and what to send when something goes wrong. The short version is at its top.

**The files:**

| File | What |
|---|---|
| `halo_psp_b148.zip` | The game. It goes to the bake tool, which puts it on the Memory Stick's layout |
| `halo-psp-bake-windows-b148.zip` | The bake tool for Windows 10 or 11 |
| `halo-psp-bake-linux-b148.tar.gz` | The bake tool for Linux x86-64 with **glibc 2.39 or later** (Ubuntu 24.04, Debian 13, Fedora 40, current Arch) |

Each file's SHA-256 is beside it on the release page. There is no macOS bake tool with b148.

**Requirements:**
- a PSP with custom firmware (it's tested on ARK-4 on 6.61);
- a Memory Stick with 3.5 GB free;
- your own disc image of Halo for the original Xbox. It's tested against NTSC version 01.10.12.2276: the bake checks every map and refuses other versions and languages rather than guess;
- a PC to bake on, with about 8 GB free.

**Baking again is optional** if you baked with b147h's tool: b148 plays that bake as it is. Bake again with b148's tool to see Master Chief's armour as b148 draws it. If you baked with b143's tool, you must bake again: the game stops and asks for a new bake if it finds those maps.

**A second bake is faster.** The bake now keeps its reusable work (texture palettes and converted sounds, about 420 MB) in your user cache folder, and a bake that finds it takes about a third of the time. The guided bake asks the first time. Deleting the folder is always safe.

**What's new since b147h:**

*Restored to look as on the Xbox:*
- Master Chief's armour: its reflection where the Xbox draws it, and the armour's detail map over it, with sharper textures. It was a flat green before.

*Fixed:*
- Chief's armour no longer flashes white for a frame in cutscenes and play.
- A sleep during play could crash the PSP and corrupt its system settings. Until that's made safe, the power switch does nothing while the game runs (below).

*Also new:*
- The PSP's menu plays "A Walk in the Woods" from your disc as Sierra-III's entry sound, looping cleanly.
- Re-bakes are about three times faster (above).

**What it is:** the whole campaign, all ten levels, on a PSP-1000 at its standard 333 MHz. It doesn't run at full speed yet. The figures below were measured on a PSP-1000 just before b143, over the second half of each level's first two minutes. b148 measures the same or faster.

| Level | Frames a second (about) |
|---|---|
| *The Maw* | 30 |
| *Assault on the Control Room* | 29 |
| *Two Betrayals* | 26 |
| *The Pillar of Autumn* | 20 |
| *343 Guilty Spark* | 17 |
| *Truth and Reconciliation* | 16 |
| *Keyes* | 14 |
| *Halo* | 13 |
| *The Library* | 13 |
| *The Silent Cartographer* | 6 |

**Multiplayer isn't part of this release.**

**The performance readout** (frame rate and timings) is off. Turn it on with `overlay` in the launch menu (hold L as the game starts), or from the in-game debug menu.

**Known issues:**

- **Speed:** see above. The heaviest stretches are slow, and the slowest levels play in slow motion.
- **The boarding pods** in *The Pillar of Autumn* can lose their textures in places and draw purple and pink.
- **Some animations look a little off**, and a few are cut short.
- **Cutscene audio** can drift from its picture.
- **Not yet like the Xbox:**
  - the Pelican's canopy glass is lighter than on the PC, and its side panes stay dim;
  - there's a slight missing polygon at *The Silent Cartographer*'s far horizon;
  - some lights on world geometry are missing.
- **Sleep:** the power switch doesn't sleep the PSP while the game runs. Quit to the XMB first, then sleep it there (`docs/play.md`). Holding the switch for about 8 seconds still turns the PSP off.

**Build it yourself:** the EBOOT's SHA-256 starts `d7aebb7710abde6b`. On Linux with the pspdev toolchain (`docs/toolchain.md`):

```
PSPDEV=/opt/pspdev LABEL=b148 \
PSP_METRICS=0 PSP_PGO=use PSP_PGO_PROFILE=campaign-w148d PSP_PGO_STRICT=1 \
PSP_PGO_EXTRA=-fno-unroll-loops,-fno-peel-loops,-fno-tracer,-fno-ipa-cp-clone \
PSP_TEXT_LAYOUT=w142a_tgc90 tools/build_release.sh <output folder>
```

**Licences and credits:** Sierra-III's own code, scripts and documents are under the MIT licence (`LICENSE`). The end of `LICENSE` lists what MIT doesn't cover:
- the XMB wallpaper and the project's own look in `psp/xmb/`;
- our patches to GPL programs;
- third-party files carried as they are.

The engine, the bake's tools and the ideas borrowed from other ports are credited in `UPSTREAM.md` and the README.

Sierra-III was created under Microsoft's "[Game Content Usage Rules](https://www.xbox.com/en-US/developers/rules)" using assets from Halo: Combat Evolved, and it is not endorsed by or affiliated with Microsoft. Halo: Combat Evolved © Microsoft Corporation.

Free, and never for sale. Don't share your baked maps or your disc image.
