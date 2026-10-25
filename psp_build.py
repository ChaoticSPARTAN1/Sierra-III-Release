#!/usr/bin/env python3
"""Builds Sierra-III's EBOOT from ChupathingyCE's engine.

1. Mirrors the engine into build/tree (mirror() below: the GCC-vs-clang
   textual fixes) and applies psp_fixups.py.
2. Compiles the game (source/), the port's game-side files
   (port/linux/game), the Linux platform layer files that build as they are
   against the PSP toolchain's SDL3 and pthreads, and the PSP files
   (psp/src).
3. Links, reporting anything still undefined (and stubbing it, so the
   program links), sizes the heap to end below the game state, and packs
   EBOOT.PBP.

usage: psp_build.py [--jobs N]
environment: HALO_REPO (default /tmp/halo-sp), PSP_OUT (default /tmp/psp_halo)"""
import os, re, shlex, shutil, struct, subprocess, sys
from concurrent.futures import ThreadPoolExecutor

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.environ.get("HALO_REPO", "/tmp/halo-sp")
OUT = os.environ.get("PSP_OUT", "/tmp/psp_halo")
TREE = os.path.join(OUT, "tree")
OBJ = os.path.join(OUT, "obj")
# The toolchain: $PSPDEV as pspdev sets it (/opt/psp/pspdev or /opt/pspdev)
PSPDEV = os.environ.get("PSPDEV", "/opt/psp/pspdev")
SDK = PSPDEV + "/psp/sdk"
# mcidclan's me-core-mapper: installed into the toolchain by default (its "make install");
# PSP_MECORE names a separate install prefix holding include/me-core-mapper and lib/
MECORE = os.environ.get("PSP_MECORE", "")
JOBS = int(sys.argv[sys.argv.index("--jobs") + 1]) if "--jobs" in sys.argv else 2
TAG_WINDOW_BASE = 0x09600000   # psp_capacity.h, physical_memory_map.c
PROGRAM_BASE = 0x08804000      # where the kernel loads the module (first fit, low)
STACK_RESERVE = 0x40000        # main thread and other low allocations made before the heap

# The Linux platform layer's files the PSP compiles. Left out because
# nothing of them would link (b125): gl_functions (the GL loader),
# nv2a_psh (the pixel shader translator) and xgpu_text, which only the GL
# renderer used (psp_fixups.py, gl_never), and xiso (the PSP reads its own
# bake, never a disc image).
PLATFORM_FILES = [
    "bink_null", "xbox_xapi", "xbox_files", "xbox_kernel", "msvc_crt", "msvc_wide", "posix_files",
    "xnet", "halo_linker_common", "port_config", "sdl_platform", "xinput_sdl", "dsound_sdl",
    "d3d8_gl", "d3d8_resources", "xbox_textures", "nv2a_vsh", "xbdm",
]
# build 107: the campaign-only engine. Co-op and multiplayer become a branch
# of their own, with their own budgets (2026-10-08), so the campaign
# build leaves out the networking, the netcode and the multiplayer game
# engines: 0.55 MB of code and buffers (docs/dead_code_inventory.md).
# psp/src/psp_campaign.c answers what the campaign still calls in them, "no
# network game". Multiplayer maps still load the way they are tested
# (map_name levels\test\..., which runs no game engine). PSP_MULTIPLAYER=1
# builds them in as before: the starting point for that branch
MULTIPLAYER = os.environ.get("PSP_MULTIPLAYER", "") == "1"
MULTIPLAYER_UNITS = ("source/networking/", "source/bungie_net/", "source/game/game_engine_",
                     "port/linux/game/network_", "port/linux/game/coop_", "port/linux/src/xnet.c")
# (bungie_net's threads and mutexes are the saved games' and the menus' too)
CAMPAIGN_KEEPS = ("source/bungie_net/common/thread_win32.c",)
def multiplayer_unit(rel):
    return rel.startswith(MULTIPLAYER_UNITS) and rel not in CAMPAIGN_KEEPS
# The multiplayer build is a client (2026-10-17): it leaves out
# ChupathingyCE's network co-op too, a game on a campaign map that needs a
# host (psp/src/psp_nocoop.c answers it, as the campaign build's does).
# PSP_MP_COOP=1 builds the co-op units back in
MP_COOP = os.environ.get("PSP_MP_COOP", "") == "1"
# ... and hosts nothing: PSP_MP_HOST=1 (or co-op, which needs a host) builds
# hosting back in (psp_limits.h's PSP_MP_CLIENT_ONLY)
MP_HOST = MP_COOP or os.environ.get("PSP_MP_HOST", "") == "1"
# PSP_POOLS_XBOX=1: the campaign's actor, prop, light and glow particle
# arrays at the Xbox's sizes, and the game state as long as they need
# (psp_capacity.h's PSP_POOLS_GAME_STATE_SAVED). A layout, not a keep word:
# for long runs that measure the arrays' peaks against the Xbox's limits
POOLS_XBOX = os.environ.get("PSP_POOLS_XBOX", "") == "1"
# psp_capacity.h (the multiplayer build without co-op 640 KB higher, its
# pools for AI small; the campaign's 400 KB higher, its arrays sized from
# their peaks: PSP_POOLS_GAME_STATE_SAVED and PSP_POOLS_GPU_SAVED)
GAME_STATE_BASE = 0x092A0000 + (0xA0000 if MULTIPLAYER and not MP_COOP and os.environ.get("PSP_BIG_RAM", "") != "1" else 0) + (
    0x64000 if not MULTIPLAYER and not POOLS_XBOX else 0)
COOP_UNITS = ("port/linux/game/coop_", "port/linux/game/network_coop.c", "port/linux/game/network_actors.c")
def coop_unit(rel):
    return rel.startswith(COOP_UNITS)
def nocoop():
    return not MULTIPLAYER or not MP_COOP

# ChupathingyCE's desktop features the PSP build leaves out (its configure
# turns them on for every desktop build):
# - Custom Edition (Halo PC) maps: their own 23 MB tag cache, Halo PC's layouts
# - the game browser, its stats and moderation
# - the PC menus: XML menus built into ui.map's tags as it loads (the PSP keeps
#   the game's own, psp_menu.c)
# - the tag schema check: a bit per byte of the tag window (1.1 MB) and a walk
#   of every tag as a map loads, against the Xbox's layouts, which the PSP's
#   bake changes (slim blocks, packed BSP vertices, geometry outside)
# - touch input
# psp/src/psp_chupa.c answers what the game still calls in them
CHUPA_FLAGS_DROPPED = ("-DHALO_CUSTOM_EDITION", "-DHALO_GAME_BROWSER")
CHUPA_DESKTOP_UNITS = ("port/linux/game/ce_", "port/linux/game/browser_screen.c", "port/linux/game/server_browser.c",
                       "port/linux/game/game_events.c", "port/linux/game/game_list_claims.c",
                       "port/linux/game/game_stats.c", "port/linux/game/moderation_screen.c",
                       "port/linux/game/map_families.c", "port/linux/game/ui_map_list.c",
                       "port/linux/game/menu_functions.c", "port/linux/game/menu_tags.c",
                       "port/linux/game/tag_schema_", "port/linux/game/tag_validate",
                       "port/linux/game/touch_game.c", "port/linux/game/hud_hires_tags.c")
# ... except what the multiplayer build needs as it is: map_families.c names a
# game's map in the game's protocol (an Xbox map as itself, a Custom Edition
# map custom_maps\<name>), in the host's advertisement and settings and the
# client's reading of them
CHUPA_MULTIPLAYER_KEEPS = ("port/linux/game/map_families.c",)
# internet play in the multiplayer build (mpb1): the engine's p2p layer and
# what it's built on, compiled as the desktop compiles them
INTERNET_UNITS = ("port/linux/src/p2p.c", "port/linux/src/p2p_signal.c", "port/linux/src/p2p_lobby.c",
                  "port/linux/src/p2p_copies.c",
                  "port/linux/src/p2p_crypto.c", "port/third_party/monocypher/monocypher.c",
                  "port/third_party/monocypher/monocypher-ed25519.c", "port/third_party/kcp/ikcp.c")
