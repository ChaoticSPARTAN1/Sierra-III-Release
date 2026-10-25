/* psp_trace.c - the load trace (build 96), the blackbox (build 98)

Why: b95's runs 1 and 2 on the PSP-1000 died loading The Library, just
after the texture pack opened, with the ME on and off: the log stopped, the
heartbeat thread (which outranks the game) stopped with it, no crash.txt was
written, and about 10 s later the PSP powered itself off. The emulator loads
the same files with the same settings. A user-mode fault would have reached
the crash handler; this looks like the kernel or the bus giving out, which
user code reaches through system calls. So from the map's geometry pack
opening until two seconds of play, every Memory Stick open, seek and read,
every DMA copy, every GE list and display change is logged before it is made
and after it returns, with the breadcrumb steps between them. Start-up log
lines go to the file at once, so the last line is the last call made.

The calls are wrapped at link time (psp_build.py's --wrap list); the log's
own file is passed through untraced. */
#include <pspkernel.h>
#include <pspiofilemgr.h>
#include <pspdebug.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "psp_paths.h"   /* ms0:/halo or ms0:/halomp, and the maps */

void platform_log(const char *format, ...);
int psp_keep(const char *name);
int psp_keep_loaded(void);
#define psp_keep_loaded_early psp_keep_loaded

volatile int psp_trace = 0;
extern volatile int psp_crashed; /* psp_crash.c */
static volatile int trace_lines = 0, trace_busy = 0;
#define TRACE_LIMIT 400 /* The Library takes 66 to its first frame; Blood Gulch's texture preload would take thousands */

void psp_trace_line(const char *format, ...)
{
	char text[200];
	va_list arguments;
	int n;

	if (!psp_trace || trace_busy)
		return;
	if (psp_trace == 1 && trace_lines >= TRACE_LIMIT)
	{
		psp_trace = 0;
		platform_log("trace: %d lines, stopped", TRACE_LIMIT);
		return;
	}
	trace_busy = 1;
	n = ++trace_lines;
	va_start(arguments, format);
	vsnprintf(text, sizeof(text), format, arguments);
	va_end(arguments);
	platform_log("trace %d [%x] %u: %s", n, (unsigned)sceKernelGetThreadId(), (unsigned)sceKernelGetSystemTimeLow() / 1000, text);
	trace_busy = 0;
}

void psp_trace_start(const char *why)
{
	/* build 102: only with "blackbox". On by default (b96-b101) it wrote
	hundreds of lines at once at every map load: Blood Gulch's texture
	preload took 9 s instead of 1.2 (b101's run 3) */
	if (psp_trace || !psp_keep("blackbox"))
		return;
	psp_trace = 1;
	psp_trace_line("on (%s)", why);
}

/* build 98: "blackbox" (keep.txt, the launch menu): the trace goes on in
play, without the steps (Memory Stick, DMA, GE list and display calls: a few
lines a frame), and the heartbeat saves the log four times a second, so a
freeze that takes the whole PSP down leaves its last quarter second.
b97's run 1 froze in The Library's first fight with nothing in the log after
the last 5 s batch */
void psp_trace_stop(const char *why)
{
	if (psp_trace != 1)
		return;
	if (psp_keep("blackbox"))
	{
		extern void psp_log_batch_now(void);

		psp_trace = 2;
		platform_log("trace: %d lines to %s; blackbox: tracing on in play", trace_lines, why);
		psp_log_batch_now();
		return;
	}
	psp_trace = 0;
	platform_log("trace: off after %d lines (%s)", trace_lines, why);
}


/* build 101: "crumbs". Every crash or freeze in The Library left nothing:
the crash handler's own test (b100 run 2, a store to address 4) froze the
PSP exactly like the real freezes, no report on the screen, no crash.txt,
no sound, no input, then the power-off. So whatever runs at a fault doesn't
get as far as drawing. Instead the game draws its own last steps into the
picture on the screen as it goes (four rows at the bottom, in turn, each
with a running number): what the frozen screen shows is the last thing it
started. Slow (an uncached write per pixel), so only with the keep word */
volatile unsigned int psp_shown_buffer = 0;
/* build 106: shared, so a caller can skip working out a crumb's text (a tag
name per object update) while crumbs are off: 1 on, 0 off, -1 not armed yet */
int psp_crumbs_on = -1;
static unsigned int crumb_count = 0;

/* armed at the game's first frame on the screen (keep.txt is read by then;
the launch menu's pictures don't count) */
void psp_crumbs_arm(void)
{
	psp_crumbs_on = psp_keep("crumbs");
}

int psp_crumb_mark(const char *what, const char *detail)
{
	char text[69];

	if (psp_crumbs_on <= 0 || !psp_shown_buffer)
		return 0;
	crumb_count++;
	{
		int length = snprintf(text, 49, "%06u %s %s", crumb_count % 1000000, what ? what : "", detail ? detail : "");

		/* a fixed 48 columns, the last crumb on this row written over */
		if (length < 0)
			length = 0;
		if (length > 48)
			length = 48;
		memset(text + length, ' ', 48 - length);
		text[48] = 0;
	}
	pspDebugScreenSetBase((u32 *)(0x40000000u | (psp_shown_buffer & 0x0FFFFFFFu)));
	pspDebugScreenSetXY(0, 30 + (int)(crumb_count & 3));
	pspDebugScreenPuts(text);
	return 0;
}

/* the file names behind the descriptors, for the read lines */
#define TRACE_FILES 64
static char trace_names[TRACE_FILES][40];

static const char *name_of(SceUID fd)
{
	return fd >= 0 && fd < TRACE_FILES && trace_names[fd][0] ? trace_names[fd] : "?";
}

/* build 115: "iostats" (streaming.md section 5): the Memory Stick's work by
stream, each heartbeat report's window apart: calls, bytes, the time in the
stick, the time waiting for the lock and which stream held it, and, for the
map file (sound, mostly), how many reads fetch again an offset already read.
A stream is known by the file (its extension) and, for the geometry pack's
animation pages, by the reader saying so (psp_io_kind_push) */
enum { IO_TEXTURES, IO_GEOMETRY, IO_ANIMATIONS, IO_MAP, IO_LOG, IO_OTHER, IO_KINDS };
static const char *const io_kind_names[IO_KINDS] = { "textures", "geometry", "animations", "map file", "log", "other" };
static unsigned char fd_kind[TRACE_FILES];
static int fd_pos[TRACE_FILES]; /* where the next read starts (-1: unknown; set at open) */

