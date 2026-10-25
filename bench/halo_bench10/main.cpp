/*
halo_bench v10: the Memory Stick and the Media Engine, measured for the port.

Questions, each answered on a PSP-1000 (the emulator models neither):
  Memory Stick (the game's texture and sound streaming)
  1. How fast are sequential reads at each request size, 512 bytes to 1 MB,
     and what does each call cost on its own?
  2. What does a seek cost (random reads, 4 KB and 64 KB)?
  3. While an asynchronous read (sceIoReadAsync) is in flight, how much of
     the CPU is left for the game, at the I/O's default priority and at a
     priority above the game's? And do the CPU's cache misses slow down
     while the Memory Stick's data comes in?
  4. Is ARK-4's "Memory Stick Speedup" on? (read from ARK's own settings;
     its code, core/systemctrl/src/msstor_cache.c, is a single 4 KB
     read-ahead buffer under the FAT driver: it can only help reads of 4 KB
     or less, so the small sizes in 1 are where it would show)
  Media Engine (an offload design needs these)
  5. A round trip, main CPU -> ME -> main CPU, through uncached shared
     memory: how long?
  6. Handing the ME a block of data: the main CPU's cache write-back, the
     ME's invalidate and read, for 256 bytes to 64 KB. And what happens
     without the ME's invalidate (stale data, shown, not assumed)?
  7. The ME's own cache-miss time, against the main CPU's
  8. Contention: do the main CPU's misses slow down while the ME streams
     memory, chases pointers, or only computes? And the ME's streaming,
     with the main CPU idle and busy?

Results print on screen and append to ms0:/halo_bench10.txt (a run with the
Speedup on and one with it off both stay in the file).

The Media Engine part runs on mcidclan's me-core-mapper library (MIT,
copyright (c) 2026 mcidclan (m-c/d, m-cid),
https://github.com/mcidclan/psp-media-engine-custom-core). As in the ME test:
don't press HOME while the ME part runs; the program stops the ME and exits
by itself.
*/

#include <pspkernel.h>
#include <pspdebug.h>
#include <pspctrl.h>
#include <psppower.h>
#include <psprtc.h>
#include <pspiofilemgr.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <malloc.h>
#include <me-core-mapper/me-core.h>

PSP_MODULE_INFO("halo_bench10", 0, 1, 0);
PSP_MAIN_THREAD_ATTR(PSP_THREAD_ATTR_USER | PSP_THREAD_ATTR_VFPU);
PSP_HEAP_SIZE_KB(14 * 1024);

/* ---------- ARK-4's settings (common/include/systemctrl_se.h; imported
from SystemCtrlForUser, sysctrl_import.S). Copies only when the size
matches ARK's: zeroed (magic 0) means another version, unknown */
typedef struct
{
	u32 magic;
	s16 iso_cache_size, iso_cache_num;
	u8 iso_cache, iso_cache_partition, umdseek, umdspeed, cpubus_clock, disable_pause, hidedlc, umdregion,
		vshregion, usbdevice, usbcharge, hidemac, noanalog, qaflags, launcher_mode, hidepics,
		usbdevice_rdonly, skiplogos, noumd, hibblock, oldplugin, msspeed, noled, wpa2, force_high_memory,
		custom_update;
} SEConfig;
extern "C" SEConfig *sctrlSEGetConfigEx(SEConfig *config, int size);

/* ---------- output, timing */
static const char *log_path = "ms0:/halo_bench10.txt";
static volatile u32 sink;

static void out(const char *format, ...)
{
	char line[320];
	va_list arguments;

	va_start(arguments, format);
	vsnprintf(line, sizeof(line) - 2, format, arguments);
	va_end(arguments);
	pspDebugScreenPrintf("%s\n", line);
	SceUID file = sceIoOpen(log_path, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_APPEND, 0777);
	if (file >= 0)
	{
		strcat(line, "\n");
		sceIoWrite(file, line, strlen(line));
		sceIoClose(file);
	}
}

static u64 now_us(void)
{
	u64 tick;

	sceRtcGetCurrentTick(&tick);
	return tick * 1000000ull / sceRtcGetTickResolution();
}

/* ---------- buffers */
#define CHASE_BYTES (4 * 1024 * 1024)
#define STREAM_BYTES (1024 * 1024)
#define READ_BYTES (1024 * 1024)
#define PAYLOAD_MAX (64 * 1024)

