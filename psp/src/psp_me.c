/* psp_me.c - jobs on the Media Engine (build 86: the sound mixer; build 87:
job slots and the ME's own timing).

The PSP's second CPU, the Media Engine (ME), does nothing for the game: its
firmware runs the XMB's music and video decoders, which Halo doesn't use.
Build 44's test (halo_me, a PSP-1000, 2026-10-03) ran our C on it with
mcidclan's me-core-mapper library; build 86 gave it the game's sound mixer
(b86's run 2: ~2,500 blocks, none failed, a clean HOME exit).

A job, start to end:
	1. the main CPU writes its cache back, so main RAM holds what the ME
	   must read;
	2. it fills a SLOT (a few words of UNCACHED memory: both CPUs see the
	   same words at once) and bumps the slot's request number;
	3. the ME, polling its slots, drops its own cache (stale copies from the
	   last job), runs the job (integers or FPU, never the VFPU, which the ME
	   lacks; no system calls, no library calls), writes its cache back, and
	   sets the slot's done number;
	4. the main CPU drops its cached copies of what the ME changed.
The ME writes only what the job owns, on whole 64-byte cache lines (a line
written back carries all 64 bytes, so a shared line would overwrite a
neighbour's newer bytes), and its own stack in its private memory.

One slot per client thread (the audio thread; the renderer later), so two
threads never share a slot; the ME serves the slots in order, audio first.

Timing: the ME reads its own cycle counter (coprocessor 0 register 9)
around each job; its rate is measured against the main CPU's microsecond
clock at start. So the log has the ME's own time a job, its share of the
ME, and the main CPU's side (hand-over and waiting) apart.

While idle, the ME reads the uncached words once every ~25 us and otherwise
spins in registers: the bus is the PSP's scarce resource (memory_traffic.md).

Stopping: the ME must be stopped before the PSP leaves the game (HOME, quit)
or sleeps (an ME left running crashed the PSP on HOME). Stop asks, waits for
the ME to acknowledge and halt itself; with no answer it holds the ME in
reset through the kernel. The library's sleep handler (it replaces the
firmware's own ME one) holds it in reset too. After a stop every job runs on
the main CPU instead.

After a sleep the ME stays stopped unless "mewake" asks for it to be
started again ("the wake" below): a reset release from the main thread at a
frame's end, once no job of any slot or ring ticket can still be out. On
the PSP-1000 the first such restart found no ME answering, and the HOME exit
after it hung the PSP, so it is opt-in until a PSP shows it working.

The ME is the firmware's until the next reboot of the PSP once taken: XMB
music and video stay off until then. PPSSPP doesn't run ME code at all:
there, each job runs on the main CPU ("simulated"), to exercise everything
but the ME itself.

Built on mcidclan's me-core-mapper (psp-media-engine-custom-core), MIT
licence, copyright (c) 2025-2026 mcidclan (m-c/d, m-cid); LICENSE-mecore.md
ships beside the EBOOT. */
#include <pspkernel.h>
#include <pspiofilemgr.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <malloc.h>
#include <string.h>
#include <unistd.h>
#include <me-core-mapper/me-core.h>
#include "psp_me.h"
#include "psp_paths.h"   /* ms0:/halo or ms0:/halomp, and the maps */

void platform_log(const char *format, ...);
void psp_log_flush(void);
int psp_keep(const char *name);
int psp_keep_number(const char *prefix, int fallback);

/* the jobs (their own files): run on the ME, or on the main CPU when
simulated or after a stop */
unsigned long psp_dsound_mix_list(void *const *list, unsigned long count, int *sums, short *output,
	unsigned long frames, unsigned long *clipped);
unsigned long psp_dsound_mix_split(void *const *list, unsigned long count, int *sums, short *output,
	unsigned long frames, unsigned long *clipped, unsigned int *split);
unsigned int psp_ge_me_classify(const unsigned int *args, unsigned int *results);
unsigned int psp_ge_me_angle(const unsigned int *args, unsigned int *results);
unsigned int psp_ge_me_replay(const unsigned int *args, unsigned int *results);
unsigned int psp_ge_me_slant(const unsigned int *args, unsigned int *results);
void psp_perception_me_rays(const unsigned int *args, unsigned int *results);
void psp_pose_me_make(const unsigned int *args, unsigned int *results); /* objects.c (psp_fixups.py FIXES_131) */
void psp_points_me_make(const unsigned int *args, unsigned int *results); /* point_physics.c (psp_fixups.py FIXES_MEJOBS) */

#define AUDIO_LIST_MAX 256
#define MIX_FRAMES_MAX 512

struct me_slot
{
	unsigned int request;  /* the main CPU: the number of the job to run */
	unsigned int done;     /* the ME: the number of the last job finished */
	unsigned int type;     /* ME_JOB_... */
	unsigned int cycles;   /* the ME: its cycle counter across the last job */
	unsigned int job_cycles; /* the ME: of those, the job's own code (without
	                         the two cache hand-overs around it) */
	unsigned int args[ME_JOB_ARGS];
	unsigned int results[ME_JOB_RESULTS];
} __attribute__((aligned(64)));

struct me_control
{
	unsigned int alive;    /* the ME: idle polls, counted */
	unsigned int stop;     /* the main CPU 1: stop; the ME 2: stopped */
	unsigned int clock;    /* the ME: its cycle counter at the last poll */
	unsigned int jobs;     /* the ME: jobs run */
	unsigned int cause_before, cause_after; /* build 88: its Cause register at start */
	/* the main CPU, set once after start: ME_FEATURE_HALT ("mehalt": the ME
	halts when it has nothing to do and the main CPU wakes it with an
	interrupt, instead of polling the slots), ME_FEATURE_RING (the job ring).
	One word, so the idle loop's look at it costs what mehalt's alone did */
	unsigned int features;
	unsigned int halted;    /* the ME: 1 from just before a halt until it wakes */
	unsigned int halts;     /* the ME: halts, counted */
	unsigned int wakes;     /* the ME: wake interrupts taken (meLibOnExternalInterrupt) */
} __attribute__((aligned(64)));
#define ME_FEATURE_HALT 1u
#define ME_FEATURE_RING 2u
/* audio first: a waiting audio block is mixed before each ring job and
between the steps of a long one ("meaudiofirst"; off until a rig A/B shows
what it costs the ring jobs it is mixed inside) */
#define ME_FEATURE_AUDIO_FIRST 4u
/* the audio job's cache hand-over by ranges (psp_dsound_me_lines), off
unless "meaudioranges": on the rig it didn't shorten the ME's mix, and c40's
cinematic ticks were longer with it among the words on */
#define ME_FEATURE_AUDIO_RANGES 8u
/* the debug stash kept, when "mestash" asks for it (a hang to diagnose) */
#define ME_FEATURE_STASH 16u
/* "meyieldoff": a slot job doesn't serve the ring between its steps, and
the ring refuses pushes while one is out, as before batches */
#define ME_FEATURE_NO_YIELD 32u
/* "meedramstate": the ME's private state (below) kept in its eDRAM. Off
until a PSP run shows that stretch of the eDRAM is ours: edramcheck watches
0-192 KB and patterns from 256 KB, not between */
#define ME_FEATURE_EDRAM_STATE 64u
/* "meclaimold": a batch job claims its record before it looks at the limit,
as before the stale-claim fix (batch_job says why that order holds the main
CPU) */
#define ME_FEATURE_CLAIM_OLD 128u
/* "mebcc1": the ME's 0xBCC00000 unit's word 0x40 set to 1, as m-cid
(mcidclan) found fastest for code on the ME (psp-media-engine-reload's
README), where me-core's minimal VME set-up leaves it at 2. A test until a
PSP-1000 A/B shows whether the ME's jobs run faster */
#define ME_FEATURE_BCC 256u
/* "mefaultoff": the ME's exception vector left as me-core leaves it, not
pointed at psp_me_fault_vector */
#define ME_FEATURE_FAULT_OFF 512u
/* "mefaulttest": a test of the record, never for play: about 5 s after
start the ME runs a break instruction (an exception on any MIPS: an odd
address's load was let through on the ME, in a run on the PSP), so the
main CPU's next deadline stops it and the log shows the record */
#define ME_FEATURE_FAULT_TEST 1024u

/* The debug stash: where the ME is, in words of its own (two cache lines,
only ever used through the uncached view), so a main CPU that gives up on a
job can say what the ME was doing (me_stash_dump). The ME writes it, the
main CPU only reads it */
#define STASH_AT 0        /* STASH_IDLE, ... */
#define STASH_TICKET 1    /* the ring job in hand */
#define STASH_TYPE 2      /* its type (or the slot job's) */
#define STASH_STEP 3      /* how far into a long job (psp_me_ring_step) */
#define STASH_CLOCK 4     /* the ME's cycle counter at the last note */
#define STASH_RING_BEGUN 5
#define STASH_RING_ENDED 6
#define STASH_AUDIO_INSIDE 7 /* audio blocks mixed before or inside a ring job */
#define STASH_SLOT_BEGUN 8
#define STASH_SLOT_ENDED 9
#define STASH_PUSHED 10   /* the ring's pushed count as the ME last read it */
#define STASH_AUDIO_RANGES 11 /* audio blocks the ME handed over by ranges */
#define STASH_WORDS 32
#define STASH_IDLE 1
#define STASH_SLOT_DROP 2 /* a slot job's cache work before it */
#define STASH_SLOT_JOB 3
#define STASH_SLOT_WRITE 4 /* and after it */
#define STASH_RING_DROP 5
#define STASH_RING_JOB 6
#define STASH_RING_WRITE 7
#define STASH_HALTED 8
#define STASH_STOPPED 9
static unsigned int stash_storage[STASH_WORDS] __attribute__((aligned(64)));

/* whole cache lines of their own; only ever used through the uncached view */
static struct me_control control_storage __attribute__((aligned(64)));

/* B31: what the ME's exception vector saw, written by psp_me_fault_vector
on the ME and read by the main CPU when it stops an ME that didn't answer:
a fault in a job (a bad or unaligned address, a firmware call, a trap) stops
the ME with nothing to say where, since me-core installs no vector unless
mehalt asks. Uncached on both sides; a line of its own */
struct me_fault
{
	unsigned int count, cause, epc, bad_address, sp, ra, status, pad[9];
} __attribute__((aligned(64)));
struct me_fault psp_me_fault_storage __attribute__((aligned(64)));
static struct me_slot slot_storage[ME_SLOTS] __attribute__((aligned(64)));
static void *audio_list_storage[AUDIO_LIST_MAX] __attribute__((aligned(64)));
#define UNCACHED(p) ((volatile __typeof__(*(p)) *)(0x40000000u | (unsigned int)(p)))
#define CONTROL UNCACHED(&control_storage)
#define SLOT(k) UNCACHED(&slot_storage[k])
#define AUDIO_LIST ((void *volatile *)(0x40000000u | (unsigned int)audio_list_storage))
#define STASH ((volatile unsigned int *)(0x40000000u | (unsigned int)stash_storage))

/* the ME's note in the stash; the features word is read each time (one
uncached read) so the main CPU's choice holds without a copy on the ME */
static inline void me_note(unsigned int at)
{
	unsigned int count;

	if (!(CONTROL->features & ME_FEATURE_STASH))
		return;
	__asm__ __volatile__("mfc0 %0, $9" : "=r"(count));
	STASH[STASH_AT] = at;
	STASH[STASH_CLOCK] = count;
}

static inline void me_count(unsigned int which)
{
	if (CONTROL->features & ME_FEATURE_STASH)
		STASH[which]++;
}

/* the ME code's audio hand-over by ranges (psp_dsound.c): 1 done, 0 over the
size where the whole cache is cheaper (the caller does the whole cache) */
int psp_dsound_me_lines(void *const *list, unsigned long count, unsigned long frames, int after);

/* ---------- the job ring (ME phase 1; psp_me.h has its contract)

A slot holds one job, and its caller sleeps or spins until the ME is done.
The ring holds up to ME_RING_ENTRIES jobs, so the main CPU can hand one over,
get on with its own share of the work, and collect the answer when it needs
it. The single-writer queue (research_brief_for_master.md 1.4-1.5):
- ring_pushed is written by the main CPU only and ring_popped by the ME
  only, each on a 64-byte line of its own, touched only through the uncached
  view; both only grow (unsigned wrap is fine), and popped passing a job
  means the job is done, its results and output in main RAM.
- A request's lines are written by the main CPU (cached, then written back
  before pushed moves); a result's lines and the job's output by the ME
  (cached, written back before popped moves). Each CPU's cache writes back
  whole lines and doesn't see the other's, so no line has two writers.
- The ME drops its copies of a request's lines and of the job's inputs
  before reading them; the main CPU drops its copies of a result's lines and
  of the output before reading them. */
#define RING_MASK (ME_RING_ENTRIES - 1)
/* past this a range of cache operations costs more than doing the whole
16 KB data cache: one instruction a line, whether the line is held or not */
#define RING_WHOLE_CACHE_BYTES (16 * 1024)
/* a job may use this much of its CPU's stack; the word past it is the
sentinel checked at the job's end */
#define RING_JOB_STACK_BYTES 4096
/* a ray walks the level's BSP and each object's recursively: its frames and
results take more (actor_perception.c, psp_perception_me_rays) */
#define RING_RAYS_STACK_BYTES (24 * 1024)
#define RING_SENTINEL 0x5E471E1Du
#define RING_OK 0
#define RING_BAD_TYPE 1
#define RING_STACK 2

struct me_ring_counter
{
	unsigned int value;
	unsigned int pad[15];
} __attribute__((aligned(64)));

struct me_ring_request
{
	unsigned int type, ticket;
	unsigned int input[ME_RING_INPUTS], input_bytes[ME_RING_INPUTS];
	unsigned int output, output_bytes;
	unsigned int args[ME_RING_ARGS];
	unsigned int pad[32 - 4 - 2 * ME_RING_INPUTS - ME_RING_ARGS];
} __attribute__((aligned(64)));

struct me_ring_result
{
	unsigned int ticket;      /* the job these are for */
	unsigned int status;      /* RING_OK, ... */
	unsigned int job_cycles;  /* the ME's cycle counter across the job's own code */
	unsigned int all_cycles;  /* and across the whole job, its cache work included */
	unsigned int look_cycles; /* from the ME's last look that found the ring empty to the job's start: how
	                          long the job can have waited to be picked up (under mehalt the time halted
	                          before the wake counts too, so only the polling ME's figure is tight) */
	unsigned int results[ME_RING_RESULTS];
} __attribute__((aligned(64)));

static struct me_ring_counter ring_pushed_storage, ring_popped_storage;
static struct me_ring_request ring_requests[ME_RING_ENTRIES];
static struct me_ring_result ring_results[ME_RING_ENTRIES];
#define RING_PUSHED (UNCACHED(&ring_pushed_storage)->value)
#define RING_POPPED (UNCACHED(&ring_popped_storage)->value)

/* hit operations on the lines holding [start, start + bytes): 0x1A writes a
line back, 0x1B writes it back and drops it. On either CPU, in user mode (as
list_take's and ge_writeback's in psp_ge.c). A range seen through the
uncached view never went through a cache: nothing to do */
static inline void lines_writeback(unsigned int start, unsigned int bytes)
{
	unsigned int line, end = start + bytes;

	if (start & 0x40000000u)
		return;
	for (line = start & ~63u; line < end; line += 64)
		__builtin_allegrex_cache(0x1A, line);
	__asm__ __volatile__("sync");
}

static inline void lines_drop(unsigned int start, unsigned int bytes)
{
	unsigned int line, end = start + bytes;

	if (start & 0x40000000u)
		return;
	for (line = start & ~63u; line < end; line += 64)
		__builtin_allegrex_cache(0x1B, line);
	__asm__ __volatile__("sync");
}

/* 0 off, 1 the ME runs the jobs, 2 simulated (the emulator: the main CPU
runs them), -1 stopped (after HOME, quit, sleep or a failure) */
static volatile int me_state = 0;
/* counted by the kernel's sleep and wake handlers: a restart that sees it
move slept again partway, and the ME may be in reset under it */
static volatile unsigned int me_sleeps = 0;
/* The reset register holds every block's reset, not only the ME's: written
whole, a hold or a release also releases whatever the firmware's drivers
hold in reset at that moment (mid-sleep, mid-wake). So the ME's two bits are
set and cleared alone, as the firmware's own sysreg driver does.
"meresetwhole": the whole-register writes again */
static int me_reset_whole = 0;

/* The system registers that decide whether the ME can run, as the main CPU
sees them: resets (0xbc10004c), bus clocks (0xbc100050: the ME's is bit 0),
clocks (0xbc100058) and AVC power (0xbc100070); with them, the ME's start
marks (below). Read at launch, in the kernel's sleep and wake handlers,
around a restart's release and at exit, so the log shows what a sleep
changes */
struct me_sysregs
{
	unsigned int reset, bus_clock, clock, avc, marks[2];
};
static struct me_sysregs regs_launch, regs_sleep, regs_wake, regs_exit;

/* kernel mode only, like the registers */
static inline void reset_hold(void)
{
	if (me_reset_whole)
		HW_SYS_RESET_ENABLE = SC_HW_RESET;
	else
	{
		u32 interrupts;

		meLibSuspendCpuIntr(interrupts);
		HW_SYS_RESET_ENABLE |= SC_HW_RESET;
		meLibResumeCpuIntr(interrupts);
	}
	meLibSync();
}

static inline void reset_release(void)
{
	if (me_reset_whole)
		HW_SYS_RESET_ENABLE = 0;
	else
	{
		u32 interrupts;

		meLibSuspendCpuIntr(interrupts);
		HW_SYS_RESET_ENABLE &= ~SC_HW_RESET;
		meLibResumeCpuIntr(interrupts);
	}
	meLibSync();
}

/* Two words of the ME's reset-vector SRAM, written by the ME as its start
goes on, so a start that never answers still says how far it got. Away from
the library's shared words (0xbfc00600) and the firmware's RPC words, which
are idle while the ME runs our code */
#define ME_MARK_START   0xbfc006c0 /* the wake's start stub (psp_me_wake_stub) */
#define ME_MARK_PROCESS 0xbfc006c4 /* meLibOnProcess */
#define ME_MARK_STUB_IN     0x57a90001u
#define ME_MARK_STUB_DONE   0x57a90002u
#define ME_MARK_PROCESS_IN  0x57a90003u
#define ME_MARK_PROCESS_RUN 0x57a90004u

/* kernel mode only: the registers and the SRAM are out of a user thread's
reach */
static inline void sysregs_read(struct me_sysregs *regs)
{
	regs->reset = HW_SYS_RESET_ENABLE;
	regs->bus_clock = HW_SYS_BUS_CLOCK_ENABLE;
	regs->clock = hw(0xbc100058);
	regs->avc = HW_SYS_AVC_POWER;
	regs->marks[0] = hw(ME_MARK_START);
	regs->marks[1] = hw(ME_MARK_PROCESS);
}

/* 1 while a client thread is inside a slot's job (psp_me_run, or from
psp_me_submit to the end of psp_me_finish). Each slot has one client thread,
the flag's only writer; it is raised before that thread looks at me_state,
so the wake's restart, which finds every flag down while me_state is still
-1, knows no thread can be inside a slot's words until it sets me_state */
static volatile int slot_busy[ME_SLOTS];
static unsigned int slot_request[ME_SLOTS];
static unsigned int last_job_us[ME_SLOTS]; /* build 87: psp_me_last_job_us */
/* the ME's cycle counter, counts a microsecond (measured at start) */
static float me_cycles_per_us = 0.0f;

/* ---------- the Media Engine's side: no system calls, no library calls, no
VFPU; everything it touches is listed above */

static inline unsigned int me_cycles(void)
{
	unsigned int count;

	__asm__ __volatile__("mfc0 %0, $9" : "=r"(count));
	return count;
}

static inline void me_idle(void)
{
	unsigned int k;

	/* ~25 us at 333 MHz in registers and the instruction cache */
	for (k = 0; k < 2000; k++)
		__asm__ __volatile__("nop");
}

