/* psp_loading.c: the load between the menu and a level, and between
levels, as the loading screen sees it (docs/plan_loading_screen.md).

A load on the PSP is one blocking chain on the main thread: the tag data in
one read, then the BSP, the stencils and the texture pack's table. Nothing
is drawn meanwhile, so the screen holds the menu's faded last frame. This
unit keeps the load's progress by bytes (psp_fixups.py FIXES_LOADING splits
the tag read into pieces and reports each one), times the load's parts for
the log, plays the Xbox's chant over it, and hands the GE layer a view to
draw at most once a frame. After the load, the settle draws the level's
first view with no tick until its textures are in. The drawing itself is
the GE layer's (psp_ge_loading_*, linked weakly): without it the screen
holds as before, with the chant, the settle and the timings.
The same screen covers the start-up, from keep.txt's reading to the main
menu ("the boot screen", below).
Keep words: "loadingoff" (none of it), "chantoff", "chantheap" (the chant's voices
from the heap: psp_dsound.c), "settleoff", "settleN", "loadholdN" (the screen's
shortest showing, 6 s by default; "loadhold0" none); the boot screen's "bootlog",
"boothintN", "bootsettleoff". */
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "psp_loading.h"
#include "psp_paths.h" /* PSP_DATA_ROOT: ms0:/halo or ms0:/halomp */
#include "psp_metrics.h"
#include <pspiofilemgr.h>
#include <pspkernel.h>

/* the loading screen's pacing, settle and input release run on a clock no
keep word turns off (psp_microseconds answers 0 under notimers, lean and in
a release) */
extern unsigned long psp_heartbeat_clock(void);
extern void platform_log(const char *format, ...);
extern int psp_keep(const char *name);
extern int psp_keep_number(const char *prefix, int fallback);
extern unsigned long psp_tex_load_count(void);
extern unsigned long psp_tex_untextured_frames(void);
extern unsigned long psp_ge_draws_shown(void);
/* "heapmap" (psp_libc.c) and "poolpeaks" (psp_fixups.py FIXES_RECON
#poolpeaks, so linked weakly): measured where a level ends and begins */
extern void psp_heap_map(const char *label);
extern void psp_pool_peaks_report(const char *label, int level_end) __attribute__((weak));

static struct
{
	int active;
	int screen;               /* drawing asked for (not "loadingoff") */
	int boot;                 /* the start-up's load (from init.txt, or the main menu's) */
	char map[64];
	unsigned long begin_us;
	unsigned long last_draw_us;
	unsigned long expected;   /* bytes the load says it will read */
	unsigned long done;
	unsigned long draws;
	float shown;              /* the progress as drawn, eased toward the true one */
	unsigned long settle_us;  /* the settle's start */
	unsigned long settle_cap_us;
	unsigned long settle_loads; /* the texture cache's count at the last settle frame */
	unsigned long settle_loads_first; /* and at the settle's start */
	unsigned long settle_untextured;  /* frames that drew something untextured, at the last settle frame */
	unsigned long settle_draws_first; /* draws shown, at the settle's start */
	int settle_frames;
	int settle_quiet;         /* settle frames in a row that read no texture */
	unsigned long settle_not_before_us; /* the settle lasts at least to here (0: no such end) */
	unsigned long hold_us;    /* the screen's shortest showing, from begin_us (0: none) */
} loading;

/* the boot screen's state (below); "active" from the program's start until
the start-up's load, or the main menu's settle, ends */
static struct
{
	int active;
	volatile int drawing;     /* the boot screen's thread has the screen */
	volatile int stop;
	SceUID thread;
	unsigned long start_us;
	unsigned long hint_us;    /* the hint shows this long from start_us (0: not at all) */
	unsigned long frames;
	int loaded;               /* the start-up's load has ended: its progress is whole */
	int settling;             /* the main menu's first frames under the veil, its input held */
	float shown;              /* the progress as the boot screen's thread drew it */
} boot;

static const char *boot_hint(unsigned long now);

/* the frame shown when the load began, kept where the GE layer chooses
(the shown buffer may simply stay in VRAM): no static copy here, since a
480 x 272 frame would cost the heap 255 KB in every level */
static const void *captured;

