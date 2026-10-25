/* psp_metrics.c: the test builds' four metrics, per map (PSP_METRICS, on by
default; psp_build.py). Built with PSP_METRICS=0 this unit is empty and the
hooks (psp/include/psp_metrics.h) compile to nothing.

Why. The heartbeat's lines are 5 s windows and its cache and heap lines run
from the launch, so a map's own figures had to be cut out of a log by hand,
and nothing said what a slow frame was doing. These lines give each map's
figures on their own, written when the map ends (a map change, or the game's
own exit) and every 60 s of game time meanwhile ("so far"), so a run stopped
from outside still leaves its last map's figures. tools/hwreport.py reads them.

The lines (times in ms; "ends" or "so far" after the map's name):
  metrics budget (MAP, ends): game time G, W s played, F frames, T ticks | tick A ms/s (R ticks/s x M ms) | draw B ms/s (P fps x N ms) | frame end C ms/s | rest D ms/s
    where each second of the map went: the ticks, the drawing, the frame's
    end (the GE and the swap) and the rest (the main loop, sound, waits),
    adding to 1,000.
  metrics hitches (MAP, ends): over 100 ms A, over 250 ms B, over 500 ms C; worst W ms at game time G (CAUSE); causes of those over 100 ms: checkpoint a, cutscene b, textures c, geometry d, animations e, map start f, other g
  metrics hitch (MAP): W ms at game time G, CAUSE (ticks T ms in K, draw D, end E; stick textures X geometry Y animations Z)
    one for each frame over 250 ms as it happens (the first 32 a map).
  metrics io (MAP, ends): textures R reads, K KB, T ms; geometry ...; animations ...
    the map's own blocking Memory Stick reads (the geometry's reads in the
    background, which overlap other work, aren't counted).
  metrics heap (MAP, ends): free low F KB at game time G; largest block low L KB at game time H; in use peak U KB; requests failed N; user memory free low M KB (largest block Q KB); S samples, walk X us
    sampled every 5 s of game time: the heap's free space (inside it and
    not yet taken from the system) and its largest free block, the one a
    large request needs.

A frame's time is from one frame's end to the next, as frametrace's. A hitch's
cause is the first that fits: a checkpoint written or read since the last
frame, a cutscene running, one of the readers' stick time over a quarter of
the frame (the largest), the map's first 30 frames, or other. */
#include "psp_metrics.h"

#if defined(PSP_METRICS) && PSP_METRICS
#include <stdio.h>
#include <string.h>
#include <pspkernel.h>
#include <pspsysmem.h>

void platform_log(const char *format, ...);
void psp_frametrace_tick(unsigned long microseconds);
void psp_frametrace_frame(unsigned long draw_us, unsigned long ticks, unsigned long end_us);
void psp_heap_map(const char *label);
void psp_me_exit(const char *why);
long psp_game_time_peek(void);
unsigned long psp_heartbeat_clock(void);
extern unsigned char cinematic_in_progress(void); /* the engine's boolean */
void psp_heap_survey(unsigned long *free_bytes, unsigned long *largest_bytes, unsigned long *used_bytes);
extern volatile unsigned long psp_heap_failures; /* psp_libc.c */

unsigned long psp_metrics_stick_us[PSP_METRICS_STICK_KINDS];
unsigned long psp_metrics_stick_reads[PSP_METRICS_STICK_KINDS];
unsigned long psp_metrics_stick_bytes[PSP_METRICS_STICK_KINDS];
volatile int psp_metrics_events; /* PSP_METRICS_CHECKPOINT | PSP_METRICS_REVERT since the last frame */

#define HITCH_THRESHOLDS 3
static const unsigned long hitch_us[HITCH_THRESHOLDS] = { 100000, 250000, 500000 };
#define HITCHES_LOGGED 32
#define MAP_START_FRAMES 30
#define HEAP_SAMPLE_TICKS 150
#define SO_FAR_TICKS 1800

enum
{
	CAUSE_CHECKPOINT,
	CAUSE_CUTSCENE,
	CAUSE_TEXTURES,
	CAUSE_GEOMETRY,
	CAUSE_ANIMATIONS,
	CAUSE_MAP_START,
	CAUSE_OTHER,
	CAUSES
};
static const char *const cause_name[CAUSES] =
	{ "checkpoint", "cutscene", "textures", "geometry", "animations", "map start", "other" };
static const char *const stick_name[PSP_METRICS_STICK_KINDS] = { "textures", "geometry", "animations" };

