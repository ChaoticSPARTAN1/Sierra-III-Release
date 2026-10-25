/* psp_bakecheck.c - whether the maps on the stick are the bake this build
reads (go_live 3.4)

Players bake their own maps with the bundle released beside each build
(bake/README.md). A build reading maps from another bundle's format would
draw them wrong or crash somewhere far from the cause, so before anything
else the game reads the bake's stamp, ms0:/halo/bake.txt (bake.py's
write_stamp: "format: N", "bundle: b139", the commit and the date), and
compares its format with the one this build was made for (PSP_BAKE_FORMAT,
bake.py's BAKE_FORMAT, passed in by psp_build.py). On a mismatch, a missing
stamp (bakes from before the stamp) or no maps at all, the player sees
plainly what to do, the log gets the same lines, and START quits to the XMB.
Both builds read the maps in ms0:/halo (psp_paths.h), so one stamp serves
both.

Keep word "bakeany": the check is skipped (test bakes made before the
stamp). A scripted run (input.txt: the emulator, test hardware) shows the screen,
saves it as shots/bakecheck.bmp and quits a few seconds later, so the run
ends rather than waiting for a button. */
#include <pspctrl.h>
#include <pspdebug.h>
#include <pspdisplay.h>
#include <pspiofilemgr.h>
#include <pspkernel.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "psp_paths.h"

#ifndef PSP_BAKE_FORMAT
#error "PSP_BAKE_FORMAT comes from bake/bake.py's BAKE_FORMAT (psp_build.py)"
#endif

#define STAMP_PATH PSP_MAPS_HOME "/bake.txt"
/* the debug screen's own buffer, as the launch menu's: the GE isn't up yet */
#define SCREEN_VRAM 0x44000000u

void platform_log(const char *format, ...);
int psp_keep(const char *name);
int psp_build_number(void);
void psp_screen_log_freeze(void);
void psp_exit_game(const char *why);

struct stamp
{
	int found;    /* the file could be read */
	int has_format;
	int format;
	char bundle[32];
	char commit[16];
	char baked[24];
};

static int file_exists(const char *path)
{
	SceIoStat status;

	return sceIoGetstat(path, &status) >= 0;
}

/* a value as the screen can show it: printable, at most size - 1 characters */
static void copy_value(char *to, int size, const char *from)
{
	int n = 0;

	while (*from == ' ')
		from++;
	while (*from && *from != '\r' && *from != '\n' && n < size - 1)
	{
		to[n++] = *from >= ' ' && *from < 127 ? *from : '?';
		from++;
	}
	while (n > 0 && to[n - 1] == ' ')
		n--;
	to[n] = 0;
}

static void read_stamp(struct stamp *s)
{
	char text[1024], *line;
	SceUID file;
	int length;

	memset(s, 0, sizeof(*s));
	file = sceIoOpen(STAMP_PATH, PSP_O_RDONLY, 0);
	if (file < 0)
		return;
	length = sceIoRead(file, text, sizeof(text) - 1);
	sceIoClose(file);
	if (length < 0)
		return;
	text[length] = 0;
	s->found = 1;
	for (line = text; line && *line; line = strchr(line, '\n') ? strchr(line, '\n') + 1 : NULL)
	{
		if (strncmp(line, "format:", 7) == 0)
		{
			s->has_format = 1;
			s->format = atoi(line + 7);
		}
		else if (strncmp(line, "bundle:", 7) == 0)
			copy_value(s->bundle, sizeof(s->bundle), line + 7);
		else if (strncmp(line, "commit:", 7) == 0)
			copy_value(s->commit, 8, line + 7); /* seven characters: the short form */
		else if (strncmp(line, "baked:", 6) == 0)
			copy_value(s->baked, sizeof(s->baked), line + 6);
	}
	if (!s->bundle[0])
		strcpy(s->bundle, "unknown");
}