# ... and the Multiplayer menu's ONLINE GAMES item, which the engine builds
# with its game browser (HALO_GAME_BROWSER): only these three units take the
# flag, so the item, its screen's hooks and the Multiplayer menu's ending of
# a game left behind come in without the web list, its stats and
# moderation. The screen is the PSP's own (psp/src/psp_online.c)
ONLINE_GAMES_UNITS = ("source/interface/ui_widget.c", "source/interface/ui_widget_event_handler_functions.c",
                      "source/cache/cache_files.c")
def desktop_unit(rel):
    return rel.startswith(CHUPA_DESKTOP_UNITS) and not (MULTIPLAYER and rel in CHUPA_MULTIPLAYER_KEEPS)

PSP_FILES = ["psp_memory", "psp_platform", "psp_sdl", "psp_libc", "psp_ge", "psp_tex", "psp_vfpu", "psp_menu", "psp_geo", "psp_anim", "psp_debug", "psp_me", "psp_trace", "psp_crash", "psp_scratch", "psp_watch", "psp_frametrace", "psp_loading", "psp_bakecheck", "psp_metrics"]
# the bake format this build reads (bake/bake.py's BAKE_FORMAT): one number
# for the bake's stamp and the game's check (psp/src/psp_bakecheck.c)
_bake_format = re.search(r"^BAKE_FORMAT = (\d+)$", open(os.path.join(HERE, "bake", "bake.py"), encoding="utf-8").read(), re.M)
if not _bake_format:
    sys.exit("psp_build: no BAKE_FORMAT line in bake/bake.py")
BAKE_FORMAT = int(_bake_format.group(1))

# build 38: the game's optimisation level (PSP_GAME_OPT=-Os builds the A/B
# variant that trades speed tricks for smaller code: the hardware profiler
# showed the CPU waiting on instruction fetches for about a quarter of its time)
GAME_OPT = os.environ.get("PSP_GAME_OPT", "-O2")
# build 44: PSP_BIG_RAM=1 builds the campaign recon variant for 64 MB models
# (and the emulator set to one): a 22 MB tag window as the Xbox's, user RAM
# to 0x0C000000, MEMSIZE=1 in the PARAM.SFO so the firmware (and PPSSPP)
# extend the user partition. Recon only: the PSP-1000 is the target.
BIG_RAM = os.environ.get("PSP_BIG_RAM", "") == "1"
# the heap runs from the program's end up to the game state; in the 64 MB
# variant the game state sits above the tag window (psp_capacity.h), and the
# heap runs up to the window (room for PSP_PGO=generate's counters)
HEAP_CEILING = TAG_WINDOW_BASE if BIG_RAM else GAME_STATE_BASE
# PSP_TICK_STEPS=1 adds the tick breadcrumbs (bipeds, units, actors, object_update;
# psp_fixups.py) that cost a few percent of the tick: recon builds only
TICK_STEPS = os.environ.get("PSP_TICK_STEPS", "") == "1"
# PSP_RECON=1 builds in the recon censuses (psp_fixups.py FIXES_RECON, psp_ge.c's
# draw census, psp_me.c's bus load): measuring instruments whose hooks sit in
# hot code, so a release build carries none of them (docs/recon_134.md)
RECON = os.environ.get("PSP_RECON", "") == "1"
# build 105: PSP_DEBUG_TOOLS=1 keeps the developer instruments that cost
# memory and that the game itself never needs (so far: the Media Engine
# measure job "meclassify", 68 KB of buffers; the game's render-debug cache
# at its full size). Off by default: the shipping build leaves them out and
# the heap gets the space. The campaign/multiplayer split will be a flavour
# of the same kind
DEBUG_TOOLS = os.environ.get("PSP_DEBUG_TOOLS", "") == "1"
# PSP_METRICS: each map's figures in the log (psp/src/psp_metrics.c: where
# each second went, the slow frames and what each was doing, the reads, the
# heap's low point). On unless PSP_METRICS=0, so every test build has them; a
# final release is built with PSP_METRICS=0, and then every hook
# (psp/include/psp_metrics.h) compiles to nothing
METRICS = os.environ.get("PSP_METRICS", "1") != "0"
# PSP_DITHERSHIFT=1: the GE's dither moved each pass (keep words dithershift,
# dithershiftall). Its tests sit in psp_ge.c's hot paths, so a default build
# leaves them out. Passed to psp_ge.c's own compile, which takes none of the
# engine's defines
DITHERSHIFT = os.environ.get("PSP_DITHERSHIFT", "") == "1"
# build 110: PSP_MAP_CHECKS=1 keeps ChupathingyCE's checks of a map's data as
# the tick reads it (animations, collision walks). Off by default: the PSP's
# maps are its own bake, trusted as the Xbox game trusts its own (psp_fixups.py,
# FIXES_CHUPA "trusted maps")
MAP_CHECKS = os.environ.get("PSP_MAP_CHECKS", "") == "1"
# PGO, the profile-guided build (big_levers.md section 6, L4).
# PSP_PGO=generate builds the hot units (those at -O2) counting their own
# branches and calls; the counters don't fit the PSP-1000's layout, so it
# needs PSP_BIG_RAM=1 (the emulator set to a 64 MB model). The game writes
# the counts to ms0:/halo/gcda at "gcovdumpN" seconds of game time.
# PSP_PGO=use with PSP_PGO_PROFILE=<name> builds the same units with the
# counts in pgo/<name>.tgz: an ordinary build, for the PSP-1000. The counts'
# file names are the objects' paths under PSP_OUT (-fprofile-prefix-path),
# so a profile made on one machine builds on another
PGO = os.environ.get("PSP_PGO", "")
PGO_PROFILE = os.environ.get("PSP_PGO_PROFILE", "")
PGO_DIR = os.path.join(OUT, "pgo-profile")
# PSP_PGO_PARTIAL=0: what the training run never reached is optimised for
# size, as GCC does by default (smaller, but slow wherever the run didn't go);
# 1, the default: optimised as usual
PGO_PARTIAL = os.environ.get("PSP_PGO_PARTIAL", "1") != "0"
# PSP_PGO_EXTRA: more flags for the counted units in a PSP_PGO=use build,
# separated by commas or spaces (e.g. -fno-unroll-loops: -fprofile-use
# unrolls and peels loops, which grows the program, and the heap is what's
# left of 24 MB after it)
PGO_EXTRA = os.environ.get("PSP_PGO_EXTRA", "").replace(",", " ").split()
# The counted units' layout and loop flags in a PSP_PGO=use build
# (review_b123_flags.md: the cache models' instruction misses):
# - PSP_PGO_ALIGN_FUNCTIONS=N, PSP_PGO_ALIGN_LOOPS=N: align to N bytes
#   (64 and 32 unless set; 0 leaves GCC's own, 4 bytes on allegrex). A
#   function or loop that starts on a 64-byte cache line fetches fewer lines
#   for the same code; the padding costs ~110 KB of heap
# - PSP_PGO_LOOP_TRANSFORMS=1: keeps GCC's loop unswitching, splitting,
#   versioning for strides and predictive commoning, which copy loops (more
#   code for the 16 KB instruction cache to hold); off unless set
# - PSP_PGO_HOT_FRACTION=N: --param hot-bb-count-fraction: a block is hot
#   when it ran at least 1/N as often as the program's busiest counter.
#   20000 unless set (GCC's own is 10000): the threshold moves with each new
#   profile's busiest counter, and one 4% higher left the tick's small
#   animation and matrix helpers just under it, called rather than inlined
#   (c10's tick +8% in the cache model, +10% on the PSP); at 20000 they
#   stay inlined (docs/review_c10hot.md). A smaller N lost (more
#   instructions, more misses on b30); 10000 builds GCC's own
# - PSP_PGO_HOT_PERMILLE=N: --param hot-bb-count-ws-permille; GCC reads it
#   only with LTO, so it changes nothing here (the sweep's control)
# These are switches of their own because a test harness's switch value can't
# hold an "=" (PSP_NAME=letters, digits and -_.,)
for _sw, _flag, _default in (("PSP_PGO_HOT_FRACTION", "--param=hot-bb-count-fraction=", "20000"),
                             ("PSP_PGO_HOT_PERMILLE", "--param=hot-bb-count-ws-permille=", ""),
                             ("PSP_PGO_ALIGN_FUNCTIONS", "-falign-functions=", "64"),
                             ("PSP_PGO_ALIGN_LOOPS", "-falign-loops=", "32")):
    _value = os.environ.get(_sw, "") or _default
    if not re.fullmatch(r"[0-9]*", _value):
        sys.exit("psp_build: %s is a number" % _sw)
    if _value not in ("", "0"):
        PGO_EXTRA.append(_flag + _value)
