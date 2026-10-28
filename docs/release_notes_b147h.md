# Sierra-III b147h: release notes

**Sierra-III b147h** is Halo: Combat Evolved's campaign on the PSP. It is built for the PSP-1000, and the PSP-2000 and 3000 work too. It contains nothing from the game: you bake the maps yourself, from your own Xbox disc, on your own PC. The full source of this release is in this repository.

**How to play:** follow [`docs/play.md`](docs/play.md) step by step. It covers custom firmware, imaging your disc, the bake, the Memory Stick, the first launch, and what to send when something goes wrong. The short version is at its top.

**The files:**

| File | What |
|---|---|
| `halo_psp_b147h.zip` | The game. It goes to the bake tool, which puts it on the Memory Stick's layout |
| `halo-psp-bake-windows-b147h.zip` | The bake tool for Windows 10 or 11 |
| `halo-psp-bake-linux-b147h.tar.gz` | The bake tool for Linux x86-64 with **glibc 2.39 or later** (Ubuntu 24.04, Debian 13, Fedora 40, current Arch) |

Each file's SHA-256 is beside it on the release page. There is no macOS bake tool with b147h.

**Requirements:**
- a PSP with custom firmware (it's tested on ARK-4 on 6.61);
- a Memory Stick with 3.5 GB free;
- your own disc image of Halo for the original Xbox. It's tested against NTSC version 01.10.12.2276: the bake checks every map and refuses other versions and languages rather than guess;
- a PC to bake on, with about 8 GB free.

**Bake again** if you baked with b143's tool. b147h's maps carry more than b143's, so the game stops and asks for a new bake if it finds the old ones.

**What's new since b143:**

*Restored to look as on the Xbox:*
- Chief's gold visor reflects the world again.
- The Pillar of Autumn's alert lights sweep the walls with their own light.
- The HUD's meters are drawn as on the Xbox.
- The main menu's ring looks better.

*Fixed:*
- Chief no longer vanishes as he climbs out of his cryo pod in the opening.
- The music no longer cuts out in *The Pillar of Autumn*'s bridge cutscene. Sounds are no longer turned away when the sound memory runs short.
- Textures arrive in time far more often.
- A likely cause of a rare slowdown is gone. In that slowdown, drawing took twice as long for the rest of a session.

*Also new:* the bake records which disc image it read, in `halo/bake.txt` and its log, to help when you report a problem.

**What it is:** the whole campaign, all ten levels, on a PSP-1000 at its standard 333 MHz. It doesn't run at full speed yet. The figures below were measured on a PSP-1000 just before b143, over the second half of each level's first two minutes. b147h measures the same or faster.

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
  - Chief's armour is flatter than on the Xbox;
  - the Pelican's canopy glass is lighter than on the PC, and its side panes stay dim;
  - there's a slight missing polygon at *The Silent Cartographer*'s far horizon;
  - some lights on world geometry are missing.
- **Sleep:** quit before you put the PSP to sleep (`docs/play.md`).

**Build it yourself:** the EBOOT's SHA-256 starts `71413e925562aff4`. On Linux with the pspdev toolchain (`docs/toolchain.md`):

```
PSPDEV=/opt/pspdev LABEL=b147h \
PSP_METRICS=0 PSP_PGO=use PSP_PGO_PROFILE=campaign-b147 PSP_PGO_STRICT=1 \
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