/* Open files don't survive a sleep: after the wake every read on a
descriptor opened before it fails, and streamed geometry, textures and
sounds came back as garbage (hardware_b120.md). So each open file's full
path, flags and mode are kept here, by the descriptor its caller was given,
and psp_io_reopen_all() opens them again on the wake. Callers keep the
descriptors they hold: where the kernel hands back a different one, the
wrappers pass the new one on (io_real) */
#define REOPEN_PATH 160
static struct reopen
{
	char path[REOPEN_PATH]; /* empty: no file open under this descriptor */
	int flags;
	SceMode mode;
	SceUID real; /* the kernel's descriptor for it now */
} reopen_table[TRACE_FILES];
static int reopen_untracked = 0, reopen_collisions = 0;

static SceUID io_real(SceUID fd)
{
	return fd >= 0 && fd < TRACE_FILES && reopen_table[fd].path[0] ? reopen_table[fd].real : fd;
}

static const char *path_of_fd(SceUID fd)
{
	return fd >= 0 && fd < TRACE_FILES && reopen_table[fd].path[0] ? reopen_table[fd].path : NULL;
}

static void reopen_note(SceUID fd, const char *path, int flags, SceMode mode)
{
	if (fd < 0)
		return;
	if (fd >= TRACE_FILES || !path || strlen(path) >= REOPEN_PATH)
	{
		reopen_untracked++;
		return;
	}
	/* a descriptor the kernel gave out that a caller still holds under a
	translation: both now mean this file, and the older one can't be served */
	if (reopen_table[fd].path[0] && reopen_table[fd].real != fd)
		reopen_collisions++;
	snprintf(reopen_table[fd].path, REOPEN_PATH, "%s", path);
	reopen_table[fd].flags = flags;
	reopen_table[fd].mode = mode;
	reopen_table[fd].real = fd;
}
static struct io_window
{
	unsigned int calls, waits, map_rereads;
	unsigned long long read_bytes, write_bytes, in_us, wait_us, reread_bytes;
	unsigned int worst_in_us, worst_wait_us;
	unsigned long long behind_us[IO_KINDS]; /* waited while that stream held the lock */
} io_win[IO_KINDS];
static int io_stats = -1;
static unsigned char *map_seen = NULL; /* a bit per 4 KB of the map file read so far */
#define MAP_SEEN_BYTES (16 * 1024)  /* 512 MB of map file */
static volatile SceUID io_override_thread = 0;
static volatile int io_override_kind = IO_OTHER, io_override_depth = 0;

static int io_on(void)
{
	if (io_stats < 0 && psp_keep_loaded_early())
		io_stats = psp_keep("iostats") != 0;
	return io_stats > 0;
}

static int kind_of_path(const char *path)
{
	const char *dot;

	if (!path)
		return IO_OTHER;
	if (strstr(path, "/log/"))
		return IO_LOG;
	dot = strrchr(path, '.');
	if (dot && !strcmp(dot, ".ptex"))
		return IO_TEXTURES;
	if (dot && !strcmp(dot, ".pgeo"))
		return IO_GEOMETRY;
	if (dot && !strcmp(dot, ".map"))
		return IO_MAP;
	return IO_OTHER;
}

static int kind_of_fd(SceUID fd)
{
	int kind = fd >= 0 && fd < TRACE_FILES ? fd_kind[fd] : IO_OTHER;

	if (io_override_depth > 0 && io_override_thread == sceKernelGetThreadId() && kind == IO_GEOMETRY)
		kind = io_override_kind;
	return kind;
}

/* the animation cache's page reads go through the geometry pack
(psp_geo_pack_read): it says so around them */
void psp_io_kind_push_animations(void)
{
	io_override_thread = sceKernelGetThreadId();
	io_override_kind = IO_ANIMATIONS;
	io_override_depth++;
}

void psp_io_kind_pop(void)
{
	if (io_override_depth > 0)
		io_override_depth--;
}

/* a read of the map file: does it fetch again a 4 KB block read before? */
static void map_read(SceUID fd, int bytes)
{
	int at, first, last, block, again;

	if (fd < 0 || fd >= TRACE_FILES || bytes <= 0 || fd_pos[fd] < 0)
		return;
	at = fd_pos[fd];
	fd_pos[fd] += bytes;
	if (!map_seen)
	{
		map_seen = malloc(MAP_SEEN_BYTES);
		if (!map_seen)
			return;
		memset(map_seen, 0, MAP_SEEN_BYTES);
	}
	first = at >> 12;
	last = (at + bytes - 1) >> 12;
	if (last >= MAP_SEEN_BYTES * 8)
		return;
	again = (map_seen[first >> 3] >> (first & 7)) & 1;
	for (block = first; block <= last; block++)
		map_seen[block >> 3] |= (unsigned char)(1 << (block & 7));
	if (again)
	{
		io_win[IO_MAP].map_rereads++;
		io_win[IO_MAP].reread_bytes += bytes;
	}
}

/* build 100: one thread on the Memory Stick at a time. Every freeze in
The Library came with the game's loader thread reading the map while the
main thread read textures or the heartbeat wrote the log (Blood Gulch
preloads everything and never does this, the emulator's file system has no
timing at all). Each wrapped call below takes this lock (re-entrant, for
the log's own writes from inside a traced call); keep word "stickparallel"
leaves it off */
static SceUID stick_lock = -1;

/* build 102: where the Memory Stick's time goes, by thread: the calls'
own time (lock or no lock: the stick does one request at a time, so a
thread's synchronous read also waits for any other thread's read in the
driver; an asynchronous read's transfer is not in it) and, with the lock,
the time spent waiting for it. b101's run 2 waited 59 s for the lock
over 2,379 waits, 0.9 s at worst, and which thread waited decides whether
that was a stall on the screen (the game thread) or a late block (the
loader, the sound) */
#define STICK_THREADS 8
static struct stick_thread
{
	SceUID id;
	int depth, locked, kind;
	unsigned int calls, waits, start;
	unsigned long long in_us, wait_us;
	unsigned int worst_in_us, worst_wait_us;
	const char *op;  /* the outermost call, for the slow calls' report */
	char what[24];   /* its file's name */
} stick_threads[STICK_THREADS], stick_spare;

