/* psp_frametrace.c: "frametrace" (keep.txt), every frame timed and split
(build 126).

Why. The heartbeat logs averages and each window's worst frame. Averages can't
tell a steady overrun from a rare hitch, and at 30 ticks a second the game loses
time only on a frame over 66.7 ms (two ticks' worth): the project's floor, 15 fps
at 30 ticks a second, is a question about the slow frames, not the mean.

What. With the keep word each frame is timed from the end of one to the end of
the next (its "wall" time) and split into:
  t  its ticks (game_tick, as many as ran since the last frame);
  d  its drawing (main_game_render: the engine's render pass and the GE lists);
  e  the frame's end (psp_ge_frame_end: waiting for the GE, the swap);
  r  the rest (the main loop, input, sound, waiting for the frame lock);
  s  the Memory Stick reads made meanwhile (textures, geometry, animations;
     their time is inside t, d or r, wherever the read was made).
Every 300 frames one log line gives the window's average, its frames over
66.7 ms and the game time lost, its three slowest frames split as above, and
the whole run's 99th and 99.9th percentile frame times, from a histogram of
every frame in 1 ms bins. tools/frametrace.py reads the lines.

The line, for the reader's parser (times in ms, one decimal):
  frametrace: N frames to game time G: A ms avg; over 66.7 ms O, lost L s of W s;
  slowest WALL:tT/K dD eE rR sS, ...; run F frames, 99% P99 ms, 99.9% P999 ms

Measurement only: nothing the game does changes. With the word off nothing is
kept: the histogram (4 KB) is allocated when the word is first seen. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int psp_keep(const char *name);
void platform_log(const char *format, ...);
long psp_game_time_peek(void);
unsigned long psp_heartbeat_clock(void);

/* every Memory Stick read's time, added by the readers (psp_tex.c, psp_geo.c,
psp_anim.c) as they finish; it only ever grows, and wraps safely (a frame's
share is a difference) */
volatile unsigned long psp_stick_read_us = 0;

#define FRAMETRACE_WINDOW 300
#define FRAMETRACE_BINS 1000            /* 1 ms each; the last holds every longer frame */
#define FRAMETRACE_SLOWEST 3
#define FRAMETRACE_TWO_TICKS_US 66667   /* a frame over this loses game time */
#define FRAMETRACE_TICK_US 33333
/* a frame shown for three display refreshes or more (the vertical blank
every 16.7 ms; two refreshes is the 30 fps frame) */
#define FRAMETRACE_LATE_US 41667
/* a late frame whose ticks, drawing and end took less than this would have
fitted two refreshes with room for the loop's own work: something outside
the game's work held it */
#define FRAMETRACE_HELD_US 25000
#define FRAMETRACE_GAPS 64

/* the heartbeat's work (psp_platform.c): a late frame that overlapped a beat
is told apart from one the game's own work made late */
extern volatile int psp_heartbeat_busy;
extern volatile unsigned long psp_heartbeat_spans, psp_heartbeat_work_us;

struct frametrace_frame
{
	unsigned long wall, tick, draw, end, stick;
	unsigned long ticks;
};

static int frametrace_on = -1;          /* -1: the keep word not read yet */
static unsigned long *frametrace_bins;  /* the run's histogram, 1 ms bins */
static unsigned long frametrace_run_frames;
static unsigned long frametrace_last_end, frametrace_last_stick, frametrace_tick_us;

static struct
{
	unsigned long frames, wall, ticks, over;
	struct frametrace_frame slowest[FRAMETRACE_SLOWEST];
	/* late frames: how many, those overlapping a beat, those held by
	something outside the game's work, those with stick reads, and the gaps
	between them (a period shows as a steady gap) */
	unsigned long late, late_beat, late_held, late_stick, gaps;
	unsigned long gap[FRAMETRACE_GAPS];
	unsigned long beat_work_start;
} frametrace_window;
static unsigned long frametrace_last_late, frametrace_late_seen, frametrace_spans_seen;

/* each tick's time, from psp_tick_account (psp_platform.c) */
void psp_frametrace_tick(unsigned long microseconds)
{
	if (frametrace_on > 0)
		frametrace_tick_us += microseconds;
}

/* the frame time (whole ms) that per_thousand / 1000 of the run's frames
reach or pass */
static unsigned long frametrace_percentile(unsigned long per_thousand)
{
	unsigned long need = (frametrace_run_frames * per_thousand + 999) / 1000, seen = 0;
	int bin;

	for (bin = FRAMETRACE_BINS - 1; bin > 0; bin--)
	{
		seen += frametrace_bins[bin];
		if (seen >= need)
			return (unsigned long)bin;
	}
	return 0;
}

