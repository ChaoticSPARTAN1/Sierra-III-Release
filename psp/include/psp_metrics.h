/* psp_metrics.h: the test builds' metrics (psp/src/psp_metrics.c).

Every log point is one of these macros. Built with PSP_METRICS=0 (the final
release: psp_build.py) each compiles to nothing, or to the call it stands in
for, so the build carries none of the metrics' code or strings and its code
is the code without them. */
#ifndef PSP_METRICS_H
#define PSP_METRICS_H

enum
{
	PSP_METRICS_TEXTURES,
	PSP_METRICS_GEOMETRY,
	PSP_METRICS_ANIMATIONS,
	PSP_METRICS_STICK_KINDS
};

enum
{
	PSP_METRICS_CHECKPOINT = 1,
	PSP_METRICS_REVERT = 2
};

#if defined(PSP_METRICS) && PSP_METRICS
/* Each hook that calls takes the place of a call its host already makes, and
makes that call itself: a new call site would change the host's control
flow, and a profile-guided build would then drop the host's counts */
void psp_metrics_tick(unsigned long microseconds);
void psp_metrics_frame(unsigned long draw_us, unsigned long ticks, unsigned long end_us);
void psp_metrics_leaving(const char *label);
void psp_metrics_loaded(const char *label, const char *map_name);
void psp_metrics_exit(const char *why);
/* each reader's blocking Memory Stick time, so a slow frame can name the
cache that read in it */
extern unsigned long psp_metrics_stick_us[PSP_METRICS_STICK_KINDS];
extern unsigned long psp_metrics_stick_reads[PSP_METRICS_STICK_KINDS];
extern unsigned long psp_metrics_stick_bytes[PSP_METRICS_STICK_KINDS];
extern volatile int psp_metrics_events;
/* in place of psp_frametrace_tick(us) */
#define PSP_METRICS_FRAMETRACE_TICK(us) psp_metrics_tick(us)
/* in place of psp_frametrace_frame(draw_us, ticks, end_us) */
#define PSP_METRICS_FRAMETRACE_FRAME(draw_us, ticks, end_us) psp_metrics_frame(draw_us, ticks, end_us)
/* in place of psp_heap_map(label) as a load begins, before the old map goes */
#define PSP_METRICS_HEAP_MAP_LEAVING(label) psp_metrics_leaving(label)
/* in place of psp_heap_map(label) once the new map has loaded and settled */
#define PSP_METRICS_HEAP_MAP_LOADED(label, map_name) psp_metrics_loaded(label, map_name)
/* in place of psp_me_exit(why) as the game exits */
#define PSP_METRICS_ME_EXIT(why) psp_metrics_exit(why)
#define PSP_METRICS_STICK(kind, bytes, us) (psp_metrics_stick_reads[kind]++, psp_metrics_stick_bytes[kind] += (bytes), psp_metrics_stick_us[kind] += (us))
#define PSP_METRICS_EVENT(event) (psp_metrics_events |= (event))
#else
#define PSP_METRICS_FRAMETRACE_TICK(us) psp_frametrace_tick(us)
#define PSP_METRICS_FRAMETRACE_FRAME(draw_us, ticks, end_us) psp_frametrace_frame(draw_us, ticks, end_us)
#define PSP_METRICS_HEAP_MAP_LEAVING(label) psp_heap_map(label)
#define PSP_METRICS_HEAP_MAP_LOADED(label, map_name) psp_heap_map(label)
#define PSP_METRICS_ME_EXIT(why) psp_me_exit(why)
#define PSP_METRICS_STICK(kind, bytes, us) ((void)0)
#define PSP_METRICS_EVENT(event) ((void)0)
#endif

#endif