if os.environ.get("PSP_PGO_LOOP_TRANSFORMS", "") != "1":
    PGO_EXTRA += ["-fno-unswitch-loops", "-fno-split-loops", "-fno-version-loops-for-strides",
                  "-fno-predictive-commoning"]
# the units PSP_PGO=generate counts in: source/<name>/ for each name, "psp"
# for psp/src/. All the hot units counted, the program grew 5.3 MB and the
# heap fell to 3.9 MB in the 64 MB layout, under b30's 5.2 MB; these are the
# tick's and the drawing's own (about 3.4 MB) [U]. PSP_PGO_UNITS=a,b,...
# chooses others. The use build gives every hot unit its counts, where it
# has them
PGO_UNITS = os.environ.get("PSP_PGO_UNITS", "ai,rasterizer,objects,physics,units,structures,render,models,psp").split(",")
if PGO not in ("", "generate", "use"):
    sys.exit("psp_build: PSP_PGO is generate or use")
if PGO == "generate" and not BIG_RAM:
    sys.exit("psp_build: PSP_PGO=generate needs PSP_BIG_RAM=1 (the counters don't fit 24 MB)")
if PGO == "use" and not re.fullmatch(r"[A-Za-z0-9_-]+", PGO_PROFILE):
    sys.exit("psp_build: PSP_PGO=use needs PSP_PGO_PROFILE=<name> (pgo/<name>.tgz)")

def pgo_counted(rel):
    r = rel.replace("\\", "/")
    return any(r.startswith("psp/src/" if u == "psp" else "source/%s/" % u) for u in PGO_UNITS if u)

def pgo_flags(rel):
    # only the counted units: -fprofile-use without counts still turns on
    # its loop unrolling and peeling, which grew the uncounted units ~100 KB
    # for nothing (the first local build)
    if not pgo_counted(rel):
        return []
    if PGO == "generate":
        return ["-fprofile-generate=/pgo", "-fprofile-update=single", "-fprofile-prefix-path=" + OUT]
    if PGO == "use":
        # partial training (PGO_PARTIAL): code the run never reached is
        # optimised as usual, not for size (one map's opening doesn't reach
        # the rest). A function whose source location moved but whose
        # branches didn't (a line added above it) still uses its counts, with
        # a warning; one whose branches changed is built without them (GCC
        # checks both)
        # -fprofile-correction: counts merged from several runs
        # (psp-gcov-tool merge) can leave a value profile (a division's or a
        # copy's usual operand) a little off its block's count, which GCC
        # otherwise refuses ("corrupted value profile")
        return ["-fprofile-use=" + PGO_DIR, "-fprofile-prefix-path=" + OUT, "-fprofile-correction"] + (
                ["-fprofile-partial-training"] if PGO_PARTIAL else []) + [
                "-Wno-missing-profile", "-Wno-error=coverage-mismatch"] + PGO_EXTRA
    return []
PSP = ["psp-gcc", "-G0", GAME_OPT, "-march=allegrex", "-ffp-contract=off", "-fshort-wchar",
       "-fms-extensions", "-fcommon", "-fno-strict-aliasing", "-fwrapv",
       "-fno-delete-null-pointer-checks", "-fno-omit-frame-pointer", "-w",
       "-ffunction-sections", "-fdata-sections", "-fno-math-errno", "-fsingle-precision-constant",
       "-DHALO_PSP=1", "-DHALO_RELEASE=1", "-DHALO_ANDROID=1",
       ] + (["-DPSP_MULTIPLAYER=1"] if MULTIPLAYER else []) + (["-DPSP_MP_COOP=1"] if MULTIPLAYER and MP_COOP else []) + (["-DPSP_MP_HOST=1"] if MULTIPLAYER and MP_HOST else []) + (["-DPSP_MAP_CHECKS=1"] if MAP_CHECKS else []) + (["-DPSP_POOLS_XBOX=1"] if POOLS_XBOX else []) + (["-DPSP_RECON_64MB=1"] if BIG_RAM else []) + (["-DPSP_DEBUG_TOOLS=1"] if DEBUG_TOOLS else []) + (["-DPSP_TICK_STEPS=1"] if TICK_STEPS else []) + (["-DPSP_RECON=1"] if RECON else []) + (["-DPSP_METRICS=1"] if METRICS else []) + (["-DPSP_PGO_GENERATE=1"] if PGO == "generate" else []) + os.environ.get("PSP_CFLAGS", "").split() + [
       # build 19: failed asserts are cold paths, laid out away from the hot
       # code (the 16 KB instruction cache holds more of what runs)
       "-DPSP_ASSERTS_COLD=1", "-freorder-blocks-and-partition", "-mno-explicit-relocs",
       # the port's own sources are compiled by full path, and __FILE__ (asserts)
       # would put that path in the EBOOT: written relative to the checkout, the
       # same commit builds the same bytes on any machine
       "-ffile-prefix-map=" + HERE + "/=",
       # build 66: the inline tag block accessor (build 46's, for the collision
       # files) for the whole game: the HUD, the visibility walk and the
       # objects' updates reach their tag data through it thousands of times a
       # frame, each an out-of-line call with five asserts
       "-DPSP_FAST_TAG_BLOCKS=1",
       "-I" + os.path.join(HERE, "psp/include"), "-I" + SDK + "/include",
       # the MSVC C runtime's _open/_read/_close... (msvc_crt.c) would replace
       # newlib's system-call hooks of the same names, which newlib's own
       # open/read/close call: close -> _close -> close -> ... forever
       ] + ["-D%s=msvc_%s" % (name, name) for name in ("_close", "_fstat", "_lseek", "_open", "_read", "_stat", "_write")] + [

       "-include", os.path.join(HERE, "psp_limits.h"),
       "-include", os.path.join(HERE, "psp_pre.h")]

def ninja_flags(target):
    out = subprocess.run(["ninja", "-t", "commands", target], cwd=REPO, capture_output=True, text=True).stdout
    args = shlex.split(out.strip().splitlines()[-1]); keep = []; i = 0
    while i < len(args):
        a = args[i]
        if a in ("-include", "-idirafter", "-iquote"):
            keep += [a, args[i + 1]]; i += 2; continue
        if a.startswith(("-I", "-D", "-std=", "-U")) and a != "-D_FILE_OFFSET_BITS=64" and a not in CHUPA_FLAGS_DROPPED:
            keep.append(a)
        i += 1
    return keep

