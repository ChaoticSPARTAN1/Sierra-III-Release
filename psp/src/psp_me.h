/* psp_me.h - jobs on the Media Engine (psp_me.c, build 87) */
#ifndef PSP_ME_H
#define PSP_ME_H

#define ME_SLOTS 3
#define ME_SLOT_AUDIO 0
#define ME_SLOT_RENDER 1
#define ME_SLOT_PROBE 2 /* build 115: the heartbeat's eDRAM probe (edramcheck) */

#define ME_JOB_ARGS 12
#define ME_JOB_RESULTS 8

#define ME_JOB_AUDIO 1
#define ME_JOB_CLASSIFY 2
#define ME_JOB_ANGLE 3
#define ME_JOB_GE 4 /* build 112: a run of the GE layer's waiting calls (psp_ge.c, "gemeon") */
#define ME_JOB_EDRAM 5 /* build 115: fill or check a stretch of the ME's eDRAM (edramcheck) */
/* 6: ME_JOB_SLANT, a ring job (below) */
#define ME_JOB_SOUNDCOPY 11 /* "soundedram": staged sound pieces copied into the sound cache in eDRAM (psp_dsound.c) */

int psp_me_start(void);
int psp_me_running(void);
void psp_me_stop(const char *why);
int psp_me_run(int which, unsigned int type, const unsigned int *args, int arg_count, unsigned int *results,
	unsigned int first_look_us);
void psp_me_handover(int which, unsigned long us);
void psp_me_report(void);
float psp_me_busy_percent(void);
void psp_me_edram_probe(void);
/* build 118: a job handed over without waiting, then finished (gemeoverlap) */
int psp_me_submit(int which, unsigned int type, const unsigned int *args, int arg_count);
int psp_me_finish(int which, unsigned int *results);
int psp_me_submitted_done(int which);

/* sleep and wake: the power callback stops the ME as the PSP sleeps and asks
for a restart once it is awake; with "mewake" the main thread's frame end
carries the restart out, and otherwise the ME stays stopped */
void psp_me_sleep_stop(void);
void psp_me_wake_request(void);
void psp_me_wake_poll(void);
/* "mewaketest": a sleep's hold on the ME, without a sleep */
void psp_me_wake_test_sleep(void);
/* the exit (HOME, the game's own): the ME stopped, and held in reset if it
may still be running, before the firmware takes the PSP back */
void psp_me_exit(const char *why);
/* how many times the ME has stopped (a failure, a sleep, the exit): whatever
it held in its eDRAM is taken as gone from the first stop on */
extern volatile unsigned int psp_me_stops;
/* 1 when the ME's jobs are simulated (the emulator: the main CPU runs them) */
int psp_me_simulated(void);

/* The job ring (ME phase 1, keep word "meslant" turns it on): jobs handed
to the ME without a slot of their own, collected later. Each job is a leaf:
everything it reads is named at the push and stays unchanged until the wait;
it writes only its output (whole 64-byte lines of its own) and its results.
The ring serves one producer thread, the main CPU's renderer. */
#define ME_RING_ENTRIES 8 /* a power of two */
#define ME_RING_ARGS 16
#define ME_RING_INPUTS 3
#define ME_RING_RESULTS 11
#define ME_JOB_SLANT 6 /* angle_sort's bucket loop over part of a draw (psp_ge.c) */
#define ME_JOB_RINGTEST 7 /* "meringtest": the ring's own check (psp_me_ring_test) */
#define ME_JOB_PERC_RAYS 8 /* "meraysperc": perception's queued rays (actor_perception.c, psp_fixups.py FIXES_122) */
#define ME_JOB_POSES 9 /* next tick's base poses (objects.c, psp_fixups.py FIXES_131 #poseahead) */
#define ME_JOB_POINTS 12 /* "mepoints": the frame's particles' and contrail points' physics (point_physics.c, psp_fixups.py FIXES_MEJOBS) */
#ifdef PSP_RECON
#define ME_JOB_BUSLOAD 10 /* "mebusN": a memory load on the ME through a tick (psp_me_bus_phase) */
/* the tick's start (phase 1) and end (phase 0), from psp_phase */
void psp_me_bus_phase(int phase);
#endif
/* an input of this many bytes, from NULL: the job may read anywhere in main
RAM (the level, the objects), so the main CPU writes back its whole cache
before the push (psp_me_writeback(NULL, ME_RING_ALL_OF_RAM)) and the ME drops
its whole cache before the job */
#define ME_RING_ALL_OF_RAM 0x01000000u
/* the last argument of a ring job, non-zero: an earlier job of the same
hand-over (pushed just before it, with nothing written by the main CPU in
between that this job reads) has already made this job's inputs fresh on the
ME, so the ME doesn't drop them again (psp_me_push_perc_rays) */
#define ME_RING_ARG_CONTINUES (ME_RING_ARGS - 1)