static struct
{
	int playing;
	char map[32];
	unsigned long started, last_end;      /* the clock at play's start and at the last frame's end */
	unsigned long long wall_us, tick_us, draw_us, end_us;
	unsigned long frames, ticks;
	unsigned long frame_tick_us, frame_ticks; /* since the last frame */
	unsigned long frame_stick[PSP_METRICS_STICK_KINDS];
	unsigned long hitches[HITCH_THRESHOLDS], causes[CAUSES], logged;
	unsigned long worst_us;
	long worst_at;
	int worst_cause;
	/* the readers' totals at play's start */
	unsigned long stick_us[PSP_METRICS_STICK_KINDS], stick_reads[PSP_METRICS_STICK_KINDS], stick_bytes[PSP_METRICS_STICK_KINDS];
	/* the heap */
	unsigned long heap_samples, heap_free_low, heap_largest_low, heap_used_peak, heap_walk_us;
	long heap_free_at, heap_largest_at;
	unsigned long failures_start;
	int user_free_low_kb, user_largest_low_kb;
	unsigned long ticks_since_sample, ticks_since_report;
} m;

static void heap_sample(void)
{
	unsigned long free_bytes, largest, used, start = psp_heartbeat_clock(), took;
	int user_free = sceKernelTotalFreeMemSize() / 1024, user_largest = sceKernelMaxFreeMemSize() / 1024;
	long now = psp_game_time_peek();

	psp_heap_survey(&free_bytes, &largest, &used);
	took = psp_heartbeat_clock() - start;
	if (took > m.heap_walk_us)
		m.heap_walk_us = took;
	if (!m.heap_samples || free_bytes < m.heap_free_low)
	{
		m.heap_free_low = free_bytes;
		m.heap_free_at = now;
	}
	if (!m.heap_samples || largest < m.heap_largest_low)
	{
		m.heap_largest_low = largest;
		m.heap_largest_at = now;
	}
	if (used > m.heap_used_peak)
		m.heap_used_peak = used;
	if (!m.heap_samples || user_free < m.user_free_low_kb)
		m.user_free_low_kb = user_free;
	if (!m.heap_samples || user_largest < m.user_largest_low_kb)
		m.user_largest_low_kb = user_largest;
	m.heap_samples++;
}

static void report(const char *when)
{
	double wall_s = m.wall_us / 1e6, tick_ms, draw_ms, end_ms;
	char io[3][80];
	int k;

	if (!m.playing || !m.frames || wall_s <= 0.0)
		return;
	tick_ms = m.tick_us / 1000.0 / wall_s;
	draw_ms = m.draw_us / 1000.0 / wall_s;
	end_ms = m.end_us / 1000.0 / wall_s;
	platform_log("metrics budget (%s, %s): game time %ld, %.1f s played, %lu frames, %lu ticks | tick %.0f ms/s (%.1f ticks/s x %.2f ms) | "
		"draw %.0f ms/s (%.1f fps x %.2f ms) | frame end %.0f ms/s | rest %.0f ms/s",
		m.map, when, psp_game_time_peek(), wall_s, m.frames, m.ticks,
		tick_ms, m.ticks / wall_s, m.ticks ? m.tick_us / 1000.0 / m.ticks : 0.0,
		draw_ms, m.frames / wall_s, m.draw_us / 1000.0 / m.frames,
		end_ms, 1000.0 - tick_ms - draw_ms - end_ms);
	platform_log("metrics hitches (%s, %s): over 100 ms %lu, over 250 ms %lu, over 500 ms %lu; worst %.0f ms at game time %ld (%s); "
		"causes of those over 100 ms: checkpoint %lu, cutscene %lu, textures %lu, geometry %lu, animations %lu, map start %lu, other %lu",
		m.map, when, m.hitches[0], m.hitches[1], m.hitches[2], m.worst_us / 1000.0, m.worst_at,
		m.worst_us ? cause_name[m.worst_cause] : "none",
		m.causes[CAUSE_CHECKPOINT], m.causes[CAUSE_CUTSCENE], m.causes[CAUSE_TEXTURES], m.causes[CAUSE_GEOMETRY],
		m.causes[CAUSE_ANIMATIONS], m.causes[CAUSE_MAP_START], m.causes[CAUSE_OTHER]);
	for (k = 0; k < PSP_METRICS_STICK_KINDS; k++)
		snprintf(io[k], sizeof(io[k]), "%s %lu reads, %lu KB, %.0f ms", stick_name[k],
			psp_metrics_stick_reads[k] - m.stick_reads[k], (psp_metrics_stick_bytes[k] - m.stick_bytes[k]) / 1024,
			(psp_metrics_stick_us[k] - m.stick_us[k]) / 1000.0);
	platform_log("metrics io (%s, %s): %s; %s; %s", m.map, when, io[0], io[1], io[2]);
	if (m.heap_samples)
		platform_log("metrics heap (%s, %s): free low %lu KB at game time %ld; largest block low %lu KB at game time %ld; in use peak %lu KB; "
			"requests failed %lu; user memory free low %d KB (largest block %d KB); %lu samples, walk %lu us",
			m.map, when, m.heap_free_low / 1024, m.heap_free_at, m.heap_largest_low / 1024, m.heap_largest_at,
			m.heap_used_peak / 1024, psp_heap_failures - m.failures_start, m.user_free_low_kb, m.user_largest_low_kb,
			m.heap_samples, m.heap_walk_us);
}