/* the screen as a 480 x 272 24-bit BMP, read from the 16-bit debug screen
(as psp_crash.c's crashdraw shot; kept apart so neither unit's code moves) */
static void save_shot(const char *path)
{
	static unsigned char row[480 * 3];
	unsigned char header[54];
	unsigned int image = 480 * 3 * 272, size = 54 + image, value;
	const volatile unsigned short *frame = (const volatile unsigned short *)SCREEN_VRAM;
	SceUID file;
	int x, y;

	sceIoMkdir(PSP_DATA_ROOT "/shots", 0777);
	file = sceIoOpen(path, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
	if (file < 0)
		return;
	memset(header, 0, sizeof(header));
	header[0] = 'B'; header[1] = 'M';
	memcpy(header + 2, &size, 4);
	header[10] = 54; header[14] = 40;
	value = 480; memcpy(header + 18, &value, 4);
	value = 272; memcpy(header + 22, &value, 4);
	header[26] = 1; header[28] = 24;
	memcpy(header + 34, &image, 4);
	sceIoWrite(file, header, sizeof(header));
	for (y = 271; y >= 0; y--)
	{
		for (x = 0; x < 480; x++)
		{
			unsigned int v = frame[y * 512 + x];

			row[x * 3 + 2] = (unsigned char)((v & 31) << 3);
			row[x * 3 + 1] = (unsigned char)(((v >> 5) & 63) << 2);
			row[x * 3 + 0] = (unsigned char)((v >> 11) << 3);
		}
		sceIoWrite(file, row, sizeof(row));
	}
	sceIoClose(file);
}

/* the screen's lines: the first is the heading; each also goes to the log */
#define LINES 12
static char lines[LINES][64];
static int line_count;

static void say(const char *format, ...) __attribute__((format(printf, 1, 2)));

static void say(const char *format, ...)
{
	va_list arguments;

	if (line_count >= LINES)
		return;
	va_start(arguments, format);
	vsnprintf(lines[line_count], sizeof(lines[0]), format, arguments);
	va_end(arguments);
	if (lines[line_count][0])
		platform_log("bake check: %s", lines[line_count]);
	line_count++;
}

static void show_and_quit(void)
{
	SceCtrlData pad;
	int i, row = 6, scripted = file_exists(PSP_DATA_ROOT "/input.txt");

	/* the log's own screen would otherwise keep drawing over this one */
	psp_screen_log_freeze();
	pspDebugScreenInitEx((void *)SCREEN_VRAM, PSP_DISPLAY_PIXEL_FORMAT_565, 1);
	pspDebugScreenSetBackColor(0xFF201008);
	pspDebugScreenClear();
	pspDebugScreenSetTextColor(0xFFAAAAAA);
	pspDebugScreenSetXY(4, 3);
	pspDebugScreenPrintf("Halo PSP, build %d", psp_build_number());
	for (i = 0; i < line_count; i++)
	{
		pspDebugScreenSetTextColor(i == 0 ? 0xFF40D0FF : 0xFFFFFFFF);
		pspDebugScreenSetXY(4, row);
		pspDebugScreenPuts(lines[i]);
		/* a blank row under the heading */
		row += i == 0 ? 2 : 1;
	}
	pspDebugScreenSetTextColor(0xFFAAAAAA);
	pspDebugScreenSetXY(4, row + 1);
	pspDebugScreenPuts("Press START to quit.");
	sceKernelDcacheWritebackAll();
	save_shot(PSP_DATA_ROOT "/shots/bakecheck.bmp");
	if (scripted)
	{
		platform_log("bake check: a scripted run (input.txt): quitting in 3 s");
		sceKernelDelayThread(3000000);
		psp_exit_game("bake check");
	}
	sceCtrlSetSamplingCycle(0);
	sceCtrlSetSamplingMode(PSP_CTRL_MODE_DIGITAL);
	/* START let go first, so a press held from the XMB doesn't quit unseen */
	do
	{
		sceCtrlReadBufferPositive(&pad, 1);
		sceKernelDelayThread(50000);
	}
	while (pad.Buttons & PSP_CTRL_START);
	do
	{
		sceCtrlReadBufferPositive(&pad, 1);
		sceKernelDelayThread(50000);
	}
	while (!(pad.Buttons & PSP_CTRL_START));
	psp_exit_game("bake check");
}

void psp_bake_check(void)
{
	struct stamp s;
	int build = psp_build_number();

	if (psp_keep("bakeany"))
	{
		platform_log("bake check: skipped (keep word bakeany); this build reads bake format %d", PSP_BAKE_FORMAT);
		return;
	}
	read_stamp(&s);
	if (s.found && s.has_format && s.format == PSP_BAKE_FORMAT)
	{
		platform_log("bake check: %s says bake format %d, bundle %s (commit %s, baked %s): this build's format",
			STAMP_PATH, s.format, s.bundle, s.commit[0] ? s.commit : "?", s.baked[0] ? s.baked : "?");
		return;
	}
	platform_log("bake check: this build reads bake format %d; %s %s", PSP_BAKE_FORMAT, STAMP_PATH,
		s.found ? "has another" : "is missing");
	line_count = 0;
	if (!s.found && !file_exists(PSP_MAPS_ROOT "/ui.map"))
	{
		say("NO MAPS FOUND");
		say("There are no baked maps in ms0:/halo/maps.");
		say("");
		say("Bake your Halo disc on your computer with the bundle");
		say("that came with this build (b%d), then copy the halo", build);
		say("folder it makes to the root of the Memory Stick.");
	}
	else if (s.found && s.has_format && s.format > PSP_BAKE_FORMAT)
	{
		say("THESE MAPS ARE NEWER THAN THIS BUILD");
		say("They were baked with bundle %s (format %d);", s.bundle, s.format);
		say("this build reads bake format %d.", PSP_BAKE_FORMAT);
		say("");
		say("Install the build that came with bundle %s, or bake", s.bundle);
		say("your disc again with this build's bundle (b%d).", build);
	}
	else
	{
		say("YOUR MAPS NEED BAKING AGAIN");
		if (s.found && s.has_format)
			say("These maps were baked with bundle %s (format %d).", s.bundle, s.format);
		else if (s.found)
			say("Their stamp (halo/bake.txt) can't be read.");
		else
			say("These maps have no stamp: an older bundle made them.");
		say("This build needs bake format %d: its own bundle, b%d.", PSP_BAKE_FORMAT, build);
		say("");
		say("Bake your disc again on your computer with the bundle");
		say("that came with this build, then copy the halo folder");
		say("it makes to the Memory Stick's root, over the old one.");
	}
	show_and_quit();
}