struct psp_me_job
{
	unsigned int type; /* ME_JOB_... */
	unsigned int args[ME_RING_ARGS];
	/* what the job reads from main RAM: the ME drops its own copies of these
	before the job. The main CPU writes back what it wrote (psp_me_writeback) */
	const void *input[ME_RING_INPUTS];
	unsigned int input_bytes[ME_RING_INPUTS];
	/* what the job writes besides its results: 64-byte aligned, whole lines,
	read by the main CPU only after the wait */
	void *output;
	unsigned int output_bytes;
};

/* The ME's eDRAM, as offsets from its start (0x80000000 to the ME, cached;
the main CPU can't reach it). docs/me_programme.md, stage 1 */
#define ME_EDRAM 0x80000000u
#define ME_EDRAM_WATCHED_END 0x30000u  /* 0-192 KB: left as found (edramcheck hashes it) */
#define ME_EDRAM_SCRATCH 0x30000u      /* 16 KB: mebusedram's stores */
#define ME_EDRAM_PRIVATE 0x34000u      /* 48 KB: the ME's own state (psp_me.c) */
#define ME_EDRAM_RESIDENT 0x40000u     /* to 1.5 MB: data jobs keep there */
#define ME_EDRAM_RESIDENT_END 0x180000u
#define ME_EDRAM_STACK_TOP 0x200000u   /* a PSP-1000's: the stack grows down from it */
/* "soundedram" (R2): the sound cache takes 256 KB to 1,920 KB instead, the
resident room and the top of the stack's half MB with it. On both PSPs the
ME's stack never reached below 1,984 KB (edramcheck over the ten-map ladder),
so 128 KB stays its, twice the reach measured */
#define ME_EDRAM_SOUND 0x40000u
#define ME_EDRAM_SOUND_END 0x1E0000u

/* On the ME, inside a long job: an audio block waiting is mixed now and,
from a slot job (not from inside a ring job), the ring's waiting jobs run.
Each touches only its own inputs and outputs, so this is exact anywhere a
job may call it. Elsewhere it does nothing */
void psp_me_yield(void);

int psp_me_ring_ready(void);
void psp_me_writeback(const void *start, unsigned int bytes);

/* Batches (docs/me_programme.md, stage 1): a client's records handed to the
ME a chunk at a time while the main CPU adds more, each made by the same
function on whichever CPU gets to it, under the main CPU's FPU control word.
The rule: psp_me_batch_need before the main CPU reads a record's answer. It
never waits for work the ME hasn't started: a record not handed over, or
handed over and not begun, is made here instead (taken back).

A record is record_bytes (whole 64-byte lines) in the client's arena; make()
reads and writes only its own record (and memory nobody writes meanwhile).
After psp_me_batch_add returns it, the client fills the record; it must not
write it again after the hand-over (the next add or psp_me_batch_close) until
psp_me_batch_need has returned for it. */
#define ME_JOB_BATCH 11
#define ME_BATCH_ARGS 8
#define ME_BATCH_TICKETS 16

typedef void (*psp_me_record_fn)(void *record, const unsigned int *args);