def units():
    targets = subprocess.run(["ninja", "-t", "targets", "all"], cwd=REPO, capture_output=True, text=True).stdout
    game = sorted({m.group(1) + ".c" for m in re.finditer(r"^build/linux/obj/(source/.*)\.o: linux_cc", targets, re.M)})
    game = [s for s in game if "/libtiff/" not in s]
    game += sorted({m.group(1) + ".c" for m in re.finditer(r"^build/linux/obj/(port/linux/game/.*)\.o: linux_cc", targets, re.M)})
    if not MULTIPLAYER:
        game = [s for s in game if not multiplayer_unit(s)]
    elif nocoop():
        game = [s for s in game if not coop_unit(s)]
    game = [s for s in game if not desktop_unit(s)]
    items = [(s, ninja_flags("build/linux/obj/" + s[:-2] + ".o") +
              (["-DHALO_GAME_BROWSER"] if MULTIPLAYER and s in ONLINE_GAMES_UNITS else []), []) for s in game]
    compat = ["-include", os.path.join(HERE, "psp/include/psp_compat.h")]
    for name in PLATFORM_FILES:
        s = "port/linux/src/%s.c" % name
        if not MULTIPLAYER and multiplayer_unit(s):
            continue
        if name == "dsound_sdl":
            # build 25: the PSP's own mixer (psp/src/psp_dsound.c), built as the Linux one is
            items.append((os.path.join(HERE, "psp/src/psp_dsound.c"), ninja_flags("build/linux/obj/" + s[:-2] + ".o"), compat))
            continue
        items.append((s, ninja_flags("build/linux/obj/" + s[:-2] + ".o"), compat))
    items.append(("port/third_party/tomlc17/tomlc17.c", ninja_flags("build/linux/obj/port/third_party/tomlc17/tomlc17.o"), []))
    memory_flags = ninja_flags("build/linux/obj/port/linux/src/xbox_memory.o")
    for name in PSP_FILES:
        items.append((os.path.join(HERE, "psp/src/%s.c" % name), memory_flags, compat))
    if nocoop():
        items.append((os.path.join(HERE, "psp/src/psp_nocoop.c"),
                      ninja_flags("build/linux/obj/source/networking/network_game_globals.o"), []))
    if not MULTIPLAYER:
        # built as the networking units it stands in for
        items.append((os.path.join(HERE, "psp/src/psp_campaign.c"),
                      ninja_flags("build/linux/obj/source/networking/network_game_globals.o"), []))
    else:
        # the PSP's sockets and Wi-Fi for xnet.c (plain C: compile_one), and
        # the ad hoc layer it hands them to with keep word adhoc (mp5)
        items.append((os.path.join(HERE, "psp/src/psp_net.c"), [], []))
        items.append((os.path.join(HERE, "psp/src/psp_net_adhoc.c"), [], []))
        # internet play (mpb1): the engine's own p2p layer as it is, on
        # psp_net.c's sockets: the public games' listings and the invite
        # tunnel. Discord, UPnP and the web game list stay out
        # (psp_internet.c answers what they're asked)
        for unit in INTERNET_UNITS:
            items.append((unit, ninja_flags("build/linux/obj/" + unit[:-2] + ".o"),
                          [] if unit.startswith("port/third_party/") else compat))
        # ... the ONLINE GAMES screen, a game unit (built as the game's port
        # units are), which reads the lobby through p2p.h
        items.append((os.path.join(HERE, "psp/src/psp_internet.c"),
                      ninja_flags("build/linux/obj/port/linux/src/p2p_crypto.o"), compat))
        items.append((os.path.join(HERE, "psp/src/psp_online.c"),
                      ninja_flags("build/linux/obj/port/linux/game/pal_tags.o"), ["-idirafter", "port/linux/src"]))
    # what the desktop units left out answered for the PSP
    # (CHUPA_DESKTOP_UNITS): the game's calls built as the game's port units
    # are, the platform layer's as sdl_platform.c is (its flags carry the
    # version)
    items.append((os.path.join(HERE, "psp/src/psp_chupa.c"),
                  ninja_flags("build/linux/obj/port/linux/game/pal_tags.o"), []))
    items.append((os.path.join(HERE, "psp/src/psp_chupa_platform.c"),
                  ninja_flags("build/linux/obj/port/linux/src/sdl_platform.o"), compat))
    # build 108: built as pal_tags.c is, the other in-place fix of a map's tags
    # as they load (the game's headers, so its layout table is checked against them)
    items.append((os.path.join(HERE, "psp/src/psp_tag_layouts.c"),
                  ninja_flags("build/linux/obj/port/linux/game/pal_tags.o"), []))
    return items

# build 69: the game's cold code at -Os (menus, saved games and films, the
# netcode, the script interpreter, cinematics, loading, the editor and tool
# leftovers): smaller code for the same behaviour; the hot paths (physics,
# objects, units, AI, maths, models, rendering, the HUD) stay at -O2.
# PSP_NO_COLD=1 builds everything at GAME_OPT as before
COLD_PREFIXES = ("source/interface/", "source/saved games/", "source/saved films/", "source/networking/",
                 "source/hs/", "source/cutscene/", "source/text/", "source/strings/", "source/dialogs/",
                 "source/editor/", "source/tool/", "source/bungie_net/", "source/shell/", "source/bink/",
                 "source/cache/", "source/tag_files/", "source/scenario/", "source/main/", "source/game/")
COLD_KEEP_HOT = ("source/interface/hud", "source/interface/first_person", "source/main/main.c",
                 "source/game/game_time", "source/game/players", "source/game/game.c")
# build 110: and the port's units that run at start-up, as a map loads, once
# a frame or never on the PSP (the settings, the window and the controls'
# polling, the file calls under the readers, the shader translators the null
# renderer never calls, the disc image and debugger stand-ins, interpolation,
# off by default), and the PSP's answer files
COLD_PORT_UNITS = ("port/linux/src/port_config.c", "port/linux/src/xiso.c", "port/linux/src/xbdm.c",
                   "port/linux/src/sdl_platform.c", "port/linux/src/xinput_sdl.c", "port/linux/src/xbox_files.c",
                   "port/linux/src/xbox_xapi.c", "port/linux/game/render_interpolation.c",
                   "port/linux/src/posix_files.c", "port/linux/src/halo_linker_common.c",
                   "port/linux/src/nv2a_psh.c", "port/linux/src/nv2a_vsh.c", "port/linux/src/gl_functions.c",
                   "port/linux/src/bink_null.c", "port/third_party/tomlc17/tomlc17.c",
                   "port/linux/game/pal_tags.c", "psp/src/psp_campaign.c", "psp/src/psp_nocoop.c", "psp/src/psp_chupa.c",
                   "psp/src/psp_chupa_platform.c", "psp/src/psp_tag_layouts.c", "psp/src/psp_bakecheck.c",
                   # mpb1: internet play runs on its own threads, a join's
                   # keys once; the ONLINE GAMES screen only in the menus
                   "psp/src/psp_online.c", "psp/src/psp_internet.c") + INTERNET_UNITS
def cold_code(rel):
    if os.environ.get("PSP_NO_COLD") or GAME_OPT != "-O2":
        return False
    r = rel.replace("\\", "/")
    return (r.startswith(COLD_PREFIXES) and not r.startswith(COLD_KEEP_HOT)) or r in COLD_PORT_UNITS

# PSP_NOINLINE=psp_ge,collision_bsp (recon only): the units whose path
# contains one of these words are compiled with no inlining, so the cache
# map (tools/cachemap.py) names each helper's instructions and misses rather
# than folding them into their caller. Slower; never for play
NOINLINE_UNITS = [w for w in os.environ.get("PSP_NOINLINE", "").split(",") if w]