/* me-core's exception handler calls this on the ME for each
external interrupt (the main CPU's wake, me_wake below). The handler saves
only k0, k1, ra and at, so this may use only k0 and k1: no C. It counts the
wake, so the log can tell "wakes arrive but the ME stays halted" (halt
resuming at itself) from "wakes are lost" (me-core's samples/exceptions
counts interrupts the same way) */
__attribute__((noinline, aligned(4)))
void meLibOnExternalInterrupt(void)
{
	__asm__ __volatile__(
		".set push\n"
		".set noreorder\n"
		"la      $k0, %0\n"
		"li      $k1, 0x40000000\n" /* the uncached view, as UNCACHED() */
		"or      $k0, $k0, $k1\n"
		"lw      $k1, 0($k0)\n"
		"addiu   $k1, $k1, 1\n"
		"sw      $k1, 0($k0)\n"
		"sync\n"
		".set pop\n"
		:
		: "i"(&control_storage.wakes)
		: "k0", "k1", "memory");
}

/* B31: the ME's exception vector, unless "mefaultoff": the exception's
Cause, EPC, BadVAddr, $sp, $ra and Status, and a count, into
psp_me_fault_storage through the uncached view, then a spin. Only k0 and k1
are used, so the record holds the faulting code's own $sp and $ra. A fault
is fatal either way: the main CPU's 40 ms deadline stops the ME and reports
the record. Interrupts stay off on the ME without mehalt, so nothing else
comes here; mehalt installs me-core's vector instead */
void psp_me_fault_vector(void);
__asm__(
	".text\n"
	".align 4\n"
	".globl psp_me_fault_vector\n"
	".ent psp_me_fault_vector\n"
	"psp_me_fault_vector:\n"
	".set push\n"
	".set noreorder\n"
	".set noat\n"
	"	la $k0, psp_me_fault_storage\n"
	"	li $k1, 0x40000000\n"
	"	or $k0, $k0, $k1\n"
	"	mfc0 $k1, $13\n"
	"	sw $k1, 4($k0)\n"
	"	mfc0 $k1, $14\n"
	"	sw $k1, 8($k0)\n"
	"	mfc0 $k1, $8\n"
	"	sw $k1, 12($k0)\n"
	"	sw $sp, 16($k0)\n"
	"	sw $ra, 20($k0)\n"
	"	mfc0 $k1, $12\n"
	"	sw $k1, 24($k0)\n"
	"	lw $k1, 0($k0)\n"
	"	addiu $k1, $k1, 1\n"
	"	sw $k1, 0($k0)\n"
	"	sync\n"
	"1:	b 1b\n"
	"	nop\n"
	".set pop\n"
	".end psp_me_fault_vector\n");

static void me_fault_vector_install(void)
{
	unsigned int vector = 0x80000000u | (unsigned int)psp_me_fault_vector;

	__asm__ __volatile__("cache 0x8, 0(%0)\n\tsync\n\tmtc0 %0, $25\n\tsync" : : "r"(vector) : "memory");
}

/* "mehalt"'s idle. halted is raised before the slots are looked
at once more, so a job asked for during this pass is served, not slept
through. One window is left: a wake that lands between that look and the
halt is taken by the handler before the halt. The main CPU covers it by
waking again while it waits (me_wake), so it costs one look, never a job */
static void me_halt_idle(volatile struct me_control *control, int ring_ready, unsigned int ring_popped)
{
	int k, waiting = 0;

	control->halted = 1;
	__asm__ __volatile__("sync");
	for (k = 0; k < ME_SLOTS; k++)
		if (SLOT(k)->request != SLOT(k)->done)
			waiting = 1;
	if (ring_ready && RING_PUSHED != ring_popped)
		waiting = 1;
	if (!waiting && !control->stop)
	{
		control->halts++;
		me_note(STASH_HALTED);
		meLibHalt();
	}
	control->halted = 0;
	__asm__ __volatile__("sync");
}

/* one job (either CPU; on_me: this is the ME) */
static void run_job(volatile struct me_slot *slot, int *sums, int on_me)
{
	switch (slot->type)
	{
	case ME_JOB_AUDIO:
	{
		unsigned long clipped = 0, voices;
		/* on the ME, where its cycles went (results 2-5); the main CPU
		can't read the ME's counter, so it leaves them 0 */
		unsigned int split[4] = { 0, 0, 0, 0 }, k;

		/* args: count, frames, output */
		voices = psp_dsound_mix_split((void *const *)AUDIO_LIST, slot->args[0], sums, (short *)slot->args[2],
			slot->args[1], &clipped, on_me ? split : NULL);
		slot->results[0] = (unsigned int)voices;
		slot->results[1] = (unsigned int)clipped;
		for (k = 0; k < 4; k++)
			slot->results[2 + k] = split[k];
		break;
	}
	case ME_JOB_CLASSIFY:
	case ME_JOB_ANGLE:
	case ME_JOB_GE:
	{
		unsigned int args[ME_JOB_ARGS], results[ME_JOB_RESULTS] = { 0 }, k;

		for (k = 0; k < ME_JOB_ARGS; k++)
			args[k] = slot->args[k];
		/* build 113: the replay's fourth argument says whether it runs on
		the ME, where it may mix audio between records (psp_me_serve_audio);
		gemesim marks the main CPU's own work as the ME's, so the context
		word can't tell */
		if (slot->type == ME_JOB_GE)
			args[3] = (unsigned int)on_me;
		if (slot->type == ME_JOB_CLASSIFY)
			psp_ge_me_classify(args, results);
		else if (slot->type == ME_JOB_ANGLE)
			psp_ge_me_angle(args, results);
		else
			psp_ge_me_replay(args, results);
		for (k = 0; k < ME_JOB_RESULTS; k++)
			slot->results[k] = results[k];
		break;
	}
	case ME_JOB_EDRAM:
	{
		/* build 115: "edramcheck". args: 0 fill / 1 check, the first byte
		(eDRAM offset), the bytes. The ME's own view of its eDRAM, cached
		(me_serve drops the cache before the job and writes it back after).
		Each word holds its own address mixed with a constant, so a word
		moved or left from elsewhere shows too. Only on the ME: the main CPU
		can't see eDRAM (simulated, nothing is touched) */
		unsigned int first = slot->args[1], bytes = slot->args[2], at, bad = 0, first_bad = 0xFFFFFFFFu, found = 0;
		unsigned int hash = 0x811C9DC5u, sp;

		if (!on_me)
		{
			slot->results[0] = 0xFFFFFFFFu;
			break;
		}
		/* mode 2 hashes a stretch it must not write (the bottom of eDRAM,
		nothing of ours); a fill never comes within 16 KB of this job's own
		stack, so the stretch below the stack can be patterned while the ME
		runs on it, and a later check finds how far down the stack reached */
		__asm__ ("move %0, $sp" : "=r"(sp));
		sp = (sp & 0x1FFFFFFFu) - 16 * 1024;
		for (at = first; at < first + bytes; at += 4)
		{
			volatile unsigned int *word = (volatile unsigned int *)(0x80000000u + at);
			unsigned int want = at ^ 0x5A5AA5A5u;

			if (slot->args[0] == 2)
				hash = (hash ^ *word) * 16777619u;
			else if (slot->args[0] == 0)
			{
				if (at < sp)
					*word = want;
			}
			else if (*word != want)
			{
				if (!bad++)
				{
					first_bad = at;
					found = *word;
				}
			}
		}
		if (slot->args[0] == 2)
			bad = hash;
		slot->results[3] = sp + 16 * 1024;
		slot->results[0] = bad;
		slot->results[1] = first_bad;
		slot->results[2] = found;
		break;
	}
	case ME_JOB_SOUNDCOPY:
	{
		/* "soundedram": up to four pieces (source, destination, bytes in
		whole 16s), staged in main RAM by the stick reader, copied into the
		sound cache. me_serve drops the ME's cache before the job (so the
		staging is read fresh) and writes it back after (into the eDRAM). Off
		the ME only when simulated, where the cache is in main RAM; an eDRAM
		destination there is refused */
		unsigned int k, copied = 0;

		for (k = 0; k + 2 < ME_JOB_ARGS && slot->args[k + 2]; k += 3)
		{
			const unsigned int *from = (const unsigned int *)slot->args[k];
			unsigned int *to = (unsigned int *)slot->args[k + 1], words = slot->args[k + 2] >> 2, w;

			if (!on_me && (slot->args[k + 1] & 0x1FFFFFFFu) < 0x08000000u)
			{
				copied = 0xFFFFFFFFu;
				break;
			}
			for (w = 0; w < words; w += 4)
			{
				unsigned int a = from[w], b = from[w + 1], c = from[w + 2], d = from[w + 3];

				to[w] = a;
				to[w + 1] = b;
				to[w + 2] = c;
				to[w + 3] = d;
			}
			copied += words << 2;
		}
		slot->results[0] = copied;
		break;
	}
	default:
		break;
	}
}

/* build 113: a slot's job run on the ME, with the cache hand-overs around it
(meLibOnProcess's loop, and psp_me_serve_audio below) */
static void me_serve(volatile struct me_slot *slot, int *sums)
{
	volatile struct me_control *control = CONTROL;
	unsigned int request = slot->request, start, job_start, job_end, type = slot->type;
	int ranges;

	start = me_cycles();
	me_count(STASH_SLOT_BEGUN);
	me_note(STASH_SLOT_DROP);
	/* what the main CPU wrote: drop any copy left from the last job. The
	audio job by ranges, so a ring job it is mixed between or inside keeps
	what it has in the cache: the voices it reads (psp_dsound_me_lines says
	why that is all of them), or the whole cache past 16 KB */
	ranges = type == ME_JOB_AUDIO && (control->features & ME_FEATURE_AUDIO_RANGES) &&
		psp_dsound_me_lines((void *const *)AUDIO_LIST, slot->args[0], slot->args[1], 0);
	if (!ranges)
		meLibDcacheWritebackInvalidateAll();
	else
		me_count(STASH_AUDIO_RANGES);
	job_start = me_cycles();
	me_note(STASH_SLOT_JOB);
	run_job(slot, sums, 1);
	job_end = me_cycles();
	me_note(STASH_SLOT_WRITE);
	/* what this job wrote, to main RAM, before saying so: the voices' lines
	it changed and the output block, or the whole cache */
	if (ranges)
	{
		psp_dsound_me_lines((void *const *)AUDIO_LIST, slot->args[0], slot->args[1], 1);
		lines_drop(slot->args[2], slot->args[1] * 2 * sizeof(short));
	}
	else
		meLibDcacheWritebackInvalidateAll();
	slot->cycles = me_cycles() - start;
	slot->job_cycles = job_end - job_start;
	control->jobs++;
	me_count(STASH_SLOT_ENDED);
	me_note(STASH_IDLE);
	__asm__ __volatile__("sync");
	slot->done = request;
	__asm__ __volatile__("sync");
}

/* The ME's own state: with "meedramstate" in its eDRAM (psp_me.h's map), so
what only the ME reads and writes stays off the shared bus; otherwise in main
RAM on lines of its own, which the main CPU never touches. Only ever touched
on the ME */
struct me_private
{
	/* the ring as the ME's loop last left it, for a slot job that serves the
	ring between its steps (psp_me_yield); ring_ready 0 outside such a job */
	unsigned int ring_ready, ring_popped, ring_last_look;
	/* 1 inside a ring job: a yield there mixes audio but runs no ring job
	(the ring's jobs run in order, and this one isn't popped yet) */
	unsigned int in_ring;
	unsigned int pad[12];
};
static struct me_private me_private_main __attribute__((aligned(64)));
/* the sums of audio blocks mixed inside other jobs (4 KB in the eDRAM) */
static int me_audio_sums_main[MIX_FRAMES_MAX * 2] __attribute__((aligned(64)));
#define ME_EDRAM_STATE_ON (CONTROL->features & ME_FEATURE_EDRAM_STATE)
#define ME_PRIVATE (ME_EDRAM_STATE_ON ? (volatile struct me_private *)(ME_EDRAM + ME_EDRAM_PRIVATE) : &me_private_main)
#define ME_AUDIO_SUMS (ME_EDRAM_STATE_ON ? (int *)(ME_EDRAM + ME_EDRAM_PRIVATE + 0x1000) : me_audio_sums_main)

/* the audio slot's job, if one is waiting: 1 when one was mixed. Only on
the ME (a long job's step, psp_me_yield) */
static int me_audio_now(void)
{
	volatile struct me_slot *slot = SLOT(ME_SLOT_AUDIO);

	if (slot->request == slot->done)
		return 0;
	me_serve(slot, ME_AUDIO_SUMS);
	return 1;
}

static unsigned int me_ring_drain(unsigned int popped, unsigned int *last_look);

/* called by a long job on the ME (the GE layer's replay, psp_ge.c's
defer_replay) every few records: an audio block asked for meanwhile is mixed
now, between two records, instead of waiting behind the whole render job.
b112's Test 3 on the PSP: audio blocks waited up to 38 ms behind render jobs,
and 7 of 109 were late [M] (b113_candidates.md, step 4b). Only ever called on
the ME (the replay's on-the-ME argument) */
void psp_me_serve_audio(void)
{
	psp_me_yield();
}

void psp_me_yield(void)
{
	volatile struct me_private *p;

	if (!psp_on_me())
		return;
	me_audio_now();
	p = ME_PRIVATE;
	if (p->ring_ready && !p->in_ring && RING_PUSHED != p->ring_popped)
	{
		unsigned int last_look = p->ring_last_look;

		p->ring_popped = me_ring_drain(p->ring_popped, &last_look);
		p->ring_last_look = last_look;
	}
}

/* a ring job's step (psp_me.h): the slant job every 64 triangles (~1 us
each, b90), the rays' job every ray (~27 us: a job of 16 is ~430 us [M]), so
an audio block waits that long at most for the ME instead of a whole job, or
eight of them queued (2026-10-14: audio comes before everything else on
the ME). Mixing here is exact: the audio job reads and writes only the
voices, its output and its own sums, none of which a ring job touches */
void psp_me_ring_step(unsigned int step)
{
	unsigned int features;

	if (!psp_on_me())
		return;
	features = CONTROL->features;
	if (features & ME_FEATURE_STASH)
		STASH[STASH_STEP] = step;
	if ((features & ME_FEATURE_AUDIO_FIRST) && me_audio_now())
	{
		me_count(STASH_AUDIO_INSIDE);
		me_note(STASH_RING_JOB);
	}
}

#ifdef PSP_RECON
/* "mebusN" (keep.txt; PSP_RECON builds; docs/recon_134.md): how much the two
CPUs slow each other when both wait on main RAM at once, the question behind
running the display list's building on the ME beside the next tick. This
job loads main RAM from the ME until the main CPU raises the stop word: a
line at a scattered address, then N steps of arithmetic (mebus0: misses
only; ~mebus50: about the main CPU's own mix, a miss every ~120 cycles), then
a store into its 16 KB output, so its lines are written back as the GE
layer's list would be. It reads memory no job writes into (user RAM, read
only) and writes only its own output, so the game never sees it.
"mebusedram" (with mebusN): the same load on the ME's own eDRAM, reads and
stores both, so the line says what work that never leaves the ME costs the
main CPU, the question behind putting ME jobs' data there. Its reads span
1 MB of the stretch edramcheck patterns (reads only), its stores a 16 KB
scratch below that stretch: not to be run with edramcheck's watch of it */
static struct me_ring_counter bus_stop_storage;
#define BUS_STOP (UNCACHED(&bus_stop_storage)->value)
#define BUS_OUT_WORDS 4096
static unsigned int bus_out[BUS_OUT_WORDS] __attribute__((aligned(64)));
#define BUS_BASE 0x08900000u
#define BUS_LINES (0x01000000u / 64)
#define BUS_EDRAM_BASE (ME_EDRAM + ME_EDRAM_RESIDENT)
#define BUS_EDRAM_LINES (0x00100000u / 64)
#define BUS_EDRAM_OUT (ME_EDRAM + ME_EDRAM_SCRATCH)

static void bus_load_job(const unsigned int *args, unsigned int *results)
{
	unsigned int work = args[0], limit = args[1], x = args[2] | 1u, sink = 0, n = 0, k, start = me_cycles();
	/* args[3]: 1 for the eDRAM; the main CPU can't reach it, so a simulated
	run (the emulator) keeps to main RAM */
	int edram = args[3] && psp_on_me();
	unsigned int base = edram ? BUS_EDRAM_BASE : BUS_BASE, lines = edram ? BUS_EDRAM_LINES : BUS_LINES;
	unsigned int *out = edram ? (unsigned int *)BUS_EDRAM_OUT : bus_out;

	/* the stop word is an uncached main-RAM read: polled every load, it put a
	bus trip into every eDRAM load and hid the eDRAM's rate behind it, so it
	is read every 16th */
	while (((n & 15u) || !BUS_STOP) && me_cycles() - start < limit)
	{
		x = x * 1664525u + 1013904223u;
		sink += *(volatile const unsigned int *)(base + ((x >> 8) % lines) * 64u);
		for (k = 0; k < work; k++)
			sink = sink * 31u + k;
		out[(n * 16u) % BUS_OUT_WORDS] = sink;
		/* an audio block waiting is mixed now, whether or not "meaudiofirst"
		is on: the load holds the ME for a whole tick, and an audio block
		left 100 ms stops the ME for the session (the first
		runs) */
		if ((++n & 63u) == 0)
		{
			psp_me_ring_step(n);
			me_audio_now();
		}
	}
	results[0] = n;
	results[1] = me_cycles() - start;
	results[2] = sink;
}
#endif

/* "meringtest"'s job (psp_me_ring_test): each output word from its input
word, and the input's sum, so a stale input line on the ME or a stale output
line on the main CPU shows as a wrong word */
#define RINGTEST_WORDS 64
static void ring_test_job(const unsigned int *args, unsigned int *results)
{
	const unsigned int *in = (const unsigned int *)args[0];
	unsigned int *out = (unsigned int *)args[1], i, sum = 0;

	for (i = 0; i < RINGTEST_WORDS; i++)
	{
		sum += in[i];
		out[i] = (in[i] ^ 0x5A5A5A5Au) + i;
	}
	results[0] = sum;
	results[1] = args[2];
}

/* ---------- batches (psp_me.h): the ME's side

One ring job makes records [first, first + count) of a batch, in order. Before
each it claims the record (started), then reads the main CPU's limit: a
record at or past it has been taken back, so the claim is withdrawn and the
job ends. The main CPU writes the limit before it reads started, and the ME
writes started before it reads the limit, all uncached with a sync between,
so for any record either the ME sees the limit or the main CPU sees the claim
(psp_me_batch_need waits out the second case for that one record). A record
is written back before done passes it. args: 0 the batch, 1 first, 2 count,
3 the main CPU's FPU control word, 4 make, 5 the records, 6 a record's bytes,
7-14 make's own.

done is one word for the whole batch, so psp_me_batch_need trusts every
handed-over record below it. That holds because a job ends early only at the
limit, which only falls: every job after it withdraws at its first record, and
done never passes a record the ME didn't make */
static int batch_sim_half = 0; /* "mebatchsimhalf": the main CPU's, read by the simulated ME */
/* the batch whose job the simulated ME stopped partway: an ME that is behind,
which a real one is while the main CPU takes records back. The jobs after
it wait behind it, as they would in the ring, so they make nothing; jobs that
made records after the stop would carry done past the ones never made */
static const struct psp_me_batch *batch_sim_behind = NULL;
/* whole jobs before the stop, 0-3 by turns, so the ME's records, the ones
taken back and the ones never handed over fall in different places each pass */
static unsigned int batch_sim_passes = 0, batch_sim_whole = 0;
/* "meclaimold", for the simulated ME (the real one reads ME_FEATURE_CLAIM_OLD) */
static int me_claim_old = 0;

