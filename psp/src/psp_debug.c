/* psp_debug.c - the in-game debug menu (build 77).

SELECT + LEFT opens it over the game; SELECT closes it. While it is open the
game gets no input (the player stands still; the game keeps running), the
D-pad moves (up/down) and changes the value (left/right), and cross steps
the value on. Every change is logged with the frame time, so a run's log
lines up with what was seen.

The text is drawn by the CPU into the finished frame (after the GE is done
with it and after any screenshot, before the buffer is shown), with the
debug font of pspdebug: no GE state is touched, screenshots never show it,
and the temporal blend's history (taken before the HUD) never holds it. */
#include <pspkernel.h>
#include <pspctrl.h>
#include <pspdebug.h>
#include <pspdisplay.h>
#include <stdio.h>
#include <string.h>

void platform_log(const char *format, ...);
float psp_me_busy_percent(void);
int psp_ge_setting(int which, int set, int value);
long psp_game_time_peek(void);
/* the last heartbeat's budget (psp_platform.c): wall ms a frame, fps, ticks
ms, frame ms, the GE renderer's CPU ms, waiting for the GE ms, ticks a second */
extern float psp_budget_last[7];

#define SETTING_DETAIL 0
#define SETTING_NUDGE 1
#define SETTING_PASSES 2
#define SETTING_NEAR 3
#define SETTING_BLEND 4
#define SETTING_JITTER 5
#define SETTING_ADAPTIVE 6
#define SETTING_SOFTEN 7
#define SETTING_GESERIAL 9
#define SETTING_UVCENTRE 10
#define SETTING_FLOOR 11
#define SETTING_LIFT_SHARE 12
#define SETTING_LIFT_CEILING 13
#define SETTING_SHADOW 14
#define SETTING_LIGHT 15
#define SETTING_STENCILS 16
#define SETTING_COMPACT 17
#define SETTING_DECALS 18
#define SETTING_DETAIL_LEVEL 19
#define LOCAL_OVERLAY 100
#define LOCAL_AUDIO 101
/* psp_dsound.c (build 86): 0 the main CPU mixes the sound, 1 the Media Engine */
int psp_audio_setting(int set, int value);

struct item
{
	const char *label;
	int setting;
	int count;
	const char *const *names;
};

static const char *const off_on[2] = { "off", "on" };
static const char *const blend_names[4] = { "off", "25%", "37%", "50%" };
static const char *const soften_names[4] = { "off", "light", "medium", "strong" };
static const char *const pass_names[5] = { "all", "base only", "base+lightmap", "lightmap on white", "base+detail" };
static const char *const near_names[4] = { "auto", "x4", "x2", "x1" };
static const char *const lift_names[5] = { "off", "1%", "2%", "4%", "8%" };
static const char *const floor_names[4] = { "0", "16", "64", "256" };
static const char *const share_names[4] = { "0.5%", "1%", "2%", "4%" };
static const char *const ceiling_names[4] = { "0.1", "0.2", "0.4", "0.8" };
/* values fit the menu's 13 columns (the debug font's 60 a row) */
static const char *const shadow_names[3] = { "copy, EQUAL", "wall, EQUAL", "GEQUAL nudge" };
static const char *const light_names[4] = { "copy, EQUAL", "level array", "GEQUAL nudge", "off" };
static const char *const mixer_names[2] = { "main CPU", "Media Engine" };
static const char *const stencil_names[3] = { "baked, EQUAL", "baked, +2", "old nudge" };
static const char *const compact_names[2] = { "float", "16-bit" };
static const char *const decal_names[2] = { "nudged (old)", "on the wall" };
static const char *const detail_level_names[2] = { "slant per triangle (old)", "by distance" };

static const struct item items[] =
{
	{ "Soften (spatial AA)", SETTING_SOFTEN, 4, soften_names },
	{ "Blend (temporal; off w/ soften)", SETTING_BLEND, 4, blend_names },
	{ "  jitter", SETTING_JITTER, 2, off_on },
	{ "  adaptive (less when turning)", SETTING_ADAPTIVE, 2, off_on },
	{ "Performance overlay", LOCAL_OVERLAY, 2, off_on },
	{ "UV centring (ground wobble)", SETTING_UVCENTRE, 2, off_on },
	{ "GE serial (busy-time test)", SETTING_GESERIAL, 2, off_on },
	{ "Detail maps", SETTING_DETAIL, 2, off_on },
	{ "Decal lift limit (% of dist)", SETTING_NUDGE, 5, lift_names },
	{ "Stencil lift (% of distance)", SETTING_LIFT_SHARE, 4, share_names },
	{ "Stencil lift ceiling (units)", SETTING_LIFT_CEILING, 4, ceiling_names },
	{ "Nudge floor (depth steps, test)", SETTING_FLOOR, 4, floor_names },
	{ "Stencils (wall decorations)", SETTING_STENCILS, 3, stencil_names },
	{ "Painted symbols (decals)", SETTING_DECALS, 2, decal_names },
	{ "Detail map level", SETTING_DETAIL_LEVEL, 2, detail_level_names },
	{ "Level vertices (test)", SETTING_COMPACT, 2, compact_names },
	{ "Light pass (test)", SETTING_LIGHT, 4, light_names },
	{ "Audio mixer (needs meaudio)", LOCAL_AUDIO, 2, mixer_names },
	{ "Shadow pass (test)", SETTING_SHADOW, 3, shadow_names },
	{ "Level passes", SETTING_PASSES, 5, pass_names },
	{ "Near plane", SETTING_NEAR, 4, near_names },
};
#define ITEM_COUNT ((int)(sizeof(items) / sizeof(items[0])))