static u32 *chase;     /* 4 MB: random 64-byte lines, a dependent chain */
static u32 *chase_me;  /* 4 MB more, for the ME's chase while the CPU chases its own */
static u32 *stream;    /* 1 MB the ME streams through */
static u32 *payload;   /* the block handed to the ME */
static u8 *readbuf;    /* Memory Stick reads land here */

/* a random cyclic chain through every line (each line holds the next's word index) */
static void build_chase(u32 *base, u32 bytes, u32 seed)
{
	u32 lines = bytes / 64, x = 0, i;

	for (i = 0; i < lines; i++)
	{
		u32 next = (x * 1103515245u + 12345u + seed * 64u) & (lines - 1);

		base[x * 16] = next * 16;
		x = next;
	}
}

static double chase_ns(const u32 *base, u32 steps)
{
	u32 i = 0, n = steps;
	u64 start = now_us();

	while (n--)
		i = *(volatile const u32 *)&base[i];
	sink = i;
	return (double)(now_us() - start) * 1000.0 / steps;
}

/* a unit of the CPU's own work: 64 dependent adds, 1000 times (64,000
cycles, 0.19 ms at 333 MHz, no memory) */
#define ADD1 "addiu $9, $9, 1\n"
#define ADD4 ADD1 ADD1 ADD1 ADD1
#define ADD16 ADD4 ADD4 ADD4 ADD4
#define ADD64 ADD16 ADD16 ADD16 ADD16
static void work_unit(void)
{
	u32 n = 1000;

	__asm__ volatile(".set push\n.set noreorder\nmove $9, $0\n1:\n" ADD64
		"addiu %0, %0, -1\nbnez %0, 1b\nnop\n.set pop\n" : "+r"(n) : : "$9");
}

/* ============================================================ Memory Stick */

static SceUID ms_file = -1;
static SceOff ms_size = 0, ms_position = 0;
static char ms_path[160];

/* the largest .map in ms0:/halo/maps */
static int ms_open(void)
{
	SceUID dir = sceIoDopen("ms0:/halo/maps");
	SceIoDirent entry;
	SceOff best = 0;

	if (dir < 0)
		return 0;
	memset(&entry, 0, sizeof(entry));
	while (sceIoDread(dir, &entry) > 0)
	{
		int length = strlen(entry.d_name);

		if (length > 4 && !strcasecmp(entry.d_name + length - 4, ".map") && entry.d_stat.st_size > best)
		{
			best = entry.d_stat.st_size;
			snprintf(ms_path, sizeof(ms_path), "ms0:/halo/maps/%s", entry.d_name);
		}
		memset(&entry, 0, sizeof(entry));
	}
	sceIoDclose(dir);
	if (best < 16 * 1024 * 1024)
		return 0;
	ms_size = best;
	ms_file = sceIoOpen(ms_path, PSP_O_RDONLY, 0);
	return ms_file >= 0;
}

/* each test reads a part of the file no earlier test touched (no cache can
help), moving on 64 KB-aligned; wraps at the end */
static SceOff ms_fresh(u32 bytes)
{
	SceOff at;

	if (ms_position + bytes > ms_size)
		ms_position = 0;
	at = ms_position;
	ms_position = (ms_position + bytes + 65535) & ~(SceOff)65535;
	return at;
}

static void ms_sequential(void)
{
	static const u32 sizes[] = {512, 2048, 4096, 16384, 65536, 262144, 1048576};
	static const u32 totals[] = {256 * 1024, 512 * 1024, 1024 * 1024, 2048 * 1024, 4096 * 1024, 4096 * 1024, 4096 * 1024};
	u32 k;

	out("-- 1. sequential reads (each size from a part of the file not read before) --");
	for (k = 0; k < sizeof(sizes) / sizeof(sizes[0]); k++)
	{
		u32 calls = totals[k] / sizes[k], c, bad = 0;
		SceOff at = ms_fresh(totals[k]);
		u64 start;
		double us;

		sceIoLseek(ms_file, at, PSP_SEEK_SET);
		start = now_us();
		for (c = 0; c < calls; c++)
			if (sceIoRead(ms_file, readbuf, sizes[k]) != (int)sizes[k])
				bad++;
		us = (double)(now_us() - start);
		out("%7lu bytes a read: %4lu KB in %7.0f us = %6.0f KB/s; %6.0f us a call%s", (unsigned long)sizes[k],
			(unsigned long)(totals[k] / 1024), us, totals[k] / 1024.0 / (us / 1e6), us / calls, bad ? " (SHORT READS)" : "");
	}
}