static void batch_job(const unsigned int *args, unsigned int *results, int on_me)
{
	struct psp_me_batch *b = (struct psp_me_batch *)args[0];
	volatile unsigned int *limit = &UNCACHED(&b->limit)->value, *started = &UNCACHED(&b->started)->value;
	volatile unsigned int *done = &UNCACHED(&b->done)->value, *withdrawn = &UNCACHED(&b->withdrawn)->value;
	psp_me_record_fn make = (psp_me_record_fn)args[4];
	unsigned char *records = (unsigned char *)args[5];
	unsigned int r = args[1], end = args[1] + args[2], bytes = args[6], made = 0, control = 0, user[ME_BATCH_ARGS], k;
	int claim_old = on_me ? (CONTROL->features & ME_FEATURE_CLAIM_OLD) != 0 : me_claim_old;

	for (k = 0; k < ME_BATCH_ARGS; k++)
		user[k] = args[7 + k];
	/* the emulator runs a pushed job inside the push: making only half of
	it there leaves the rest for psp_me_batch_need to take back, so that path
	is proven where play can be compared */
	if (!on_me && batch_sim_half)
	{
		if (batch_sim_behind == b)
			end = r;
		else if (batch_sim_whole)
			batch_sim_whole--;
		else if (!batch_sim_behind && args[2] > 1)
		{
			end = args[1] + args[2] / 2;
			batch_sim_behind = b;
		}
	}
	if (on_me)
	{
		__asm__ __volatile__("cfc1 %0, $31" : "=r"(control));
		__asm__ __volatile__("ctc1 %0, $31" : : "r"(args[3]));
	}
	for (; r < end; r++)
	{
		unsigned char *record = records + r * bytes;

		/* A record at the limit is declined before it is claimed. started
		is one word for every job of the batch: a later job that claimed its
		first record and then withdrew would leave started at that record,
		past one an earlier job had withdrawn at, and the main CPU would wait
		for that one as if the ME still held it (until its deadline) */
		if (!claim_old && r >= *limit)
		{
			*withdrawn = *withdrawn + 1;
			__asm__ __volatile__("sync");
			break;
		}
		*started = r + 1;
		__asm__ __volatile__("sync");
		if (r >= *limit)
		{
			*started = r;
			*withdrawn = *withdrawn + 1;
			__asm__ __volatile__("sync");
			break;
		}
		make(record, user);
		if (on_me)
			lines_writeback((unsigned int)record, bytes);
		__asm__ __volatile__("sync");
		*done = r + 1;
		__asm__ __volatile__("sync");
		made++;
		psp_me_ring_step(r);
	}
	if (on_me)
		__asm__ __volatile__("ctc1 %0, $31" : : "r"(control));
	results[0] = made;
}

/* the word a job's stack allowance below the caller's stack pointer */
static inline volatile unsigned int *ring_sentinel(unsigned int allowance)
{
	unsigned int sp;

	__asm__ __volatile__("move %0, $sp" : "=r"(sp));
	return (volatile unsigned int *)((sp - allowance) & ~3u);
}

/* one ring job, on the ME (on_me), or on the main CPU when simulated, where
there is no ME cache to look after and no ME cycle counter to read.
look_cycles: how long it can have waited to be picked up */
static __attribute__((noinline)) void ring_run(unsigned int index, int on_me, unsigned int look_cycles)
{
	struct me_ring_request *request = &ring_requests[index];
	struct me_ring_result *result = &ring_results[index];
	unsigned int args[ME_RING_ARGS], results[ME_RING_RESULTS], start = 0, job_start = 0, job_end = 0, inputs = 0;
	unsigned int status = RING_OK, k;
	volatile unsigned int *sentinel;

	if (on_me)
	{
		start = me_cycles();
		me_count(STASH_RING_BEGUN);
		me_note(STASH_RING_DROP);
		/* the request as the main CPU wrote it back, not a copy from the last
		time this entry was used */
		lines_drop((unsigned int)request, sizeof(*request));
		if (CONTROL->features & ME_FEATURE_STASH)
		{
			STASH[STASH_TICKET] = request->ticket;
			STASH[STASH_TYPE] = request->type;
			STASH[STASH_STEP] = 0;
		}
		/* a continuing job's inputs: fresh since its hand-over's first job
		dropped them, and unchanged since (ME_RING_ARG_CONTINUES) */
		if (!request->args[ME_RING_ARG_CONTINUES])
		{
			for (k = 0; k < ME_RING_INPUTS; k++)
				inputs += request->input_bytes[k];
			if (inputs > RING_WHOLE_CACHE_BYTES)
				meLibDcacheWritebackInvalidateAll();
			else
				for (k = 0; k < ME_RING_INPUTS; k++)
					if (request->input_bytes[k])
						lines_drop(request->input[k], request->input_bytes[k]);
		}
		me_note(STASH_RING_JOB);
	}
	for (k = 0; k < ME_RING_ARGS; k++)
		args[k] = request->args[k];
	for (k = 0; k < ME_RING_RESULTS; k++)
		results[k] = 0;
	sentinel = ring_sentinel(request->type == ME_JOB_PERC_RAYS || request->type == ME_JOB_POINTS ?
		RING_RAYS_STACK_BYTES : RING_JOB_STACK_BYTES);
	*sentinel = RING_SENTINEL;
	if (on_me)
	{
		ME_PRIVATE->in_ring = 1;
		job_start = me_cycles();
	}
	switch (request->type)
	{
	case ME_JOB_SLANT:
		psp_ge_me_slant(args, results);
		break;
	case ME_JOB_RINGTEST:
		ring_test_job(args, results);
		break;
	case ME_JOB_PERC_RAYS:
		psp_perception_me_rays(args, results);
		break;
	case ME_JOB_POSES:
		psp_pose_me_make(args, results);
		break;
	case ME_JOB_BATCH:
		batch_job(args, results, on_me);
		break;
	case ME_JOB_POINTS:
		psp_points_me_make(args, results);
		break;
#ifdef PSP_RECON
	case ME_JOB_BUSLOAD:
		bus_load_job(args, results);
		break;
#endif
	default:
		status = RING_BAD_TYPE;
		break;
	}
	if (on_me)
	{
		job_end = me_cycles();
		ME_PRIVATE->in_ring = 0;
	}
	/* a job that ran past its stack allowance may have written over whatever
	lies below it: reported, and the ring stops */
	if (*sentinel != RING_SENTINEL)
		status = RING_STACK;
	if (on_me)
		me_note(STASH_RING_WRITE);
	if (on_me && request->output_bytes)
	{
		if (request->output_bytes > RING_WHOLE_CACHE_BYTES)
			meLibDcacheWritebackInvalidateAll();
		else
			lines_writeback(request->output, request->output_bytes);
	}
	result->ticket = request->ticket;
	result->status = status;
	result->job_cycles = job_end - job_start;
	result->look_cycles = look_cycles;
	for (k = 0; k < ME_RING_RESULTS; k++)
		result->results[k] = results[k];
	result->all_cycles = on_me ? me_cycles() - start : 0;
	if (on_me)
	{
		lines_writeback((unsigned int)result, sizeof(*result));
		me_count(STASH_RING_ENDED);
		me_note(STASH_IDLE);
	}
	__asm__ __volatile__("sync");
}

/* the ME: every job pushed since its last look, in order. last_look: the
cycle count when the ring was last seen with nothing waiting past popped.
Audio first: an audio block asked for meanwhile is mixed before each job,
so it waits for one job at most, not for all eight the ring holds */
static unsigned int me_ring_drain(unsigned int popped, unsigned int *last_look)
{
	unsigned int looked = me_cycles(), pushed = RING_PUSHED;
	int audio_first = (CONTROL->features & ME_FEATURE_AUDIO_FIRST) != 0;

	if (CONTROL->features & ME_FEATURE_STASH)
		STASH[STASH_PUSHED] = pushed;
	while (popped != pushed)
	{
		if (audio_first && me_audio_now())
			me_count(STASH_AUDIO_INSIDE);
		ring_run(popped & RING_MASK, 1, me_cycles() - *last_look);
		popped++;
		RING_POPPED = popped;
		__asm__ __volatile__("sync");
	}
	/* anything pushed after the read above came after this moment */
	*last_look = looked;
	return popped;
}

/* after a ring job, the ME looks at the ring every ~1.5 us instead of once
an idle pass (~36 us on the PSP, b86): the renderer pushes its jobs in a
burst (a level's draws), and each microsecond a job waits to be picked up is
one the main CPU may wait at the end. A look is one uncached read; a linger
pass is as long as an idle pass, and the slots are still looked at every
pass. RING_LINGER_PASSES passes (~2 ms) after the last job, the idle is as
before */
#define RING_LINGER_PASSES 64
#define RING_LINGER_LOOKS 16
#define RING_LINGER_NOPS 125
static void me_ring_linger(unsigned int popped, unsigned int *last_look)
{
	int look, k;

	for (look = 0; look < RING_LINGER_LOOKS; look++)
	{
		unsigned int looked = me_cycles();

		if (RING_PUSHED != popped)
			return;
		*last_look = looked;
		for (k = 0; k < RING_LINGER_NOPS; k++)
			__asm__ __volatile__("nop");
	}
}

__attribute__((noinline, aligned(4)))
void meLibOnProcess(void)
{
	volatile struct me_control *control = CONTROL;
	int sums[MIX_FRAMES_MAX * 2];
	int halt_ready = 0; /* mehalt: the exception handler is set up */
	int ring_ready = 0; /* the main CPU has turned the ring on */
	unsigned int ring_popped = 0, ring_last_look = 0, ring_quiet = RING_LINGER_PASSES;
	int features_seen = 0; /* the features word has been written (mebcc1 applied) */
	unsigned int fault_test_at = 0; /* mefaulttest: when, by the ME's cycle counter */

	hw(ME_MARK_PROCESS) = ME_MARK_PROCESS_IN;
	meLibSync();
	/* build 88: the cycle counter (coprocessor 0 register 9) stood still on
	a PSP-1000 (b87: "runs at 0.0 a microsecond"), so every ME time read
	0. MIPS32r2's Cause bit 27 (DC, "disable count") stops it; the firmware
	may leave it set on the ME. Cleared here; Cause before and after go to
	the log either way */
	{
		unsigned int cause;

		__asm__ __volatile__("mfc0 %0, $13" : "=r"(cause));
		control->cause_before = cause;
		cause &= ~(1u << 27);
		__asm__ __volatile__("mtc0 %0, $13\n\tsync" : : "r"(cause));
		/* build 89: and the counter written, as me-core's own timer set-up
		does before relying on it (mtc0 $0, $9) */
		__asm__ __volatile__("mtc0 $0, $9\n\tsync");
		__asm__ __volatile__("mfc0 %0, $13" : "=r"(cause));
		control->cause_after = cause;
	}
	meLibDcacheWritebackInvalidateAll();
	/* the features aren't set yet: the main-RAM copy; the eDRAM copy is
	written before each job that reads it */
	me_private_main.ring_ready = 0;
	me_private_main.in_ring = 0;
	hw(ME_MARK_PROCESS) = ME_MARK_PROCESS_RUN;
	meLibSync();
	for (;;)
	{
		int k;

		/* the features word is looked at every 256th pass (it is set once,
		after start), so the polling idle costs what it would without it */
		if ((control->alive & 255) == 0 && (!halt_ready || !ring_ready || !features_seen))
		{
			unsigned int features = control->features;

			if (features && !features_seen)
			{
				features_seen = 1;
				if (!(features & (ME_FEATURE_FAULT_OFF | ME_FEATURE_HALT)))
					me_fault_vector_install();
				if (features & ME_FEATURE_FAULT_TEST)
				{
					fault_test_at = me_cycles() + 333u * 1000000u * 5u;
					UNCACHED(&psp_me_fault_storage)->pad[0] = 1; /* armed */
				}
				if (features & ME_FEATURE_BCC)
				{
					hw(0xBCC00040) = 1;
					meLibSync();
				}
			}

			if ((features & ME_FEATURE_HALT) && !halt_ready)
			{
				meLibExceptionHandlerInit(0);
				halt_ready = 1;
			}
			if ((features & ME_FEATURE_RING) && !ring_ready)
			{
				ring_ready = 1;
				/* 0 at launch; after a wake the main CPU has moved both
				counters past every ticket handed out before the sleep */
				ring_popped = RING_POPPED;
				ring_last_look = me_cycles();
			}
		}
		control->alive++;
		control->clock = me_cycles();
		if (fault_test_at && (int)(control->clock - fault_test_at) > 0)
		{
			UNCACHED(&psp_me_fault_storage)->pad[0] = 2; /* about to break */
			__asm__ __volatile__("break");
		}
		/* build 89: the stop word is looked at after the slots (below), so a
		job asked for just before a stop is still served: b86-b88 halted with
		it pending and its caller waited 40 ms holding the mixer's lock.
		The ring after the audio slot: a ring job is small and the main CPU
		may be waiting for it; audio is due on time */
		for (k = 0; k < ME_SLOTS; k++)
		{
			volatile struct me_slot *slot = SLOT(k);

			if (slot->request != slot->done)
			{
				volatile struct me_private *p = ME_PRIVATE;

				if (k != ME_SLOT_AUDIO)
				{
					p->ring_ready = ring_ready && !(control->features & ME_FEATURE_NO_YIELD);
					p->ring_popped = ring_popped;
					p->ring_last_look = ring_last_look;
					p->in_ring = 0;
				}
				me_serve(slot, sums);
				if (k != ME_SLOT_AUDIO)
				{
					if (p->ring_popped != ring_popped)
						ring_quiet = 0;
					ring_popped = p->ring_popped;
					ring_last_look = p->ring_last_look;
					p->ring_ready = 0;
				}
			}
			if (k == ME_SLOT_AUDIO && ring_ready)
			{
				unsigned int before = ring_popped;

				ring_popped = me_ring_drain(ring_popped, &ring_last_look);
				if (ring_popped != before)
					ring_quiet = 0;
			}
		}
		if (control->stop)
			break;
		if (ring_ready && ring_quiet < RING_LINGER_PASSES)
		{
			ring_quiet++;
			me_ring_linger(ring_popped, &ring_last_look);
		}
		else if (halt_ready)
			me_halt_idle(control, ring_ready, ring_popped);
		else
			me_idle();
	}
	me_note(STASH_STOPPED);
	control->stop = 2;
	__asm__ __volatile__("sync");
	meLibHalt();
}

/* the library's sleep handler (its replacement for the firmware's own ME one)
calls these in kernel mode: the ME held in reset through the sleep and after
it, and the main CPU runs the jobs until the wake's restart (psp_me_wake_poll)
has the ME answering again. Nothing more here: kernel mode is no place to
wait for the ME, and the game's threads may be anywhere in a job */
void meLibOnSleep(void)
{
	sysregs_read(&regs_sleep);
	reset_hold();
	me_state = -1;
	me_sleeps++;
}

void meLibOnWake(void)
{
	sysregs_read(&regs_wake);
	reset_hold();
	me_state = -1;
	me_sleeps++;
}

/* ---------- the main CPU's side */

/* the log's figures, per slot, since the last report */
static struct
{
	unsigned long jobs, failures, simulated;
	unsigned long long me_cycles, me_job_cycles, wait_us, handover_us;
	unsigned long me_worst_us, wait_worst_us;
	/* me_run_slot's looks after the first, and the first's planned time */
	unsigned long looks, first_look_us;
} stats[ME_SLOTS];
/* the last job's looks after the first, per slot (psp_me_audio's adaptive
first look reads the audio slot's) */
static unsigned int last_looks[ME_SLOTS];

/* The audio job's first look ("mepoll", design_mepoll.md). The audio thread
sleeps while the ME mixes, looking at the slot after a first sleep and then
every 100 us; each look wakes it above the game's thread (30-60 us of the
game's CPU on the PSP-1000). The job is ~0.7 ms of the ME's time, but it
waits 0.9-2.4 ms behind the ME's other work, so a first look at 200 us is
followed by ~10 more [M]. By default (200) it is as before.
"mepoll" (audiolook0, the default under it): the first look at the last
block's wait less AUDIO_LOOK_MARGIN_US, so a block takes one to three looks.
When the first look already found the job done, the wait isn't known (it may
have ended long before), so the next first look comes AUDIO_LOOK_MARGIN_US
earlier; the measured wait is used only when there were later looks, so
the delay's own overshoot can't ratchet the look later block after block.
At most AUDIO_LOOK_MAX_US: a look later than the job's end holds the mixer's
lock (the game thread may wait for it) and delays the block's output by as
much, against a 23 ms block; the late-blocks count says if it ever costs.
"mepoll audiolookN" (N > 0): a fixed first look at N us */
#define AUDIO_LOOK_DEFAULT_US 200
#define AUDIO_LOOK_MARGIN_US 300
#define AUDIO_LOOK_MAX_US 2000
static int audio_look_adaptive = 0;
static unsigned int audio_first_look_us = AUDIO_LOOK_DEFAULT_US;

static void audio_look_start(void)
{
	int n;

	if (!psp_keep("mepoll"))
		return;
	n = psp_keep_number("audiolook", 0);
	audio_look_adaptive = n <= 0;
	audio_first_look_us = n <= 0 ? AUDIO_LOOK_DEFAULT_US : (unsigned int)(n > AUDIO_LOOK_MAX_US ? AUDIO_LOOK_MAX_US : n);
	platform_log("ME: mepoll: the audio job's first look %s", audio_look_adaptive ? "adaptive" : "fixed");
}

static void audio_look_next(unsigned long waited_us)
{
	unsigned long next;

	if (!audio_look_adaptive)
		return;
	if (!last_looks[ME_SLOT_AUDIO])
		next = audio_first_look_us > AUDIO_LOOK_DEFAULT_US + AUDIO_LOOK_MARGIN_US ?
			audio_first_look_us - AUDIO_LOOK_MARGIN_US : AUDIO_LOOK_DEFAULT_US;
	else
		next = waited_us > AUDIO_LOOK_DEFAULT_US + AUDIO_LOOK_MARGIN_US ? waited_us - AUDIO_LOOK_MARGIN_US : AUDIO_LOOK_DEFAULT_US;
	audio_first_look_us = (unsigned int)(next > AUDIO_LOOK_MAX_US ? AUDIO_LOOK_MAX_US : next);
}
static unsigned long long report_mark = 0;
static float busy_percent_last = 0.0f;

/* "mehalt" on this side. A wake is a kernel call (me-core's
meLibEmitSoftwareInterrupt), so it is sent only to a halted ME, and again
on each look while a job waits (me_halt_idle says why). The first 256 are
timed, for the kernel call's cost */
static int me_halting = 0;
static int ring_on = 0; /* the job ring, decided at start */
/* a push writes back and drops the job's output lines itself, so no line of
the output can be dirty here when the ME writes it ("mepushdrop"; otherwise
the caller's care, as before) */
static int ring_push_drop = 0;
/* perception's rays: one cache hand-over for a tick's jobs, not one a job
("meraysbatch"; otherwise one a job, as before) */
static int rays_batch = 0;
static int me_yield_off = 0;
/* "megraceoff": the ring's and the batches' deadlines as they were, the
first wait past one stopping the ME (see BATCH_RECORD_DEADLINE_US) */
static int me_grace_off = 0;
/* a late ring job's rest (RING_DEADLINE_US says what it is): the first
RING_REST_US; again within RING_AGAIN_US of coming back, twice the last, up
to RING_REST_MAX_US */
#define RING_REST_US 1000000ull
#define RING_REST_MAX_US 16000000ull
#define RING_AGAIN_US 5000000ull
static int ring_give_up = 0;              /* "ringgiveup" */
static unsigned int ring_lates = 0, ring_revivals = 0;
static unsigned long long ring_late_mark = 0, ring_revive_mark = 0, ring_rest_base_us = RING_REST_US, ring_rest_us = RING_REST_US;
#ifdef PSP_RECON
/* "ringlateN" (recon): ring jobs whose ticket is a multiple of N (N > 0)
are taken as late by the wait, so the rest and the revival can be watched
where no job is ever late (the emulator runs a job inside its push).
Simulated, the ME is also behind from that job until the revival check
lets it catch up, as a real late ME is. "ringhang" (with ringlateN): it
never catches up, as a hung ME, and the ring must stay off */
static unsigned int ring_late_every = 0;
static int ring_hang_test = 0, ring_sim_behind = 0;
#endif
/* lines said about waits past a deadline, at most DEADLINE_LINES */
static unsigned int deadline_lines = 0;
#define DEADLINE_LINES 8