struct psp_me_batch_word
{
	unsigned int value;
	unsigned int pad[15];
} __attribute__((aligned(64)));

struct psp_me_batch
{
	/* only ever through the uncached view: limit is the main CPU's (the ME
	makes no record from it on), started and done the ME's (it claimed and
	finished every record below them), withdrawn the ME's count of jobs it
	ended at the limit */
	struct psp_me_batch_word limit, started, done, withdrawn;
	psp_me_record_fn make;
	unsigned char *records;
	unsigned char *state;       /* a byte a record, the main CPU's */
	unsigned int record_bytes, capacity, chunk;
	unsigned int args[ME_BATCH_ARGS];
	unsigned int added, handed, limit_now;
	unsigned int tickets[ME_BATCH_TICKETS], ticket_count;
	/* counts since the last report: made by the ME, made here (never handed,
	taken back, after a failure), waits for a record the ME was on */
	unsigned long by_me, by_main, taken, waits, refused;
};

/* records: capacity * record_bytes, 64-byte aligned; state: capacity bytes.
chunk: records a hand-over. Returns 0 if the shapes break the rules */
int psp_me_batch_init(struct psp_me_batch *b, psp_me_record_fn make, void *records, unsigned char *state,
	unsigned int record_bytes, unsigned int capacity, unsigned int chunk, const unsigned int *args);
/* a new pass: every record of the last one made (psp_me_batch_finish) */
void psp_me_batch_begin(struct psp_me_batch *b);
/* the next record to fill, its index in *index; NULL when full (the client
makes it itself) */
void *psp_me_batch_add(struct psp_me_batch *b, unsigned int *index);
/* what's added and not handed over goes now */
void psp_me_batch_close(struct psp_me_batch *b);
/* record index made and readable here: 1 by the ME, 0 made here */
int psp_me_batch_need(struct psp_me_batch *b, unsigned int index);
/* every record made, every job collected: the arena is the client's again */
void psp_me_batch_finish(struct psp_me_batch *b);
/* "mebatchtest": the batch's own check, once a frame */
void psp_me_batch_test(void);
int psp_me_push(const struct psp_me_job *job, unsigned int *ticket);
int psp_me_wait(unsigned int ticket, unsigned int *results);
/* a pushed job collected later: collect runs before the ring would reuse its entry */
void psp_me_hold(unsigned int ticket, void (*collect)(void));
void psp_me_ring_test(void);
/* a long ring job's step, on the ME: the debug stash notes it, and an audio
block waiting is mixed now (audio comes before every other job). Nothing
anywhere else */
void psp_me_ring_step(unsigned int step);

/* build 118: 1 on the Media Engine. me-core starts it on a stack in its own
eDRAM ($sp 0x80200000 or 0x80400000, me-core.h), and every thread of the main
CPU runs on a stack in main RAM, from 0x08000000 up. So code both CPUs run
(the GE layer's, psp_ge.c) can tell where it is without a shared word: a word
both CPUs write would share a cache line between them */
static inline int psp_on_me(void)
{
	unsigned int sp;

	__asm__ ("move %0, $sp" : "=r"(sp));
	return (sp & 0x1FFFFFFFu) < 0x01000000u;
}

/* build 118: a variable of the GE layer's or the texture cache's files that
the main CPU writes while the Media Engine works (gemeoverlap: the recorder's
words, the heartbeat's windows, the see-through level triangles' wall
answers). psp_build.py's link script puts these together on cache lines of
their own, away from the ME's variables of the same files. A static gets it
before its type: static PSP_MAIN_SIDE int x = 0; */
#define PSP_MAIN_SIDE __attribute__((section(".data.psp_main_side")))

/* work that is (or, in the emulator, stands for) the Media Engine's: on the
ME, or marked as its by psp_me_context (gemesim: the main CPU runs it) */
extern volatile unsigned int psp_me_context;
#define PSP_ME_WORK() (psp_me_context != 0 || psp_on_me())

#endif
