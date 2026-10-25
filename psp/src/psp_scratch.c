/* psp_scratch.c - the scratchpad (build 108)

The PSP-1000's CPU has 16 KB of SRAM at 0x00010000 beside it, off the system
bus: a load from it costs what a data-cache hit costs (24 ns), and it never
waits for the GE's memory traffic and never evicts anything from the 16 KB
data cache (halo_bench v9, psp_hardware_reference.md). Nothing in the game
used it before this build.

What goes there is decided by measurement: the emulator's cache model
(PSP_DCACHE, tools/ppsspp_local_patches.diff) counts every data miss by the
data it touched and the code that touched it (tools/cachemap.py), so a
table earns a place by the misses it takes per byte. docs/overnight_b108.md
has the figures.

It is plain memory to every thread on the CPU, saved by nothing: so the
space is handed out once, at start-up, in fixed pieces (psp_scratch_take),
each with one job, and never freed or shared as working space. The pieces
(b108, 15.1 of 16 KB): the near-cut corner table (psp_ge.c's model_cut,
8,448 bytes) and the profiler's slots and front table (psp_platform.c, 6,656;
the heartbeat thread reads them for its report, as it read the old table).

keep.txt "noscratch": every piece comes from the heap instead, for the
hardware A/B (the same code, the same data, only its place differs). */
#include <pspkernel.h>
#include <stdlib.h>
#include <string.h>

void *memalign(size_t alignment, size_t size); /* newlib's; the SDK headers leave it out */
void platform_log(const char *format, ...);
int psp_keep(const char *name);

#define SCRATCH_BASE 0x00010000u
#define SCRATCH_BYTES 0x4000u

static unsigned int scratch_used = 0;
static int scratch_off = -1;

/* bytes from the scratchpad (16-byte aligned), or from the heap with
"noscratch" or when it is full; never freed. what: for the log */
void *psp_scratch_take(unsigned int bytes, const char *what)
{
	unsigned int at = (scratch_used + 15) & ~15u;
	void *pointer;

	if (scratch_off < 0)
		scratch_off = psp_keep("noscratch");
	if (!scratch_off && at + bytes <= SCRATCH_BYTES)
	{
		pointer = (void *)(SCRATCH_BASE + at);
		scratch_used = at + bytes;
		memset(pointer, 0, bytes);
		platform_log("scratchpad: %u bytes for %s at %08x (%u of %u used)", bytes, what, SCRATCH_BASE + at,
			scratch_used, SCRATCH_BYTES);
		return pointer;
	}
	pointer = memalign(16, bytes);
	if (pointer)
		memset(pointer, 0, bytes);
	platform_log("scratchpad: %u bytes for %s from the heap (%s)", bytes, what,
		scratch_off ? "noscratch" : "the scratchpad is full");
	return pointer;
}

/* whether a pointer is the scratchpad's (for the reports) */
int psp_scratch_holds(const void *pointer)
{
	unsigned int at = (unsigned int)pointer;

	return at >= SCRATCH_BASE && at < SCRATCH_BASE + SCRATCH_BYTES;
}

/* execution probe (keep.txt "spadexec"): can the CPU run code from the
scratchpad, and how fast? Nothing documents it (docs/research/
research_icache.md, open question 4). The tick's instruction misses are
mostly capacity misses (more code than the 16 KB cache holds), so code that
could run from the scratchpad without going through the instruction cache
would take the hottest few KB out of that competition. The same counting
loop runs from a heap buffer (through the cache) and from the scratchpad,
and the log says how long each took. A fault here leaves the "calling" line
last in the log and the crash handler's address in the scratchpad's range */
static const unsigned int probe_loop[] = {
	0x00001021, /* addu  v0, zero, zero */
	0x24420001, /* addiu v0, v0, 1 */
	0x1444fffe, /* bne   v0, a0, (the addiu) */
	0x00000000, /* nop */
	0x03e00008, /* jr    ra */
	0x00000000, /* nop */
};

static unsigned int probe_time(void *code, unsigned int count, unsigned int *result)
{
	unsigned int (*run)(unsigned int) = (unsigned int (*)(unsigned int))code;
	unsigned int start;

	(void)run(16); /* once to warm whichever cache it goes through */
	start = sceKernelGetSystemTimeLow();
	*result = run(count);
	return sceKernelGetSystemTimeLow() - start;
}