/* The stick's slow calls, a quarter second and more: the lock's totals say
how long each thread held it, not which call it was. A b126 run on the
PSP-1000 had the log's thread hold the stick 1.46 s in one call, with an
animation read stalled behind it. The slowest four of
each report's window are kept */
#define STICK_SLOW_US 250000
#define STICK_SLOW_KEPT 4
static struct stick_slow
{
	SceUID thread;
	const char *op;
	char what[24];
	unsigned int took_ms;
	long game_time;
} stick_slow[STICK_SLOW_KEPT];
static unsigned int stick_slow_calls = 0;
static volatile int stick_enabled = -1;
int psp_keep_loaded(void);

static struct stick_thread *stick_thread_of(SceUID me)
{
	int k;

	for (k = 0; k < STICK_THREADS; k++)
		if (stick_threads[k].id == me)
			return &stick_threads[k];
	for (k = 0; k < STICK_THREADS; k++)
		if (!stick_threads[k].id)
		{
			/* a slot is only ever taken, never given back: at worst two
			threads race for one and one of them counts in the other's */
			stick_threads[k].id = me;
			return &stick_threads[k];
		}
	return &stick_spare;
}

/* build 104: a thread's name for the lock notes */
const char *psp_thread_name(SceUID id, char *buffer, int size)
{
	SceKernelThreadInfo info;

	memset(&info, 0, sizeof(info));
	info.size = sizeof(info);
	if (id && sceKernelReferThreadStatus(id, &info) >= 0)
		snprintf(buffer, size, "%s", info.name);
	else
		snprintf(buffer, size, "%x", (unsigned)id);
	return buffer;
}

static volatile SceUID stick_holder = 0; /* build 104 */
static volatile int stick_holder_kind = IO_OTHER; /* build 115 */
static volatile unsigned long stick_long_waits = 0;

static void stick_take_kind(int kind)
{
	SceUID me = sceKernelGetThreadId();
	struct stick_thread *thread = stick_thread_of(me);
	unsigned int start = sceKernelGetSystemTimeLow();

	/* re-entrant: the log's own writes from inside a traced call */
	if (thread->depth++ > 0)
		return;
	thread->kind = kind;
	/* asked each time until keep.txt is read */
	if (stick_enabled < 0)
	{
		int on = !psp_keep("stickparallel");

		if (on && stick_lock < 0)
			stick_lock = sceKernelCreateSema("halo stick", 0, 1, 1, NULL);
		if (psp_keep_loaded())
			stick_enabled = on && stick_lock >= 0;
	}
	/* build 115: "iostats", asked once keep.txt is read */
	if (io_stats < 0 && psp_keep_loaded())
		io_stats = psp_keep("iostats") != 0;
	thread->locked = 0;
	if (stick_enabled != 0 && stick_lock >= 0 && !psp_keep("stickparallel"))
	{
		if (sceKernelPollSema(stick_lock, 1) < 0)
		{
			unsigned int waited;
			int behind = stick_holder_kind;
			/* build 104: after a crash, 200 ms at most (the crash handler
			writes crash.txt; the holder may never let go) */
			SceUInt crash_timeout = 200000;

			if (psp_crashed)
			{
				if (sceKernelWaitSema(stick_lock, 1, &crash_timeout) < 0)
				{
					thread->start = start;
					return;
				}
			}
			else
			{
				/* build 104: in 2 s steps, saying on the screen who holds
				the stick when a wait gets that long (still waiting: the
				stick is one thread's at a time) */
				SceUInt step = 2000000;
				int notes = 0;

				while (sceKernelWaitSema(stick_lock, 1, &step) < 0)
				{
					void psp_screen_note(const char *text);
					char note[80];

					step = 2000000;
					stick_long_waits++;
					if (notes++ < 2)
					{
						char name[32];

						snprintf(note, sizeof(note), "LOCK: the stick's lock waited 2 s; held by %s", psp_thread_name(stick_holder, name, sizeof(name)));
						psp_screen_note(note);
					}
				}
			}
			waited = sceKernelGetSystemTimeLow() - start;
			thread->waits++;
			thread->wait_us += waited;
			if (waited > thread->worst_wait_us)
				thread->worst_wait_us = waited;
			if (io_stats > 0)
			{
				struct io_window *w = &io_win[kind];

				w->waits++;
				w->wait_us += waited;
				if (waited > w->worst_wait_us)
					w->worst_wait_us = waited;
				if (behind >= 0 && behind < IO_KINDS)
					w->behind_us[behind] += waited;
			}
			start = sceKernelGetSystemTimeLow();
		}
		thread->locked = 1;
		stick_holder = me;
		stick_holder_kind = kind;
	}
	thread->start = start;
}

static void stick_take(void)
{
	stick_take_kind(IO_OTHER);
}

/* stick_take_kind, naming the call and its file for the slow calls'
report (a call made inside another keeps the outer one's name) */
static void stick_take_op(int kind, const char *op, const char *path)
{
	struct stick_thread *thread;

	stick_take_kind(kind);
	thread = stick_thread_of(sceKernelGetThreadId());
	if (thread->depth == 1)
	{
		const char *slash = path ? strrchr(path, '/') : NULL;

		thread->op = op;
		snprintf(thread->what, sizeof(thread->what), "%s", slash ? slash + 1 : path ? path : "?");
	}
}

static void stick_slow_note(struct stick_thread *thread, unsigned int took)
{
	extern long psp_game_time_peek(void);
	int k, least = 0;

	stick_slow_calls++;
	for (k = 1; k < STICK_SLOW_KEPT; k++)
		if (stick_slow[k].took_ms < stick_slow[least].took_ms)
			least = k;
	if (took / 1000 <= stick_slow[least].took_ms)
		return;
	stick_slow[least].thread = thread->id;
	stick_slow[least].op = thread->op ? thread->op : "call";
	memcpy(stick_slow[least].what, thread->what, sizeof(stick_slow[least].what));
	stick_slow[least].took_ms = took / 1000;
	stick_slow[least].game_time = psp_game_time_peek();
}