static void ms_random(void)
{
	static const u32 sizes[] = {4096, 65536};
	u32 k, c, seed = 777;

	out("-- 2. random reads (32 each, anywhere in the file) --");
	for (k = 0; k < 2; k++)
	{
		u64 start = now_us();

		for (c = 0; c < 32; c++)
		{
			seed = seed * 1103515245u + 12345u;
			SceOff at = ((SceOff)(seed >> 4) % (ms_size / 65536 - 1)) * 65536;

			sceIoLseek(ms_file, at, PSP_SEEK_SET);
			sceIoRead(ms_file, readbuf, sizes[k]);
		}
		double us = (double)(now_us() - start) / 32;
		out("%6lu bytes at a random place: %6.0f us a read (sequential at this size: see 1)", (unsigned long)sizes[k], us);
	}
}

/* 3: an asynchronous read and the CPU's work beside it */
static double units_alone_per_ms(void)
{
	u32 units = 0;
	u64 start = now_us();

	while (now_us() - start < 200000)
	{
		work_unit();
		units++;
	}
	return units / ((now_us() - start) / 1000.0);
}

static void ms_async(void)
{
	double rate = units_alone_per_ms(), chase_alone, us;
	u64 start;
	int priority_case;

	out("-- 3. a 1 MB read, and the CPU beside it --");
	out("CPU work alone: %.2f units a ms (a unit: 64,000 cycles of adds)", rate);
	{
		SceOff at = ms_fresh(READ_BYTES);

		sceIoLseek(ms_file, at, PSP_SEEK_SET);
		start = now_us();
		sceIoRead(ms_file, readbuf, READ_BYTES);
		us = (double)(now_us() - start);
		out("sceIoRead, waiting: %.0f us (%.0f KB/s)", us, 1024.0 / (us / 1e6));
	}
	for (priority_case = 0; priority_case < 2; priority_case++)
	{
		SceOff at = ms_fresh(READ_BYTES);
		u32 units = 0;
		SceInt64 result = 0;
		int changed;

		/* 0x10: above this thread's 0x20, first; then -1, the default
		(the emulator: at the default the read never ran while this thread
		stayed busy; the PSP's scheduler doesn't share time between threads
		of one priority) */
		changed = sceIoChangeAsyncPriority(ms_file, priority_case ? -1 : 0x10);
		sceIoLseek(ms_file, at, PSP_SEEK_SET);
		start = now_us();
		if (sceIoReadAsync(ms_file, readbuf, READ_BYTES) < 0)
		{
			out("sceIoReadAsync failed");
			return;
		}
		int finished = 1;

		while (sceIoPollAsync(ms_file, &result) == 1)
		{
			work_unit();
			units++;
			if (now_us() - start > 3000000)
			{
				finished = 0;
				break;
			}
		}
		us = (double)(now_us() - start);
		out("sceIoReadAsync at priority %s (change returned %08x): %s after %.0f us; the CPU did %lu units = %.0f%% of its speed alone; read %lld bytes",
			priority_case ? "default" : "0x10 (above the game's)", (unsigned)changed,
			finished ? "done" : "NOT DONE (the read waits for the busy thread to block)", us, (unsigned long)units,
			100.0 * units / (rate * us / 1000.0), (long long)result);
		if (!finished)
		{
			/* blocking lets it run: how long it then takes */
			start = now_us();
			sceIoWaitAsync(ms_file, &result);
			out("  then waiting for it (the thread blocked): %.0f us more", (double)(now_us() - start));
		}
	}
	/* the CPU's misses while the read's data arrives */
	sceKernelDcacheWritebackInvalidateAll();
	chase_alone = chase_ns(chase, 200000);
	{
		SceOff at = ms_fresh(READ_BYTES);
		SceInt64 result = 0;
		u32 steps = 0, i = 0;

		sceIoChangeAsyncPriority(ms_file, 0x10);
		sceIoLseek(ms_file, at, PSP_SEEK_SET);
		start = now_us();
		sceIoReadAsync(ms_file, readbuf, READ_BYTES);
		while (sceIoPollAsync(ms_file, &result) == 1 && now_us() - start < 10000000)
		{
			u32 n = 2000;

			while (n--)
				i = *(volatile const u32 *)&chase[i];
			steps += 2000;
		}
		sink = i;
		us = (double)(now_us() - start);
		out("cache misses: %.1f ns each alone; %.1f ns each while the read ran (%.0f us, its time included)",
			chase_alone, us * 1000.0 / (steps ? steps : 1), us);
	}
}