static float ease(float shown, float target, float seconds)
{
	/* toward the true progress at a rate set in time, not in frames: the
	Xbox moved a fifth of the way each frame at 30 fps */
	float step = seconds * 6.0f;

	if (step > 1.0f)
		step = 1.0f;
	return shown + (target - shown) * step;
}

static float brightness_of(float progress)
{
	/* the Xbox's sin(progress * pi)^0.9, without libm's pow on the
	hot side of a load: x^0.9 ~= x * (1.1 - 0.1x) on 0..1 is within
	0.012 of it, finer than the 16-bit frame can show */
	float x = progress;
	float s;

	if (x <= 0.0f || x >= 1.0f)
		return 0.0f;
	/* sin(pi x) by its 4th-order Bhaskara form: within 0.002 */
	s = 16.0f * x * (1.0f - x) / (5.0f - 4.0f * x * (1.0f - x));
	return s * (1.1f - 0.1f * s);
}

/* The chant: the Xbox's four looping copies of one tone at four pitches
(progress_bar.c's SetPitch -4681, -6386, -3999, -8773, in 1/4096 octave from
48 kHz), each swelling over its own part of the load. The tone is the
game's own (Xbox ADPCM at 22,050 Hz in default.xbe), so the EBOOT carries no
copy: the bake reads it from the player's disc into halo/chant.adp
(bake/lib/chant.py), and the screen is silent without it. Static, so a load
takes no heap block for it (the holes mem138 closed); written back before
the Media Engine mixes it */
#define CHANT_VOICES 4
#define CHANT_BYTES 18396
static unsigned char chant_tone[CHANT_BYTES] __attribute__((aligned(64)));
static int chant_tone_state; /* 0 unread, 1 read, -1 missing */
extern void *psp_dsound_loop_start(const void *adpcm, unsigned long bytes, unsigned long sample_rate,
	unsigned long frequency, long millibels);
extern void psp_dsound_loop_volume(void *voice, long millibels);
extern void psp_dsound_loop_stop(void *voice);

static const unsigned long chant_frequency[CHANT_VOICES] = { 21738, 16290, 24397, 10876 };
static const float chant_range[CHANT_VOICES][2] = { { 0.0f, 1.0f }, { 0.4f, 1.0f }, { 0.5f, 1.0f }, { 0.55f, 1.0f } };
static const float chant_swell[CHANT_VOICES] = { 3500.0f, 4500.0f, 3500.0f, 4500.0f };
static void *chant[CHANT_VOICES];

static int chant_read(void)
{
	if (!chant_tone_state)
	{
		/* the bake writes it into halo/ only; the multiplayer build
		(ms0:/halomp) reads that copy when it has none of its own */
		SceUID file = sceIoOpen(PSP_DATA_ROOT "/chant.adp", PSP_O_RDONLY, 0);
		int got;

		if (file < 0)
			file = sceIoOpen("ms0:/halo/chant.adp", PSP_O_RDONLY, 0);
		got = file >= 0 ? sceIoRead(file, chant_tone, CHANT_BYTES) : -1;

		if (file >= 0)
			sceIoClose(file);
		chant_tone_state = got == CHANT_BYTES ? 1 : -1;
		if (chant_tone_state > 0)
			sceKernelDcacheWritebackRange(chant_tone, CHANT_BYTES);
		platform_log("loading: the chant %s", chant_tone_state > 0 ? "read from the bake (chant.adp)" :
			"is silent: chant.adp isn't in the bake (re-bake from the disc to hear it)");
	}
	return chant_tone_state > 0;
}

static void chant_start(void)
{
	int k;

	if (psp_keep("chantoff") || !chant_read())
		return;
	for (k = 0; k < CHANT_VOICES; k++)
		chant[k] = psp_dsound_loop_start(chant_tone, CHANT_BYTES, 22050, chant_frequency[k], -5000);
}

static void chant_gains(float progress)
{
	int k;

	for (k = 0; k < CHANT_VOICES; k++)
	{
		float t = 0.0f;

		if (!chant[k])
			continue;
		if (progress >= chant_range[k][0] && progress <= chant_range[k][1])
			t = (progress - chant_range[k][0]) / (chant_range[k][1] - chant_range[k][0]);
		/* the Xbox's sin(t pi)^0.3 of each voice's swell over -50 dB */
		psp_dsound_loop_volume(chant[k], (long)(powf(sinf(t * 3.1415f), 0.3f) * chant_swell[k] - 5000.0f));
	}
}