static void stick_give(void)
{
	struct stick_thread *thread = stick_thread_of(sceKernelGetThreadId());
	unsigned int took;

	if (thread->depth <= 0 || --thread->depth > 0)
		return;
	took = sceKernelGetSystemTimeLow() - thread->start;
	if (took >= STICK_SLOW_US)
		stick_slow_note(thread, took);
	thread->op = NULL;
	thread->what[0] = 0;
	thread->calls++;
	thread->in_us += took;
	if (took > thread->worst_in_us)
		thread->worst_in_us = took;
	if (io_stats > 0 && thread->kind >= 0 && thread->kind < IO_KINDS)
	{
		struct io_window *w = &io_win[thread->kind];

		w->calls++;
		w->in_us += took;
		if (took > w->worst_in_us)
			w->worst_in_us = took;
	}
	if (thread->locked)
	{
		thread->locked = 0;
		sceKernelSignalSema(stick_lock, 1);
	}
}

/* the heartbeat's report: one line, the threads by name */
/* a seek and the read it positions, under one hold of the lock: taken per
call, another thread's write could slip in between and the read wait twice
(a geometry read waited behind a log write, then again behind the log's
close). The lock is re-entrant, so the wrapped calls inside keep this
hold's name */
void psp_stick_hold(SceUID fd)
{
	stick_take_op(kind_of_fd(fd), "read", path_of_fd(fd));
}

void psp_stick_release(void)
{
	stick_give();
}

void psp_stick_report(void)
{
	char line[400];
	int k, length;

	length = snprintf(line, sizeof(line), "stick (%s):", stick_enabled > 0 && !psp_keep("stickparallel") ? "one thread at a time" : "parallel");
	for (k = 0; k < STICK_THREADS && length < (int)sizeof(line) - 1; k++)
	{
		struct stick_thread *thread = &stick_threads[k];
		SceKernelThreadInfo info;
		const char *name = "?";

		if (!thread->id || !thread->calls)
			continue;
		memset(&info, 0, sizeof(info));
		info.size = sizeof(info);
		if (sceKernelReferThreadStatus(thread->id, &info) >= 0)
			name = info.name;
		length += snprintf(line + length, sizeof(line) - length, " %s %u calls %llu ms (worst %u)",
			name, thread->calls, thread->in_us / 1000, thread->worst_in_us / 1000);
		if (thread->waits && length < (int)sizeof(line) - 1)
			length += snprintf(line + length, sizeof(line) - length, ", waited %u times %llu ms (worst %u);",
				thread->waits, thread->wait_us / 1000, thread->worst_wait_us / 1000);
		else if (length < (int)sizeof(line) - 1)
			length += snprintf(line + length, sizeof(line) - length, ";");
	}
	platform_log("%s", line);
	if (stick_slow_calls)
	{
		int k;

		length = snprintf(line, sizeof(line), "stick: %u calls of 250 ms or more since the last report; the slowest:", stick_slow_calls);
		for (k = 0; k < STICK_SLOW_KEPT && length < (int)sizeof(line) - 1; k++)
		{
			char name[32];

			if (!stick_slow[k].took_ms)
				continue;
			length += snprintf(line + length, sizeof(line) - length, " %s %s %s %u ms at game time %ld;",
				psp_thread_name(stick_slow[k].thread, name, sizeof(name)), stick_slow[k].op, stick_slow[k].what,
				stick_slow[k].took_ms, stick_slow[k].game_time);
		}
		platform_log("%s", line);
		stick_slow_calls = 0;
		memset(stick_slow, 0, sizeof(stick_slow));
	}
	if (io_on())
	{
		/* build 115: "iostats": each stream's window since the last report */
		extern long psp_game_time_peek(void);
		int kind, behind;

		platform_log("iostats: game time %ld; per stream: calls, KB read/written, s in the stick (worst ms), s waiting for the lock (times, worst ms; behind whom, ms)",
			psp_game_time_peek());
		for (kind = 0; kind < IO_KINDS; kind++)
		{
			struct io_window *w = &io_win[kind];
			char who[160];
			int at = 0;

			if (!w->calls && !w->waits)
				continue;
			who[0] = 0;
			for (behind = 0; behind < IO_KINDS && at < (int)sizeof(who) - 1; behind++)
				if (w->behind_us[behind] >= 1000)
					at += snprintf(who + at, sizeof(who) - at, "%s%s %llu", at ? ", " : "", io_kind_names[behind], w->behind_us[behind] / 1000);
			platform_log("iostats:   %s: %u calls, %llu/%llu KB, %llu.%02llu s (worst %u), waited %llu.%02llu s (%u times, worst %u%s%s%s)",
				io_kind_names[kind], w->calls, w->read_bytes / 1024, w->write_bytes / 1024,
				w->in_us / 1000000, (w->in_us / 10000) % 100, w->worst_in_us / 1000,
				w->wait_us / 1000000, (w->wait_us / 10000) % 100, w->waits, w->worst_wait_us / 1000,
				who[0] ? "; behind " : "", who, "");
			if (kind == IO_MAP && w->map_rereads)
				platform_log("iostats:   map file: %u reads fetched again a block read before, %llu KB of the %llu",
					w->map_rereads, w->reread_bytes / 1024, w->read_bytes / 1024);
		}
		memset(io_win, 0, sizeof(io_win));
	}
	{
		/* build 103 */
		void psp_handles_line(char *line, int size);

		psp_handles_line(line, sizeof(line));
		platform_log("%s", line);
	}
}

