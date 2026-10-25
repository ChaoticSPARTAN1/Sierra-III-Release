/* psp_paths.h - where each build keeps its files on the Memory Stick

The campaign build keeps everything in ms0:/halo. The multiplayer build
(PSP_MULTIPLAYER) keeps its own in ms0:/halomp: config.toml, keep.txt,
init.txt, the log, saves, the crash report, screenshots (2026-10-11:
both builds ready on one stick, nothing swapped between them). The maps
stay in the campaign build's folder, and both builds read them there: one
bake to keep up to date. xbox_files.c's d:\maps (psp_fixups.py) and the
pack readers (psp_tex.c, psp_geo.c, psp_ge.c) go to PSP_MAPS_ROOT.

Both the engine's units and the port's plain C units include this
(psp_build.py gives the latter PSP_MULTIPLAYER too). */
#ifndef PSP_PATHS_H
#define PSP_PATHS_H

#ifdef PSP_MULTIPLAYER
#define PSP_DATA_ROOT "ms0:/halomp"
#else
#define PSP_DATA_ROOT "ms0:/halo"
#endif
/* the folder holding maps/, and the maps themselves */
#define PSP_MAPS_HOME "ms0:/halo"
#define PSP_MAPS_ROOT PSP_MAPS_HOME "/maps"

#endif