static void chant_stop(void)
{
	int k;

	for (k = 0; k < CHANT_VOICES; k++)
	{
		psp_dsound_loop_stop(chant[k]);
		chant[k] = NULL;
	}
}

/* the progress as the screen and the chant show it: while loading at most
once a frame's time; while settling once each of the game's frames. While
the boot screen's thread has the screen it draws, and eases, by itself */
static void update(int phase)
{
	struct psp_loading_view view;
	unsigned long now = psp_heartbeat_clock();
	float target;

	if (!loading.screen || boot.drawing)
		return;
	if (phase == 0 && loading.draws && now - loading.last_draw_us < 33000)
		return;
	if (phase)
		target = 1.0f;
	else
		target = loading.expected ? (float)loading.done / (float)loading.expected : 0.0f;
	/* a load faster than the hold shows its progress by the clock, so the
	chant's crescendo has its length */
	if (loading.hold_us && target > (float)(now - loading.begin_us) / (float)loading.hold_us)
		target = (float)(now - loading.begin_us) / (float)loading.hold_us;
	if (target > 1.0f)
		target = 1.0f;
	loading.shown = ease(loading.shown, target, loading.draws ? (now - loading.last_draw_us) / 1e6f : 0.0f);
	loading.last_draw_us = now;
	loading.draws++;
	chant_gains(loading.shown);
	view.progress = loading.shown;
	view.brightness = brightness_of(loading.shown);
	view.phase = phase;
	view.captured = captured;
	view.notice = boot_hint(now);
	if (phase == 0 && psp_ge_loading_frame)
		psp_ge_loading_frame(&view);
	if (phase == 1 && psp_ge_loading_veil)
		psp_ge_loading_veil(&view);
}

static void loading_begin(const char *map_name, int with_chant)
{
	char label[80];

	/* before the old level is let go */
	snprintf(label, sizeof(label), "leaving for %s", map_name ? map_name : "?");
	if (psp_pool_peaks_report)
		psp_pool_peaks_report(label, 1);
	PSP_METRICS_HEAP_MAP_LEAVING(label);
	memset(&loading, 0, sizeof(loading));
	loading.active = 1;
	loading.screen = !psp_keep("loadingoff");
	loading.boot = boot.active;
	loading.begin_us = psp_heartbeat_clock();
	/* the screen's shortest showing (the Xbox's chant needs a few
	seconds, and the PSP loads a level in one or two), the load and the
	settle hidden behind it; not at boot, whose screen has its own pace.
	"loadholdN" sets N seconds, "loadhold0" shows it only as long as the load */
	loading.hold_us = loading.screen && !loading.boot ? (unsigned long)psp_keep_number("loadhold", 6) * 1000000ul : 0;
	strncpy(loading.map, map_name ? map_name : "?", sizeof(loading.map) - 1);
	{
		extern void psp_ge_stencils_expect(const char *map_name);

		psp_ge_stencils_expect(map_name);
	}
	/* the boot screen's thread draws the start-up's load, from black */
	captured = loading.screen && !boot.drawing && psp_ge_loading_capture ? psp_ge_loading_capture() : NULL;
	if (loading.screen && with_chant)
		chant_start();
	platform_log("loading: %s begins%s%s", loading.map,
		!loading.screen ? " (loadingoff: no screen)" : psp_ge_loading_frame ? "" : " (no screen in this build)",
		boot.drawing ? " (under the boot screen)" : "");
}

void psp_loading_begin(const char *map_name)
{
	loading_begin(map_name, 1);
}

void psp_loading_expect(unsigned long bytes)
{
	if (loading.active)
		loading.expected += bytes;
}

void psp_loading_progress(unsigned long bytes)
{
	if (!loading.active)
		return;
	loading.done += bytes;
	update(0);
}

int psp_loading_active(void)
{
	return loading.active;
}