static void memory_stick_tests(void)
{
	SEConfig config;

	memset(&config, 0, sizeof(config));
	sctrlSEGetConfigEx(&config, sizeof(config));
	if (config.magic)
		out("ARK-4 settings: Memory Stick Speedup %s (msspeed %u), CPU clock setting %u",
			config.msspeed ? "ON" : "off", config.msspeed, config.cpubus_clock);
	else
		out("ARK-4 settings: not readable (another ARK version, or no ARK): note the Speedup setting by hand");
	if (!ms_open())
	{
		out("no .map of 16 MB or more in ms0:/halo/maps: skipping the Memory Stick tests");
		return;
	}
	out("reading %s (%lld MB)", ms_path, (long long)(ms_size >> 20));
	ms_sequential();
	ms_random();
	ms_async();
	sceIoClose(ms_file);
	ms_file = -1;
}

/* ============================================================ Media Engine */

/* shared, uncached. The main CPU sets op and arguments, then the sequence
number; the ME sees the new number, works, writes the result, then the
acknowledgement (the same number) */
meLibSetSharedUncached32(16);
#define me_sequence (meLibSharedMemory[0])
#define me_op       (meLibSharedMemory[1])
#define me_arg0     (meLibSharedMemory[2])
#define me_arg1     (meLibSharedMemory[3])
#define me_arg2     (meLibSharedMemory[4])
#define me_ack      (meLibSharedMemory[5])
#define me_result   (meLibSharedMemory[6])
#define me_stop     (meLibSharedMemory[7])
#define me_alive    (meLibSharedMemory[8])
#define me_halted   (meLibSharedMemory[9])

enum { OP_PING = 1, OP_SUM, OP_CHASE, OP_STREAM_UNTIL_STOP, OP_CHASE_UNTIL_STOP, OP_ADDS_UNTIL_STOP, OP_HALT };

/* ---------- the ME's side: no kernel calls, no library code but mecore's */
static u32 me_sum(const u32 *base, u32 bytes)
{
	u32 sum = 0, k;

	for (k = 0; k < bytes / 4; k++)
		sum += base[k];
	return sum;
}

__attribute__((noinline, aligned(4)))
void meLibOnProcess(void)
{
	u32 seen = 0;

	meLibDcacheWritebackInvalidateAll();
	for (;;)
	{
		u32 sequence = me_sequence;

		if (sequence == seen)
		{
			me_alive++;
			continue;
		}
		seen = sequence;
		u32 op = me_op, a0 = me_arg0, a1 = me_arg1, a2 = me_arg2, result = 0;

		switch (op)
		{
		case OP_PING:
			break;
		case OP_SUM:
			/* a2: invalidate first (the right way) or not (to show why) */
			if (a2)
				meLibDcacheInvalidateRange(a0, a1);
			result = me_sum((const u32 *)a0, a1);
			break;
		case OP_CHASE:
		{
			const u32 *base = (const u32 *)a0;
			u32 i = 0, n = a1;

			while (n--)
				i = *(volatile const u32 *)&base[i];
			result = i;
			break;
		}
		case OP_STREAM_UNTIL_STOP:
		{
			/* acknowledged at the start, the passes counted in result */
			me_result = 0;
			me_ack = sequence;
			while (!me_stop)
			{
				/* the sum must be used, or the compiler drops the reads (v10
				on hardware: "2.4 million 1 MB passes a second") */
				sink = me_sum((const u32 *)a0, a1);
				me_result = me_result + 1;
			}
			continue;
		}
		case OP_CHASE_UNTIL_STOP:
		{
			const u32 *base = (const u32 *)a0;
			u32 i = 0, steps = 0;

			me_result = 0;
			me_ack = sequence;
			while (!me_stop)
			{
				u32 n = 1000;

				while (n--)
					i = *(volatile const u32 *)&base[i];
				steps += 1000;
				me_result = steps;
			}
			continue;
		}
		case OP_ADDS_UNTIL_STOP:
		{
			u32 x = 0, rounds = 0;

			me_result = 0;
			me_ack = sequence;
			while (!me_stop)
			{
				u32 n = 1000;

				while (n--)
					x = x * 1664525u + 1013904223u;
				rounds++;
				me_result = rounds;
			}
			sink = x;
			continue;
		}
		case OP_HALT:
			me_ack = sequence;
			me_halted = 1;
			meLibHalt();
			break;
		}
		me_result = result;
		me_ack = sequence;
	}
}