void psp_scratch_exec_probe(void)
{
	const unsigned int count = 3000000, bytes = sizeof(probe_loop);
	unsigned int ram_us, scratch_us, ram_result = 0, scratch_result = 0;
	void *ram, *scratch;

	if (!psp_keep("spadexec"))
		return;
	ram = memalign(64, 64);
	scratch = psp_scratch_take(64, "the execution probe");
	if (!ram || !psp_scratch_holds(scratch))
	{
		platform_log("spadexec: no room for the probe (heap %p, scratchpad %p)", ram, scratch);
		return;
	}
	memcpy(ram, probe_loop, bytes);
	memcpy(scratch, probe_loop, bytes);
	sceKernelDcacheWritebackAll();
	sceKernelIcacheInvalidateAll();
	ram_us = probe_time(ram, count, &ram_result);
	platform_log("spadexec: %u loops from main RAM (cached): %u us; calling the scratchpad copy at %p", count, ram_us, scratch);
	scratch_us = probe_time(scratch, count, &scratch_result);
	platform_log("spadexec: %u loops from the scratchpad: %u us (main RAM %u us); results %u and %u (3 instructions a loop: %.2f and %.2f cycles each at 333 MHz)",
		count, scratch_us, ram_us, scratch_result, ram_result,
		(double)scratch_us * 333.0 / (3.0 * count), (double)ram_us * 333.0 / (3.0 * count));
}

/* the second probe (keep.txt "spadexec2"): does code run from the
scratchpad go through the instruction cache? The first probe showed it runs
at cached speed, which a loop small enough to stay in the cache would show
either way. Two 12 KB straight-line routines, one in main RAM and one in the
scratchpad, are run in turn: if the scratchpad's fetches bypass the cache,
the main-RAM routine keeps its 12 KB of the 16 KB cache and stays fast; if
they are cached, the two (24 KB) evict each other every pass and the
main-RAM routine misses on every line. It takes 12 KB of the scratchpad
before the other pieces ask, so most of them go to the heap for that run */
#define PROBE2_WORDS 3072u

static void probe2_fill(unsigned int *code)
{
	unsigned int k;

	code[0] = 0x00001021; /* addu v0, zero, zero */
	for (k = 1; k < PROBE2_WORDS - 2; k++)
		code[k] = 0x24420001; /* addiu v0, v0, 1 */
	code[PROBE2_WORDS - 2] = 0x03e00008; /* jr ra */
	code[PROBE2_WORDS - 1] = 0x00000000; /* nop */
}

static unsigned int probe2_run(void *a, void *b, unsigned int passes)
{
	unsigned int (*run_a)(void) = (unsigned int (*)(void))a, (*run_b)(void) = (unsigned int (*)(void))b;
	unsigned int start = sceKernelGetSystemTimeLow(), k, sum = 0;

	for (k = 0; k < passes; k++)
	{
		sum += run_a();
		if (run_b)
			sum += run_b();
	}
	(void)sum;
	return sceKernelGetSystemTimeLow() - start;
}

void psp_scratch_exec_probe2(void)
{
	const unsigned int bytes = PROBE2_WORDS * 4u, passes = 200;
	unsigned int ram_alone, scratch_alone, both;
	void *ram, *scratch;

	if (!psp_keep("spadexec2"))
		return;
	ram = memalign(64, bytes);
	scratch = psp_scratch_take(bytes, "the second execution probe");
	if (!ram || !psp_scratch_holds(scratch))
	{
		platform_log("spadexec2: no room for the probe (heap %p, scratchpad %p)", ram, scratch);
		return;
	}
	probe2_fill((unsigned int *)ram);
	probe2_fill((unsigned int *)scratch);
	sceKernelDcacheWritebackAll();
	sceKernelIcacheInvalidateAll();
	(void)probe2_run(ram, NULL, 2);
	ram_alone = probe2_run(ram, NULL, passes);
	(void)probe2_run(scratch, NULL, 2);
	scratch_alone = probe2_run(scratch, NULL, passes);
	both = probe2_run(ram, scratch, passes);
	platform_log("spadexec2: %u passes of 12 KB: main RAM alone %u us, scratchpad alone %u us, both in turn %u us (alone summed %u us): %s",
		passes, ram_alone, scratch_alone, both, ram_alone + scratch_alone,
		both <= (ram_alone + scratch_alone) * 5 / 4 ? "the scratchpad's fetches leave the instruction cache alone" :
		"the scratchpad's fetches go through the instruction cache (the two evict each other)");
}