/* The settle: once the level is loaded, its first view is drawn with no tick
until the texture cache stops filling (three frames in a row with nothing
read) or the cap passes, under the GE layer's veil. The textures the opening
needs are then read while the game stands still, rather than while it
plays with surfaces missing and frames slow. main.c's psp_main_settle
draws the frames (psp_fixups.py FIXES_LOADING); the engine's own frame-lock
reset after a load (main_start_time) drops the time they took.
Keep words: "settleoff" (none), "settleN" (cap N seconds, 3 by default). */
static int settle_start(unsigned long not_before_us)
{
	int cap = psp_keep_number("settle", 3);

	if (cap <= 0)
		return 0;
	loading.settle_cap_us = (unsigned long)cap * 1000000ul;
	loading.settle_us = psp_heartbeat_clock();
	/* the cap stops a settle that never warms; a shortest showing (the boot's
	hint) stretches it */
	loading.settle_not_before_us = not_before_us;
	if (not_before_us && (long)(not_before_us - (loading.settle_us + loading.settle_cap_us)) > 0)
		loading.settle_cap_us = not_before_us - loading.settle_us;
	loading.settle_loads = loading.settle_loads_first = psp_tex_load_count();
	loading.settle_untextured = psp_tex_untextured_frames();
	loading.settle_draws_first = psp_ge_draws_shown();
	loading.settle_frames = 0;
	loading.settle_quiet = 0;
	return 1;
}

int psp_loading_settle_begin(void)
{
	if (!loading.active || psp_keep("settleoff"))
		return 0;
	return settle_start(loading.hold_us ? loading.begin_us + loading.hold_us : 0);
}

int psp_loading_settle_frame(void)
{
	static int warm = -1;
	unsigned long count = psp_tex_load_count(), now = psp_heartbeat_clock();
	unsigned long untextured = psp_tex_untextured_frames(), draws = psp_ge_draws_shown();
	int still;

	/* warm takes more than no reads (B1: at game time 0 a level settle read
	none and handed over after 4 frames): no frame drawing something
	untextured either, and something drawn at all. The counts are of the
	frames shown, so they trail the frame just drawn by one.
	"settlewarmoff": reads alone, as before */
	if (warm < 0)
		warm = !psp_keep("settlewarmoff");
	loading.settle_frames++;
	still = count == loading.settle_loads && (!warm || untextured == loading.settle_untextured);
	loading.settle_quiet = still ? loading.settle_quiet + 1 : 0;
	loading.settle_loads = count;
	loading.settle_untextured = untextured;
	/* warm, and the screen's progress (and the chant) played out to its end */
	if ((loading.settle_frames >= 4 && loading.settle_quiet >= 3 && (!warm || draws != loading.settle_draws_first) &&
		(!loading.screen || loading.shown >= 0.99f) &&
		(!loading.settle_not_before_us || (long)(now - loading.settle_not_before_us) >= 0)) ||
		now - loading.settle_us >= loading.settle_cap_us)
	{
		platform_log("loading: %s settled in %lu ms, %d frames, %lu texture reads and waits, %lu draws shown%s", loading.map,
			(now - loading.settle_us) / 1000, loading.settle_frames, count - loading.settle_loads_first,
			draws - loading.settle_draws_first,
			loading.settle_quiet < 3 ? " (at the cap, still reading)" :
			draws == loading.settle_draws_first ? " (at the cap, nothing drawn)" : "");
		return 0;
	}
	return 1;
}

void psp_loading_settle_veil(void)
{
	update(1);
}

void psp_loading_end(void)
{
	char label[80];

	if (!loading.active)
		return;
	platform_log("loading: %s took %lu ms (%lu of %lu bytes counted, %lu frames drawn)", loading.map,
		(psp_heartbeat_clock() - loading.begin_us) / 1000, loading.done, loading.expected, loading.draws);
	loading.active = 0;
	chant_stop();
	captured = NULL;
	if (loading.boot)
	{
		boot.loaded = 1;
		boot.active = 0;
		boot.settling = 0;
	}
	{
		extern void psp_ge_stencils_retry(void);

		psp_ge_stencils_retry();
	}
	/* after the time above, which it would add to, and once the chant has
	let its voices go */
	snprintf(label, sizeof(label), "%s loaded", loading.map);
	PSP_METRICS_HEAP_MAP_LOADED(label, loading.map);
}