static int menu_open = 0, cursor = 0, overlay = -1, swallowing = 0, select_released = 0;
int psp_keep(const char *name);
int psp_keep_number(const char *prefix, int fallback);
static unsigned int last_buttons = 0;
static int screen_ready = 0;
static unsigned long long fps_mark = 0, tps_mark = 0, launch_mark = 0;

/* build 83: seconds since the game started drawing, the overlay's T and
the log's [t] on the lines the menu and screenshots write */
float psp_debug_seconds(void)
{
	unsigned long long now = sceKernelGetSystemTimeWide();

	if (!launch_mark)
		launch_mark = now;
	return (float)(now - launch_mark) / 1e6f;
}
static float fps_smooth = 0.0f, tps_smooth = 0.0f;
static long tps_last_tick = -1;

static int value_of(const struct item *item)
{
	if (item->setting == LOCAL_OVERLAY)
	{
		/* build 82: on unless the launch menu's "nooverlay" (build 114: or "lean") */
		if (overlay < 0)
			overlay = !psp_keep("nooverlay") && !psp_keep("lean");
		return overlay;
	}
	if (item->setting == LOCAL_AUDIO)
		return psp_audio_setting(0, 0);
	return psp_ge_setting(item->setting, 0, 0);
}

static void change(const struct item *item, int step)
{
	int value = (value_of(item) + step + item->count) % item->count;

	if (item->setting == LOCAL_OVERLAY)
		overlay = value;
	else if (item->setting == LOCAL_AUDIO)
		psp_audio_setting(1, value);
	else
		psp_ge_setting(item->setting, 1, value);
	platform_log("[t %.1f] debug menu: %s = %s (%.1f fps, %.1f ms a frame at the last heartbeat)", psp_debug_seconds(),
		item->label, item->names[value], psp_budget_last[1], psp_budget_last[0]);
}

/* the pad, every poll (psp_sdl.c): non-zero when the menu takes the input,
and the game should see an idle pad */
int psp_debug_input(unsigned int buttons)
{
	unsigned int pressed = buttons & ~last_buttons;

	last_buttons = buttons;
	if (!menu_open)
	{
		if (swallowing)
		{
			/* after closing, nothing reaches the game until every button
			is let go (a held select would read as a zoom tap) */
			if (buttons & (PSP_CTRL_SELECT | PSP_CTRL_LEFT | PSP_CTRL_RIGHT | PSP_CTRL_UP | PSP_CTRL_DOWN | PSP_CTRL_CROSS))
				return 1;
			swallowing = 0;
			return 0;
		}
		if ((buttons & PSP_CTRL_SELECT) && (pressed & PSP_CTRL_LEFT))
		{
			menu_open = 1;
			select_released = 0;
			platform_log("[t %.1f] debug menu: opened", psp_debug_seconds());
			return 1;
		}
		return 0;
	}
	if (!(buttons & PSP_CTRL_SELECT))
		select_released = 1;
	if (select_released && (pressed & PSP_CTRL_SELECT))
	{
		menu_open = 0;
		swallowing = 1;
		platform_log("[t %.1f] debug menu: closed", psp_debug_seconds());
		return 1;
	}
	if (pressed & PSP_CTRL_UP)
		cursor = (cursor + ITEM_COUNT - 1) % ITEM_COUNT;
	if (pressed & PSP_CTRL_DOWN)
		cursor = (cursor + 1) % ITEM_COUNT;
	if (pressed & (PSP_CTRL_RIGHT | PSP_CTRL_CROSS))
		change(&items[cursor], 1);
	if (pressed & PSP_CTRL_LEFT)
		change(&items[cursor], -1);
	return 1;
}

int psp_debug_menu_open(void)
{
	return menu_open;
}

static void text(int column, int row, unsigned int colour, const char *string)
{
	pspDebugScreenSetTextColor(colour);
	pspDebugScreenSetXY(column, row);
	pspDebugScreenPuts(string);
}