/* The slant split is on by default: the rig showed it exact (0 of 414,000
triangles differing, 0 failed jobs) and a little faster (b30 and c20: the
sort 0.6-1.4 ms a frame less). "meslantoff" turns it off; "nomeaudio" keeps
the ME stopped unless "meslant" asks for the ring */
int psp_me_rays_wanted(void);

int psp_me_slant_wanted(void)
{
	if (psp_keep("meslantoff"))
		return 0;
	return psp_keep("meslant") || !psp_keep("nomeaudio");
}

/* Perception's rays on the ME, on by default the same way: the rig showed
the real ME's answers exact (0 of 31,940 checked mismatched, 0 failed jobs)
and a50's tick 1.0-1.8 ms shorter. "meraysoff" turns it off; "nomeaudio"
keeps the ME stopped unless "meraysperc" asks for the ring */
int psp_me_rays_wanted(void)
{
	if (psp_keep("meraysoff"))
		return 0;
	return psp_keep("meraysperc") || !psp_keep("nomeaudio");
}
static unsigned long wakes_sent, wakes_timed;
static unsigned long long wake_us;

static void me_wake(void)
{
	if (!me_halting || !CONTROL->halted)
		return;
	if (wakes_timed < 256)
	{
		unsigned long long start = sceKernelGetSystemTimeWide();

		meLibEmitSoftwareInterrupt();
		wake_us += sceKernelGetSystemTimeWide() - start;
		wakes_timed++;
	}
	else
		meLibEmitSoftwareInterrupt();
	wakes_sent++;
}

static int me_hold_reset(void)
{
	reset_hold();
	return 0;
}

static int me_sysregs_kernel(void *regs)
{
	sysregs_read((struct me_sysregs *)regs);
	return 0;
}

/* the launch's read, with the ME running: the system registers alone, not
the ME's two words in its own RAM, which it may be running from or writing.
The one hang seen at start-up (the PSP-3000) came after the line before this
read and before its own; reading the ME's RAM under its feet is the one part
of the read no other moment does. "memarksatlaunch" reads them as before */
static int me_sysregs_running_kernel(void *regs)
{
	struct me_sysregs *r = (struct me_sysregs *)regs;

	r->reset = HW_SYS_RESET_ENABLE;
	r->bus_clock = HW_SYS_BUS_CLOCK_ENABLE;
	r->clock = hw(0xbc100058);
	r->avc = HW_SYS_AVC_POWER;
	r->marks[0] = r->marks[1] = 0;
	return 0;
}

static int emulator(void)
{
	int result = 0;

	/* PPSSPP answers its own "kemulator:" device (3: is this an emulator) */
	return sceIoDevctl("kemulator:", 3, NULL, 0, &result, sizeof(result)) == 0 && result;
}

/* build 105: for the rest of the platform layer (the bus probe) */
int psp_is_emulator(void)
{
	return emulator();
}

/* 1 (the ME's) or 2 (simulated) once started at launch; 0 after a stop for
any other reason than a sleep (a job that timed out, HOME), since an ME that
failed once is not trusted again by a wake */
static int me_restartable = 0;
/* 1 from a release of the ME's reset (launch, a restart) until it is seen
halted or is held in reset again: while set, the ME may still be running
our code, and the exit holds it before the firmware takes the PSP back */
static int me_maybe_running = 0;
/* the library's kernel helper is loaded: kernel calls can be made */
static int me_kernel_ready = 0;

volatile unsigned int psp_me_stops = 0;

int psp_me_simulated(void)
{
	return me_state == 2;
}

/* psp_ge.c's mechunk: the frame's job reads its records as the main CPU
publishes them. On the ME, before each piece, what it has cached written back
and dropped (the line the last piece ended in held words written since);
whether a stop is asked, so a job waiting for more ends; and, on the main
CPU, the job's watchdog started again at each piece: a frame recorded slowly
(a hitch, a load) is no ME late */
void psp_me_cache_refresh(void)
{
	if (psp_on_me())
		meLibDcacheWritebackInvalidateAll();
}

int psp_me_stop_asked(void)
{
	return CONTROL->stop != 0;
}

static void me_stop(const char *why, int for_sleep)
{
	volatile struct me_control *control = CONTROL;
	int k;

	if (me_state > 0)
		psp_me_stops++;
	if (me_state > 0 && !for_sleep)
		me_restartable = 0;
	if (me_state == 1)
	{
		/* build 89: no new jobs from here; one already asked for is served
		(the ME runs its slots before it looks at the stop word) */
		me_state = -1;
		control->stop = 1;
		me_wake();
		for (k = 0; k < 100 && control->stop != 2; k++)
		{
			sceKernelDelayThread(1000);
			me_wake();
		}
		me_maybe_running = 0;
		if (control->stop == 2)
			platform_log("ME: stopped (%s); halted itself after %d ms; the main CPU runs its jobs from here", why, k);
		else
		{
			volatile struct me_fault *fault = UNCACHED(&psp_me_fault_storage);

			kcall_2(me_hold_reset, 0);
			platform_log("ME: stopped (%s): NO answer in 100 ms, held in reset; the main CPU runs its jobs from here", why);
			/* B31: where it faulted (EPC is in kseg0: less 0x88804000, the
			link map's address), or that it didn't */
			if (fault->count)
				platform_log("ME: fault: %u exceptions; Cause %08x (code %u), EPC %08x, BadVAddr %08x, sp %08x, ra %08x, Status %08x",
					fault->count, fault->cause, (fault->cause >> 2) & 31, fault->epc, fault->bad_address, fault->sp,
					fault->ra, fault->status);
			else
				platform_log("ME: fault: none recorded (%s): the ME was running, not faulted",
					psp_keep("mefaultoff") ? "the vector off, mefaultoff" : "the vector in");
			if (fault->pad[0])
				platform_log("ME: fault: mefaulttest %s", fault->pad[0] == 2 ? "reached its break" : "armed, not reached");
		}
	}
	else if (me_state == 2)
		platform_log("ME (simulated): stopped (%s)", why);
	if (me_state > 0)
		me_state = -1;
}

void psp_me_stop(const char *why)
{
	me_stop(why, 0);
}

/* the power callback's, as the PSP goes to sleep: the wake may start it again */
void psp_me_sleep_stop(void)
{
	me_stop("sleep", 1);
}

static void stop_at_exit(void)
{
	psp_me_stop("exit");
}

static void sysregs_line(char *line, int size, const char *name, const struct me_sysregs *regs)
{
	snprintf(line, size, "%s %08x %08x %08x %08x (marks %08x %08x)", name, regs->reset, regs->bus_clock, regs->clock,
		regs->avc, regs->marks[0], regs->marks[1]);
}

/* HOME, or the game's own exit: the firmware reboots into the XMB and boots
its own ME image there, and that boot waits for the ME without a time limit.
So the ME is left as the firmware leaves it before a boot: halted, or held
in reset, never running our code. Nothing here waits on the ME for longer
than the stop's 100 ms. The registers go to the log, against launch's, for
a hang that comes after this */
void psp_me_exit(const char *why)
{
	char line[96];

	me_stop(why, 0);
	if (!me_kernel_ready)
		return;
	if (me_maybe_running)
	{
		kcall_2(me_hold_reset, 0);
		me_maybe_running = 0;
		platform_log("ME: held in reset for the exit (a restart may have had it running)");
	}
	kcall_3(me_sysregs_kernel, 0, &regs_exit);
	sysregs_line(line, sizeof(line), "exit", &regs_exit);
	platform_log("ME: system registers (reset, bus clocks, clocks, AVC power) at %s; %s %08x %08x %08x %08x", line,
		"launch", regs_launch.reset, regs_launch.bus_clock, regs_launch.clock, regs_launch.avc);
}

/* launch (build 87: on unless the launch menu's "nomeaudio"): the ME started,
its first signs of life checked, its clock measured. Non-zero when jobs can
be handed over */
int psp_me_start(void)
{
	volatile struct me_control *control = CONTROL;
	unsigned int before, clock_before;
	unsigned long long mark;
	int table, k;

	/* nomeaudio keeps the ME off unless a ring client asks for it; the
	mixer then stays on the main CPU (psp_dsound.c) */
	if (me_state || (psp_keep("nomeaudio") && !psp_keep("meslant") && !psp_keep("meringtest") && !psp_keep("meraysperc") &&
		psp_keep("nomepoints")))
		return me_state > 0;
	/* no cached copy of them anywhere on this CPU; then the words that
	matter (they are in zeroed memory already) */
	sceKernelDcacheWritebackInvalidateRange(&control_storage, sizeof(control_storage));
	sceKernelDcacheWritebackInvalidateRange(&psp_me_fault_storage, sizeof(psp_me_fault_storage));
	memset((void *)UNCACHED(&psp_me_fault_storage), 0, sizeof(psp_me_fault_storage));
	sceKernelDcacheWritebackInvalidateRange(slot_storage, sizeof(slot_storage));
	sceKernelDcacheWritebackInvalidateRange(audio_list_storage, sizeof(audio_list_storage));
	control->alive = control->stop = control->clock = control->jobs = 0;
	for (k = 0; k < ME_SLOTS; k++)
		SLOT(k)->request = SLOT(k)->done = 0;
	sceKernelDcacheWritebackInvalidateRange(&ring_pushed_storage, sizeof(ring_pushed_storage));
	sceKernelDcacheWritebackInvalidateRange(&ring_popped_storage, sizeof(ring_popped_storage));
	sceKernelDcacheWritebackInvalidateRange(ring_requests, sizeof(ring_requests));
	sceKernelDcacheWritebackInvalidateRange(ring_results, sizeof(ring_results));
	RING_PUSHED = RING_POPPED = 0;
	sceKernelDcacheWritebackInvalidateRange(stash_storage, sizeof(stash_storage));
	for (k = 0; k < STASH_WORDS; k++)
		STASH[k] = 0;
	ring_on = psp_me_slant_wanted() || psp_me_rays_wanted() || psp_keep("meringtest") || psp_keep("mebatchtest") ||
		!psp_keep("nomepoints");
#ifdef PSP_RECON
	ring_on = ring_on || psp_keep_number("mebus", -1) >= 0;
#endif
	ring_push_drop = psp_keep("mepushdrop");
	rays_batch = psp_keep("meraysbatch");
	me_yield_off = psp_keep("meyieldoff");
	me_grace_off = psp_keep("megraceoff");
	me_claim_old = psp_keep("meclaimold");
	ring_give_up = psp_keep("ringgiveup");
	{
		int rest = psp_keep_number("ringrest", -1);

		if (rest >= 0)
			ring_rest_base_us = ring_rest_us = (unsigned long long)rest * 1000ull;
	}
#ifdef PSP_RECON
	{
		int n = psp_keep_number("ringlate", 0);

		ring_late_every = n > 0 ? (unsigned int)n : 0u;
		ring_hang_test = ring_late_every && psp_keep("ringhang");
		if (ring_late_every)
			platform_log("ME: ringlate: every ring job whose ticket is a multiple of %u taken as late%s",
				ring_late_every, ring_hang_test ? "; ringhang: the ME never catches up" : "");
	}
#endif
	audio_look_start();
	report_mark = sceKernelGetSystemTimeWide();
	if (emulator())
	{
		me_state = 2;
		me_restartable = 2;
		platform_log("ME: the emulator runs no ME code; each job is SIMULATED (the main CPU runs it)%s",
			ring_on ? "; the job ring on (meslant)" : "");
		return 1;
	}
	{
		/* the library writes its kernel helper, kcall.prx, to the current
		folder and loads it from there: ms0:/halo, whatever the game's own
		current folder is */
		char folder[256];
		int have = getcwd(folder, sizeof(folder)) != NULL;

		me_reset_whole = psp_keep("meresetwhole");
		platform_log("ME: starting the Media Engine (a hang here: hold the power switch up 10 s; launch with nomeaudio)");
		psp_log_flush();
		chdir(PSP_DATA_ROOT);
		table = meLibDefaultInit();
		if (have)
			chdir(folder);
	}
	/* the library's kernel helper is in from here; the ME's reset was
	released whether or not the start below sees it running */
	me_kernel_ready = table >= 0 || table < -4;
	me_maybe_running = table >= 0;
	if (table < 0)
	{
		me_state = -1;
		platform_log("ME: the library couldn't start the ME (%d: -3/-4 kcall.prx not written or loaded, -5 no ME event handler, -6 unknown firmware image)", table);
		return 0;
	}
	sceKernelDelayThread(2000);
	before = control->alive;
	clock_before = control->clock;
	mark = sceKernelGetSystemTimeWide();
	sceKernelDelayThread(20000);
	if (control->alive == before)
	{
		me_state = -1;
		kcall_2(me_hold_reset, 0);
		me_maybe_running = 0;
		platform_log("ME: the ME didn't start (alive %u -> %u in 20 ms; table %d); held in reset", before, control->alive, table);
		return 0;
	}
	{
		unsigned int cycles = control->clock - clock_before;
		unsigned long long us = sceKernelGetSystemTimeWide() - mark;

		me_cycles_per_us = us ? (float)cycles / (float)us : 0.0f;
	}
	me_state = 1;
	me_restartable = 1;
	atexit(stop_at_exit);
	/* mehalt only now, so the start check above saw the ME polling */
	me_halting = psp_keep("mehalt");
	control->features = (me_halting ? ME_FEATURE_HALT : 0u) | (ring_on ? ME_FEATURE_RING : 0u) |
		(psp_keep("meaudiofirst") ? ME_FEATURE_AUDIO_FIRST : 0u) |
		(psp_keep("meaudioranges") ? ME_FEATURE_AUDIO_RANGES : 0u) | (psp_keep("mestash") ? ME_FEATURE_STASH : 0u) |
		(me_yield_off ? ME_FEATURE_NO_YIELD : 0u) | (psp_keep("meedramstate") ? ME_FEATURE_EDRAM_STATE : 0u) |
		(me_claim_old ? ME_FEATURE_CLAIM_OLD : 0u) | (psp_keep("mebcc1") ? ME_FEATURE_BCC : 0u) |
		(psp_keep("mefaultoff") ? ME_FEATURE_FAULT_OFF : 0u) | (psp_keep("mefaulttest") ? ME_FEATURE_FAULT_TEST : 0u);
	platform_log("ME: the ME runs (firmware table %d; %u idle polls in 20 ms; its cycle counter runs at %.1f a microsecond%s; Cause %08x, after clearing DC %08x)",
		table, control->alive - before, me_cycles_per_us, me_cycles_per_us < 1.0f ? ": ME times will read 0" : "",
		control->cause_before, control->cause_after);
	kcall_3(psp_keep("memarksatlaunch") ? me_sysregs_kernel : me_sysregs_running_kernel, 0, &regs_launch);
	{
		char line[96];

		sysregs_line(line, sizeof(line), "launch", &regs_launch);
		platform_log("ME: system registers (reset, bus clocks, clocks, AVC power) at %s%s", line,
			me_reset_whole ? "; meresetwhole: the reset register written whole" : "");
	}
	if (me_halting)
		platform_log("ME: mehalt: the ME halts when idle and is woken by interrupt");
	if (ring_on)
		platform_log("ME: the job ring on (meslant): %d entries", ME_RING_ENTRIES);
	psp_log_flush();
	return 1;
}

int psp_meaudio_start(void)
{
	return psp_me_start();
}

int psp_me_running(void)
{
	return me_state > 0;
}

int psp_meaudio_running(void)
{
	return me_state > 0;
}

static void me_stash_dump(const char *why);

/* the simulated jobs' mixing sums (the emulator; one job at a time there) */
static int simulated_sums[MIX_FRAMES_MAX * 2] __attribute__((aligned(64)));

/* a job in a slot, run to the end: on the ME (the caller sleeps meanwhile),
or on this CPU when simulated. args as the job type takes them; results
filled. Returns 1 when done, 0 if the ME failed (it is stopped then; the
caller runs the job its own way). first_look_us: how long the job is
expected to take, the first sleep before looking (then every 0.1 ms) */
static int me_run_slot(int which, unsigned int type, const unsigned int *args, int arg_count, unsigned int *results,
	unsigned int first_look_us)
{
	volatile struct me_slot *slot;
	unsigned long long start;
	unsigned long waited, me_us;
	unsigned int looks = 0;
	int k;

	if (me_state <= 0 || arg_count > ME_JOB_ARGS)
		return 0;
	slot = SLOT(which);
	last_looks[which] = 0;
	start = sceKernelGetSystemTimeWide();
	slot->type = type;
	for (k = 0; k < arg_count; k++)
		slot->args[k] = args[k];
	if (me_state == 2)
	{
		/* the emulator: the job's own code, on this CPU */
		run_job(slot, simulated_sums, 0);
		me_us = (unsigned long)(sceKernelGetSystemTimeWide() - start);
		stats[which].simulated++;
		waited = me_us;
		last_job_us[which] = (unsigned int)me_us;
	}
	else
	{
		__asm__ __volatile__("sync");
		slot->request = ++slot_request[which];
		__asm__ __volatile__("sync");
		me_wake();
		sceKernelDelayThread(first_look_us ? first_look_us : 100);
		while (slot->done != slot_request[which])
		{
			me_wake();
			/* build 89: the ME halted (a stop) before it saw this job */
			if (CONTROL->stop == 2 && slot->done != slot_request[which])
			{
				stats[which].failures++;
				return 0;
			}
			/* build 113: a render job gets 500 ms. The 40 ms limit was set
			for audio jobs (about 1 ms each); b112's render jobs ran up to
			39.6 ms on the PSP, and the one past 40 ms stopped the ME [M] */
			/* build 115: the eDRAM probe too, which may wait behind a render job */
			if (sceKernelGetSystemTimeWide() - start > (type == ME_JOB_GE || type == ME_JOB_EDRAM ? 500000 : 40000))
			{
				platform_log("ME: a job (type %u) not finished in %u ms (job %u; the ME at job %u, %u idle polls)", type,
					type == ME_JOB_GE || type == ME_JOB_EDRAM ? 500 : 40, slot_request[which], slot->done, CONTROL->alive);
				me_stash_dump("a slot job late");
				stats[which].failures++;
				psp_me_stop("a job timed out");
				return 0;
			}
			sceKernelDelayThread(100);
			looks++;
		}
		waited = (unsigned long)(sceKernelGetSystemTimeWide() - start);
		last_looks[which] = looks;
		stats[which].looks += looks;
		stats[which].first_look_us += first_look_us ? first_look_us : 100;
		stats[which].me_cycles += slot->cycles;
		stats[which].me_job_cycles += slot->job_cycles;
		last_job_us[which] = me_cycles_per_us > 0.0f ? (unsigned int)((float)slot->job_cycles / me_cycles_per_us) : 0;
		me_us = me_cycles_per_us > 0.0f ? (unsigned long)((float)slot->cycles / me_cycles_per_us) : 0;
	}
	for (k = 0; k < ME_JOB_RESULTS; k++)
		results[k] = slot->results[k];
	stats[which].jobs++;
	stats[which].wait_us += waited;
	if (waited > stats[which].wait_worst_us)
		stats[which].wait_worst_us = waited;
	if (me_us > stats[which].me_worst_us)
		stats[which].me_worst_us = me_us;
	return 1;
}

int psp_me_run(int which, unsigned int type, const unsigned int *args, int arg_count, unsigned int *results,
	unsigned int first_look_us)
{
	int done;

	if (which < 0 || which >= ME_SLOTS)
		return 0;
	slot_busy[which] = 1;
	done = me_run_slot(which, type, args, arg_count, results, first_look_us);
	slot_busy[which] = 0;
	return done;
}