/* ---------- the boot screen (from the hand test: the
loading screen over the start-up rather than the log scrolling; a few
seconds' hint on the launch menu while the game loads behind it; the main
menu settled before it is shown, so nothing pops in)

From keep.txt's reading (main) to the game's first frame, a thread of its
own draws the loading screen about 30 times a second: a cold start's black,
the start-up load's progress (ui.map's, or init.txt's map), and for the
first seconds the hint. The GE starts early for it (psp_ge_boot_start, with
a display list of its own); the game's thread starts up as before and draws
nothing until its first frame, whose ge_initialize stops this thread
(psp_loading_boot_screen_stop). The thread outranks the game's only so its
frames keep coming while the game's thread yields to the Memory Stick's
reader; a frame is a few hundred microseconds of CPU, the rest is sleep.

On the way to the main menu (main.c's main_setup_connection) the menu's
first frames are then drawn under the veil, as a level's settle, while its
input is held (ui_widget.c's process_ui_widgets, which the Xbox held the
same way under its own loading screen), until its textures, the showcase's
too, stop coming in, and not before the hint has had its seconds. Unlike a
level's settle the menu ticks meanwhile: its scenes and music run as before.
The level's settle stays off at boot: before main_loop's main_initialize_time
starts the port's vertical blank thread, D3DDevice_Present waits for a
second frame's flip that never comes (interpolation off: d3d8_gl.c), which
is where loading2's boots from init.txt sat at game time 0.
Keep words: "bootlog" (no boot screen: the log on the screen, as before),
"boothintN" (the hint for N seconds, 3 by default; "boothint0": none; left
out after the launch menu), "bootsettleoff" (the menu shown and taking input
at once, as before). */
#define BOOT_HINT "Hold L as the game starts for the development menu"
/* above the game's thread (0x20), below the Memory Stick's reader (0x1c) */
#define BOOT_PRIORITY 0x1f
#define BOOT_FRAME_US 30000

static const char *boot_hint(unsigned long now)
{
	return boot.active && boot.hint_us && now - boot.start_us < boot.hint_us ? BOOT_HINT : NULL;
}

/* the pause between the boot screen's frames, then, in the emulator, the end
of any Media Engine job the game's thread is running. The emulator has no
ME: the game's thread runs each job itself with psp_me_context set (psp_me.c,
ring_push_simulated), and the GE layer takes all work done meanwhile for the
ME's (PSP_ME_WORK). That word is global, so a boot frame drawn inside such a
job would have its stall updates held for the game's thread to pass on: its
list never reaches its FINISH, this thread waits on the GE for good, and the
game's first frame waits behind it. Giving the CPU up lets the job finish.
The first frame needs no wait: this thread starts it from main(), before the
ME starts. On the PSP the ME runs its own jobs and the word stays 0. (A
function of its own in the delay's place, so the boot thread keeps its
profile) */
static __attribute__((noinline)) void boot_frame_pause(void)
{
	extern volatile unsigned int psp_me_context;

	sceKernelDelayThread(BOOT_FRAME_US);
	while (psp_me_context)
		sceKernelDelayThread(100);
}

static int boot_thread(SceSize args, void *argp)
{
	extern void psp_fpu_mask_traps(void);
	unsigned long last = psp_heartbeat_clock();

	(void)args;
	(void)argp;
	/* as every thread of the game's (the Xbox ran with the FPU's traps masked) */
	psp_fpu_mask_traps();
	/* for the loading picture the bake may carry (read as a load's frame is
	captured); the frame itself is a cold start's black, so it isn't kept */
	if (psp_ge_loading_capture)
		psp_ge_loading_capture();
	while (!boot.stop)
	{
		struct psp_loading_view view;
		unsigned long now = psp_heartbeat_clock();
		float target = boot.loaded ? 1.0f :
			loading.active && loading.expected ? (float)loading.done / (float)loading.expected : 0.0f;

		if (target > 1.0f)
			target = 1.0f;
		boot.shown = ease(boot.shown, target, (now - last) / 1e6f);
		last = now;
		view.progress = boot.shown;
		view.brightness = brightness_of(boot.shown);
		view.phase = 0;
		view.captured = NULL;
		view.notice = boot_hint(now);
		psp_ge_loading_frame(&view);
		boot.frames++;
		boot_frame_pause();
	}
	return 0;
}