/* the finished frame, before it is shown (psp_ge.c): vram is the draw
buffer through the uncached view */
void psp_debug_draw(void *vram)
{
	char line[96];
	unsigned long long now = sceKernelGetSystemTimeWide();
	int heap_warning;

	/* the frame rate, smoothed over about half a second */
	if (fps_mark)
	{
		float ms = (float)(now - fps_mark) / 1000.0f;

		if (ms > 0.0f)
			fps_smooth = fps_smooth <= 0.0f ? 1000.0f / ms : fps_smooth * 0.9f + (1000.0f / ms) * 0.1f;
	}
	fps_mark = now;
	psp_debug_seconds();
	if (overlay < 0)
		overlay = !psp_keep("nooverlay") && !psp_keep("lean");
	/* build 80: game ticks a second, live: counted over windows of at least
	a quarter second (ticks come 1 or 2 a frame), smoothed like the fps */
	{
		long tick = psp_game_time_peek();

		if (tps_last_tick < 0 || tick < tps_last_tick)
		{
			tps_last_tick = tick;
			tps_mark = now;
		}
		else if (now - tps_mark >= 250000)
		{
			float rate = (float)(tick - tps_last_tick) * 1e6f / (float)(now - tps_mark);

			tps_smooth = tps_smooth <= 0.0f ? rate : tps_smooth * 0.5f + rate * 0.5f;
			tps_last_tick = tick;
			tps_mark = now;
		}
	}
	{
		/* a full heap shows with the overlay, which a test build starts with:
		allocations that fail from the moment a map loads would otherwise
		show only when the controls died. A release starts without the
		overlay, and a player gets no warning to act on; the log has it */
		extern volatile unsigned long psp_heap_failures;

		heap_warning = psp_heap_failures > 0 && overlay;
	}
	if (!menu_open && !overlay && !heap_warning)
		return;
	if (!screen_ready)
	{
		/* set up on the depth buffer: the call clears what it is given, and
		that buffer is cleared every frame anyway. Build 80: at its ABSOLUTE
		address. b77-b79 took it relative to the current draw buffer, and
		when that was the second one the clear landed 256 KB further on,
		over the start of the VRAM texture cache: the HUD's textures, which
		are loaded once, went purple for the rest of the run (b79's runs
		1 and 2; 50/50 by which buffer was current when the menu first opened) */
		pspDebugScreenInitEx((void *)(0x44000000 + 2 * 512 * 272 * 2), PSP_DISPLAY_PIXEL_FORMAT_565, 0);
		pspDebugScreenClearLineDisable();
		pspDebugScreenEnableBackColor(1);
		pspDebugScreenSetBackColor(0xFF202020);
		screen_ready = 1;
	}
	pspDebugScreenSetBase((u32 *)vram);
	if (overlay)
	{
		/* build 80: MangoHud-style, across the top, left to right. The debug
		font is 8 px (60 columns), so it takes two rows; rows 0 and 1 lie
		above the HUD's ammo and shield boxes. Live: fps, game ticks a
		second, frame time. From the last heartbeat: the tick's ms a frame,
		the render's, the port renderer's share of it, the wait for the GE */
		/* build 87: the Media Engine's busy share (psp_me.c, last heartbeat) */
		snprintf(line, sizeof(line), "FPS %4.1f  TPS %4.1f  FRAME %5.1f MS  ME %2.0f%%  T %.1f", fps_smooth, tps_smooth,
			fps_smooth > 0.0f ? 1000.0f / fps_smooth : 0.0f, psp_me_busy_percent(), psp_debug_seconds());
		text(0, 0, 0xFF80FF80, line);
		snprintf(line, sizeof(line), "TICK %4.1f MS   REND %4.1f MS   PORT %4.1f MS   WAIT %4.1f MS", psp_budget_last[2],
			psp_budget_last[3], psp_budget_last[4], psp_budget_last[5]);
		text(0, 1, 0xFF80FF80, line);
		/* a test job's runs numbered ("rigrunN rigofM"), so whoever
		watches the PSP knows which of a job's runs is playing */
		{
			static int run = -1, runs = 0;

			if (run < 0)
			{
				run = psp_keep_number("rigrun", 0);
				runs = psp_keep_number("rigof", 0);
			}
			if (run > 0)
			{
				snprintf(line, sizeof(line), "RUN %d/%d", run, runs);
				text(0, 3, 0xFFFFFF80, line);
			}
		}
	}
	if (heap_warning)
	{
		extern volatile unsigned long psp_heap_failures;

		snprintf(line, sizeof(line), "HEAP FULL: %lu requests failed (see the log)", psp_heap_failures);
		text(0, 2, 0xFF4040FF, line);
	}
	if (menu_open)
	{
		int index, row = 8;

		text(12, row++, 0xFF40FFFF, "  DEBUG MENU  (select closes)                 ");
		text(12, row++, 0xFFC0C0C0, "  up/down choose, left/right/cross change     ");
		for (index = 0; index < ITEM_COUNT; index++)
		{
			const struct item *item = &items[index];

			snprintf(line, sizeof(line), "%c %-31s %-13s", index == cursor ? '>' : ' ', item->label,
				item->names[value_of(item)]);
			text(12, row++, index == cursor ? 0xFF40FFFF : 0xFFFFFFFF, line);
		}
		snprintf(line, sizeof(line), "  %5.1f fps, %4.1f ticks/s                      ", fps_smooth, tps_smooth);
		text(12, row++, 0xFFC0C0C0, line);
	}
}