/* build 118: a job handed over without waiting (L1 step 4, psp_ge.c's
"gemeoverlap"): the main CPU goes on with its own work and finishes the job
later with psp_me_finish. One job a slot at a time. Returns 1 if handed over
(or, simulated, already run here: the emulator runs no ME code), 0 if the ME
can't take it (off, stopped, a job already out): the caller runs it itself.
The caller writes back its data cache first, as for psp_me_run */
static int submitted[ME_SLOTS]; /* 0 none, 1 out with the ME, 2 run here (simulated) */
static unsigned long long submit_mark[ME_SLOTS];
/* the slot job out serves the ring between its steps (the GE layer's
replay yields every 32 records): pushes go on while it runs */
static int slot_yields[ME_SLOTS];

void psp_me_touch(int which)
{
	if (which >= 0 && which < ME_SLOTS && submitted[which] == 1)
		submit_mark[which] = sceKernelGetSystemTimeWide();
}

int psp_me_submit(int which, unsigned int type, const unsigned int *args, int arg_count)
{
	volatile struct me_slot *slot;
	int k;

	if (which < 0 || which >= ME_SLOTS || arg_count > ME_JOB_ARGS || submitted[which])
		return 0;
	/* down again at psp_me_finish's end */
	slot_busy[which] = 1;
	if (me_state <= 0)
	{
		slot_busy[which] = 0;
		return 0;
	}
	slot = SLOT(which);
	slot->type = type;
	for (k = 0; k < arg_count; k++)
		slot->args[k] = args[k];
	slot_yields[which] = type == ME_JOB_GE && !me_yield_off;
	submit_mark[which] = sceKernelGetSystemTimeWide();
	if (me_state == 2)
	{
		run_job(slot, simulated_sums, 0);
		last_job_us[which] = (unsigned int)(sceKernelGetSystemTimeWide() - submit_mark[which]);
		stats[which].simulated++;
		submitted[which] = 2;
		return 1;
	}
	__asm__ __volatile__("sync");
	slot->request = ++slot_request[which];
	__asm__ __volatile__("sync");
	me_wake();
	submitted[which] = 1;
	return 1;
}

/* the job handed over, waited for: 1 when done (results filled), 0 if the
ME failed (it is stopped then, and the job may be half done). The wait spins
on the slot's uncached done word: hand-overs come every few milliseconds,
and a sleep would cost the PSP's scheduler tick (lever X3). After 2 ms it
sleeps in 50 us steps, so a slow job doesn't hold the CPU from the other
threads.
"mefinishslow" (code_review_b119_me.md, finding 1): each look is
an uncached read on the bus the ME is reading its job's inputs from, and
the clock is a system call. By default a look comes every 64 nops (~0.2 us)
with the clock and the stop word every look; spaced, a look every ~3 us and
the clock and stop word every 16th look, so a wake is still a few us late
at most */
#define FINISH_SPIN_NOPS 64        /* between looks, by default */
#define FINISH_SPIN_NOPS_SLOW 1000 /* "mefinishslow": ~3 us at 333 MHz */
#define FINISH_SLOW_CLOCK_EVERY 16 /* "mefinishslow": the clock every 16th look */

static int me_finish_slot(int which, unsigned int *results)
{
	static int slow = -1;
	volatile struct me_slot *slot;
	unsigned long long start, now;
	unsigned long waited, looks = 0;
	int k, state;

	if (slow < 0)
		slow = psp_keep("mefinishslow");
	state = submitted[which];
	submitted[which] = 0;
	slot = SLOT(which);
	start = now = sceKernelGetSystemTimeWide();
	if (state == 1)
	{
		while (slot->done != slot_request[which])
		{
			int spin = slow ? FINISH_SPIN_NOPS_SLOW : FINISH_SPIN_NOPS;

			if (!slow || looks % FINISH_SLOW_CLOCK_EVERY == 0)
			{
				now = sceKernelGetSystemTimeWide();
				/* the ME halted (a stop: HOME, sleep) before it finished this job */
				if (CONTROL->stop == 2 && slot->done != slot_request[which])
				{
					stats[which].failures++;
					return 0;
				}
				if (now - submit_mark[which] > 500000)
				{
					platform_log("ME: a handed-over job (type %u) not finished in 500 ms (job %u; the ME at job %u, %u idle polls)",
						slot->type, slot_request[which], slot->done, CONTROL->alive);
					me_stash_dump("a handed-over job late");
					stats[which].failures++;
					psp_me_stop("a handed-over job timed out");
					return 0;
				}
				/* mehalt: a wake that may have been lost (me_halt_idle) */
				me_wake();
			}
			looks++;
			if (now - start < 2000)
			{
				for (k = 0; k < spin; k++)
					__asm__ __volatile__("nop");
			}
			else
				sceKernelDelayThread(50);
		}
		stats[which].me_cycles += slot->cycles;
		stats[which].me_job_cycles += slot->job_cycles;
		last_job_us[which] = me_cycles_per_us > 0.0f ? (unsigned int)((float)slot->job_cycles / me_cycles_per_us) : 0;
		{
			unsigned long me_us = me_cycles_per_us > 0.0f ? (unsigned long)((float)slot->cycles / me_cycles_per_us) : 0;

			if (me_us > stats[which].me_worst_us)
				stats[which].me_worst_us = me_us;
		}
		waited = (unsigned long)(sceKernelGetSystemTimeWide() - start);
	}
	else
		waited = last_job_us[which]; /* simulated: the job's own time, run at the hand-over */
	for (k = 0; k < ME_JOB_RESULTS; k++)
		results[k] = slot->results[k];
	stats[which].jobs++;
	stats[which].wait_us += waited;
	if (waited > stats[which].wait_worst_us)
		stats[which].wait_worst_us = waited;
	return 1;
}

/* a job handed over with psp_me_submit is done (or was run here), so
psp_me_finish won't wait; 0 while the ME is still on it, or none is out */
int psp_me_submitted_done(int which)
{
	if (which < 0 || which >= ME_SLOTS || !submitted[which])
		return 0;
	return submitted[which] == 2 || SLOT(which)->done == slot_request[which];
}

int psp_me_finish(int which, unsigned int *results)
{
	int done;

	if (which < 0 || which >= ME_SLOTS || !submitted[which])
		return 0;
	done = me_finish_slot(which, results);
	slot_busy[which] = 0;
	return done;
}

/* ---------- the job ring, the main CPU's side */

/* a job that isn't done this long after its push is reported, and the main
CPU does its work instead (psp_me_wait returns 0). The ring then rests: the
ME may still finish the job late and write its output, which nobody reads,
so nothing more is pushed until the ME has popped every job pushed before
(then none of its writes is still to come), and not before the rest's end
(RING_REST_US; "ringrestN": N ms). It comes back at a frame end
(ring_revive), where the ring's one producer is between frames. An ME that
never catches up (hung) keeps it off. One late again within
RING_AGAIN_US of coming back rests twice as long each time, up to
RING_REST_MAX_US, so an ME that is late again and again costs the main CPU
at most one deadline's wait a rest. "ringgiveup": the first late job turns
the ring off for the session, as before. A slant job is ~0.1-2 ms of
work (~1 us a triangle, b90) and may wait behind one audio block (~0.6 ms);
a ray job can wait behind a whole render job's stretch between yields */
#define RING_DEADLINE_US 10000
/* between two looks at popped while waiting: ~1 us, so the look costs the
bus little while the ME reads its inputs (code_review_b119_me.md, 1) */
#define RING_WAIT_NOPS 250
#define RING_WAIT_CLOCK_EVERY 16

extern volatile unsigned int psp_me_window[16];
static unsigned int ring_next = 0;        /* the next ticket */
static unsigned int ring_popped_seen = 0; /* popped, as last read */
static int ring_dead = 0;                 /* a job failed: 1 the ring takes no more, 2 it rests (RING_REST_US) */
/* the first ticket after the wake's restart; tickets before it went to an ME
that was reset, and their entries hold no answer for them */
static unsigned int ring_epoch = 0;
static int ring_restarted = 0;
static unsigned int ring_push_mark[ME_RING_ENTRIES]; /* the push's time, a ticket's */
static unsigned int ring_thread = 0;      /* simulated: the producer's thread, for psp_me_context */
static struct
{
	unsigned long jobs, at_once, failures, refused_busy, refused_full, refused_check;
	unsigned long long job_cycles, all_cycles, look_cycles, push_us, wait_us, round_us;
	unsigned long long simulated_us; /* the emulator: the jobs' own time, run inside the push */
	unsigned long look_worst_cycles, wait_worst_us;
} ring_stats;

int psp_me_ring_ready(void)
{
	return ring_on && !ring_dead && me_state > 0;
}

/* what the main CPU wrote that a job will read, out of its cache to main
RAM: the lines of the range, or past 16 KB the whole cache */
void psp_me_writeback(const void *start, unsigned int bytes)
{
	if (bytes > RING_WHOLE_CACHE_BYTES || bytes == ME_RING_ALL_OF_RAM)
		sceKernelDcacheWritebackAll();
	else if (bytes)
		lines_writeback((unsigned int)start, bytes);
}

/* in main RAM, where the ME can reach it (not VRAM, not the scratchpad);
cached or uncached view alike */
static int ring_in_ram(unsigned int address, unsigned int bytes)
{
	unsigned int at = address & 0x1FFFFFFFu;

	return !bytes || (at >= 0x08000000u && at + bytes <= 0x0C000000u && at + bytes > at);
}

/* the emulator's stand-in for the ME's own stack (in its eDRAM on the PSP):
a job's locals written through pointers (a ray's leaf list, say) would
otherwise land below the producer's stack pointer, on lines the producer
writes again as soon as the push returns, and the write trace would count
them as written by both CPUs. psp_me_sim_call(index, top, fn) runs fn(index)
with its stack pointer just below top (the words it keeps above the
callee's argument area) */
#define RING_SIM_STACK_BYTES (48 * 1024)
void psp_me_sim_call(unsigned int index, void *top, void (*fn)(unsigned int));
__asm__(
	".text\n"
	".globl psp_me_sim_call\n"
	".ent psp_me_sim_call\n"
	"psp_me_sim_call:\n"
	".set push\n"
	".set noreorder\n"
	"	addiu $a1, $a1, -32\n"
	"	sw $ra, 16($a1)\n"
	"	sw $sp, 20($a1)\n"
	"	jalr $a2\n"
	"	move $sp, $a1\n"
	"	lw $ra, 16($sp)\n"
	"	lw $sp, 20($sp)\n"
	"	jr $ra\n"
	"	nop\n"
	".set pop\n"
	".end psp_me_sim_call\n");

static void ring_run_simulated(unsigned int index)
{
	ring_run(index, 0, 0);
}

/* the emulator: the job's code here and now, marked as the ME's for the
ME-strict check and the write trace (emu.sh's gemesim and mewrites), on a
stack of its own as on the ME; the window stays open until the wait. Cold,
as the failure paths below: the PSP's push and wait stay fewer cache lines */
static __attribute__((noinline, cold)) void ring_push_simulated(unsigned int index, unsigned int t)
{
	static unsigned char *sim_stack = NULL;
	static int sim_stack_tried = 0;
	unsigned int job_start;

	if (!ring_thread)
		ring_thread = (unsigned int)sceKernelGetThreadId();
	if (!sim_stack_tried)
	{
		sim_stack_tried = 1;
		sim_stack = (unsigned char *)memalign(64, RING_SIM_STACK_BYTES);
		if (!sim_stack)
			platform_log("ME (simulated): no room for the job stack; jobs run on the producer's");
	}
	psp_me_window[0] = 1;
	job_start = sceKernelGetSystemTimeLow();
	psp_me_context = ring_thread;
	if (sim_stack)
		psp_me_sim_call(index, sim_stack + RING_SIM_STACK_BYTES, ring_run_simulated);
	else
		ring_run(index, 0, 0);
	psp_me_context = 0;
	ring_stats.simulated_us += sceKernelGetSystemTimeLow() - job_start;
#ifdef PSP_RECON
	if (ring_late_every && t && t % ring_late_every == 0)
		ring_sim_behind = 1;
	if (ring_sim_behind)
		return;
#endif
	RING_POPPED = t + 1;
}

/* the debug stash, to the log, when the main CPU gives up on a job: where
the ME was (STASH_AT: 1 between jobs, 2-4 a slot job's cache work, code and
write-back, 5-7 a ring job's, 8 halted, 9 stopped), the ring job and how far
into it, its cycle counter then and now (the same: the ME has stopped
counting; apart: it runs, and was there that long ago) */
static __attribute__((noinline, cold)) void me_stash_dump(const char *why)
{
	volatile unsigned int *stash = STASH;
	unsigned int now = CONTROL->clock;
	float rate = me_cycles_per_us > 0.0f ? me_cycles_per_us : 1.0f;

	if (!(CONTROL->features & ME_FEATURE_STASH))
		return;

	platform_log("ME: stash (%s): at %u; ring job %u (type %u), step %u; ring jobs begun %u, ended %u, pushed as last seen %u, popped %u; "
		"slot jobs begun %u, ended %u; audio blocks mixed before or inside ring jobs %u; the ME's clock at the note %u, at its last idle pass %u (%d us after the note: below 0, it hasn't been back to its idle loop since), idle polls %u",
		why, stash[STASH_AT], stash[STASH_TICKET], stash[STASH_TYPE], stash[STASH_STEP], stash[STASH_RING_BEGUN],
		stash[STASH_RING_ENDED], stash[STASH_PUSHED], RING_POPPED, stash[STASH_SLOT_BEGUN], stash[STASH_SLOT_ENDED],
		stash[STASH_AUDIO_INSIDE], stash[STASH_CLOCK], now, (int)((float)(int)(now - stash[STASH_CLOCK]) / rate),
		CONTROL->alive);
	psp_log_flush();
}

/* a failed wait, reported (why: 0 stopped, 1 too late, 2 a bad answer); the
ring stops unless the ME itself was stopped */
static __attribute__((noinline, cold)) int ring_fail(int why, unsigned int ticket, unsigned int index)
{
	const struct me_ring_request *request = &ring_requests[index];
	const struct me_ring_result *result = &ring_results[index];

	if (why == 1 && ring_dead == 0)
	{
		unsigned long long now = sceKernelGetSystemTimeWide();

		/* the jobs pushed with it are late too: one late stretch, one rest */
		ring_lates++;
		if (ring_revivals && now - ring_revive_mark <= RING_AGAIN_US)
			ring_rest_us = ring_rest_us * 2 > RING_REST_MAX_US ? RING_REST_MAX_US : ring_rest_us * 2;
		else
			ring_rest_us = ring_rest_base_us;
		ring_late_mark = now;
		ring_dead = ring_give_up ? 1 : 2;
		if (ring_give_up)
			platform_log("ME: ring job %u (type %u) not done %u ms after its push (popped %u); the main CPU does it, and the ring is off from here (ringgiveup)",
				ticket, request->type, RING_DEADLINE_US / 1000, RING_POPPED);
		else
			platform_log("ME: ring job %u (type %u) not done %u ms after its push (popped %u); the main CPU does it, and the ring rests at least %lu ms, until the ME has caught up (late stretches %u)",
				ticket, request->type, RING_DEADLINE_US / 1000, RING_POPPED, (unsigned long)(ring_rest_us / 1000), ring_lates);
		if (me_state == 1)
			me_stash_dump("a ring job late");
	}
	else if (why == 2)
	{
		platform_log("ME: ring job %u (type %u) came back %s (ticket %u); the main CPU does it, and the ring is off from here",
			ticket, request->type, result->status == RING_STACK ? "past its stack allowance" :
			result->status == RING_BAD_TYPE ? "as an unknown type" : "with another job's answer", result->ticket);
		ring_dead = 1;
	}
	ring_stats.failures++;
	return 0;
}

/* the frame end's look at a resting ring (RING_DEADLINE_US says why it
rests): back on once the ME runs, has popped every job pushed, and the rest
is over. Pushes come only from the game thread, which is here, so none can
land between the look at popped and the ring's return */
static __attribute__((noinline, cold)) void ring_revive(void)
{
	unsigned long long now = sceKernelGetSystemTimeWide();

	if (now - ring_late_mark < ring_rest_us || me_state <= 0 || CONTROL->stop)
		return;
#ifdef PSP_RECON
	if (ring_hang_test)
		return;
	if (ring_sim_behind)
	{
		ring_sim_behind = 0;
		RING_POPPED = ring_next;
	}
#endif
	if (RING_POPPED != ring_next)
		return;
	ring_popped_seen = ring_next;
	ring_dead = 0;
	ring_revivals++;
	ring_revive_mark = now;
	platform_log("ME: the ring back on, %lu ms after its late job (late stretches %u, back on %u times)",
		(unsigned long)((now - ring_late_mark) / 1000), ring_lates, ring_revivals);
}

/* a job collected later than the ring's next ME_RING_ENTRIES pushes would
allow (next tick's poses, collected after the frame's drawing has pushed
its own jobs): its collector runs before the push that would reuse its
entry, so its answer is never written over */
static unsigned int ring_held_ticket;
static void (*ring_held_collect)(void);

void psp_me_hold(unsigned int ticket, void (*collect)(void))
{
	ring_held_ticket = ticket;
	ring_held_collect = collect;
}

/* a job handed to the ring: 1, with its ticket for psp_me_wait; 0 if the
ring can't take it (off, full, the ME busy with a long slot job, or a
transfer that breaks the rules above): the caller does the work itself */
#ifdef PSP_RECON
static void bus_collect(void);
static int bus_running = 0;
#endif

int psp_me_push(const struct psp_me_job *job, unsigned int *ticket)
{
	struct me_ring_request *request;
	unsigned int start, t, index, k;

	if (!psp_me_ring_ready())
		return 0;
#ifdef PSP_RECON
	/* a real job goes first: "mebus"'s load stops and is collected */
	if (bus_running && job->type != ME_JOB_BUSLOAD)
		bus_collect();
#endif
	if (ring_held_collect && ring_next - ring_held_ticket >= ME_RING_ENTRIES - 1)
	{
		void (*collect)(void) = ring_held_collect;

		ring_held_collect = NULL;
		collect();
	}
	start = sceKernelGetSystemTimeLow();
	/* a render or probe job holds the ME for milliseconds (gemeon's are
	~10 ms on the PSP, review_b120.md 5): a ring job behind it would be
	waited for longer than its work is worth */
	for (k = 0; k < ME_SLOTS; k++)
		if (k != ME_SLOT_AUDIO && submitted[k] && !slot_yields[k])
		{
			ring_stats.refused_busy++;
			return 0;
		}
	if (ring_next - ring_popped_seen >= ME_RING_ENTRIES)
	{
		ring_popped_seen = RING_POPPED;
		if (ring_next - ring_popped_seen >= ME_RING_ENTRIES)
		{
			ring_stats.refused_full++;
			return 0;
		}
	}
	/* the transfer rules: the output is whole lines (a line written back
	carries all 64 bytes), everything in main RAM */
	if (((unsigned int)job->output & 63u) || (job->output_bytes & 63u) ||
		!ring_in_ram((unsigned int)job->output, job->output_bytes) || (job->output_bytes && ((unsigned int)job->output & 0x40000000u)))
	{
		ring_stats.refused_check++;
		return 0;
	}
	for (k = 0; k < ME_RING_INPUTS; k++)
		if (!(job->input[k] == NULL && job->input_bytes[k] == ME_RING_ALL_OF_RAM) &&
			!ring_in_ram((unsigned int)job->input[k], job->input_bytes[k]))
		{
			ring_stats.refused_check++;
			return 0;
		}
	t = ring_next;
	index = t & RING_MASK;
	request = &ring_requests[index];
	request->type = job->type;
	request->ticket = t;
	for (k = 0; k < ME_RING_INPUTS; k++)
	{
		request->input[k] = (unsigned int)job->input[k];
		request->input_bytes[k] = job->input_bytes[k];
	}
	request->output = (unsigned int)job->output;
	request->output_bytes = job->output_bytes;
	for (k = 0; k < ME_RING_ARGS; k++)
		request->args[k] = job->args[k];
	ring_push_mark[index] = start;
	ring_next = t + 1;
	if (me_state == 2)
		ring_push_simulated(index, t);
	else
	{
		/* the output's lines written back and dropped here: the wait drops
		them with a write-back too (0x1B), and a line the caller had left
		dirty would then land on the ME's answer */
		if (ring_push_drop && job->output_bytes)
		{
			if (job->output_bytes > RING_WHOLE_CACHE_BYTES)
				sceKernelDcacheWritebackInvalidateAll();
			else
				lines_drop((unsigned int)job->output, job->output_bytes);
		}
		lines_writeback((unsigned int)request, sizeof(*request));
		RING_PUSHED = t + 1;
		__asm__ __volatile__("sync");
		me_wake();
	}
	*ticket = t;
	ring_stats.push_us += sceKernelGetSystemTimeLow() - start;
	return 1;
}