/* main(), keep.txt read */
void psp_loading_boot_begin(void)
{
	extern int psp_front_door_launch_menu(void);
	int hint = psp_keep_number("boothint", 3);

	memset(&boot, 0, sizeof(boot));
	boot.active = 1;
	boot.start_us = psp_heartbeat_clock();
	/* whoever came through the launch menu knows the way to it */
	boot.hint_us = hint > 0 && !psp_front_door_launch_menu() ? (unsigned long)hint * 1000000ul : 0;
	if (psp_keep("bootlog") || !psp_ge_boot_start || !psp_ge_loading_frame)
	{
		platform_log("boot screen: off (%s): the log on the screen", psp_keep("bootlog") ? "bootlog" : "not in this build");
		return;
	}
	boot.thread = sceKernelCreateThread("halo boot screen", boot_thread, BOOT_PRIORITY, 0x4000,
		PSP_THREAD_ATTR_USER | PSP_THREAD_ATTR_VFPU, NULL);
	if (boot.thread < 0)
	{
		platform_log("boot screen: no thread (%08x): the log on the screen", (unsigned int)boot.thread);
		return;
	}
	if (!psp_ge_boot_start())
	{
		sceKernelDeleteThread(boot.thread);
		return;
	}
	boot.drawing = 1;
	sceKernelStartThread(boot.thread, 0, NULL);
}

/* main.c's boot path, before the main menu's load: the menu's own music
starts as it ends, so no chant */
void psp_loading_boot_menu_begin(void)
{
	loading_begin("levels\\ui\\ui", 0);
}

/* and after it: the menu's first frames under the veil until it's warm */
void psp_loading_boot_loaded(void)
{
	if (!loading.active)
		return;
	if (psp_keep("bootsettleoff") || !settle_start(boot.hint_us ? boot.start_us + boot.hint_us : 0))
	{
		psp_loading_end();
		return;
	}
	boot.settling = 1;
}

static void boot_settled(void)
{
	platform_log("boot: the main menu shown %lu ms after the boot screen's start (its input also waits for the saved-game check)",
		(psp_heartbeat_clock() - boot.start_us) / 1000);
	psp_loading_end();
}

/* main_loop, each frame the game draws, before it is shown */
/* the main menu's veil, in its own frame: main_loop calls this between the
frame's render and its account (psp_fixups.py, FIXES_LOADING #bootscreen).
After the frame's end the list is finished or the Media Engine's, so the
veil drawn there found the frame shut and drew nothing (B1: the ring orange
in the menu's first frames). "settleveiloff": after, as before */
static int boot_veil_early(void)
{
	static int early = -1;

	if (early < 0)
		early = !psp_keep("settleveiloff");
	return early;
}

void psp_loading_boot_veil(void)
{
	if (boot.settling && boot_veil_early())
		update(1);
}

void psp_loading_boot_frame(void)
{
	if (!boot.settling)
		return;
	if (!boot_veil_early())
		update(1);
	if (!psp_loading_settle_frame())
		boot_settled();
}

/* ui_widget.c's process_ui_widgets: the menu takes no input while settling */
int psp_loading_holds_input(void)
{
	/* a settle whose frames stopped coming lets go a second past its cap */
	if (boot.settling && psp_heartbeat_clock() - loading.settle_us > loading.settle_cap_us + 1000000ul)
	{
		platform_log("boot: the main menu's settle drew no frames to its cap; its input let go");
		boot_settled();
	}
	return boot.settling;
}

/* ge_initialize, at the game's first frame: the game's thread draws from here */
void psp_loading_boot_screen_stop(void)
{
	SceUInt wait = 1000000;

	if (!boot.drawing)
		return;
	boot.stop = 1;
	if (sceKernelWaitThreadEnd(boot.thread, &wait) < 0)
	{
		platform_log("boot screen: its thread didn't end within a second; ended");
		sceKernelTerminateThread(boot.thread);
	}
	sceKernelDeleteThread(boot.thread);
	boot.drawing = 0;
	/* the load's progress goes on from where the boot screen showed it */
	if (loading.active)
	{
		loading.shown = boot.shown;
		loading.last_draw_us = psp_heartbeat_clock();
		loading.draws += boot.frames;
	}
	platform_log("boot screen: %lu frames in %lu ms; the game's frames from here", boot.frames,
		(psp_heartbeat_clock() - boot.start_us) / 1000);
}