def compile_one(item):
    src, flags, extra = item
    rel = os.path.relpath(src, HERE) if src.startswith("/") else src
    obj = os.path.join(OBJ, rel[:-2] + ".o")
    if cold_code(rel):
        extra = extra + ["-Os"]
    elif GAME_OPT == "-O2":
        extra = extra + pgo_flags(rel)
    if any(word in rel for word in NOINLINE_UNITS):
        extra = extra + ["-fno-inline", "-fno-inline-small-functions", "-fno-inline-functions-called-once"]
    os.makedirs(os.path.dirname(obj), exist_ok=True)
    defines = ["-Dmain=halo_main"] if src.endswith("shell/shell_xbox.c") else []
    if src.endswith("psp_bakecheck.c"):
        defines.append("-DPSP_BAKE_FORMAT=%d" % BAKE_FORMAT)
    if src.endswith("psp_platform.c"):
        defines.append("-DPSP_HEAP_KB=%d" % HEAP_KB)
        if PGO == "use":
            # the log's first lines say which profile built it
            defines.append('-DPSP_PGO_PROFILE="%s"' % PGO_PROFILE)
    # PSP_REUSE: keep an object only when it is newer than its source. Build
    # 46 shipped with a stale psp_tex.o (reused without this check, so an
    # edit to psp_tex.c never reached the EBOOT): the learned-binds file was
    # written without "vramranked", as the first version did
    if (os.environ.get("PSP_REUSE") and os.path.exists(obj) and not src.endswith(("psp_platform.c", "psp_bakecheck.c"))
            and os.path.getmtime(obj) > os.path.getmtime(os.path.join(TREE, src))):
        return src, 0, "", obj
    # PGO: the object named relative to the tree (the compiler's directory),
    # so its counts file is the object's path under PSP_OUT, flattened
    # ("tree#^#obj#source#ai#actors.gcda"): an absolute -o keeps the absolute
    # path, nested in folders the PSP's libgcov can't make
    out = os.path.relpath(obj, TREE) if PGO else obj
    cmd = PSP + flags + extra + defines + ["-include", os.path.join(HERE, "psp_post.h"), "-c", src, "-o", out]
    if src.endswith(("psp_sdl.c", "psp_libc.c", "psp_ge.c", "psp_tex.c", "psp_vfpu.c", "psp_menu.c", "psp_geo.c", "psp_anim.c",
                     "psp_debug.c", "psp_me.c", "psp_trace.c", "psp_crash.c", "psp_watch.c", "psp_net.c", "psp_net_adhoc.c")):
        # plain C against the SDL3 headers, none of the game's MSVC prefix
        # (build 110: the launch menu, the crash handler's side, the overlay
        # and the load trace's wrappers for size; the rest for speed)
        opt = "-Os" if src.endswith(("psp_menu.c", "psp_crash.c", "psp_debug.c", "psp_trace.c", "psp_watch.c", "psp_net.c", "psp_net_adhoc.c")) else "-O2"
        # after b116: -fno-math-errno, as the engine's units have: sqrtf is
        # the FPU's sqrt.s alone, without a library fallback that only sets
        # errno (the same results; an independent review)
        cmd = ["psp-gcc", "-G0", opt, "-march=allegrex", "-ffunction-sections", "-fdata-sections", "-fno-math-errno",
               "-ffile-prefix-map=" + HERE + "/=", "-I" + SDK + "/include",
               # (psp_paths.h, by quoted includes only: psp/include's stand-in
               # system headers stay out of these units)
               "-iquote", os.path.join(HERE, "psp/include")] + (["-DPSP_MULTIPLAYER=1"] if MULTIPLAYER else []) + (
               ["-DPSP_MP_COOP=1"] if MULTIPLAYER and MP_COOP else []) + (
               ["-DPSP_MP_HOST=1"] if MULTIPLAYER and MP_HOST else []) + (
               pgo_flags(rel) if opt == "-O2" else []) + (
               ["-DPSP_DITHERSHIFT=1"] if DITHERSHIFT and src.endswith("psp_ge.c") else []) + (
               ["-DPSP_RECON=1"] if RECON else []) + (
               ["-DPSP_METRICS=1"] if METRICS else []) + (
               # the switches their #if blocks test (psp_ge.c's debug tools)
               ["-DPSP_DEBUG_TOOLS=1"] if DEBUG_TOOLS else []) + (
               ["-DPSP_TICK_STEPS=1"] if TICK_STEPS else []) + [
               "-c", src, "-o", out]
        if src.endswith("psp_me.c"):
            # build 86: mcidclan's me-core-mapper headers (installed beside the SDK)
            cmd[-4:-4] = ["-I" + (MECORE or os.path.dirname(SDK)) + "/include"]
    if PGO:
        # PGO: the port's own files named through the tree's link to the
        # checkout (tree/sierra), as the engine's are named relative to the
        # tree: a function's file name is part of its profile's checksum, so
        # counts made on one machine match a build on any machine
        cmd = [a.replace(HERE + "/", "sierra/") for a in cmd]
    # PGO=use: a counted unit compiles with its warnings on (they change no
    # code), so GCC's word that it dropped a function's counts reaches
    # profile_mismatches(); -w hides it (b133's d9e7cf3 ran 3-20% slower
    # with five hot functions compiled without their counts, unseen)
    checked = PGO == "use" and any(a.startswith("-fprofile-use=") for a in cmd)
    plain = cmd
    if checked:
        cmd = [a for a in cmd if a != "-w"]
    r = subprocess.run(cmd, cwd=TREE, capture_output=True, text=True)
    if r.returncode and checked:
        # (a unit that only compiles with -w: compiled as before, unchecked)
        cmd = plain
        r = subprocess.run(cmd, cwd=TREE, capture_output=True, text=True)
    if r.returncode and "-mno-explicit-relocs" in cmd:
        # inline assembly that uses $at (".set noat") needs explicit relocations
        r = subprocess.run([a for a in cmd if a not in ("-mno-explicit-relocs", "-freorder-blocks-and-partition")],
                           cwd=TREE, capture_output=True, text=True)
    return src, r.returncode, r.stderr, obj

# Functions whose code differs between the counting build and the using one
# by design, so their counts never match: psp_tick_account dumps the counts
# (PSP_PGO_GENERATE), main logs the profile's name (PSP_PGO_PROFILE)
PGO_MODE_DEPENDENT = {("psp_platform.c", "main"), ("psp_platform.c", "psp_tick_account")}

def profile_mismatches(results):
    """the functions GCC compiled without their counts: their code changed
    since the profile was counted (GCC checks each function's branches)"""
    found = []
    for src, code, err, obj in results:
        for line in err.splitlines():
            if "does not match its profile data" in line:
                m = re.search(r"function '([^']+)'", line)
                name = m.group(1) if m else line.strip()
                if (os.path.basename(src), name) not in PGO_MODE_DEPENDENT:
                    found.append("%s: %s" % (src, name))
    return sorted(set(found))

LIBS = ["-lpspvram", "-lpspaudio", "-lpspvfpu", "-lpspdisplay", "-lpspgum", "-lpspgu", "-lpspge",
        "-lpsphprm", "-lpspdmac", "-lpspctrl", "-lpsppower", "-lpthread", "-lpsprtc", "-lpspdebug", "-lme-core", "-lm"]
# the multiplayer build's network (psp/src/psp_net.c): the stack, the access
# point's connection and the WLAN switch. Not pspnet_inet or pspnet_resolver:
# the toolchain's specs add those after libcglue, and naming them here as
# well splits their import table, so calls reach the wrong functions
# (multiplayer_recon.md, section 1.3). mp5: and the ad hoc libraries
# (psp_net_adhoc.c), whose module is loaded only with keep word adhoc
if MULTIPLAYER:
    LIBS += ["-lpspnet_adhocctl", "-lpspnet_adhoc", "-lpspnet", "-lpspnet_apctl", "-lpspwlan",
             # mpb1: the OpenPSID, for internet play's keys (psp_internet.c)
             "-lpspopenpsid"]

def elf_section_names(path):
    """the section names of a 32-bit little-endian ELF object (the PSP's)"""
    with open(path, "rb") as f:
        data = f.read()
    shoff, = struct.unpack_from("<I", data, 0x20)
    shentsize, shnum, shstrndx = struct.unpack_from("<HHH", data, 0x2E)
    def offset(i):  # a section header's sh_offset
        return struct.unpack_from("<I", data, shoff + i * shentsize + 16)[0]
    def name(i):    # its sh_name, an offset into the section-name table
        return struct.unpack_from("<I", data, shoff + i * shentsize)[0]
    table = offset(shstrndx)
    names = []
    for i in range(shnum):
        start = table + name(i)
        names.append(data[start:data.index(b"\0", start)].decode())
    return names

def hot_pairs(objs):
    """hotgroup's placements, per object file: each hot function's part,
    then its own cold part (.text.unlikely.<fn>, from
    -freorder-blocks-and-partition). The two parts branch to each other with
    MIPS's 16-bit branches, which reach only 128 KB, so they stay side by
    side; the cold parts of hot functions are a few dozen KB, so the hot
    block stays dense. Each placement names its file, because a function
    copied into several files (a static inline) may be hot in one and not in
    another, and each copy's cold part must stay beside its own main part"""
    lines = []
    for o in objs:
        names = elf_section_names(o)
        cold = set(n[15:] for n in names if n.startswith(".text.unlikely."))
        for n in names:
            if n.startswith(".text.hot."):
                lines.append('"%s"(%s)' % (o, n))
                if n[10:] in cold:
                    lines.append('"%s"(.text.unlikely.%s)' % (o, n[10:]))
    return lines