/* perception's queued rays (psp_fixups.py FIXES_122, actor_perception.c
#merays). The engine's units don't see this header, so the job is put
together here. A ray reads the level and the objects anywhere in RAM: this
CPU's whole cache is written back first, and the ME drops its whole cache.
Once a hand-over, not once a job: a tick's jobs are pushed one after the
other with nothing written in between (continues: not the first of them), and
nothing a ray reads changes until all of them are collected (the five
functions that count psp_object_epoch wait for them first), so what the first
job made fresh on the ME stays fresh for the rest */
int psp_me_push_perc_rays(const void *rays, unsigned int count, void *answers, unsigned int answer_bytes,
	unsigned int *ticket, int continues)
{
	struct psp_me_job job;

	memset(&job, 0, sizeof(job));
	job.type = ME_JOB_PERC_RAYS;
	job.args[0] = (unsigned int)rays;
	job.args[1] = (unsigned int)answers;
	job.args[2] = count;
	job.args[ME_RING_ARG_CONTINUES] = continues && rays_batch;
	job.input[0] = NULL;
	job.input_bytes[0] = ME_RING_ALL_OF_RAM;
	job.output = answers;
	job.output_bytes = answer_bytes;
	if (!job.args[ME_RING_ARG_CONTINUES])
		psp_me_writeback(NULL, ME_RING_ALL_OF_RAM);
	return psp_me_push(&job, ticket);
}

/* next tick's base poses (psp_fixups.py FIXES_131, objects.c #poseahead): a
decode reads only tag data (the model, the animation, both fixed within a
map), so the whole cache is handed over as for the rays, and the job writes
only the poses, in the arena (output) */
int psp_me_push_poses(const void *entries, unsigned int count, void *arena, unsigned int arena_bytes,
	unsigned int *ticket)
{
	struct psp_me_job job;
	unsigned int control;

	memset(&job, 0, sizeof(job));
	job.type = ME_JOB_POSES;
	job.args[0] = (unsigned int)entries;
	job.args[1] = count;
	/* this CPU's FPU control (rounding, flushing, traps): the ME decodes
	under the same, so its arithmetic is this CPU's bit for bit */
	__asm__ __volatile__("cfc1 %0, $31" : "=r"(control));
	job.args[2] = control;
	job.input[0] = NULL;
	job.input_bytes[0] = ME_RING_ALL_OF_RAM;
	job.output = arena;
	job.output_bytes = arena_bytes;
	psp_me_writeback(NULL, ME_RING_ALL_OF_RAM);
	return psp_me_push(&job, ticket);
}

/* the frame's point physics (psp_fixups.py FIXES_MEJOBS, point_physics.c
#mepoints): a point reads the level, its water, the wind and its tags, so
the whole cache is handed over as for the rays. The ME writes only the
answers (output) and its word; it reads the main CPU's word, which tells it
where to stop, through the uncached view. Both words' lines are the callers' */
int psp_me_push_points(const void *inputs, unsigned int count, void *answers, unsigned int answer_bytes,
	unsigned int generation, void *main_word, void *me_word, unsigned int *ticket)
{
	struct psp_me_job job;
	unsigned int control;

	memset(&job, 0, sizeof(job));
	job.type = ME_JOB_POINTS;
	job.args[0] = (unsigned int)inputs;
	job.args[1] = (unsigned int)answers;
	job.args[2] = count;
	job.args[3] = generation;
	/* this CPU's FPU control: the ME's arithmetic is then this CPU's */
	__asm__ __volatile__("cfc1 %0, $31" : "=r"(control));
	job.args[4] = control;
	job.args[5] = (unsigned int)main_word;
	job.args[6] = (unsigned int)me_word;
	job.input[0] = NULL;
	job.input_bytes[0] = ME_RING_ALL_OF_RAM;
	job.output = answers;
	job.output_bytes = answer_bytes;
	psp_me_writeback(NULL, ME_RING_ALL_OF_RAM);
	return psp_me_push(&job, ticket);
}

/* a pushed job's answer: 1 when done (results filled, its output readable
on this CPU), 0 if the ME failed it (stopped, too late, or the job broke a
rule): the caller does the work itself, and must not read the output */
int psp_me_wait(unsigned int ticket, unsigned int *results)
{
	unsigned int index = ticket & RING_MASK, start = sceKernelGetSystemTimeLow(), now, looks = 0, k;
	struct me_ring_request *request = &ring_requests[index];
	struct me_ring_result *result = &ring_results[index];
	unsigned long waited;

	if (ring_next - ticket - 1 >= ME_RING_ENTRIES)
		return 0;
	/* simulated, the push opened the window, whatever has stopped since */
	if (me_restartable == 2 || me_state == 2)
		psp_me_window[0] = 0;
	/* without this, popped (moved past it by the restart) would say it was
	done, and another job's answer would stop the ring for the session */
	if (ring_restarted && ticket - ring_epoch >= ring_next - ring_epoch)
		return ring_fail(0, ticket, index);
#ifdef PSP_RECON
	if (ring_late_every && ticket && ticket % ring_late_every == 0)
	{
		while (sceKernelGetSystemTimeLow() - ring_push_mark[index] <= RING_DEADLINE_US)
			for (k = 0; k < RING_WAIT_NOPS; k++)
				__asm__ __volatile__("nop");
		return ring_fail(1, ticket, index);
	}
#endif
	if ((int)(RING_POPPED - ticket) > 0)
		ring_stats.at_once++;
	else
	{
		while ((int)(RING_POPPED - ticket) <= 0)
		{
			if (looks++ % RING_WAIT_CLOCK_EVERY == 0)
			{
				now = sceKernelGetSystemTimeLow();
				/* stopped (HOME, sleep, a slot job's timeout) before this job */
				if (me_state <= 0 || CONTROL->stop == 2)
					return ring_fail(0, ticket, index);
				if (now - ring_push_mark[index] > RING_DEADLINE_US)
				{
					/* as a batch record's: late only if still not done */
					if (!me_grace_off && (int)(RING_POPPED - ticket) > 0)
					{
						if (deadline_lines < DEADLINE_LINES)
						{
							deadline_lines++;
							platform_log("ME: ring job %u (type %u) past %u ms by the clock, done at the look after it (pushed %u us before)",
								ticket, request->type, RING_DEADLINE_US / 1000, now - ring_push_mark[index]);
						}
						break;
					}
					return ring_fail(1, ticket, index);
				}
				/* mehalt: a wake that may have been lost (me_halt_idle) */
				me_wake();
			}
			for (k = 0; k < RING_WAIT_NOPS; k++)
				__asm__ __volatile__("nop");
		}
	}
	/* this CPU's copies of what the ME wrote are stale: dropped (it never
	writes these lines itself, so dropping loses nothing of its own) */
	lines_drop((unsigned int)result, sizeof(*result));
	if (request->output_bytes > RING_WHOLE_CACHE_BYTES)
		sceKernelDcacheWritebackInvalidateAll();
	else if (request->output_bytes)
		lines_drop(request->output, request->output_bytes);
	if (result->ticket != ticket || result->status != RING_OK)
		return ring_fail(2, ticket, index);
	for (k = 0; k < ME_RING_RESULTS; k++)
		results[k] = result->results[k];
	now = sceKernelGetSystemTimeLow();
	waited = now - start;
	ring_stats.jobs++;
	ring_stats.wait_us += waited;
	if (waited > ring_stats.wait_worst_us)
		ring_stats.wait_worst_us = waited;
	ring_stats.round_us += now - ring_push_mark[index];
	ring_stats.job_cycles += result->job_cycles;
	ring_stats.all_cycles += result->all_cycles;
	ring_stats.look_cycles += result->look_cycles;
	if (result->look_cycles > ring_stats.look_worst_cycles)
		ring_stats.look_worst_cycles = result->look_cycles;
	return 1;
}

/* ---------- batches: the main CPU's side (psp_me.h has the contract) */

/* a record's state, the main CPU's own byte */
#define BATCH_ADDED 0   /* filled here, not handed over */
#define BATCH_OUT 1     /* in a job pushed to the ME */
#define BATCH_HERE 2    /* made by the main CPU */
#define BATCH_BY_ME 3   /* made by the ME, its lines dropped here */
/* the most a record the ME is on may hold the main CPU before it looks
again: past it, a record still not done is slow, and one still not done at
BATCH_RECORD_GIVEUP_US stops the ME (the limit a slot's audio job has).
With "megraceoff", the first deadline stops the ME */
#define BATCH_RECORD_DEADLINE_US 10000
#define BATCH_RECORD_GIVEUP_US 40000

static struct
{
	unsigned long passes, by_me, by_main, taken, waits, refused, late;
	/* past the deadline by the clock: done at the look after it (this CPU
	was off its wait meanwhile), or still on the ME (slow) */
	unsigned long second_look, slow;
	/* jobs the ME ended at the limit; the longest wait for a record, and
	the longest stretch between two looks in one (a thread above the game's
	ran meanwhile) */
	unsigned long withdrawn, wait_worst_us, gap_worst_us;
	/* waits ended because every job of the batch had ended with the record
	not made: a claim left behind by a later job's withdrawal */
	unsigned long stale;
} batch_stats;


int psp_me_batch_init(struct psp_me_batch *b, psp_me_record_fn make, void *records, unsigned char *state,
	unsigned int record_bytes, unsigned int capacity, unsigned int chunk, const unsigned int *args)
{
	unsigned int k;

	if (((unsigned int)b & 63u) || !make || !records || !state || !record_bytes || (record_bytes & 63u) ||
		((unsigned int)records & 63u) || !capacity || !chunk || !ring_in_ram((unsigned int)records, capacity * record_bytes))
		return 0;
	memset(b, 0, sizeof(*b));
	/* the words go through the uncached view only: no cached copy of their
	lines may be left here to be written back over the ME's */
	sceKernelDcacheWritebackInvalidateRange(b, 4 * sizeof(struct psp_me_batch_word));
	b->make = make;
	b->records = (unsigned char *)records;
	b->state = state;
	b->record_bytes = record_bytes;
	b->capacity = capacity;
	b->chunk = chunk;
	for (k = 0; k < ME_BATCH_ARGS; k++)
		b->args[k] = args ? args[k] : 0;
	batch_sim_half = psp_keep("mebatchsimhalf");
	psp_me_batch_begin(b);
	return 1;
}

void psp_me_batch_begin(struct psp_me_batch *b)
{
	if (batch_sim_behind == b)
		batch_sim_behind = NULL;
	batch_sim_whole = batch_sim_passes++ & 3u;
	b->added = b->handed = b->ticket_count = 0;
	b->limit_now = 0xFFFFFFFFu;
	UNCACHED(&b->limit)->value = 0xFFFFFFFFu;
	UNCACHED(&b->started)->value = 0;
	UNCACHED(&b->done)->value = 0;
	UNCACHED(&b->withdrawn)->value = 0;
	__asm__ __volatile__("sync");
}

/* records [b->handed, end) to the ME as one ring job; left here if the ring
can't take it */
static void batch_hand(struct psp_me_batch *b, unsigned int end)
{
	struct psp_me_job job;
	unsigned int first = b->handed, k, ticket, control;

	if (end <= first || b->ticket_count >= ME_BATCH_TICKETS || !psp_me_ring_ready())
		return;
	memset(&job, 0, sizeof(job));
	job.type = ME_JOB_BATCH;
	job.args[0] = (unsigned int)b;
	job.args[1] = first;
	job.args[2] = end - first;
	__asm__ __volatile__("cfc1 %0, $31" : "=r"(control));
	job.args[3] = control;
	job.args[4] = (unsigned int)b->make;
	job.args[5] = (unsigned int)b->records;
	job.args[6] = b->record_bytes;
	for (k = 0; k < ME_BATCH_ARGS; k++)
		job.args[7 + k] = b->args[k];
	job.input[0] = b->records + first * b->record_bytes;
	job.input_bytes[0] = (end - first) * b->record_bytes;
	psp_me_writeback(job.input[0], job.input_bytes[0]);
	if (!psp_me_push(&job, &ticket))
	{
		b->refused++;
		return;
	}
	for (k = first; k < end; k++)
		b->state[k] = BATCH_OUT;
	b->tickets[b->ticket_count++] = ticket;
	b->handed = end;
}

void *psp_me_batch_add(struct psp_me_batch *b, unsigned int *index)
{
	if (b->added >= b->capacity)
		return NULL;
	/* the records before this one are filled: a chunk's worth goes now */
	if (b->added - b->handed >= b->chunk)
		batch_hand(b, b->added);
	*index = b->added;
	b->state[b->added] = BATCH_ADDED;
	return b->records + b->added++ * b->record_bytes;
}

void psp_me_batch_close(struct psp_me_batch *b)
{
	batch_hand(b, b->added);
}

static void batch_make_here(struct psp_me_batch *b, unsigned int index)
{
	b->make(b->records + index * b->record_bytes, b->args);
	b->state[index] = BATCH_HERE;
	b->by_main++;
}

int psp_me_batch_need(struct psp_me_batch *b, unsigned int index)
{
	volatile unsigned int *started = &UNCACHED(&b->started)->value, *done = &UNCACHED(&b->done)->value;
	unsigned char *record = b->records + index * b->record_bytes;
	unsigned int start;

	if (index >= b->added)
		return 0;
	switch (b->state[index])
	{
	case BATCH_BY_ME:
		return 1;
	case BATCH_HERE:
		return 0;
	case BATCH_ADDED:
		/* never handed over: what was added before it goes now, if the
		ring takes it, and this one is made here */
		if (index >= b->handed)
		{
			batch_hand(b, index);
			/* this one, and any before it the ring refused, stay here */
			b->handed = index + 1;
		}
		batch_make_here(b, index);
		return 0;
	}
	if (index >= b->limit_now)
	{
		batch_make_here(b, index);
		return 0;
	}
	if (*done <= index)
	{
		/* not finished: taken back unless the ME has claimed it */
		b->limit_now = index;
		UNCACHED(&b->limit)->value = index;
		__asm__ __volatile__("sync");
		if (*started > index)
		{
			unsigned int now, last, gap, waited;
			int slow = 0;

			b->waits++;
			start = last = sceKernelGetSystemTimeLow();
			while (*done <= index && *started > index)
			{
				/* an ME stopped meanwhile: it halts once done, or is held in
				reset, and what it made is used only if it finished */
				if (!psp_me_ring_ready())
					break;
				/* the batch's last job ended: nothing on the ME will make
				this record, whatever started says */
				if (!me_grace_off && (int)(RING_POPPED - b->tickets[b->ticket_count - 1]) > 0)
				{
					if (*done <= index)
						batch_stats.stale++;
					break;
				}
				now = sceKernelGetSystemTimeLow();
				gap = now - last;
				if (gap > batch_stats.gap_worst_us)
					batch_stats.gap_worst_us = gap;
				last = now;
				if (now - start <= BATCH_RECORD_DEADLINE_US)
					continue;
				if (!me_grace_off)
				{
					/* the look before this clock may be from before a
					stretch this CPU spent on a thread above the game's: the
					record counts as held only if it still is */
					if (*done > index || *started <= index)
					{
						batch_stats.second_look++;
						if (deadline_lines < DEADLINE_LINES)
						{
							deadline_lines++;
							platform_log("ME: a batch record (%u) past %u ms by the clock (%u us), done at the look after it; this CPU was off its wait %u us just before",
								index, BATCH_RECORD_DEADLINE_US / 1000, now - start, gap);
						}
						continue;
					}
					if (!slow)
					{
						slow = 1;
						batch_stats.slow++;
						if (deadline_lines < DEADLINE_LINES)
						{
							deadline_lines++;
							platform_log("ME: a batch record (%u) still on the ME %u ms after the wait began; waiting to %u ms",
								index, (now - start) / 1000, BATCH_RECORD_GIVEUP_US / 1000);
							me_stash_dump("a batch record slow");
						}
					}
					if (now - start <= BATCH_RECORD_GIVEUP_US)
						continue;
				}
				batch_stats.late++;
				platform_log("ME: a batch record (%u) held past %u ms; the ME is stopped", index,
					(me_grace_off ? BATCH_RECORD_DEADLINE_US : BATCH_RECORD_GIVEUP_US) / 1000);
				me_stash_dump("a batch record late");
				psp_me_stop("a batch record late");
				break;
			}
			waited = sceKernelGetSystemTimeLow() - start;
			if (waited > batch_stats.wait_worst_us)
				batch_stats.wait_worst_us = waited;
		}
		if (*done <= index)
		{
			b->taken++;
			batch_make_here(b, index);
			return 0;
		}
	}
	/* the ME's answer, past the copy this CPU had from before the hand-over */
	__asm__ __volatile__("sync");
	lines_drop((unsigned int)record, b->record_bytes);
	b->state[index] = BATCH_BY_ME;
	b->by_me++;
	return 1;
}

void psp_me_batch_finish(struct psp_me_batch *b)
{
	unsigned int index, k, results[ME_RING_RESULTS];

	for (index = 0; index < b->added; index++)
		psp_me_batch_need(b, index);
	/* every record is made; the jobs only have their withdrawn claims left,
	and must be done before the arena or the words are used again */
	for (k = 0; k < b->ticket_count; k++)
		if (ring_next - b->tickets[k] - 1 < ME_RING_ENTRIES)
			psp_me_wait(b->tickets[k], results);
	b->ticket_count = 0;
	batch_stats.withdrawn += UNCACHED(&b->withdrawn)->value;
	batch_stats.passes++;
	batch_stats.by_me += b->by_me;
	batch_stats.by_main += b->by_main;
	batch_stats.taken += b->taken;
	batch_stats.waits += b->waits;
	batch_stats.refused += b->refused;
	b->by_me = b->by_main = b->taken = b->waits = b->refused = 0;
}

/* "mebatchtest": a batch's own check, once a frame. Each record's answer is
a function of its inputs, made by the ME or here, and checked here against
the same function. Records are needed in a scrambled order as they are
added, so every path runs: made by the ME, never handed over, taken back,
and (on the PSP) the ME on the record at that moment */
#define BATCHTEST_RECORDS 48
#define BATCHTEST_WORDS 16
static unsigned int batchtest_records[BATCHTEST_RECORDS][BATCHTEST_WORDS] __attribute__((aligned(64)));
static unsigned char batchtest_state[BATCHTEST_RECORDS];
static struct psp_me_batch batchtest_batch;
static int batchtest_on = -1;
static unsigned int batchtest_seq;
static unsigned long batchtest_checked, batchtest_wrong;

/* words 0-7 in, 8-15 out: a few hundred cycles of integer work, so the ME
is often still on a record when the main CPU asks for it */
static void batchtest_make(void *record, const unsigned int *args)
{
	unsigned int *w = (unsigned int *)record, h = args[0], k, j;

	for (k = 0; k < 8; k++)
	{
		h ^= w[k];
		for (j = 0; j < 24; j++)
			h = h * 2654435761u + (h >> 13) + j;
		w[8 + k] = h;
	}
}

