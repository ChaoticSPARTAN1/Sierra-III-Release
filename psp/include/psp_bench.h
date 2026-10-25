/*
PSP_BENCH.H

The ceiling bench's switches (keep word "ceilingbench", psp_platform.c):
each bit in psp_bench_skip turns one big part of the game off for a measured
window, so the PSP shows what that part costs. The engine reads them where
psp_fixups.py's FIXES_112 wraps the calls, psp_ge.c at its draw entry points;
0, the default, changes nothing.
*/
#ifndef __PSP_BENCH_H
#define __PSP_BENCH_H

#define PSP_SKIP_AI 1       /* ai_update */
#define PSP_SKIP_OBJECTS 2  /* objects_update: every object's update */
#define PSP_SKIP_EFFECTS 4  /* effects_update, particles_update, render_particles */
#define PSP_SKIP_MODELS 8   /* render_objects, render_object_shadows */
#define PSP_SKIP_LEVEL 16   /* structure_render_lightmaps, structure_render_transparent_geometry */
#define PSP_SKIP_GE 32      /* every draw handed to the GE layer (psp_ge.c), dropped on arrival */

extern int psp_bench_skip;

#endif /* __PSP_BENCH_H */