def link(objs, stubs):
    with open(os.path.join(OUT, "stubs.c"), "w") as f:
        f.write("/* generated by psp_build.py: symbols nothing defines yet */\n")
        for s in sorted(stubs):
            f.write("int %s(void) { return 0; }\n" % s)
    subprocess.run(["psp-gcc", "-G0", "-O2", "-w", "-c", os.path.join(OUT, "stubs.c"), "-o", os.path.join(OUT, "stubs.o")], check=True)
    with open(os.path.join(OUT, "objs.rsp"), "w") as f:
        for o in objs + [os.path.join(OUT, "stubs.o")]:
            f.write('"%s"\n' % o)
    # build 46: the import stub for halo_profile.prx's system call
    # (psp/profile/exports.exp; psp-build-exports -s writes halo_profile.S).
    # The module loads after the game starts; the firmware links the import
    # then, as it does for mecore's kcall
    # (the new stub format, psp-build-exports -k: two objects, the function's
    # stub referring to the library's entry, so --gc-sections keeps both; the
    # old format's library entry was dropped as unreferenced)
    # (build 105: a second system call, the bus-priority probe: part 0002)
    for part in ("0000", "0001", "0002"):
        stub = os.path.join(OUT, "halo_profile_%s.o" % part)
        subprocess.run(["psp-gcc", "-G0", "-I" + SDK + "/include", "-DF_halo_profile_" + part, "-c",
                        os.path.join(HERE, "psp/profile/halo_profile.S"), "-o", stub], check=True)
        with open(os.path.join(OUT, "objs.rsp"), "a") as f:
            f.write('"%s"\n' % stub)
    # build 86: the import stub for mecore's kcall.prx (psp/meaudio/kcall.S,
    # psp-build-exports -k of mecore's kernel/exports.exp), for the same
    # reason: the library's own stub is the old format, which --gc-sections
    # would drop. Ours come first, so the library's member is never pulled
    for part in ("0000", "0001", "0002", "0003"):
        stub = os.path.join(OUT, "kcall_%s.o" % part)
        subprocess.run(["psp-gcc", "-G0", "-I" + SDK + "/include", "-DF_kcall_" + part, "-c",
                        os.path.join(HERE, "psp/meaudio/kcall.S"), "-o", stub], check=True)
        with open(os.path.join(OUT, "objs.rsp"), "a") as f:
            f.write('"%s"\n' % stub)
    # PSP_TEXT_LAYOUT reorders the code for the 16 KB instruction cache. The
    # SDK's script places every code section in input order, which scatters
    # hot functions among cold ones; each layout puts some sections first,
    # then everything else as before. Same code, new addresses: exact.
    #   coldfirst: every function's cold part (.text.unlikely.*, from
    #     -freorder-blocks-and-partition) together at the start;
    #   hotgroup: every function PGO's profile found hot (.text.hot.*, from
    #     -freorder-functions) as one block, each followed by its own cold
    #     part (hot_pairs says why); everything else as without a layout. Hot
    #     code stops sharing cache lines with cold code (research_icache.md,
    #     T1). Only a PGO build has .text.hot sections;
    #   a file name: coldfirst, then that file's hot functions in its order
    #     (one section name a line, tools/icache_order.py's order.txt); or,
    #     when its lines are placements ("*<object>(<section>)", the test
    #     tools/codeorder.py: the call graph's order, T2), those in its order
    #     and nothing first, each function's cold part placed beside it.
    # A linker script gives each input section to the first rule that
    # matches it, so the most specific placements come first.
    # A function's main part is named by its profile: .text.hot.F when PGO's
    # counts found it hot, .text.F when they didn't or the function changed
    # since it was counted (a recon build, PSP_TICK_STEPS, debug tools), while
    # its cold part stays .text.unlikely.F either way. A layout that placed
    # the cold part by name and missed the main part under its other name put
    # the two far apart, and the cold part's branches back into the main part
    # (R_MIPS_PC16, +-128 KB) no longer reached: every placement of a main
    # part matches both names, and a cold part the layout doesn't place
    # itself (the function wasn't split when the layout was made) follows
    # its main part, so the pair stays together in any build
    def layout_placements(placements):
        out, seen = [], set(placements)
        for p in placements:
            m = re.match(r"^(\S*)\((\.text\.(?:hot\.)?)(\S+)\)$", p)
            more = []
            if m and not m.group(3).startswith(("unlikely.", "startup.", "exit.")):
                p = "%s(.text.hot.%s .text.%s)" % (m.group(1), m.group(3), m.group(3))
                cold = "%s(.text.unlikely.%s)" % (m.group(1), m.group(3))
                if cold not in seen:
                    more = [cold]
            for q in [p] + more:
                if q not in out:
                    out.append(q)
        return out

    linkfile = SDK + "/lib/linkfile.prx"
    text = open(linkfile).read()
    layout = os.environ.get("PSP_TEXT_LAYOUT", "")
    if layout:
        if layout == "hotgroup":
            first = hot_pairs(objs)
        else:
            first = ["*(.text.unlikely .text.unlikely.*)"]
        if layout not in ("coldfirst", "hotgroup"):
            # a bare name is a placement file of this repo's (psp/layout/), as
            # a profile's name is pgo/'s
            if not os.path.isfile(layout) and os.path.isfile(os.path.join(HERE, "psp/layout", layout + ".txt")):
                layout = os.path.join(HERE, "psp/layout", layout + ".txt")
            lines = [l.strip() for l in open(layout) if l.strip() and not l.startswith("#")]
            if lines and all(l.startswith("*") for l in lines):
                first = layout_placements(lines)
            else:
                first += layout_placements(["*(%s)" % l for l in lines if l.startswith(".text.")])
        old_text = "    _ftext = . ;\n    *(.text .stub .text.* .gnu.linkonce.t.*)\n"
        if old_text not in text:
            sys.exit("psp_build: the SDK's linkfile.prx has changed; PSP_TEXT_LAYOUT can't place the code")
        text = text.replace(old_text, "    _ftext = . ;\n" + "".join("    %s\n" % f for f in first) +
                            "    *(.text .stub .text.* .gnu.linkonce.t.*)\n")
    # build 118: the variables of the code the Media Engine runs (the GE
    # layer, the texture cache, pspgu) on cache lines of their own, and the
    # main CPU's own variables in those files (PSP_MAIN_SIDE, psp_me.h) on
    # others, each block 64-byte aligned at both ends. With gemeoverlap both
    # CPUs work at once, each through its own write-back cache, and a line
    # holding one CPU's variable and the other's would lose one CPU's write
    # (docs/big_levers.md, L1 step 4; the emulator's write trace finds them)
    old_data = "    _fdata = . ;\n    *(.data .data.* .gnu.linkonce.d.*)\n"
    old_bss = "   *(.dynbss)\n   *(.bss .bss.* .gnu.linkonce.b.*)\n"
    if old_data not in text or old_bss not in text:
        sys.exit("psp_build: the SDK's linkfile.prx has changed; the Media Engine's variables can't be placed")
    me_objects = ("*psp_ge.o", "*psp_tex.o", "*libpspgu.a:*")
    text = text.replace(old_data, "    _fdata = . ;\n"
                        "    . = ALIGN(64);\n    __psp_main_side_start = .;\n    KEEP(*(.data.psp_main_side))\n"
                        "    . = ALIGN(64);\n    __psp_main_side_end = .;\n    __psp_me_data_start = .;\n" +
                        "".join("    %s(.data .data.*)\n" % o for o in me_objects) +
                        "    . = ALIGN(64);\n    __psp_me_data_end = .;\n"
                        "    *(.data .data.* .gnu.linkonce.d.*)\n")
    text = text.replace(old_bss, "   *(.dynbss)\n   . = ALIGN(64);\n   __psp_me_bss_start = .;\n" +
                        "".join("   %s(.bss .bss.* COMMON)\n" % o for o in me_objects) +
                        "   . = ALIGN(64);\n   __psp_me_bss_end = .;\n"
                        "   *(.bss .bss.* .gnu.linkonce.b.*)\n")
    linkfile = os.path.join(OUT, "linkfile_layout.prx")
    open(linkfile, "w").write(text)
    # build 107: the link map (halo.map: each function's and variable's
    # address, size and object file) is written every link, for
    # tools/coverage.py's per-file inventory
    cmd = ["psp-gcc", "-G0", "-L" + SDK + "/lib"] + (["-L" + MECORE + "/lib"] if MECORE else []) + (
           ["-fprofile-generate"] if PGO == "generate" else []) + ["-specs=" + SDK + "/lib/prxspecs",
           "-Wl,-q,-T" + linkfile, "-Wl,-zmax-page-size=128", "-Wl,--gc-sections",
           "-Wl,-Map=" + os.path.join(OUT, "halo.map"),
           "-Wl,--wrap=malloc,--wrap=calloc,--wrap=realloc,--wrap=memalign,--wrap=sceKernelCreateThread,--wrap=sceGeListUpdateStallAddr,"
           "--wrap=sceIoOpen,--wrap=sceIoLseek32,--wrap=sceIoLseek,--wrap=sceIoRead,--wrap=sceIoReadAsync,--wrap=sceIoClose,"
           "--wrap=sceDmacMemcpy,--wrap=sceGeListEnQueue,--wrap=sceDisplaySetFrameBuf,--wrap=sceIoWrite,--wrap=sceIoGetstat,"
           "--wrap=sceIoDopen,--wrap=sceIoDclose,--wrap=open,--wrap=sceIoWaitAsync,--wrap=sceIoPollAsync,--wrap=sceIoChangeAsyncPriority,"
           "--wrap=sceIoRemove,--wrap=sceIoRename,"
           # build 106: condition variables that sleep instead of polling (psp_libc.c)
           "--wrap=pthread_cond_init,--wrap=pthread_cond_destroy,--wrap=pthread_cond_wait,--wrap=pthread_cond_timedwait,"
           "--wrap=pthread_cond_signal,--wrap=pthread_cond_broadcast,"
           # build 105: newlib's printf engines and strtod under one lock (psp_libc.c)
           "--wrap=_svfprintf_r,--wrap=_vfprintf_r,--wrap=_strtod_r,--wrap=vfprintf,--wrap=strtod,--wrap=strtof,"
           # build 114: the texture state's functions clear the bind memo (psp_tex.c, statememo)
           "--wrap=sceGuTexImage,--wrap=sceGuTexMode,--wrap=sceGuClutLoad,--wrap=sceGuClutMode,--wrap=sceGuTexFilter,"
           # build 114: list regions join the pending range (psp_ge.c, listcachedall)
           "--wrap=sceGuGetMemory",
           "@" + os.path.join(OUT, "objs.rsp"), SDK + "/lib/prxexports.o"] + LIBS + ["-o", os.path.join(OUT, "halo.elf")]
    return subprocess.run(cmd, capture_output=True, text=True)

