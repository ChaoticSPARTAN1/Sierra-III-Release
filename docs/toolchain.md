# Building Sierra-III from scratch

Everything needed to go from a clean Linux machine to a release zip, maps and a running game, and to test it the way the project does. Each step says what it's for. Nothing here contains game data: you bring your own disc image.

## What you'll have at the end

| Piece | Made by | Goes on the Memory Stick as |
|---|---|---|
| The game (`EBOOT.PBP`, two helper modules) | `tools/build_release.sh` | `PSP/GAME/halo/` |
| The maps, baked for the PSP | `bake/` (`halo-psp-bake`) from your own disc image | `halo/maps/` |
| An emulator for testing (optional) | PPSSPP at the pinned commit with `tools/ppsspp_local_patches.diff` | stays on your PC |

## 1. The machine

Any x86-64 Linux. The project builds on Ubuntu 24.04 and on Arch-based systems. Install:

```sh
# Ubuntu / Debian
sudo apt install git python3 python3-venv ninja-build cmake build-essential xxd curl
# Arch and Arch-based
sudo pacman -S --needed git python ninja cmake base-devel xxd curl
```

- `ninja` and `python3` configure the engine; `cmake` and `xxd` build the Media Engine library (its build embeds a kernel module with `xxd -i`).
- `python3-venv` is for the map bake (step 5).

## 2. The PSP toolchain (pspdev)

The project uses pspdev's prebuilt release (psp-gcc 15.2.0; `UPSTREAM.md` has the exact toolchain commit).

```sh
curl -fsSL -o pspdev.tar.gz https://github.com/pspdev/pspdev/releases/latest/download/pspdev-ubuntu-latest-x86_64.tar.gz
sudo tar -xzf pspdev.tar.gz -C /opt        # makes /opt/pspdev
export PSPDEV=/opt/pspdev                  # add to your shell's profile
/opt/pspdev/bin/psp-gcc --version | head -1
```

- **Why the release and not a source build:** it's what the build machines use, so your EBOOT can match theirs byte for byte (step 4).
- A newer release than the project's may build fine but won't give the same bytes. To match exactly, use a release whose `psp-gcc --version` says 15.2.0.

## 3. Build the game

```sh
git clone <this repository> sierra-iii && cd sierra-iii
PSPDEV=/opt/pspdev tools/build_release.sh ~/sierra-out
```

What it does, in order:
1. Reads the pinned upstream commits from `upstream/pins.env`.
2. Fetches and builds the Media Engine library (me-core-mapper) and checks out and configures the engine (ChupathingyCE), once each, into a cache (`~/.cache/sierra-iii`; `CACHE=` moves it). Later builds reuse them.
3. Runs `psp_build.py`: mirrors the engine, applies `psp_fixups.py`, compiles, links, packs `EBOOT.PBP`.
4. Builds the two helper modules (`psp/crash`, `psp/profile`) and packs the zip.

You get `~/sierra-out/halo_psp_<commit>.zip`, plus `halo.elf`, `halo.map` (the link map) and `build.log`. The first build takes 5 to 10 minutes; later ones about 2.

**Options** (environment variables): `JOBS=8`, `LABEL=b114` (the zip's name), and the build switches (`PSP_GAME_OPT=-Os`, …; each is described where `psp_build.py` reads it).

## 4. Check you built the same bytes

The same commit builds the same bytes on any machine: the compile records no paths or times, and the zip's files carry the commit's time. So:

```sh
sha256sum ~/sierra-out/build/EBOOT.PBP | cut -c1-16
```

should equal the EBOOT hash in that build's notes ("EBOOT …"), built with the switches they name (a release is built with its profile, `PSP_PGO=use PSP_PGO_PROFILE=<name>`). If it doesn't, the toolchain or the switches differ (step 2).

## 5. Bake the maps from your own disc

The PSP can't read the Xbox maps directly: they're rebuilt for its memory, and their textures, geometry and stencils are baked into packs it streams.

1. **The bake tool:** the release's bundle for your OS (Windows, macOS, Linux), built by `.github/workflows/bake-bundle.yml`. Or build one here: `bake/make_bundle.sh <out>` compiles the Invader fork (`upstream/invader/psp-fork.patch`) and atracdenc from source (`bake/bundle/build_tools.sh`) and makes the self-contained `halo-psp-bake` folder.
2. **Bake:** `<out>/halo-psp-bake ~/Halo.iso ~/halo-psp-out`. Details, the disc version it expects, and the Python requirements are in `bake/README.md`. All maps take 45 to 60 minutes one after another, less with several at once (`--jobs`, by default one per CPU up to 4); `--maps b30,ui` does just those.

The result (`~/halo-psp-out/halo/`) is game data: keep it on your own machines.

## 6. Install on a PSP

1. A PSP running custom firmware (built and tested on a PSP-1000 with 6.61 and ARK-4).
2. Unzip the build zip at the Memory Stick's root: it makes `PSP/GAME/halo/` (and `halo/presets.txt`, the launch menu's test presets, if the build has them).
3. Copy the baked `halo/` folder to the stick's root, so it holds `halo/maps/`.
4. Launch "halo" from the game menu. The launch menu picks the map and the keep words (`ms0:/halo/keep.txt`; `psp_keep()` in `psp/src/psp_platform.c` reads them).

The game writes its log to `halo/log/log.txt` (the last 20 launches are kept as `log/NNN_log.txt`), its profile to `log/profile.txt`, screenshots (select + right) to `halo/shots/`, and `halo/crash.txt` if it crashes.

## 7. Test in the emulator (optional)

The project's emulator is headless PPSSPP with local patches: PSP-1000 memory, a strict interpreter that reports the unaligned and out-of-range accesses a real PSP faults on, and a model of the CPU's caches.

```sh
git clone https://github.com/hrydgard/ppsspp && cd ppsspp
git checkout $(cat <sierra-iii>/tools/ppsspp_BASE_COMMIT.txt)
git submodule update --init --recursive
git apply <sierra-iii>/tools/ppsspp_local_patches.diff
cmake -B build-headless -G Ninja -DCMAKE_BUILD_TYPE=Release -DHEADLESS=ON -DHEADLESS_CROSS=ON -DUSE_DISCORD=OFF -DUSE_MINIUPNPC=OFF
ninja -C build-headless PPSSPPHeadless
```

Then one run with `tools/emu.sh` (its header lists every setting): it sets up a memory stick folder with your baked maps, the keep words and an input script, runs the game for a wall time, and summarises the log. The settings that matter most:
- `PPSSPP_FAT=1` (on by default in `emu.sh`): PSP-1000 memory.
- `STRICT=1`: the strict interpreter (slow).
- `PSP_DCACHE=1` / `PSP_ICACHE=1`: the cache models (on the interpreter), with exact miss counts.

What the emulator can and can't tell you: it says *where* time goes and whether the game behaves; only a PSP says *how long* (it has no bus contention and no uncached-store cost). The project's own harness, which runs many of these at once, isn't part of the source.

## If something fails

- **`PSPDEV must name the pspdev toolchain`:** export `PSPDEV` (step 2).
- **The engine checkout or configure fails:** the cache entry is left as `<name>.part` and rebuilt next time; `build.log` has the error. Network access to GitHub is needed for the first build.
- **`compile_errors.txt`** in the output's `build/` folder lists compile errors.
- **A fixup no longer matches** (after moving a pin): `PSP_FIXUPS_CHECK=1 python3 psp_build.py` lists them; `UPSTREAM.md` explains moving a pin.