void psp_me_batch_test(void)
{
	unsigned int args[ME_BATCH_ARGS] = { 0x9E3779B9u }, index, k, i, scramble;
	unsigned int expect[BATCHTEST_WORDS];
	unsigned int *w;

	if (PSP_ME_WORK())
		return;
	if (batchtest_on < 0)
	{
		batchtest_on = psp_keep("mebatchtest");
		if (batchtest_on && !psp_me_batch_init(&batchtest_batch, batchtest_make, batchtest_records, batchtest_state,
				sizeof(batchtest_records[0]), BATCHTEST_RECORDS, 6, args))
			batchtest_on = 0;
	}
	if (!batchtest_on)
		return;
	batchtest_seq++;
	psp_me_batch_begin(&batchtest_batch);
	scramble = batchtest_seq * 2246822519u;
	for (k = 0; k < BATCHTEST_RECORDS; k++)
	{
		w = (unsigned int *)psp_me_batch_add(&batchtest_batch, &index);
		if (!w)
			break;
		for (i = 0; i < 8; i++)
			w[i] = batchtest_seq * 0x01000193u ^ (index << 16) ^ i;
		/* now and then a record already added is needed, the last one
		(never handed over) among them */
		scramble = scramble * 1103515245u + 12345u;
		if (((scramble >> 16) & 7u) == 0)
			psp_me_batch_need(&batchtest_batch, (scramble >> 20) % (index + 1));
	}
	psp_me_batch_close(&batchtest_batch);
	for (k = 0; k < batchtest_batch.added; k++)
	{
		/* the order scrambled: a stride coprime with the count */
		index = (k * 7u + batchtest_seq) % batchtest_batch.added;
		psp_me_batch_need(&batchtest_batch, index);
		w = batchtest_records[index];
		for (i = 0; i < BATCHTEST_WORDS; i++)
			expect[i] = i < 8 ? (batchtest_seq * 0x01000193u ^ (index << 16) ^ i) : 0;
		batchtest_make(expect, args);
		for (i = 0; i < BATCHTEST_WORDS; i++)
			if (w[i] != expect[i])
			{
				batchtest_wrong++;
				break;
			}
		batchtest_checked++;
	}
	psp_me_batch_finish(&batchtest_batch);
}

/* "meringtest": the ring's own check, a burst of ME_RING_ENTRIES jobs at each
frame's end from the renderer's thread (the ring's one producer). Each job
reads an input the main CPU has just rewritten with a new pattern and writes
an output the main CPU read a frame before, so a missing write-back or drop
on either CPU shows as a wrong word; the burst fills the ring, so the index
wraps every frame, and the answers are collected last first */
static unsigned int ringtest_in[ME_RING_ENTRIES][RINGTEST_WORDS] __attribute__((aligned(64)));
static unsigned int ringtest_out[ME_RING_ENTRIES][RINGTEST_WORDS] __attribute__((aligned(64)));
static int ringtest_on = -1;
static unsigned int ringtest_seq = 0;
static unsigned long ringtest_bursts, ringtest_jobs, ringtest_wrong, ringtest_short, ringtest_failed;

void psp_me_ring_test(void)
{
	unsigned int tickets[ME_RING_ENTRIES], results[ME_RING_RESULTS], k, i, pushed = 0;
	struct psp_me_job job;

	if (PSP_ME_WORK())
		return;
	if (ringtest_on < 0)
		ringtest_on = psp_keep("meringtest");
	if (!ringtest_on || !psp_me_ring_ready())
		return;
	ringtest_seq++;
	for (k = 0; k < ME_RING_ENTRIES; k++)
	{
		for (i = 0; i < RINGTEST_WORDS; i++)
			ringtest_in[k][i] = (ringtest_seq * 2654435761u) ^ (k << 24) ^ (i * 40503u);
		psp_me_writeback(ringtest_in[k], sizeof(ringtest_in[k]));
		memset(&job, 0, sizeof(job));
		job.type = ME_JOB_RINGTEST;
		job.args[0] = (unsigned int)ringtest_in[k];
		job.args[1] = (unsigned int)ringtest_out[k];
		job.args[2] = ringtest_seq;
		job.input[0] = ringtest_in[k];
		job.input_bytes[0] = sizeof(ringtest_in[k]);
		job.output = ringtest_out[k];
		job.output_bytes = sizeof(ringtest_out[k]);
		if (!psp_me_push(&job, &tickets[k]))
			break;
		pushed++;
	}
	if (pushed < ME_RING_ENTRIES)
		ringtest_short++;
	for (k = pushed; k-- > 0;)
	{
		unsigned int sum = 0, wrong = 0;

		if (!psp_me_wait(tickets[k], results))
		{
			ringtest_failed++;
			continue;
		}
		for (i = 0; i < RINGTEST_WORDS; i++)
		{
			sum += ringtest_in[k][i];
			if (ringtest_out[k][i] != (ringtest_in[k][i] ^ 0x5A5A5A5Au) + i)
				wrong++;
		}
		if (results[0] != sum || results[1] != ringtest_seq)
			wrong++;
		ringtest_wrong += wrong;
		ringtest_jobs++;
	}
	ringtest_bursts++;
}

/* ---------- the wake: the ME started again after a sleep ("mewake")

The kernel's sleep handler holds the ME in reset from the sleep on. Once the
wake is complete the power callback asks for a restart (psp_me_wake_request),
and the main thread carries it out at a frame's end (psp_me_wake_poll):
- that thread is the ring's one producer, and between frames it has no ring
  job half handed over, and the render slot's half has been waited for;
- the other slots' threads (audio, the heartbeat's probe) are waited for
  through slot_busy, frame end by frame end;
- me_state stays -1 until the ME answers, so until then every client takes
  its job back to the main CPU, as after the sleep.
The ME's code runs from this program in main RAM, which the PSP keeps
through a sleep, and its stack is in its own eDRAM, made anew at each start.
What the sleep does take is the ME's state: a sleep powers it down, and the
library's sleep handler has replaced the firmware's own, which on a wake
would turn the ME's bus clock back on and boot it with its caches cleared
(the firmware's me_wrapper, as uOFW reads it). The restart does those two
things itself, then the library's start (psp_me_wake_stub,
me_restart_kernel). The first try, without them, found no ME answering
within 100 ms on a PSP-1000 [M] */
#define WAKE_ANSWER_US 300000 /* the ME's first idle poll after the release, at most */

/* What the firmware's ME boot code does first (its me_wrapper's boot code),
before any read through a cache: both caches' tags cleared, so nothing in
them is taken for data or written back. A sleep powers the ME down, so its
caches wake holding noise, and meLibOnProcess's first act, a write-back of
the whole data cache, would write every noisy dirty line to wherever its tag
points. At launch the firmware's boot has already cleared them, which is why
the library's start handler can go without. The loops are the firmware's,
sized from the ME's own Config register. The library's handler is copied
straight after this, so the ME runs on into it; the two marks say how far
it got. Copied to the reset vector, never run in place */
__asm__(
	".section .text\n"
	".balign 4\n"
	".set push\n"
	".set noreorder\n"
	".set noat\n"
	".globl psp_me_wake_stub\n"
	"psp_me_wake_stub:\n"
	"	lui	$k0, 0xbfc0\n"
	"	lui	$k1, 0x57a9\n"
	"	ori	$k1, $k1, 1\n"
	"	sw	$k1, 0x6c0($k0)\n"
	"	sync\n"
	"	mtc0	$zero, $28\n"
	"	mtc0	$zero, $29\n"
	"	li	$t9, 2048\n"
	"	mfc0	$t0, $16\n"
	"	ext	$t0, $t0, 9, 3\n"
	"	sllv	$k1, $t9, $t0\n"
	"1:	addi	$k1, $k1, -64\n"
	"	bnez	$k1, 1b\n"
	"	cache	0x1, 0($k1)\n"
	"	mfc0	$t0, $16\n"
	"	ext	$t0, $t0, 6, 3\n"
	"	sllv	$k1, $t9, $t0\n"
	"2:	addi	$k1, $k1, -64\n"
	"	bnez	$k1, 2b\n"
	"	cache	0x11, 0($k1)\n"
	"	sync\n"
	"	lui	$k1, 0x57a9\n"
	"	ori	$k1, $k1, 2\n"
	"	sw	$k1, 0x6c0($k0)\n"
	"	sync\n"
	".globl psp_me_wake_stub_end\n"
	"psp_me_wake_stub_end:\n"
	".set pop\n"
	".previous\n");
extern const char psp_me_wake_stub[], psp_me_wake_stub_end[];

static volatile int wake_pending = 0;
static unsigned long long wake_mark;
static unsigned int wake_frames; /* frame ends spent waiting for the clients */
static char wake_line[240], wake_regs_line[480];
static volatile int wake_line_ready = 0;
/* the registers and marks around the restart's release */
static struct me_sysregs regs_release_before, regs_release_after, regs_answer;

/* for the heartbeat (psp_me_report), which says it once */
static void wake_report(const char *format, ...)
{
	va_list list;
	char before[96], after[96];

	va_start(list, format);
	vsnprintf(wake_line, sizeof(wake_line), format, list);
	va_end(list);
	wake_regs_line[0] = '\0';
	if (me_restartable == 1)
	{
		char launch[96], sleep[96], wake[96];

		sysregs_line(launch, sizeof(launch), "launch", &regs_launch);
		sysregs_line(sleep, sizeof(sleep), "sleep", &regs_sleep);
		sysregs_line(wake, sizeof(wake), "wake", &regs_wake);
		before[0] = after[0] = '\0';
		if (regs_answer.reset | regs_answer.bus_clock)
		{
			sysregs_line(before, sizeof(before), "; release", &regs_release_before);
			sysregs_line(after, sizeof(after), " ->", &regs_answer);
		}
		snprintf(wake_regs_line, sizeof(wake_regs_line),
			"ME: system registers (reset, bus clocks, clocks, AVC power) at %s; %s; %s%s%s", launch, sleep, wake, before, after);
	}
	wake_line_ready = 1;
}

/* the power callback, the wake complete */
void psp_me_wake_request(void)
{
	if (me_state == 0)
		return;
	wake_mark = sceKernelGetSystemTimeWide();
	wake_frames = 0;
	wake_pending = 1;
}

/* The release, in kernel mode: as the firmware boots the ME (its
me_wrapper), the ME's bus clock on and its reset held while its reset
vector is written; then the vector written with the cache clear first and
the library's start after it (meLibReset's copy, moved along by the stub's
length; the handler has no address of its own in it: one branch relative,
one jump to meLibOnPreProcess's absolute address); then the caches written
back and the reset's two bits let go */
static int me_restart_kernel(void *unused)
{
	unsigned int stub = (unsigned int)(psp_me_wake_stub_end - psp_me_wake_stub);
	unsigned int handler = (unsigned int)(&__stop__me_section - &__start__me_section);
	u32 interrupts;

	(void)unused;
	sysregs_read(&regs_release_before);
	reset_hold();
	meLibSuspendCpuIntr(interrupts);
	HW_SYS_BUS_CLOCK_ENABLE |= 1;
	meLibResumeCpuIntr(interrupts);
	hw(ME_MARK_START) = 0;
	hw(ME_MARK_PROCESS) = 0;
	memcpy((void *)ME_HANDLER_BASE, psp_me_wake_stub, stub);
	memcpy((void *)(ME_HANDLER_BASE + stub), &__start__me_section, handler);
	sceKernelDcacheWritebackInvalidateAll();
	sceKernelIcacheInvalidateAll();
	reset_release();
	sysregs_read(&regs_release_after);
	return 0;
}

/* how far the ME's start got, from its marks */
static const char *wake_stage(const struct me_sysregs *regs)
{
	if (regs->marks[1] == ME_MARK_PROCESS_RUN)
		return "into its loop (past its first cache write-back)";
	if (regs->marks[1] == ME_MARK_PROCESS_IN)
		return "into the game's ME code, not past its first cache write-back";
	if (regs->marks[0] == ME_MARK_STUB_DONE)
		return "past the cache clear, not into the game's ME code (the library's start handler: its VME wait, or the jump)";
	if (regs->marks[0] == ME_MARK_STUB_IN)
		return "into the cache clear, not out of it";
	return "nowhere: not its first instruction (power, clock or reset)";
}

void psp_me_wake_poll(void)
{
	volatile struct me_control *control = CONTROL;
	unsigned long long release, now;
	unsigned int sleeps;
	int k;

	if (ring_dead == 2)
		ring_revive();
	if (!wake_pending)
		return;
	if (me_state > 0)
	{
		/* no sleep reached the ME: nothing to start */
		wake_pending = 0;
		return;
	}
	if (!psp_keep("mewake") || psp_keep("mewakeoff") || !me_restartable)
	{
		wake_pending = 0;
		wake_report("ME: stays stopped after the wake (%s); the main CPU runs its jobs",
			!me_restartable ? "it was stopped for a failure before the sleep" :
			psp_keep("mewake") ? "mewakeoff" : "the restart is opt-in: mewake");
		return;
	}
	for (k = 0; k < ME_SLOTS; k++)
		if (slot_busy[k] || submitted[k])
		{
			wake_frames++;
			return;
		}
	wake_pending = 0;
	sleeps = me_sleeps;
	/* every word the ME reads at its start, as at launch; the slots and the
	ring carried on from where the main CPU's counters are, so a ticket or
	request number handed out before the sleep can never be taken for one
	of the new ME's */
	control->stop = control->alive = control->clock = control->jobs = 0;
	control->halted = 0;
	for (k = 0; k < ME_SLOTS; k++)
		SLOT(k)->request = SLOT(k)->done = slot_request[k];
	ring_epoch = ring_next;
	ring_restarted = 1;
	ring_popped_seen = ring_next;
	RING_PUSHED = RING_POPPED = ring_next;
	/* before the release, where launch sets them after its start check: the
	ME reads them on its first pass, so the ring is served from the first
	frame (a ring job not done in 10 ms turns the ring off for the session) */
	control->features = (me_halting ? ME_FEATURE_HALT : 0u) | (ring_on ? ME_FEATURE_RING : 0u) |
		(me_yield_off ? ME_FEATURE_NO_YIELD : 0u) | (psp_keep("meedramstate") ? ME_FEATURE_EDRAM_STATE : 0u) |
		(me_claim_old ? ME_FEATURE_CLAIM_OLD : 0u) | (psp_keep("mebcc1") ? ME_FEATURE_BCC : 0u) |
		(psp_keep("mefaultoff") ? ME_FEATURE_FAULT_OFF : 0u) | (psp_keep("mefaulttest") ? ME_FEATURE_FAULT_TEST : 0u);
	__asm__ __volatile__("sync");
	now = sceKernelGetSystemTimeWide();
	if (me_restartable == 2)
	{
		me_state = 2;
		wake_report("ME (simulated): restarted after wake in %lu ms (%u frame ends waiting for its clients)",
			(unsigned long)((now - wake_mark) / 1000), wake_frames);
		return;
	}
	release = now;
	me_maybe_running = 1;
	kcall_3(me_restart_kernel, 0, 0);
	while (control->alive == 0 && (now = sceKernelGetSystemTimeWide()) - release < WAKE_ANSWER_US)
		sceKernelDelayThread(500);
	kcall_3(me_sysregs_kernel, 0, &regs_answer);
	if (control->alive == 0 || me_sleeps != sleeps)
	{
		kcall_2(me_hold_reset, 0);
		me_maybe_running = 0;
		wake_report("ME: restart after the wake FAILED (%s; the ME's start got %s); held in reset, the main CPU runs its jobs",
			control->alive == 0 ? "no idle poll within 300 ms of the reset's release" : "the PSP slept again meanwhile",
			wake_stage(&regs_answer));
		return;
	}
	me_state = 1;
	wake_report("ME: restarted after wake in %lu ms (its first idle poll %lu us after the reset's release; %u frame ends waiting for its clients)",
		(unsigned long)((now - wake_mark) / 1000), (unsigned long)(now - release), wake_frames);
}

static int me_sleep_kernel(void)
{
	meLibOnSleep();
	return 0;
}

/* "mewaketest": what a sleep does to the ME, without one, so the restart
can be run where the PSP can't sleep (the emulator) and on the PSP without
a hand on the power switch. The wake (psp_me_wake_request) is the caller's */
void psp_me_wake_test_sleep(void)
{
	if (me_state <= 0)
	{
		platform_log("mewaketest: the ME isn't running; nothing to test");
		return;
	}
	psp_me_sleep_stop();
	if (me_restartable == 1)
		kcall_2(me_sleep_kernel, 0);
	else
	{
		me_state = -1;
		me_sleeps++;
	}
	platform_log("mewaketest: the ME stopped and held as a sleep holds it");
}

/* build 87: the last job's own code time on the ME, in microseconds (its
cycle counter, without the cache hand-overs); simulated, the main CPU's time
for the whole job */

unsigned int psp_me_last_job_us(int which)
{
	return which >= 0 && which < ME_SLOTS ? last_job_us[which] : 0;
}

/* the ME's cycle counter's counts a microsecond, measured at start (0: not
measured, so ME times can't be given) */
float psp_me_cycles_per_us(void)
{
	return me_cycles_per_us;
}

/* the audio job's list: up to 256 voices, written through the uncached view */
/* where the ME's audio time went (its own counter, summed over the blocks
it mixed): the 'ME: audio split' line takes the job apart into decoding,
the voices' mixing loop and the sums' ends; the job's code less these is
its entry and the voice list */
static struct
{
	unsigned long long decode, voices, ends, voice_frames;
	unsigned long blocks;
} audio_split;

int psp_me_audio(void *const *list, unsigned long count, short *output, unsigned long frames, unsigned long *voices,
	unsigned long *clipped, unsigned long *waited_us)
{
	unsigned int args[3], results[ME_JOB_RESULTS];
	unsigned long long start = sceKernelGetSystemTimeWide();
	unsigned long k;

	if (me_state <= 0 || count > AUDIO_LIST_MAX || frames > MIX_FRAMES_MAX)
		return 0;
	for (k = 0; k < count; k++)
		AUDIO_LIST[k] = list[k];
	args[0] = (unsigned int)count;
	args[1] = (unsigned int)frames;
	args[2] = (unsigned int)output;
	/* about 0.1-1 ms of work: a first look after 0.2 ms, or mepoll's
	(audio_look_next) */
	if (!psp_me_run(ME_SLOT_AUDIO, ME_JOB_AUDIO, args, 3, results, audio_first_look_us))
		return 0;
	*voices = results[0];
	*clipped = results[1];
	if (results[3] || results[5])
	{
		audio_split.decode += results[2];
		audio_split.voices += results[3];
		audio_split.ends += results[4];
		audio_split.voice_frames += results[5];
		audio_split.blocks++;
	}
	*waited_us = (unsigned long)(sceKernelGetSystemTimeWide() - start);
	if (me_state == 1)
		audio_look_next(*waited_us);
	return 1;
}

/* build 86's entry, kept for psp_dsound.c */
int psp_meaudio_run(void *const *list, unsigned long count, short *output, unsigned long frames, unsigned long *voices,
	unsigned long *clipped, unsigned long *waited_us)
{
	return psp_me_audio(list, count, output, frames, voices, clipped, waited_us);
}

/* the main CPU's own work around a job (prepare, cache write-back and
invalidation), told by the client, for the log */
void psp_me_handover(int which, unsigned long us)
{
	if (which >= 0 && which < ME_SLOTS)
		stats[which].handover_us += us;
}

float psp_me_busy_percent(void)
{
	return busy_percent_last;
}