/* ---------- the main CPU's side */
static u32 sequence = 0;

static void me_issue(u32 op, u32 a0, u32 a1, u32 a2)
{
	me_op = op;
	me_arg0 = a0;
	me_arg1 = a1;
	me_arg2 = a2;
	__asm__ volatile("sync");
	me_sequence = ++sequence;
}

/* waits for the acknowledgement; 0 after timeout_us */
static int me_wait(u32 timeout_us)
{
	u64 start = now_us();

	while (me_ack != sequence)
		if (now_us() - start > timeout_us)
			return 0;
	return 1;
}

static int me_call(u32 op, u32 a0, u32 a1, u32 a2)
{
	me_issue(op, a0, a1, a2);
	return me_wait(5000000);
}

static void me_round_trips(void)
{
	const u32 rounds = 20000;
	u32 k;
	u64 start;

	out("-- 5. round trips: the CPU asks, the ME answers (uncached shared words) --");
	start = now_us();
	for (k = 0; k < rounds; k++)
	{
		me_issue(OP_PING, 0, 0, 0);
		while (me_ack != sequence)
			;
	}
	out("%lu round trips: %.0f ns each", (unsigned long)rounds, (double)(now_us() - start) * 1000.0 / rounds);
}

static void me_payloads(void)
{
	static const u32 sizes[] = {256, 4096, 16384, 65536};
	u32 k;

	out("-- 6. handing the ME a block: CPU write-back, ME invalidate and read, answer (us) --");
	for (k = 0; k < 4; k++)
	{
		u32 bytes = sizes[k], reps = bytes >= 16384 ? 50 : 500, r, w, mismatches = 0;
		double wb_us = 0, trip_us = 0;

		for (r = 0; r < reps; r++)
		{
			u32 expected = 0;
			u64 t0, t1, t2;

			/* new contents each time, written through the CPU's cache */
			for (w = 0; w < bytes / 4; w++)
			{
				payload[w] = w * 2654435761u + r;
				expected += payload[w];
			}
			t0 = now_us();
			sceKernelDcacheWritebackRange(payload, bytes);
			t1 = now_us();
			if (!me_call(OP_SUM, (u32)payload, bytes, 1))
			{
				out("the ME did not answer");
				return;
			}
			t2 = now_us();
			wb_us += (double)(t1 - t0);
			trip_us += (double)(t2 - t1);
			if (me_result != expected)
				mismatches++;
		}
		out("%6lu bytes: CPU write-back %7.1f | ME invalidate + read + answer %8.1f | wrong sums %lu of %lu",
			(unsigned long)bytes, wb_us / reps, trip_us / reps, (unsigned long)mismatches, (unsigned long)reps);
	}
	/* the hazard: the ME reads 4 KB (now in its cache), the CPU changes it
	and writes it back, the ME reads again without invalidating */
	{
		u32 w, expected = 0;

		for (w = 0; w < 1024; w++)
			payload[w] = w;
		sceKernelDcacheWritebackRange(payload, 4096);
		me_call(OP_SUM, (u32)payload, 4096, 1);
		for (w = 0; w < 1024; w++)
		{
			payload[w] = w * 3 + 1;
			expected += payload[w];
		}
		sceKernelDcacheWritebackRange(payload, 4096);
		me_call(OP_SUM, (u32)payload, 4096, 0);
		out("without the ME's invalidate: sum %08x, right %08x (%s)", (unsigned)me_result, (unsigned)expected,
			me_result == expected ? "same: the ME's cache didn't hold the old lines" : "STALE: the ME read its old cached copy");
	}
}