/* build 106: each thread's priority and its share of the CPU since the last
report (the heartbeat, every 30 s), and how often it was woken.

Why: b105's run spent 647 s in the cache file thread's 7,468 stick
calls, 87 ms a call, for reads of about 30 KB that the stick does in about
4 ms. The PSP's scheduler runs the highest-priority ready thread and never
shares the CPU between equals, so a reader whose read has finished waits
until the game thread (always busy at 6-8 fps) blocks, holding the stick's
lock all that time. This line shows each thread's priority and CPU share on
the PSP, so that reading can be checked (a lower number is a higher
priority) */
#define REPORT_THREADS 24
void psp_threads_report(void)
{
	static struct { SceUID id; unsigned long long run_us; unsigned int releases; } last[REPORT_THREADS];
	static unsigned long long last_report_us = 0;
	SceUID ids[REPORT_THREADS];
	unsigned long long now = sceKernelGetSystemTimeWide(), span = now - last_report_us;
	unsigned long long counted = 0;
	char line[512];
	int count = 0, k, length;

	if (sceKernelGetThreadmanIdList(SCE_KERNEL_TMID_Thread, ids, REPORT_THREADS, &count) < 0)
		return;
	if (count > REPORT_THREADS)
		count = REPORT_THREADS;
	length = snprintf(line, sizeof(line), "threads (priority, CPU share since the last line, times woken):");
	for (k = 0; k < count && length < (int)sizeof(line) - 1; k++)
	{
		SceKernelThreadInfo info;
		unsigned long long run_us, before_us = 0;
		unsigned int before_releases = 0;
		int slot, free_slot = -1;

		memset(&info, 0, sizeof(info));
		info.size = sizeof(info);
		if (sceKernelReferThreadStatus(ids[k], &info) < 0)
			continue;
		run_us = ((unsigned long long)info.runClocks.hi << 32) | info.runClocks.low;
		for (slot = 0; slot < REPORT_THREADS && last[slot].id != ids[k]; slot++)
			if (free_slot < 0 && !last[slot].id)
				free_slot = slot;
		if (slot < REPORT_THREADS)
		{
			before_us = last[slot].run_us;
			before_releases = last[slot].releases;
		}
		else if (free_slot >= 0)
			slot = free_slot;
		if (slot < REPORT_THREADS)
		{
			last[slot].id = ids[k];
			last[slot].run_us = run_us;
			last[slot].releases = info.releaseCount;
		}
		counted += run_us - before_us;
		/* the idle thread is the time nobody wanted the CPU: the remainder */
		if (!strncmp(info.name, "idle", 4))
			continue;
		/* platform_log formats into 512 characters: the line goes out in
		parts rather than lose its last threads (the audio thread among them) */
		if (length > 400)
		{
			platform_log("%s", line);
			length = snprintf(line, sizeof(line), "threads (continued):");
		}
		length += snprintf(line + length, sizeof(line) - length, " %s 0x%x %.1f%% %u;",
			info.name, info.currentPriority,
			last_report_us && span ? 100.0 * (double)(run_us - before_us) / (double)span : 0.0,
			info.releaseCount - before_releases);
	}
	if (last_report_us && span && length < (int)sizeof(line) - 1)
		snprintf(line + length, sizeof(line) - length, " idle or the kernel %.1f%%",
			counted < span ? 100.0 * (double)(span - counted) / (double)span : 0.0);
	last_report_us = now;
	platform_log("%s", line);
}

/* ---------- build 103: the open-handle table

Why: b102's run 3 (The Library, the first Flood fight) crashed in the C
library's readdir: the kernel had refused a directory open with
SCE_KERNEL_ERROR_MFILE (0x80020320, "too many open files": the firmware's
iofilemgr gives out at most 64 descriptors, files and directories together),
and libcglue's open() handed that code back as if it were a descriptor
(opendir only tests for -1). The emulator's trace of the same route peaked at
12 open handles and ended with 9: whatever uses them up happens on the
hardware only. So every open and close (files and directories) is kept here
with its path; the stick report says how many are open; a failed close is
logged (a handle the kernel kept is one we can't see otherwise); and the
first time an open fails for lack of handles, the table is written to the
log, after one handle held back since start-up is given up so that the log
(which opens its file for every batch) and crash.txt can still be written. */
#define HANDLES 96
static struct
{
	SceUID fd;
	char kind; /* 'f' file, 'd' directory */
	char path[47];
	unsigned int opened_ms;
} handles[HANDLES];
static int handles_open = 0, handles_peak = 0, handles_lost = 0, close_failures = 0, open_failures = 0, mfile_failures = 0;
/* build 105: refusals split by reason, so the out-of-handles ones stand out
from the ordinary "is this file there?" probes */
static int missing_failures = 0;
static SceUID handle_reserve = -1;
static SceUID handle_lock = -1;

/* build 104: the table's lock is held only while an entry changes, so a
wait of 200 ms means something is wrong (b103 run 1: the game thread waited
here forever while holding the log's lock, and the heartbeat behind it). A
wait that times out goes on without the lock and is counted; nothing is
logged from here (the waiting thread may hold the log's lock). After a
crash there is no waiting at all: the crashed thread may be the holder */
static volatile SceUID handle_lock_holder = 0, handle_lock_stuck_holder = 0;
static volatile unsigned long handle_lock_timeouts = 0;

static int handles_take(void)
{
	SceUInt timeout = 200000;

	if (handle_lock < 0)
		handle_lock = sceKernelCreateSema("halo handles", 0, 1, 1, NULL);
	if (handle_lock < 0 || psp_crashed)
		return 0;
	if (sceKernelWaitSema(handle_lock, 1, &timeout) < 0)
	{
		void psp_screen_note(const char *text);
		char note[80];

		handle_lock_timeouts++;
		handle_lock_stuck_holder = handle_lock_holder;
		if (handle_lock_timeouts <= 3)
		{
			char name[32];

			snprintf(note, sizeof(note), "LOCK: the handle table's lock waited out; held by %s", psp_thread_name(handle_lock_holder, name, sizeof(name)));
			psp_screen_note(note);
		}
		return 0;
	}
	handle_lock_holder = sceKernelGetThreadId();
	return 1;
}

static void handles_give(int taken)
{
	if (taken)
	{
		handle_lock_holder = 0;
		sceKernelSignalSema(handle_lock, 1);
	}
}

/* for the heartbeat (empty when nothing happened) */
void psp_handles_lock_line(char *line, int size)
{
	SceKernelThreadInfo info;
	const char *name = "?";

	line[0] = 0;
	if (!handle_lock_timeouts)
		return;
	memset(&info, 0, sizeof(info));
	info.size = sizeof(info);
	if (handle_lock_stuck_holder && sceKernelReferThreadStatus(handle_lock_stuck_holder, &info) >= 0)
		name = info.name;
	snprintf(line, size, "locks: the handle table's lock waited out %lu times (held by %s, thread %x)",
		handle_lock_timeouts, name, (unsigned)handle_lock_stuck_holder);
}