static void frametrace_report(void)
{
	char slowest[FRAMETRACE_SLOWEST][80];
	double wall_s = frametrace_window.wall / 1e6;
	double lost_s = wall_s - frametrace_window.ticks * (FRAMETRACE_TICK_US / 1e6);
	int k;

	for (k = 0; k < FRAMETRACE_SLOWEST; k++)
	{
		struct frametrace_frame const *f = &frametrace_window.slowest[k];
		long rest = (long)f->wall - (long)f->tick - (long)f->draw - (long)f->end;

		snprintf(slowest[k], sizeof(slowest[k]), "%.1f:t%.1f/%lu d%.1f e%.1f r%.1f s%.1f",
			f->wall / 1000.0, f->tick / 1000.0, f->ticks, f->draw / 1000.0, f->end / 1000.0,
			(rest > 0 ? rest : 0) / 1000.0, f->stick / 1000.0);
	}
	platform_log("frametrace: %lu frames to game time %ld: %.1f ms avg; over 66.7 ms %lu, lost %.1f s of %.1f s; "
		"slowest %s, %s, %s; run %lu frames, 99%% %lu ms, 99.9%% %lu ms",
		frametrace_window.frames, psp_game_time_peek(), frametrace_window.wall / 1000.0 / frametrace_window.frames,
		frametrace_window.over, lost_s > 0.0 ? lost_s : 0.0, wall_s, slowest[0], slowest[1], slowest[2],
		frametrace_run_frames, frametrace_percentile(10), frametrace_percentile(1));
	{
		unsigned long sorted[FRAMETRACE_GAPS], i, j;
		unsigned long n = frametrace_window.gaps < FRAMETRACE_GAPS ? frametrace_window.gaps : FRAMETRACE_GAPS;

		memcpy(sorted, frametrace_window.gap, n * sizeof(*sorted));
		for (i = 1; i < n; i++)
			for (j = i; j > 0 && sorted[j - 1] > sorted[j]; j--)
			{
				unsigned long swap = sorted[j];

				sorted[j] = sorted[j - 1];
				sorted[j - 1] = swap;
			}
		platform_log("framelate: %lu of %lu frames over %lu ms; during the heartbeat's work %lu, held (ticks, drawing and end under %lu ms) %lu, "
			"with stick reads %lu; gaps between late frames ms: min %lu, median %lu, max %lu (%lu gaps); the heartbeat's work %lu ms",
			frametrace_window.late, frametrace_window.frames, (unsigned long)(FRAMETRACE_LATE_US / 1000),
			frametrace_window.late_beat, (unsigned long)(FRAMETRACE_HELD_US / 1000), frametrace_window.late_held,
			frametrace_window.late_stick, n ? sorted[0] / 1000 : 0, n ? sorted[n / 2] / 1000 : 0, n ? sorted[n - 1] / 1000 : 0,
			frametrace_window.gaps, (psp_heartbeat_work_us - frametrace_window.beat_work_start) / 1000);
	}
	memset(&frametrace_window, 0, sizeof(frametrace_window));
	frametrace_window.beat_work_start = psp_heartbeat_work_us;
}

/* a frame has ended, from psp_frame_account (psp_platform.c): its drawing's
time, the ticks run since the last frame, and its end's time */
void psp_frametrace_frame(unsigned long draw_us, unsigned long ticks, unsigned long end_us)
{
	struct frametrace_frame f;
	unsigned long now, stick;
	int k;

	if (frametrace_on < 0)
	{
		frametrace_on = psp_keep("frametrace");
		if (frametrace_on)
		{
			frametrace_bins = calloc(FRAMETRACE_BINS, sizeof(*frametrace_bins));
			if (!frametrace_bins)
			{
				platform_log("frametrace: no room for its %u KB; off", (unsigned)(FRAMETRACE_BINS * sizeof(*frametrace_bins) / 1024));
				frametrace_on = 0;
			}
			else
				platform_log("frametrace: every frame timed (a line each %d frames)", FRAMETRACE_WINDOW);
		}
		/* the first frame starts the clock */
		frametrace_last_end = psp_heartbeat_clock();
		frametrace_last_stick = psp_stick_read_us;
		frametrace_tick_us = 0;
		return;
	}
	if (!frametrace_on)
		return;
	now = psp_heartbeat_clock();
	stick = psp_stick_read_us;
	f.wall = now - frametrace_last_end;
	f.tick = frametrace_tick_us;
	f.draw = draw_us;
	f.end = end_us;
	f.stick = stick - frametrace_last_stick;
	f.ticks = ticks;
	frametrace_last_end = now;
	frametrace_last_stick = stick;
	frametrace_tick_us = 0;

	frametrace_bins[f.wall / 1000 < FRAMETRACE_BINS ? f.wall / 1000 : FRAMETRACE_BINS - 1]++;
	frametrace_run_frames++;
	frametrace_window.frames++;
	frametrace_window.wall += f.wall;
	frametrace_window.ticks += f.ticks;
	if (f.wall > FRAMETRACE_TWO_TICKS_US)
		frametrace_window.over++;
	{
		unsigned long spans = psp_heartbeat_spans;
		int beat = psp_heartbeat_busy || spans != frametrace_spans_seen;

		frametrace_spans_seen = spans;
		if (f.wall > FRAMETRACE_LATE_US)
		{
			frametrace_window.late++;
			frametrace_window.late_beat += beat;
			frametrace_window.late_held += f.tick + f.draw + f.end < FRAMETRACE_HELD_US;
			frametrace_window.late_stick += f.stick > 0;
			if (frametrace_late_seen)
			{
				if (frametrace_window.gaps < FRAMETRACE_GAPS)
					frametrace_window.gap[frametrace_window.gaps] = now - frametrace_last_late;
				frametrace_window.gaps++;
			}
			frametrace_last_late = now;
			frametrace_late_seen = 1;
		}
	}
	/* the window's slowest frames, slowest first */
	for (k = 0; k < FRAMETRACE_SLOWEST; k++)
		if (f.wall > frametrace_window.slowest[k].wall)
		{
			memmove(&frametrace_window.slowest[k + 1], &frametrace_window.slowest[k],
				(FRAMETRACE_SLOWEST - 1 - k) * sizeof(f));
			frametrace_window.slowest[k] = f;
			break;
		}
	if (frametrace_window.frames >= FRAMETRACE_WINDOW)
		frametrace_report();
}