static void me_latency_and_contention(void)
{
	double cpu_alone, me_ns;
	u64 start;

	out("-- 7. cache misses: the ME's against the CPU's (4 MB, random lines) --");
	sceKernelDcacheWritebackInvalidateAll();
	cpu_alone = chase_ns(chase, 200000);
	start = now_us();
	me_call(OP_CHASE, (u32)chase_me, 200000, 0);
	me_ns = (double)(now_us() - start) * 1000.0 / 200000;
	out("CPU %.1f ns a miss; ME %.1f ns a miss (the round trip included: under 0.1%%)", cpu_alone, me_ns);

	out("-- 8. contention: the CPU's misses while the ME works --");
	{
		static const char *labels[3] = {"streams 1 MB over and over", "chases its own 4 MB", "only computes"};
		static const u32 ops[3] = {OP_STREAM_UNTIL_STOP, OP_CHASE_UNTIL_STOP, OP_ADDS_UNTIL_STOP};
		u32 k;

		for (k = 0; k < 3; k++)
		{
			double cpu_with;
			u32 me_work;
			u64 t0;

			me_stop = 0;
			me_issue(ops[k], k == 0 ? (u32)stream : (u32)chase_me, k == 0 ? STREAM_BYTES : 0, 0);
			if (!me_wait(2000000))
			{
				out("the ME did not start");
				return;
			}
			t0 = now_us();
			sceKernelDcacheWritebackInvalidateAll();
			cpu_with = chase_ns(chase, 200000);
			me_work = me_result;
			double seconds = (now_us() - t0) / 1e6;
			me_stop = 1;
			sceKernelDelayThread(20000);
			out("ME %-28s: CPU %.1f ns a miss (alone %.1f); the ME's rate meanwhile: %.0f a second (%s)", labels[k],
				cpu_with, cpu_alone, me_work / seconds, k == 0 ? "1 MB passes" : k == 1 ? "misses" : "1000-step rounds");
		}
	}
	/* the other way: the ME streaming, with the CPU asleep and with the CPU
	missing constantly */
	{
		u32 passes_idle, passes_busy;
		u64 t0;
		double idle_s, busy_s;

		me_stop = 0;
		me_issue(OP_STREAM_UNTIL_STOP, (u32)stream, STREAM_BYTES, 0);
		me_wait(2000000);
		t0 = now_us();
		sceKernelDelayThread(500000);
		passes_idle = me_result;
		idle_s = (now_us() - t0) / 1e6;
		t0 = now_us();
		{
			u32 base_passes = me_result;

			chase_ns(chase, 400000);
			passes_busy = me_result - base_passes;
		}
		busy_s = (now_us() - t0) / 1e6;
		me_stop = 1;
		sceKernelDelayThread(20000);
		out("ME streaming: %.1f MB/s with the CPU asleep, %.1f MB/s with the CPU missing constantly",
			passes_idle / idle_s, passes_busy / busy_s);
	}
}

static void media_engine_tests(void)
{
	int table;
	u32 alive;

	out("-- the Media Engine (mecore) --");
	table = meLibDefaultInit();
	out("meLibDefaultInit: %d", table);
	alive = me_alive;
	sceKernelDelayThread(100000);
	if (me_alive == alive)
	{
		out("the ME isn't running; skipping its tests");
		return;
	}
	me_round_trips();
	me_payloads();
	me_latency_and_contention();
	me_stop = 1;
	me_issue(OP_HALT, 0, 0, 0);
	for (int k = 0; k < 20 && !me_halted; k++)
		sceKernelDelayThread(50000);
	out("the ME %s", me_halted ? "stopped cleanly" : "did NOT acknowledge the stop");
}

int main(void)
{
	SceCtrlData pad;
	ScePspDateTime time;

	pspDebugScreenInit();
	scePowerSetClockFrequency(333, 333, 166);
	sceRtcGetCurrentClockLocalTime(&time);
	out("");
	out("==== halo_bench v10, %04d-%02d-%02d %02d:%02d:%02d; CPU %d MHz, bus %d MHz ====", time.year, time.month, time.day,
		time.hour, time.minute, time.second, scePowerGetCpuClockFrequencyInt(), scePowerGetBusClockFrequencyInt());
	chase = (u32 *)memalign(64, CHASE_BYTES);
	chase_me = (u32 *)memalign(64, CHASE_BYTES);
	stream = (u32 *)memalign(64, STREAM_BYTES);
	payload = (u32 *)memalign(64, PAYLOAD_MAX);
	readbuf = (u8 *)memalign(64, READ_BYTES);
	if (!chase || !chase_me || !stream || !payload || !readbuf)
	{
		out("could not allocate the buffers");
		sceKernelDelayThread(5000000);
		sceKernelExitGame();
		return 0;
	}
	build_chase(chase, CHASE_BYTES, 0);
	build_chase(chase_me, CHASE_BYTES, 1);
	memset(stream, 1, STREAM_BYTES);
	sceKernelDcacheWritebackInvalidateAll();

	memory_stick_tests();
	media_engine_tests();

	out("done. X or 30 s to exit.");
	for (int k = 0; k < 300; k++)
	{
		sceCtrlPeekBufferPositive(&pad, 1);
		if (pad.Buttons & PSP_CTRL_CROSS)
			break;
		sceKernelDelayThread(100000);
	}
	sceKernelExitGame();
	return 0;
}