static void handle_add(SceUID fd, char kind, const char *path)
{
	int k, free_slot = -1;
	int taken = handles_take();
	for (k = 0; k < HANDLES; k++)
	{
		if (handles[k].kind && handles[k].fd == fd)
		{
			/* the kernel gave out a descriptor we think is open: its close
			was missed (or made outside the wrappers) */
			handles_lost++;
			free_slot = k;
			break;
		}
		if (!handles[k].kind && free_slot < 0)
			free_slot = k;
	}
	if (free_slot >= 0)
	{
		if (!handles[free_slot].kind)
			handles_open++;
		handles[free_slot].fd = fd;
		handles[free_slot].kind = kind;
		/* the end of a long path says more than its start */
		{
			size_t length = path ? strlen(path) : 0;
			const char *tail = length >= sizeof(handles[0].path) ? path + length - (sizeof(handles[0].path) - 1) : (path ? path : "?");

			snprintf(handles[free_slot].path, sizeof(handles[0].path), "%s", tail);
		}
		handles[free_slot].opened_ms = sceKernelGetSystemTimeLow() / 1000;
		if (handles_open > handles_peak)
			handles_peak = handles_open;
	}
	handles_give(taken);
}

static void handle_remove(SceUID fd, char kind, int result)
{
	int k;
	char path[48] = "";
	int taken = handles_take();
	for (k = 0; k < HANDLES; k++)
		if (handles[k].kind == kind && handles[k].fd == fd)
		{
			snprintf(path, sizeof(path), "%s", handles[k].path);
			handles[k].kind = 0;
			handles_open--;
			break;
		}
	handles_give(taken);
	if (result < 0 && close_failures++ < 16)
		platform_log("files: %s %d (%s) failed (0x%08x)", kind == 'd' ? "dclose" : "close", (int)fd,
			path[0] ? path : "not in the table", (unsigned)result);
}

/* every handle open now, oldest first, into the log */
void psp_handles_dump(const char *why)
{
	int k, listed = 0;
	unsigned int now = sceKernelGetSystemTimeLow() / 1000;

	platform_log("files: %s; %d handles open (peak %d), %d closes failed, %d descriptors handed out twice:", why,
		handles_open, handles_peak, close_failures, handles_lost);
	int test_handles = 0;

	for (k = 0; k < HANDLES; k++)
		if (handles[k].kind)
		{
			listed++;
			/* handletest's own, counted rather than listed */
			if (strstr(handles[k].path, "handletest.txt"))
			{
				test_handles++;
				continue;
			}
			platform_log("files:   %c %3d  %-46s open %u s", handles[k].kind, (int)handles[k].fd, handles[k].path,
				(now - handles[k].opened_ms) / 1000);
		}
	if (test_handles)
		platform_log("files:   and %d of handletest's own", test_handles);
	if (!listed)
		platform_log("files:   (none in the table: the kernel's descriptors went to something outside the wrappers)");
}