/* a map's play begins: after its load and settle (psp_loading.c) */
static void map_play(const char *map_name)
{
	const char *name = map_name ? map_name : "?", *slash = strrchr(name, '\\');
	int k;

	memset(&m, 0, sizeof(m));
	snprintf(m.map, sizeof(m.map), "%s", slash ? slash + 1 : name);
	m.playing = 1;
	m.started = m.last_end = psp_heartbeat_clock();
	for (k = 0; k < PSP_METRICS_STICK_KINDS; k++)
	{
		m.stick_us[k] = psp_metrics_stick_us[k];
		m.stick_reads[k] = psp_metrics_stick_reads[k];
		m.stick_bytes[k] = psp_metrics_stick_bytes[k];
		m.frame_stick[k] = psp_metrics_stick_us[k];
	}
	m.failures_start = psp_heap_failures;
	heap_sample();
}

/* a map's play ends: a load begins (before the old map is let go) or the
game exits. An exit may come from HOME's thread, which mustn't wait on the
heap's lock: the heap's last sample is then the 5 s one */
static void map_end(int on_game_thread)
{
	if (!m.playing)
		return;
	if (on_game_thread)
		heap_sample();
	report("ends");
	m.playing = 0;
}

void psp_metrics_leaving(const char *label)
{
	map_end(1);
	psp_heap_map(label);
}

void psp_metrics_loaded(const char *label, const char *map_name)
{
	psp_heap_map(label);
	map_play(map_name);
}

void psp_metrics_exit(const char *why)
{
	psp_me_exit(why);
	map_end(0);
}

void psp_metrics_tick(unsigned long microseconds)
{
	psp_frametrace_tick(microseconds);
	if (!m.playing)
		return;
	m.frame_tick_us += microseconds;
	m.frame_ticks++;
	if (++m.ticks_since_sample >= HEAP_SAMPLE_TICKS)
	{
		m.ticks_since_sample = 0;
		heap_sample();
	}
	if (++m.ticks_since_report >= SO_FAR_TICKS)
	{
		m.ticks_since_report = 0;
		report("so far");
	}
}

static int hitch_cause(unsigned long wall)
{
	unsigned long most = 0;
	int k, kind = -1;

	if (psp_metrics_events)
		return CAUSE_CHECKPOINT;
	if (cinematic_in_progress())
		return CAUSE_CUTSCENE;
	for (k = 0; k < PSP_METRICS_STICK_KINDS; k++)
		if (m.frame_stick[k] > most)
			most = m.frame_stick[k], kind = k;
	if (kind >= 0 && most * 4 > wall)
		return CAUSE_TEXTURES + kind;
	if (m.frames <= MAP_START_FRAMES)
		return CAUSE_MAP_START;
	return CAUSE_OTHER;
}

/* a frame has ended (psp_frame_account): its drawing's time, its ticks, its
end's time */
void psp_metrics_frame(unsigned long draw_us, unsigned long ticks, unsigned long end_us)
{
	unsigned long now, wall, stick[PSP_METRICS_STICK_KINDS];
	int k;

	psp_frametrace_frame(draw_us, ticks, end_us);
	if (!m.playing)
		return;
	now = psp_heartbeat_clock();
	wall = now - m.last_end;
	m.last_end = now;
	m.frames++;
	m.ticks += m.frame_ticks;
	m.wall_us += wall;
	m.tick_us += m.frame_tick_us;
	m.draw_us += draw_us;
	m.end_us += end_us;
	for (k = 0; k < PSP_METRICS_STICK_KINDS; k++)
	{
		stick[k] = psp_metrics_stick_us[k];
		m.frame_stick[k] = stick[k] - m.frame_stick[k];
	}
	if (wall > hitch_us[0])
	{
		int cause = hitch_cause(wall);

		for (k = 0; k < HITCH_THRESHOLDS; k++)
			if (wall > hitch_us[k])
				m.hitches[k]++;
		m.causes[cause]++;
		if (wall > m.worst_us)
		{
			m.worst_us = wall;
			m.worst_at = psp_game_time_peek();
			m.worst_cause = cause;
		}
		if (wall > hitch_us[1] && m.logged < HITCHES_LOGGED)
		{
			m.logged++;
			platform_log("metrics hitch (%s): %.0f ms at game time %ld, %s (ticks %.1f ms in %lu, draw %.1f, end %.1f; "
				"stick textures %.1f geometry %.1f animations %.1f)",
				m.map, wall / 1000.0, psp_game_time_peek(), cause_name[cause], m.frame_tick_us / 1000.0, m.frame_ticks,
				draw_us / 1000.0, end_us / 1000.0, m.frame_stick[0] / 1000.0, m.frame_stick[1] / 1000.0,
				m.frame_stick[2] / 1000.0);
		}
	}
	/* from here frame_stick holds the readers' totals at this frame's end */
	for (k = 0; k < PSP_METRICS_STICK_KINDS; k++)
		m.frame_stick[k] = stick[k];
	m.frame_tick_us = m.frame_ticks = 0;
	psp_metrics_events = 0;
}
#endif