/* every heartbeat (psp_platform.c): per slot, the ME's own time a job and its
share of the ME, the main CPU's hand-over and wait */
/* build 115: "edramcheck" (big_levers.md section 0, "The ME's eDRAM,
checked"): whether anything but our own ME code writes the ME's eDRAM during
play. The ME fills the stretch from 256 KB to 1.5 MB (its stack starts at the
top, 2 MB, and grows down: 512 KB are left to it) with a pattern, then checks
it piece by piece, and the log says what it found. Only with the ME really
running (not simulated, not stopped).
Build 116: a 64 KB piece a heartbeat, for the fill as for the checks. b115
filled the whole 1.25 MB in one job: 56 ms on the ME, so the audio job queued
behind it passed its 40 ms limit and the ME was stopped for the session
(hardware_b115.md, test 1). A piece is about 3 ms of the ME's time */
/* Three stretches of the PSP-1000's 2 MB, one 64 KB piece a heartbeat each
pass (~30 s a pass):
- 0 to 192 KB, hashed, never written: whatever lies there is left as found,
  and a hash that moves says something else writes it;
- 256 KB to 1.5 MB, patterned: the room the ME's jobs and the sound cache
  (R2) would take; any change is something else writing eDRAM;
- 1.5 MB to 1,984 KB, patterned below the ME's stack: the stack grows down
  from 2 MB, and the lowest word changed is how deep it has reached, the
  room R2 must leave it. A change here is the ME's own, not an alarm, on a
  PSP-1000; a slim's 4 MB put its stack at 4 MB (the job says where its own
  stack is), so there this stretch is patterned room like the one before.
192-256 KB is left out: mebusedram's scratch is there */
#define EDRAM_PROBE_FIRST 0x40000u
#define EDRAM_PROBE_END 0x180000u
#define EDRAM_PROBE_PIECE 0x10000u
#define EDRAM_HASH_END 0x30000u
#define EDRAM_STACK_END 0x1F0000u
#define EDRAM_HASH_PIECES (EDRAM_HASH_END / EDRAM_PROBE_PIECE)
#define EDRAM_PIECES (EDRAM_HASH_PIECES + (EDRAM_STACK_END - EDRAM_PROBE_FIRST) / EDRAM_PROBE_PIECE)

/* the piece's first byte, k counting the hashed pieces first */
static unsigned int edram_piece(unsigned int k)
{
	return k < EDRAM_HASH_PIECES ? k * EDRAM_PROBE_PIECE : EDRAM_PROBE_FIRST + (k - EDRAM_HASH_PIECES) * EDRAM_PROBE_PIECE;
}

void psp_me_edram_probe(void)
{
	static int asked = -1;
	static unsigned int prepared = 0, next = 0, checked = 0, bad_pieces = 0, hash_moved = 0, passes = 0;
	static unsigned int stack_lowest = 0xFFFFFFFFu, hashes[EDRAM_HASH_PIECES];
	unsigned int args[3], results[ME_JOB_RESULTS] = { 0 }, first;

	if (asked < 0)
		asked = psp_keep("edramcheck") != 0;
	if (!asked || me_state != 1)
		return;
	if (prepared < EDRAM_PIECES)
	{
		/* the first pass: the hashes taken, the pattern written */
		first = edram_piece(prepared);
		args[0] = prepared < EDRAM_HASH_PIECES ? 2 : 0;
		args[1] = first;
		args[2] = EDRAM_PROBE_PIECE;
		if (psp_me_run(ME_SLOT_PROBE, ME_JOB_EDRAM, args, 3, results, 2000) && results[0] != 0xFFFFFFFFu)
		{
			if (prepared < EDRAM_HASH_PIECES)
				hashes[prepared] = results[0];
			if (++prepared == EDRAM_PIECES)
				platform_log("ME: edramcheck: eDRAM 0-%u KB hashed, %u-%u KB patterned (the ME's stack's reach from %u KB), %u KB a heartbeat",
					EDRAM_HASH_END / 1024, EDRAM_PROBE_FIRST / 1024, EDRAM_STACK_END / 1024, EDRAM_PROBE_END / 1024,
					EDRAM_PROBE_PIECE / 1024);
		}
		return;
	}
	first = edram_piece(next);
	args[0] = next < EDRAM_HASH_PIECES ? 2 : 1;
	args[1] = first;
	args[2] = EDRAM_PROBE_PIECE;
	if (!psp_me_run(ME_SLOT_PROBE, ME_JOB_EDRAM, args, 3, results, 2000) || results[0] == 0xFFFFFFFFu)
		return;
	checked++;
	if (next < EDRAM_HASH_PIECES)
	{
		if (results[0] != hashes[next])
		{
			hash_moved++;
			platform_log("ME: edramcheck: eDRAM %u to %u KB changed (hash %08x, was %08x): something writes the bottom of eDRAM",
				first / 1024, (first + EDRAM_PROBE_PIECE) / 1024, results[0], hashes[next]);
			hashes[next] = results[0];
		}
	}
	else if (first >= EDRAM_PROBE_END && results[3] < 0x200000u)
	{
		if (results[0] && results[1] < stack_lowest)
		{
			stack_lowest = results[1];
			platform_log("ME: edramcheck: the ME's stack has reached down to %u KB (%u KB deep)", stack_lowest / 1024,
				(0x200000u - stack_lowest) / 1024);
		}
	}
	else if (results[0])
	{
		bad_pieces++;
		platform_log("ME: edramcheck: %u words changed in eDRAM %u to %u KB, the first at %08x (now %08x)",
			results[0], first / 1024, (first + EDRAM_PROBE_PIECE) / 1024, results[1], results[2]);
	}
	if (++next == EDRAM_PIECES)
	{
		next = 0;
		if ((++passes % 4) == 0)
			platform_log("ME: edramcheck: %u passes, %u pieces of %u KB checked; %u patterned pieces changed, %u hashes moved; the stack's deepest %u KB",
				passes, checked, EDRAM_PROBE_PIECE / 1024, bad_pieces, hash_moved,
				stack_lowest == 0xFFFFFFFFu ? 0 : (0x200000u - stack_lowest) / 1024);
	}
}

void psp_me_report(void)
{
	static const char *const names[ME_SLOTS] = { "audio", "render", "probe" };
	unsigned long long now = sceKernelGetSystemTimeWide(), window = now - report_mark, busy_us_all = 0;
	char line[900], ring_line[400];
	int length = 0, k;

	if (!me_state)
		return;
	if (wake_line_ready)
	{
		wake_line_ready = 0;
		platform_log("%s", wake_line);
		if (wake_regs_line[0])
			platform_log("%s", wake_regs_line);
	}
	report_mark = now;
	for (k = 0; k < ME_SLOTS; k++)
	{
		unsigned long long me_us = me_cycles_per_us > 0.0f ? (unsigned long long)((float)stats[k].me_cycles / me_cycles_per_us) : 0;
		/* build 87: the job's own code, apart from the cache hand-overs */
		unsigned long long job_us = me_cycles_per_us > 0.0f ? (unsigned long long)((float)stats[k].me_job_cycles / me_cycles_per_us) : 0;

		if (stats[k].simulated)
			me_us = job_us = stats[k].wait_us;
		busy_us_all += me_us;
		if (!stats[k].jobs && !stats[k].failures)
			continue;
		length += snprintf(line + length, sizeof(line) - length,
			"; %s: %lu jobs, the ME's own time %lu us a job (the job's code %lu, cache hand-overs the rest; worst %lu), %.1f%% of the ME; the main CPU's hand-over %lu us a job, waiting %lu us a job (worst %lu); %lu failed",
			names[k], stats[k].jobs, stats[k].jobs ? (unsigned long)(me_us / stats[k].jobs) : 0,
			stats[k].jobs ? (unsigned long)(job_us / stats[k].jobs) : 0, stats[k].me_worst_us,
			window ? 100.0 * (double)me_us / (double)window : 0.0,
			stats[k].jobs ? (unsigned long)(stats[k].handover_us / stats[k].jobs) : 0,
			stats[k].jobs ? (unsigned long)(stats[k].wait_us / stats[k].jobs) : 0, stats[k].wait_worst_us, stats[k].failures);
		if (length >= (int)sizeof(line))
			length = (int)sizeof(line) - 1;
	}
	if (audio_split.blocks && me_cycles_per_us > 0.0f)
	{
		float per_block = me_cycles_per_us * (float)audio_split.blocks;

		platform_log("ME: audio split over %lu blocks: decoding %lu us a block, the voices' mixing %lu (decoding included; %lu cycles a voice-frame, %lu without decoding), the sums' zeroing and output %lu",
			audio_split.blocks, (unsigned long)((float)audio_split.decode / per_block),
			(unsigned long)((float)audio_split.voices / per_block),
			audio_split.voice_frames ? (unsigned long)(audio_split.voices / audio_split.voice_frames) : 0,
			audio_split.voice_frames ? (unsigned long)((audio_split.voices - audio_split.decode) / audio_split.voice_frames) : 0,
			(unsigned long)((float)audio_split.ends / per_block));
		memset(&audio_split, 0, sizeof(audio_split));
	}
	ring_line[0] = 0;
	if (ring_stats.jobs || ring_stats.failures || ring_stats.refused_busy || ring_stats.refused_full || ring_stats.refused_check)
	{
		/* the ring's plumbing: on the ME, the whole job against its code
		(the rest is the cache work around it) and how long a job can have
		waited to be picked up; on the main CPU, the push (the request and
		its write-back) and the wait at the end, and push to answer */
		unsigned long jobs = ring_stats.jobs ? ring_stats.jobs : 1;
		float rate = me_cycles_per_us > 0.0f ? me_cycles_per_us : 1.0f;
		unsigned long long all_us = (unsigned long long)((float)ring_stats.all_cycles / rate);

		if (me_state == 2)
			all_us = ring_stats.simulated_us;
		busy_us_all += all_us;
		/* a line of its own: the log's lines have a length limit */
		snprintf(ring_line, sizeof(ring_line),
				"ME: ring: %lu jobs (%lu done before the wait), %s %lu us a job (the job's code %lu; picked up within %lu us, worst %lu); the main CPU's push %lu us a job, wait %lu us a job (worst %lu), push to answer %lu us; refused %lu (ME busy), %lu (full), %lu (transfer rules); %lu failed",
				ring_stats.jobs, ring_stats.at_once, me_state == 2 ? "SIMULATED inside the push," : "the ME's own time",
				(unsigned long)(all_us / jobs), (unsigned long)((float)ring_stats.job_cycles / rate / (float)jobs),
				(unsigned long)((float)ring_stats.look_cycles / rate / (float)jobs), (unsigned long)((float)ring_stats.look_worst_cycles / rate),
				(unsigned long)(ring_stats.push_us / jobs), (unsigned long)(ring_stats.wait_us / jobs), ring_stats.wait_worst_us,
				(unsigned long)(ring_stats.round_us / jobs), ring_stats.refused_busy, ring_stats.refused_full, ring_stats.refused_check,
				ring_stats.failures);
		memset(&ring_stats, 0, sizeof(ring_stats));
	}
	busy_percent_last = window ? (float)(100.0 * (double)busy_us_all / (double)window) : 0.0f;
	if (me_halting && length < (int)sizeof(line) - 160)
	{
		/* mehalt's own figures since the last report. Wakes
		taken far above halts: the ME is interrupted while busy. Halts
		without wakes taken, with jobs failing: wakes don't arrive. Wakes
		taken with jobs failing: halt resumes at itself */
		static unsigned int halts_last, wakes_last;
		unsigned int halts = CONTROL->halts, wakes = CONTROL->wakes;

		length += snprintf(line + length, sizeof(line) - length,
			"; mehalt: %u halts, %lu wakes sent (a kernel call, %lu us each), %u taken",
			halts - halts_last, wakes_sent, wakes_timed ? (unsigned long)(wake_us / wakes_timed) : 0, wakes - wakes_last);
		halts_last = halts;
		wakes_last = wakes;
		wakes_sent = 0;
	}
	platform_log("ME: %s, %.1f%% busy over %.1f s%s", me_state == 1 ? "running" : me_state == 2 ? "SIMULATED (emulator)" :
		"stopped", busy_percent_last, (double)window / 1e6, length ? line : "; no jobs");
	if (ring_line[0])
		platform_log("%s", ring_line);
	if (me_state == 1 && stats[ME_SLOT_AUDIO].jobs)
	{
		/* how often the audio thread woke to look (mepoll's measure; the
		threads line's wakes count its output's returns too) */
		unsigned long jobs = stats[ME_SLOT_AUDIO].jobs, tenths = stats[ME_SLOT_AUDIO].looks * 10 / jobs;

		platform_log("ME: audio looks: %lu.%lu a job after the first, the first at %lu us on average (%s)", tenths / 10,
			tenths % 10, stats[ME_SLOT_AUDIO].first_look_us / jobs,
			audio_look_adaptive ? "mepoll: adaptive" : audio_first_look_us != AUDIO_LOOK_DEFAULT_US ? "mepoll: fixed" : "fixed");
	}
	if (me_state == 1 && (CONTROL->features & ME_FEATURE_STASH))
	{
		/* the ME's own counts since start (the stash) */
		volatile unsigned int *stash = STASH;

		platform_log("ME: since start: %u audio blocks mixed before or inside a ring job, %u of all its audio blocks handed over by ranges; ring jobs %u begun, %u ended; slot jobs %u begun, %u ended",
			stash[STASH_AUDIO_INSIDE], stash[STASH_AUDIO_RANGES], stash[STASH_RING_BEGUN], stash[STASH_RING_ENDED],
			stash[STASH_SLOT_BEGUN], stash[STASH_SLOT_ENDED]);
	}
	if (batch_stats.passes)
	{
		platform_log("ME: batches: %lu passes; records made by the ME %lu, here %lu (taken back %lu; jobs the ME ended at the limit %lu), waits for a record the ME was on %lu (longest %lu us, longest between two looks %lu us), hand-overs refused %lu; stale claims taken back %lu; past %u ms: done at a second look %lu, slow %lu, late %lu%s",
			batch_stats.passes, batch_stats.by_me, batch_stats.by_main, batch_stats.taken, batch_stats.withdrawn, batch_stats.waits,
			batch_stats.wait_worst_us, batch_stats.gap_worst_us, batch_stats.refused, batch_stats.stale, BATCH_RECORD_DEADLINE_US / 1000,
			batch_stats.second_look, batch_stats.slow, batch_stats.late, batch_sim_half ? " (mebatchsimhalf: the simulated ME makes half of each job)" : "");
		memset(&batch_stats, 0, sizeof(batch_stats));
	}
	if (batchtest_checked)
	{
		platform_log("ME: batchtest: %lu records checked, %lu wrong", batchtest_checked, batchtest_wrong);
		batchtest_checked = batchtest_wrong = 0;
	}
	if (ringtest_bursts)
	{
		platform_log("ME: ringtest: %lu bursts, %lu jobs checked, %lu wrong (words or sums), %lu bursts short of a full ring (a push refused), %lu jobs failed",
			ringtest_bursts, ringtest_jobs, ringtest_wrong, ringtest_short, ringtest_failed);
		ringtest_bursts = ringtest_jobs = ringtest_wrong = ringtest_short = ringtest_failed = 0;
	}
	memset(stats, 0, sizeof(stats));
}

#ifdef PSP_RECON
/* "mebusN"'s main CPU side (the load itself: bus_load_job). Each tick is
loaded or not by a coin (not alternately: with two ticks a frame, the first
and second tick of a frame differ), so the loaded and the unloaded ticks
sample the same play. Every 300 ticks the ME's load is also run alone for
8 ms, with the main CPU asleep, for its rate without the main CPU. The log
line: the main CPU's tick with the ME loading against without, and the ME's
loads a microsecond while the tick ran against alone */
#define BUS_LIMIT_US 200000u
static int bus_work = -2, bus_edram = 0;
static unsigned int bus_ticket, bus_coin = 0x2545F491u, bus_tick_start = 0, bus_loaded = 0;
static unsigned long bus_ticks[2], bus_tick_us[2], bus_preempted;
static unsigned long bus_loads, bus_load_cycles, bus_alone_loads[2], bus_alone_cycles[2];

static int bus_start(int edram)
{
	struct psp_me_job job;

	memset(&job, 0, sizeof(job));
	job.type = ME_JOB_BUSLOAD;
	job.args[0] = (unsigned int)bus_work;
	job.args[1] = (unsigned int)(BUS_LIMIT_US * (me_cycles_per_us > 0.0f ? me_cycles_per_us : 333.0f));
	job.args[2] = bus_coin;
	job.args[3] = (unsigned int)edram;
	job.output = bus_out;
	job.output_bytes = sizeof(bus_out);
	BUS_STOP = 0;
	if (!psp_me_push(&job, &bus_ticket))
		return 0;
	bus_running = 1;
	return 1;
}

/* the stop raised, then the job's end awaited here before psp_me_wait, which
would take a job pushed a tick ago for one past its deadline */
static void bus_finish(unsigned long *loads, unsigned long *cycles)
{
	unsigned int results[ME_RING_RESULTS], start = sceKernelGetSystemTimeLow(), k;

	bus_running = 0;
	BUS_STOP = 1;
	while ((int)(RING_POPPED - bus_ticket) <= 0 && sceKernelGetSystemTimeLow() - start < BUS_LIMIT_US)
		for (k = 0; k < RING_WAIT_NOPS; k++)
			__asm__ __volatile__("nop");
	if (psp_me_wait(bus_ticket, results))
	{
		*loads += results[0];
		*cycles += results[1];
	}
}

static void bus_collect(void)
{
	bus_preempted++;
	bus_finish(&bus_loads, &bus_load_cycles);
}

void psp_me_bus_phase(int phase)
{
	unsigned int now;

	if (bus_work == -2)
	{
		bus_work = emulator() ? -1 : psp_keep_number("mebus", -1);
		bus_edram = psp_keep("mebusedram") != 0;
	}
	if (bus_work < 0 || PSP_ME_WORK() || !psp_me_ring_ready())
		return;
	now = sceKernelGetSystemTimeLow();
	if (phase == 1)
	{
		bus_coin = bus_coin * 1103515245u + 12345u;
		bus_loaded = (bus_coin >> 16) & 1u;
		if (bus_loaded && !bus_start(bus_edram))
			bus_loaded = 0;
		bus_tick_start = sceKernelGetSystemTimeLow();
		return;
	}
	if (phase != 0 || !bus_tick_start)
		return;
	if (bus_running)
		bus_finish(&bus_loads, &bus_load_cycles);
	bus_ticks[bus_loaded]++;
	bus_tick_us[bus_loaded] += now - bus_tick_start;
	bus_tick_start = 0;
	if (bus_ticks[0] + bus_ticks[1] >= 300 && bus_ticks[0] && bus_ticks[1])
	{
		float loaded = (float)bus_tick_us[1] / bus_ticks[1], unloaded = (float)bus_tick_us[0] / bus_ticks[0];
		float per_us = me_cycles_per_us > 0.0f ? me_cycles_per_us : 333.0f, during, alone, alone_other;
		int m;

		/* the ME alone, the main CPU asleep: in the memory this run loads
		first, then in the other, so every window also says how the eDRAM
		compares with main RAM for the same work */
		for (m = 0; m < 2; m++)
			if (bus_start(m ? !bus_edram : bus_edram))
			{
				sceKernelDelayThread(8000);
				bus_finish(&bus_alone_loads[m], &bus_alone_cycles[m]);
			}
		during = bus_load_cycles ? bus_loads * per_us / bus_load_cycles : 0.0f;
		alone = bus_alone_cycles[0] ? bus_alone_loads[0] * per_us / bus_alone_cycles[0] : 0.0f;
		alone_other = bus_alone_cycles[1] ? bus_alone_loads[1] * per_us / bus_alone_cycles[1] : 0.0f;
		platform_log("mebus: work %d%s | ticks loaded %lu at %.2f ms, unloaded %lu at %.2f ms: the main CPU %+.1f%% | the ME %.3f loads a us during them (busy %.0f%% of the loaded ticks), %.3f alone: %+.1f%% | alone in %s %.3f | taken over by other ring jobs %lu",
			bus_work, bus_edram ? " eDRAM" : "", bus_ticks[1], loaded / 1000.0f, bus_ticks[0], unloaded / 1000.0f,
			unloaded > 0.0f ? 100.0f * (loaded - unloaded) / unloaded : 0.0f,
			during, bus_tick_us[1] ? 100.0f * (bus_load_cycles / per_us) / bus_tick_us[1] : 0.0f,
			alone, alone > 0.0f ? 100.0f * (during - alone) / alone : 0.0f, bus_edram ? "main RAM" : "eDRAM", alone_other,
			bus_preempted);
		bus_ticks[0] = bus_ticks[1] = bus_tick_us[0] = bus_tick_us[1] = bus_preempted = 0;
		bus_loads = bus_load_cycles = 0;
		bus_alone_loads[0] = bus_alone_loads[1] = bus_alone_cycles[0] = bus_alone_cycles[1] = 0;
	}
}
#endif