# ---------- the mirror: the engine as GCC can compile it
# The engine's Linux build relies on clang's MSVC extensions in a few places
# GCC refuses: "static __inline" (the MSVC prefix maps __inline to a weak
# inline) and the "ui64"/"i64" integer suffixes. The mirror rewrites them
# and leaves the checkout itself untouched.
STATIC_INLINE = re.compile(r"\bstatic(\s+)(__inline|_inline|__forceinline)\b")
MSVC_SUFFIX = re.compile(r"\b(0x[0-9A-Fa-f]+|[0-9]+)(ui64|i64)\b")

def mirror():
    for top in ("source", "port", "build/linux"):
        src_top = os.path.join(REPO, top)
        for root, dirs, files in os.walk(src_top):
            for f in files:
                src = os.path.join(root, f)
                dst = os.path.join(TREE, os.path.relpath(src, REPO))
                os.makedirs(os.path.dirname(dst), exist_ok=True)
                if f.endswith((".c", ".h")):
                    text = open(src, errors="surrogateescape").read()
                    text = STATIC_INLINE.sub(lambda m: "static" + m.group(1) + ("__inline__ __attribute__((always_inline))" if m.group(2) == "__forceinline" else "__inline__"), text)
                    text = MSVC_SUFFIX.sub(lambda m: m.group(1) + ("ULL" if m.group(2) == "ui64" else "LL"), text)
                    if os.path.exists(dst) and open(dst, errors="surrogateescape").read() == text:
                        continue
                    open(dst, "w", errors="surrogateescape").write(text)
                elif not os.path.exists(dst):
                    shutil.copy2(src, dst)
    # the generated header marks every inline function #pragma weak; drop the
    # names some unit declares static (GCC refuses a weak static)
    statics = set()
    for root, dirs, files in os.walk(os.path.join(TREE, "source")):
        for f in files:
            if f.endswith((".c", ".h")):
                for m in re.finditer(r"\bstatic[ \t]+[^;{()\n=]*?\b(\w+)[ \t]*\(", open(os.path.join(root, f), errors="surrogateescape").read()):
                    statics.add(m.group(1))
    sem = os.path.join(TREE, "build/linux/halo_msvc_semantics.h")
    lines = open(sem).read().splitlines()
    kept = [l for l in lines if not (l.startswith("#pragma weak ") and l.split()[2] in statics)]
    open(sem, "w").write("\n".join(kept) + "\n")


def main():
    global HEAP_KB
    os.environ["HALO_REPO"] = REPO
    mirror()
    subprocess.run([sys.executable, os.path.join(HERE, "psp_fixups.py"), TREE], check=True)
    if PGO:
        link_path = os.path.join(TREE, "sierra")
        if os.path.islink(link_path):
            os.remove(link_path)
        os.symlink(HERE, link_path)
    if PGO == "use":
        import tarfile
        shutil.rmtree(PGO_DIR, ignore_errors=True)
        os.makedirs(PGO_DIR)
        with tarfile.open(os.path.join(HERE, "pgo", PGO_PROFILE + ".tgz")) as t:
            members = [m for m in t.getmembers() if m.isfile() and m.name.endswith(".gcda") and
                       "/" not in m.name.lstrip("./") and ".." not in m.name]
            t.extractall(PGO_DIR, members=members)
        print("pgo: %d count files from pgo/%s.tgz" % (len(members), PGO_PROFILE))
    HEAP_KB = int(os.environ.get("PSP_HEAP_KB", "4096"))
    items = units()
    results = list(ThreadPoolExecutor(JOBS).map(compile_one, items))
    bad = [(s, e) for s, c, e, o in results if c]
    with open(os.path.join(OUT, "compile_errors.txt"), "w") as f:
        for s, e in bad:
            f.write("=== %s\n%s\n" % (s, e))
    print("compiled %d of %d%s" % (len(results) - len(bad), len(results),
          "; failures in compile_errors.txt: " + ", ".join(os.path.basename(s) for s, e in bad) if bad else ""))
    if bad:
        sys.exit("not linking: %d units failed to compile" % len(bad))
    if not MULTIPLAYER:
        seed_profile(items)
    if PGO == "use":
        mismatched = profile_mismatches(results)
        with open(os.path.join(OUT, "pgo_mismatch.txt"), "w") as f:
            f.write("".join(m + "\n" for m in mismatched))
        print("pgo: %d functions compiled without their counts (the code changed since pgo/%s.tgz was counted)%s"
              % (len(mismatched), PGO_PROFILE, ": pgo_mismatch.txt" if mismatched else ""))
        for m in mismatched[:20]:
            print("  " + m)
        # PSP_PGO_STRICT=1 (a release): its profile must be counted at its own code
        if mismatched and os.environ.get("PSP_PGO_STRICT", "") == "1":
            sys.exit("not linking: PSP_PGO_STRICT=1 and the profile doesn't match the code; count it at this commit")
    objs = [o for s, c, e, o in results if not c]
    stubs = set()
    for attempt in range(6):
        r = link(objs, stubs)
        missing = set(re.findall(r"undefined reference to `([^']+)'", r.stderr))
        if r.returncode == 0 or not (missing - stubs):
            break
        stubs |= missing
    if r.returncode:
        print(r.stderr[-3000:]); sys.exit(1)
    open(os.path.join(OUT, "stubbed_symbols.txt"), "w").write("\n".join(sorted(stubs)) + "\n")
    # size the heap: from the program's end up to the game state
    size = subprocess.run(["psp-size", os.path.join(OUT, "halo.elf")], capture_output=True, text=True).stdout.splitlines()[1].split()
    program = int(size[0]) + int(size[1]) + int(size[2])
    # build 116: to 4 KB, not 64. With 64, a few bytes of code past a step
    # cost the heap 64 KB at once (b115 sat 0.7 KB under one). The slack
    # above the heap is STACK_RESERVE either way: b115 ran with it plus
    # 0.7 KB, and "free" in the heartbeat (138 KB in b115) is what's left of it
    heap = (HEAP_CEILING - PROGRAM_BASE - program - STACK_RESERVE) // 1024 // 4 * 4
    print("program %.2f MB (text %d, data %d, bss %d); heap %d KB; %d symbols stubbed (stubbed_symbols.txt)"
          % (program / 1048576, int(size[0]), int(size[1]), int(size[2]), heap, len(stubs)))
    if heap != HEAP_KB:
        HEAP_KB = heap
        src = os.path.join(HERE, "psp/src/psp_platform.c")
        item = [i for i in items if i[0] == src][0]
        s, c, e, o = compile_one(item)
        assert c == 0, e
        r = link(objs, stubs)
        assert r.returncode == 0, r.stderr[-2000:]
    subprocess.run(["psp-fixup-imports", os.path.join(OUT, "halo.elf")], check=True)
    subprocess.run(["psp-prxgen", os.path.join(OUT, "halo.elf"), os.path.join(OUT, "halo.prx")], check=True)
    # the title the XMB shows: the multiplayer build's is its own, so the two
    # EBOOTs installed side by side (PSP/GAME/halo, PSP/GAME/halomp) tell apart;
    # the game is named only as what this ports, so the title claims no
    # endorsement (Microsoft's Game Content Usage Rules)
    title = os.environ.get("PSP_TITLE", "Sierra-III: Halo CE port (multiplayer)" if MULTIPLAYER else "Sierra-III: Halo CE port")
    subprocess.run(["mksfoex"] + (["-d", "MEMSIZE=1"] if BIG_RAM else []) +
                   [title, os.path.join(OUT, "PARAM.SFO")], check=True)
    icon0, icon1, pic0, pic1, snd0 = xmb_files()
    subprocess.run(["pack-pbp", os.path.join(OUT, "EBOOT.PBP"), os.path.join(OUT, "PARAM.SFO"),
                    icon0, icon1, pic0, pic1, snd0, os.path.join(OUT, "halo.prx"), "NULL"],
                   check=True, capture_output=True)
    print("wrote", os.path.join(OUT, "EBOOT.PBP"))