/* one descriptor held from start-up, given up when the kernel runs out.
Build 105: also called from the heartbeat to take it back once one is free.
It opens through the real call, so a retry while the kernel is still out of
descriptors isn't counted or reported as a failure again */
SceUID __real_sceIoOpen(const char *file, int flags, SceMode mode);
void psp_handles_reserve(void)
{
	static const char path[] = PSP_DATA_ROOT "/log/reserve.txt";

	if (handle_reserve >= 0)
		return;
	stick_take_kind(IO_LOG);
	handle_reserve = __real_sceIoOpen(path, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
	reopen_note(handle_reserve, path, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
	stick_give();
	if (handle_reserve >= 0 && handle_reserve < TRACE_FILES)
		fd_kind[handle_reserve] = IO_LOG;
	if (handle_reserve >= 0)
		handle_add(handle_reserve, 'f', path);
}

void psp_handles_release(void)
{
	if (handle_reserve >= 0)
	{
		sceIoClose(handle_reserve);
		handle_reserve = -1;
	}
}

static void open_failed(const char *path, char kind, int result)
{
	open_failures++;
	if ((unsigned)result == 0x80010002 /* the file isn't there (ENOENT) */)
		missing_failures++;
	if ((unsigned)result == 0x80020320 /* SCE_KERNEL_ERROR_MFILE */)
	{
		int had_reserve = handle_reserve >= 0;

		mfile_failures++;
		psp_handles_release();
		if (mfile_failures <= 4)
		{
			char why[120];

			snprintf(why, sizeof(why), "OUT OF FILE HANDLES: %s %s failed (0x%08x)%s", kind == 'd' ? "dopen" : "open",
				path ? path : "?", (unsigned)result, had_reserve ? "; the reserve handle given up for the log" : "");
			psp_handles_dump(why);
		}
	}
}

/* for the stick report */
void psp_handles_line(char *line, int size)
{
	snprintf(line, size, "files: %d handles open (peak %d); opens failed: %d for lack of handles, %d not there, %d otherwise; %d closes failed, %d handed out twice",
		handles_open, handles_peak, mfile_failures, missing_failures, open_failures - mfile_failures - missing_failures,
		close_failures, handles_lost);
}

SceUID __wrap_sceIoOpen(const char *file, int flags, SceMode mode)
{
	SceUID fd;
	int quiet = !file || strstr(file, "/log/") != NULL;

	if (!quiet)
		psp_trace_line("open %s (flags %x)", file, flags);
	stick_take_op(kind_of_path(file), "open", file);
	fd = __real_sceIoOpen(file, flags, mode);
	reopen_note(fd, file, flags, mode);
	stick_give();
	if (fd >= 0)
		handle_add(fd, 'f', file);
	if (fd >= 0 && fd < TRACE_FILES)
	{
		fd_kind[fd] = (unsigned char)kind_of_path(file);
		fd_pos[fd] = 0;
	}
	else
		open_failed(file, 'f', fd);
	if (!quiet)
	{
		if (fd >= 0 && fd < TRACE_FILES)
		{
			const char *slash = strrchr(file, '/');

			snprintf(trace_names[fd], sizeof(trace_names[fd]), "%s", slash ? slash + 1 : file);
		}
		psp_trace_line("open %s = %d", file, (int)fd);
	}
	return fd;
}

int __real_sceIoLseek32(SceUID fd, int offset, int whence);
int __wrap_sceIoLseek32(SceUID fd, int offset, int whence)
{
	int result;

	stick_take_op(kind_of_fd(fd), "seek", path_of_fd(fd));
	result = __real_sceIoLseek32(io_real(fd), offset, whence);
	stick_give();
	if (fd >= 0 && fd < TRACE_FILES)
		fd_pos[fd] = result >= 0 ? result : -1;

	psp_trace_line("seek %s %d to %08x (%d) = %08x", name_of(fd), (int)fd, (unsigned)offset, whence, (unsigned)result);
	return result;
}

SceOff __real_sceIoLseek(SceUID fd, SceOff offset, int whence);
SceOff __wrap_sceIoLseek(SceUID fd, SceOff offset, int whence)
{
	SceOff result;

	stick_take_op(kind_of_fd(fd), "seek", path_of_fd(fd));
	result = __real_sceIoLseek(io_real(fd), offset, whence);
	stick_give();
	if (fd >= 0 && fd < TRACE_FILES)
		fd_pos[fd] = result >= 0 && result < 0x7FFFFFFF ? (int)result : -1;

	psp_trace_line("seek64 %s %d to %08x (%d) = %08x", name_of(fd), (int)fd, (unsigned)offset, whence, (unsigned)result);
	return result;
}

int __real_sceIoRead(SceUID fd, void *data, SceSize size);
int __wrap_sceIoRead(SceUID fd, void *data, SceSize size)
{
	int result;

	psp_trace_line("read %s %d into %08x, %u bytes", name_of(fd), (int)fd, (unsigned)data, (unsigned)size);
	int kind = kind_of_fd(fd);

	stick_take_op(kind, "read", path_of_fd(fd));
	result = __real_sceIoRead(io_real(fd), data, size);
	stick_give();
	if (io_stats > 0 && result > 0)
	{
		io_win[kind].read_bytes += result;
		if (kind == IO_MAP)
			map_read(fd, result);
		else if (fd >= 0 && fd < TRACE_FILES && fd_pos[fd] >= 0)
			fd_pos[fd] += result;
	}
	else if (result > 0 && fd >= 0 && fd < TRACE_FILES && fd_pos[fd] >= 0)
		fd_pos[fd] += result;
	psp_trace_line("read %s %d = %d", name_of(fd), (int)fd, result);
	return result;
}

int __real_sceIoReadAsync(SceUID fd, void *data, SceSize size);
int __wrap_sceIoReadAsync(SceUID fd, void *data, SceSize size)
{
	int result;

	psp_trace_line("read async %s %d into %08x, %u bytes", name_of(fd), (int)fd, (unsigned)data, (unsigned)size);
	int kind = kind_of_fd(fd);

	stick_take_op(kind, "read async", path_of_fd(fd));
	result = __real_sceIoReadAsync(io_real(fd), data, size);
	stick_give();
	if (io_stats > 0 && result >= 0)
		io_win[kind].read_bytes += size; /* asked: the transfer runs after the call */
	if (fd >= 0 && fd < TRACE_FILES)
		fd_pos[fd] = -1;
	psp_trace_line("read async %s %d started = %d", name_of(fd), (int)fd, result);
	return result;
}

int __real_sceIoClose(SceUID fd);
int __wrap_sceIoClose(SceUID fd)
{
	int traced = fd >= 0 && fd < TRACE_FILES && trace_names[fd][0];
	int result;

	if (traced)
		psp_trace_line("close %s %d", name_of(fd), (int)fd);
	stick_take_op(kind_of_fd(fd), "close", path_of_fd(fd));
	result = __real_sceIoClose(io_real(fd));
	if (fd >= 0 && fd < TRACE_FILES)
		reopen_table[fd].path[0] = 0;
	stick_give();
	if (fd >= 0 && fd < TRACE_FILES)
	{
		fd_kind[fd] = IO_OTHER;
		fd_pos[fd] = -1;
	}
	handle_remove(fd, 'f', result);
	if (traced)
		trace_names[fd][0] = 0;
	return result;
}

/* the asynchronous calls name a descriptor too */
int __real_sceIoWaitAsync(SceUID fd, SceInt64 *result);
int __wrap_sceIoWaitAsync(SceUID fd, SceInt64 *result)
{
	return __real_sceIoWaitAsync(io_real(fd), result);
}

int __real_sceIoPollAsync(SceUID fd, SceInt64 *result);
int __wrap_sceIoPollAsync(SceUID fd, SceInt64 *result)
{
	return __real_sceIoPollAsync(io_real(fd), result);
}

int __real_sceIoChangeAsyncPriority(SceUID fd, int priority);
int __wrap_sceIoChangeAsyncPriority(SceUID fd, int priority)
{
	return __real_sceIoChangeAsyncPriority(io_real(fd), priority);
}

/* On the wake (the power callback), before anything reads: every file the
game held open is opened again, at the place its next read was to start.
The old descriptors are closed first, so the kernel can give the same
numbers back and nothing needs translating. Holding the stick's lock
throughout makes any read that was waiting go after this, through the new
descriptors. A file opened for writing keeps what it has (no truncating)
and goes on at its end. "shift" (the reopentest keep word) holds one extra
descriptor open first, so the kernel's numbers move and the translation is
exercised without a sleep. Fills in what happened, for the log */
void psp_io_reopen_all(int shift, char *line, int size)
{
	int fd, files = 0, moved = 0, failed = 0;
	SceUID extra = -1;

	stick_take_kind(IO_OTHER);
	for (fd = 0; fd < TRACE_FILES; fd++)
		if (reopen_table[fd].path[0])
			__real_sceIoClose(reopen_table[fd].real);
	if (shift)
		extra = __real_sceIoOpen("ms0:/halo/log/reopentest.txt", PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
	for (fd = 0; fd < TRACE_FILES; fd++)
	{
		struct reopen *file = &reopen_table[fd];
		int writing = (file->flags & PSP_O_WRONLY) != 0;
		SceUID real;

		if (!file->path[0])
			continue;
		files++;
		real = __real_sceIoOpen(file->path, file->flags & ~(PSP_O_TRUNC | PSP_O_EXCL), file->mode);
		if (real < 0)
		{
			failed++;
			continue;
		}
		file->real = real;
		if (real != fd)
			moved++;
		if (writing)
			__real_sceIoLseek32(real, 0, PSP_SEEK_END);
		else if (fd_pos[fd] >= 0)
			__real_sceIoLseek32(real, fd_pos[fd], PSP_SEEK_SET);
	}
	stick_give();
	/* the extra one stays open: a descriptor handed out over a translated
	one would be a collision, which the test is there to show doesn't happen
	in play (the kernel gives the lowest free number, and this holds it) */
	(void)extra;
	snprintf(line, size, "%d files reopened, %d at a new descriptor, %d failed; %d not tracked, %d collisions so far%s",
		files, moved, failed, reopen_untracked, reopen_collisions, shift ? " (reopentest: one extra descriptor held first)" : "");
}

int __real_sceDmacMemcpy(void *dst, const void *src, SceSize size);
int __wrap_sceDmacMemcpy(void *dst, const void *src, SceSize size)
{
	int result;

	psp_trace_line("dma %08x <- %08x, %u bytes", (unsigned)dst, (unsigned)src, (unsigned)size);
	result = __real_sceDmacMemcpy(dst, src, size);
	psp_trace_line("dma = %d", result);
	return result;
}

int __real_sceGeListEnQueue(const void *list, void *stall, int cbid, void *arg);
/* the id of the last list handed to the GE (geshow3, psp_ge.c) */
volatile int psp_ge_enqueued = -1;
int __wrap_sceGeListEnQueue(const void *list, void *stall, int cbid, void *arg)
{
	extern int psp_ge_list_callback(int cbid);
	int result;

	psp_trace_line("GE list %08x, stall %08x", (unsigned)list, (unsigned)stall);
	result = __real_sceGeListEnQueue(list, stall, psp_ge_list_callback(cbid), arg);
	psp_ge_enqueued = result;
	psp_trace_line("GE list = %d", result);
	return result;
}

int __real_sceDisplaySetFrameBuf(void *top, int width, int format, int sync);
/* geshow3 (psp_ge.c): the swap's pointers move on the game thread before the
GE has ended the frame they show; the display is set by psp_display_release
once it has */
int psp_display_hold = 0;
static void *display_held_top = NULL;
static int display_held = 0, display_held_width = 0, display_held_format = 0, display_held_sync = 0;
int __wrap_sceDisplaySetFrameBuf(void *top, int width, int format, int sync)
{
	int result;

	if (psp_display_hold)
	{
		display_held_top = top;
		display_held_width = width;
		display_held_format = format;
		display_held_sync = sync;
		display_held = 1;
		return 0;
	}
	psp_trace_line("display %08x, width %d, format %d, sync %d", (unsigned)top, width, format, sync);
	psp_shown_buffer = (unsigned int)top;
	result = __real_sceDisplaySetFrameBuf(top, width, format, sync);
	psp_trace_line("display = %d", result);
	/* "flipwait" (B21): a buffer set for the next frame is only shown at the
	next vblank, but the caller went straight on and the next frame began
	drawing into the buffer still being scanned out: its big level polygons
	(the ocean, the shore) showed half drawn while the view moved. Waiting
	for that vblank here keeps the old buffer off limits until it is off
	the screen */
	if (sync)
	{
		static int flip_wait = -1;
		int psp_keep(const char *name);
		int sceDisplayWaitVblankStart(void);

		if (flip_wait < 0)
			flip_wait = psp_keep("flipwait") != 0;
		if (flip_wait)
			sceDisplayWaitVblankStart();
	}
	return result;
}

int psp_display_release(void)
{
	if (!display_held)
		return 0;
	display_held = 0;
	return __wrap_sceDisplaySetFrameBuf(display_held_top, display_held_width, display_held_format, display_held_sync);
}

int __real_sceIoWrite(SceUID fd, const void *data, SceSize size);
int __wrap_sceIoWrite(SceUID fd, const void *data, SceSize size)
{
	int result;

	int kind = kind_of_fd(fd);

	stick_take_op(kind, "write", path_of_fd(fd));
	result = __real_sceIoWrite(io_real(fd), data, size);
	stick_give();
	if (io_stats > 0 && result > 0)
		io_win[kind].write_bytes += result;
	if (result > 0 && fd >= 0 && fd < TRACE_FILES && fd_pos[fd] >= 0)
		fd_pos[fd] += result;
	return result;
}

int __real_sceIoGetstat(const char *file, SceIoStat *stat);
int __wrap_sceIoGetstat(const char *file, SceIoStat *stat)
{
	int result;

	stick_take_op(IO_OTHER, "stat", file);
	result = __real_sceIoGetstat(file, stat);
	stick_give();
	return result;
}

/* removing and renaming change the folder and its table on the stick, as
a write does: through the lock, and timed */
int __real_sceIoRemove(const char *file);
int __wrap_sceIoRemove(const char *file)
{
	int result;

	stick_take_op(kind_of_path(file), "remove", file);
	result = __real_sceIoRemove(file);
	stick_give();
	return result;
}

int __real_sceIoRename(const char *from, const char *to);
int __wrap_sceIoRename(const char *from, const char *to)
{
	int result;

	stick_take_op(kind_of_path(from), "rename", from);
	result = __real_sceIoRename(from, to);
	stick_give();
	return result;
}

/* build 103: directories, through the same table (and the stick's lock, as
every other Memory Stick call) */
SceUID __real_sceIoDopen(const char *dirname);
SceUID __wrap_sceIoDopen(const char *dirname)
{
	SceUID fd;

	stick_take_op(IO_OTHER, "open folder", dirname);
	fd = __real_sceIoDopen(dirname);
	stick_give();
	if (fd >= 0)
		handle_add(fd, 'd', dirname);
	else
		open_failed(dirname, 'd', fd);
	return fd;
}

int __real_sceIoDclose(SceUID fd);
int __wrap_sceIoDclose(SceUID fd)
{
	int result;

	stick_take();
	result = __real_sceIoDclose(fd);
	stick_give();
	handle_remove(fd, 'd', result);
	return result;
}

/* build 103: libcglue's open() returns the kernel's error code (negative,
but not -1) when sceIoDopen refuses a directory, and newlib's opendir tests
only for -1: it then built a DIR on 0x80020320 and readdir indexed the
descriptor table with it (b102's run 3 crash). Any negative result is
made the -1 the C library promises, with errno set */
#include <errno.h>
#include <fcntl.h>
int __real_open(const char *path, int flags, ...);
int __wrap_open(const char *path, int flags, ...)
{
	va_list arguments;
	int mode, result;

	va_start(arguments, flags);
	mode = va_arg(arguments, int);
	va_end(arguments);
	result = __real_open(path, flags, mode);
	if (result < -1)
	{
		static int logged = 0;

		if (logged++ < 8)
			platform_log("files: open(%s) returned 0x%08x: made -1", path ? path : "?", (unsigned)result);
		errno = (unsigned)result == 0x80020320 ? EMFILE : EIO;
		return -1;
	}
	return result;
}
