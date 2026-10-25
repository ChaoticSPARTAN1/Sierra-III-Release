# Sierra-III b143: release notes

**Sierra-III b143**, the first public release: Halo: Combat Evolved's campaign on the PSP, built for the PSP-1000 (the PSP-2000 and 3000 work too). It contains nothing from the game: you bake the maps yourself, from your own Xbox disc, on your own PC. The full source of this release is in this repository.

**How to play:** follow [`docs/play.md`](docs/play.md) step by step: custom firmware, imaging your disc, the bake, the Memory Stick, the first launch, and what to send when it goes wrong. The short version is at its top.

**The files:**

| File | What |
|---|---|
| `halo_psp_b143.zip` | The game: it goes to the bake tool, which puts it on the Memory Stick's layout |
| `halo-psp-bake-windows-b143.zip` | The bake tool, Windows 10 or 11 |
| `halo-psp-bake-linux-b143.tar.gz` | The bake tool, Linux x86-64, **glibc 2.39 or later** (Ubuntu 24.04, Debian 13, Fedora 40, current Arch) |

Each file's SHA-256 is beside it on the release page. There is no macOS bake tool with b143.

**Requirements:** a PSP with custom firmware (ARK-4 on 6.61 is what it is tested on), a Memory Stick with 3.5 GB free, your own disc image of Halo for the original Xbox (**NTSC, version 01.10.12.2276 only**: the bake checks every map and refuses other versions and languages rather than guess; support for more is planned), and a PC to bake on with about 8 GB free.

**What it is:** the whole campaign, all ten levels, on a PSP-1000 at its standard 333 MHz. It doesn't run at full speed yet. Measured on a PSP-1000 just before b143, the second half of each level's first two minutes: *The Maw* about 30 frames a second, *Assault on the Control Room* 29, *Two Betrayals* 26, *The Pillar of Autumn* 20, *343 Guilty Spark* 17, *Truth and Reconciliation* 16, *Keyes* 14, *Halo* 13, *The Library* 13, and *The Silent Cartographer* about 6. **Multiplayer isn't part of this release.**

**What it restores and fixes:** the level's self-illumination (the Pillar of Autumn's engines and panels), the Pelican's smoked canopy glass, the alert lights on walls, dark scenes' brightness, the HUD bars' own transparency, the main menu at full texture quality, and the glow clamp; the world showing before it loads, menu music restarting, doors sliding as one, marines' warped faces, the Pelican and level draws dropped for a frame, console text over scenes, half-drawn shore polygons, black cut pieces, and the far horizon's black areas. Several speed-ups that leave the game exactly as it was.

**Bake again** if you baked with anything older: b143's bake adds the level glow maps, the alert lights' wall light and the HUD bars' own transparency. Older maps still load, but without them.

**The performance readout** (frame rate and timings) is off. Turn it on with `overlay` in the launch menu (hold L as the game starts), or from the in-game debug menu.

**Known issues:**

- **Speed:** see above; the heaviest stretches are slow, and the slowest levels play in slow motion.
- **A slight missing polygon** at *The Silent Cartographer*'s far horizon.
- **Looks not yet like the Xbox:** the Pelican's canopy glass is lighter than on the PC and its side panes stay dim.
- **Some audio and animation faults:** cutscene audio can drift from its picture, a few animations are cut short, and some lights on world geometry are missing.
- **Sleep:** quit before you put the PSP to sleep (`docs/play.md`).

**Build it yourself:** the EBOOT's SHA-256 prefix is `184572c617a4f654`. On Linux with the pspdev toolchain (`docs/toolchain.md`):

```
PSPDEV=/opt/pspdev LABEL=b143 \
PSP_METRICS=0 PSP_PGO=use PSP_PGO_PROFILE=campaign-b143 PSP_PGO_STRICT=1 \
PSP_PGO_EXTRA=-fno-unroll-loops,-fno-peel-loops,-fno-tracer,-fno-ipa-cp-clone \
PSP_TEXT_LAYOUT=w142a_tgc90 tools/build_release.sh <output folder>
```

**Licences and credits:** Sierra-III's own code, scripts and documents are under the MIT licence (`LICENSE`). Its end lists what MIT doesn't cover: the XMB wallpaper and the project's own look in `psp/xmb/`, our patches to GPL programs, and third-party files carried as they are. The engine, the bake's tools and the ideas borrowed from other ports are credited in `UPSTREAM.md` and the README.

Sierra-III was created under Microsoft's "[Game Content Usage Rules](https://www.xbox.com/en-US/developers/rules)" using assets from Halo: Combat Evolved, and it is not endorsed by or affiliated with Microsoft. Halo: Combat Evolved © Microsoft Corporation.

Free, and never for sale. Don't share your baked maps or your disc image.