# The release's ready-made profile, every campaign level open: its own
# slot, so unzipping a new build over the stick never
# touches a player's profiles. Built from code each build (psp/seed/
# all_levels.c), so it follows the save format: the profile's bytes from the
# engine's headers, the checksum, the folder and its meta file from the save
# code in the tree, each read here and refused when it isn't what this
# expects. Written to <out>/stick/, the stick's own tree.
SEED_NAME = "All Levels"

def seed_profile(items):
    import hashlib, struct
    def read(rel):
        return open(os.path.join(TREE, rel), encoding="utf-8", errors="replace").read()
    def need(pattern, text, what):
        m = re.search(pattern, text, re.S)
        if not m:
            sys.exit("psp_build: the all-levels profile: %s no longer reads as expected (%s)" % (what, pattern))
        return m
    profile_c = read("source/saved games/player_profile.c")
    files_c = read("source/saved games/saved_game_files.c")
    xapi_c = read("port/linux/src/xbox_xapi.c")
    look = int(need(r"DEFAULT_LOOK_SENSITIVITY = (\d+)", profile_c, "player_profile.c's look sensitivity").group(1))
    checked = int(need(r"PLAYER_PROFILE_CHECKSUM_DATA_SIZE = (\d+)", files_c, "the profile's checksummed size").group(1))
    # the content signature: SHA-1 of the title key (with its terminator, as
    # sizeof counts it) and then the data
    key = need(r'signature_title_key\[\] = "([^"\\]*)";', xapi_c, "the signature's key").group(1).encode() + b"\0"
    need(r"sha1_initialize\(context\);\s*sha1_update\(context, signature_title_key, sizeof\(signature_title_key\)\);",
         xapi_c, "the signature's start")
    # the save folder: <u:>\UDATA\<FNV-1a of the name's UTF-16, 48 bits, as 12 hex digits>
    need(r"hash = 1469598103934665603ULL;.*?hash \^= \(unsigned long long\)\*name;\s*hash \*= 1099511628211ULL;",
         xapi_c, "the save folder's hash")
    need(r'SAVE_DATA_DIRECTORY "\\\\%012llX\\\\", root, separator, hash & 0xffffffffffffULL', xapi_c,
         "the save folder's name")
    folder = need(r'#define SAVE_DATA_DIRECTORY "(\w+)"', xapi_c, "the save data folder").group(1)
    meta = need(r'#define SAVE_META_FILE "([\w.]+)"', xapi_c, "the save meta file").group(1)
    need(r'"%s%s", save_game_directory, "blam\.sav"\);\s*checksum_data_size = PLAYER_PROFILE_CHECKSUM_DATA_SIZE;',
         files_c, "the profile's file name")
    need(r'L"u:\\\\"', files_c, "the profiles' drive")

    flags = [f for s, f, e in items if s == "source/saved games/player_profile.c"][0]
    probe = os.path.join(HERE, "psp/seed/all_levels.c")
    obj = os.path.join(OUT, "seed", "all_levels.o")
    os.makedirs(os.path.dirname(obj), exist_ok=True)
    name_c = 'L"' + SEED_NAME + '"'
    # -G0 last: the probe's tables are read back by section name, which small
    # data (PSP_CFLAGS=-G8) would move out of .rodata
    r = subprocess.run(PSP + flags + ["-G0", "-DPSP_SEED_NAME=" + name_c, "-DPSP_SEED_LOOK_SENSITIVITY=%d" % look,
                                      "-include", os.path.join(HERE, "psp_post.h"), "-c", probe, "-o", obj],
                       cwd=TREE, capture_output=True, text=True)
    if r.returncode:
        sys.exit("psp_build: the all-levels profile didn't compile:\n" + r.stderr[-3000:])
    def section(name):
        out = obj + "." + name
        subprocess.run(["psp-objcopy", "-O", "binary", "--only-section=.rodata." + name, obj, out], check=True)
        return open(out, "rb").read()
    profile = section("psp_seed_profile")
    size, block_size, signature_size = struct.unpack("<3H", section("psp_seed_sizes"))
    if len(profile) != size or size != checked or signature_size != hashlib.sha1().digest_size:
        sys.exit("psp_build: the all-levels profile: %d bytes, %d checksummed, a %d-byte signature: not the format this writes"
                 % (len(profile), checked, signature_size))
    block = profile + hashlib.sha1(key + profile).digest()
    block += b"\0" * (block_size - len(block))
    name16 = SEED_NAME.encode("utf-16-le")
    h = 1469598103934665603
    for unit in struct.unpack("<%dH" % (len(name16) // 2), name16):
        h = ((h ^ unit) * 1099511628211) & 0xFFFFFFFFFFFFFFFF
    where = os.path.join(OUT, "stick", "halo", "save", "u", folder, "%012X" % (h & 0xFFFFFFFFFFFF))
    shutil.rmtree(os.path.join(OUT, "stick"), ignore_errors=True)
    os.makedirs(where)
    open(os.path.join(where, "blam.sav"), "wb").write(block)
    open(os.path.join(where, meta), "wb").write(name16)
    print("seed: the all-levels profile \"%s\" at %s" % (SEED_NAME, os.path.relpath(where, OUT)))

def xmb_files():
    """The XMB menu art for pack-pbp, in its order: ICON0, ICON1, PIC0, PIC1, SND0.
    Art comes from psp/xmb/<campaign|mp>/; a missing file is packed as NULL.
    SND0 is packed only when PSP_XMB_SND0 names a local .AT3, because a clip
    of the game's music is game data and never lives in this repository."""
    art = os.path.join(HERE, "psp/xmb", "mp" if MULTIPLAYER else "campaign")
    def pick(name):
        path = os.path.join(art, name)
        return path if os.path.exists(path) else "NULL"
    snd0 = os.environ.get("PSP_XMB_SND0", "")
    if snd0 and not os.path.exists(snd0):
        sys.exit("PSP_XMB_SND0 names a file that doesn't exist: " + snd0)
    return pick("ICON0.PNG"), pick("ICON1.PMF"), pick("PIC0.PNG"), pick("PIC1.PNG"), snd0 or "NULL"

if __name__ == "__main__":
    main()
