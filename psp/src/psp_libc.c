/*
PSP_LIBC.C

Small things the PSP's newlib lacks or the port's other platforms provide,
compiled as plain C (no game prefix):
- POSIX calls the Linux platform layer uses: pread/pwrite (the cache
  file thread reads maps with them), sysconf, pause, ftruncate, utimensat;
- the maths functions every port shares (port/include/halo_math.h). The PC
  ports compute them in double precision, bit-identical across machines
  for system link; the PSP computes them in single precision on its FPU
  (lever 1 in the feasibility notes), since it won't play against PCs;
- internet play (p2p.c; the campaign build only) and the Android GL host,
  which the PSP doesn't have: they report nothing.
*/

#include <errno.h>
#include <math.h>
#include <pthread.h>
#include <string.h>
#include <malloc.h>
#include <stdio.h>
#include <sys/types.h>
#include <unistd.h>
#include <pspkernel.h>
#include <pspsysmem.h>
#include <pspsdk.h>
#include <time.h>

/* ---------- POSIX */

static pthread_mutex_t positioned_io_lock = PTHREAD_MUTEX_INITIALIZER;

/* newlib has no pread/pwrite: seek and transfer under one lock, so two
threads' positioned transfers on the same file can't interleave */
ssize_t pread(int descriptor, void *buffer, size_t count, off_t offset)
{
	ssize_t result = -1;

	pthread_mutex_lock(&positioned_io_lock);
	if (lseek(descriptor, offset, SEEK_SET) == offset)
		result = read(descriptor, buffer, count);
	pthread_mutex_unlock(&positioned_io_lock);
	return result;
}

ssize_t pwrite(int descriptor, const void *buffer, size_t count, off_t offset)
{
	ssize_t result = -1;

	pthread_mutex_lock(&positioned_io_lock);
	if (lseek(descriptor, offset, SEEK_SET) == offset)
		result = write(descriptor, buffer, count);
	pthread_mutex_unlock(&positioned_io_lock);
	return result;
}

/* extends a file (the game sizes its save file this way, game_state_xbox.c)
by writing its last byte; newlib can't shrink one, so a shorter length is
left as it is */
int ftruncate(int descriptor, off_t length)
{
	off_t end, position;
	int result = 0;

	pthread_mutex_lock(&positioned_io_lock);
	position = lseek(descriptor, 0, SEEK_CUR);
	end = lseek(descriptor, 0, SEEK_END);
	if (end < 0)
		result = -1;
	else if (length > end)
	{
		char zero = 0;

		if (lseek(descriptor, length - 1, SEEK_SET) != length - 1 || write(descriptor, &zero, 1) != 1)
			result = -1;
	}
	lseek(descriptor, position, SEEK_SET);
	pthread_mutex_unlock(&positioned_io_lock);
	return result;
}

struct timespec;
int utimensat(int directory, const char *path, const struct timespec times[2], int flags)
{
	(void)directory; (void)path; (void)times; (void)flags;
	return 0;
}

int pause(void)
{
	sceKernelSleepThread();
	return -1;
}

long sysconf(int name)
{
	switch (name)
	{
	case _SC_PAGESIZE: return 4096;
	case _SC_PHYS_PAGES: return 24 * 1024 * 1024 / 4096;
	case _SC_AVPHYS_PAGES: return (long)sceKernelTotalFreeMemSize() / 4096;
	default: errno = EINVAL; return -1;
	}
}

void _ReadWriteBarrier(void)
{
	__asm__ __volatile__("" ::: "memory");
}

/* ---------- build 106: condition variables that sleep

pspdev's pthreads (pthreads-embedded) waits on a condition variable by
polling: a look at its semaphore, then sceKernelDelayThread(100), again and
again. So every thread waiting on one (the Xbox shim's events: the cache
file thread between reads, the input thread...) wakes up to 10,000 times a
second. The emulator counted 4,000 wakes a second for the cache file
thread, 5% of the CPU. Below the game thread that only used the time the
game left; the cache file thread now runs above it (psp_platform.c,
psp_cache_thread_priority), where every wake would take the CPU from the
game. These replace pthread_cond_* (psp_build.py links with --wrap): a
count of the waiters and a kernel semaphore they sleep on.

pthread_cond_t is a pointer here; PTHREAD_COND_INITIALIZER is (pointer) -1,
made into a condition at its first use. POSIX lets a waiter wake without a
signal (every caller loops on its own test), which keeps the timed-out case
simple */
struct psp_cond
{
	SceUID sleep;   /* the waiters sleep on it; a signal adds one */
	SceUID guard;   /* a binary semaphore over the count */
	int waiters;    /* waiting and not yet signalled */
};
#define PSP_COND_STATIC ((pthread_cond_t)-1)

static struct psp_cond *cond_new(void)
{
	struct psp_cond *cond = malloc(sizeof(*cond));

	if (!cond)
		return NULL;
	cond->sleep = sceKernelCreateSema("halo cond", 0, 0, 0x7FFFFFFF, NULL);
	cond->guard = sceKernelCreateSema("halo cond guard", 0, 1, 1, NULL);
	cond->waiters = 0;
	if (cond->sleep < 0 || cond->guard < 0)
	{
		if (cond->sleep >= 0)
			sceKernelDeleteSema(cond->sleep);
		if (cond->guard >= 0)
			sceKernelDeleteSema(cond->guard);
		free(cond);
		return NULL;
	}
	return cond;
}

static void cond_free(struct psp_cond *cond)
{
	sceKernelDeleteSema(cond->sleep);
	sceKernelDeleteSema(cond->guard);
	free(cond);
}

/* a statically made condition becomes a real one at its first use; two
threads racing for that make one each and one keeps its own */
static struct psp_cond *cond_get(pthread_cond_t *handle)
{
	struct psp_cond *made;
	int interrupts;

	if (!handle)
		return NULL;
	if (*handle != PSP_COND_STATIC)
		return (struct psp_cond *)*handle;
	made = cond_new();
	if (!made)
		return NULL;
	interrupts = pspSdkDisableInterrupts();
	if (*handle == PSP_COND_STATIC)
	{
		*handle = (pthread_cond_t)made;
		made = NULL;
	}
	pspSdkEnableInterrupts(interrupts);
	if (made)
		cond_free(made);
	return (struct psp_cond *)*handle;
}

int __wrap_pthread_cond_init(pthread_cond_t *handle, const pthread_condattr_t *attributes)
{
	struct psp_cond *cond = cond_new();

	(void)attributes;
	if (!handle)
		return EINVAL;
	if (!cond)
		return ENOMEM;
	*handle = (pthread_cond_t)cond;
	return 0;
}

int __wrap_pthread_cond_destroy(pthread_cond_t *handle)
{
	if (!handle)
		return EINVAL;
	if (*handle != PSP_COND_STATIC && *handle)
		cond_free((struct psp_cond *)*handle);
	*handle = NULL;
	return 0;
}

static int cond_wait(pthread_cond_t *handle, pthread_mutex_t *mutex, const struct timespec *deadline)
{
	struct psp_cond *cond = cond_get(handle);
	int slept;

	if (!cond)
		return EINVAL;
	sceKernelWaitSema(cond->guard, 1, NULL);
	cond->waiters++;
	sceKernelSignalSema(cond->guard, 1);
	pthread_mutex_unlock(mutex);
	if (deadline)
	{
		struct timespec now;
		long long left;
		SceUInt timeout;

		clock_gettime(CLOCK_REALTIME, &now);
		left = (long long)(deadline->tv_sec - now.tv_sec) * 1000000LL + (deadline->tv_nsec - now.tv_nsec) / 1000;
		timeout = left <= 0 ? 0 : left > 0x7FFFFFFFLL ? 0x7FFFFFFF : (SceUInt)left;
		slept = sceKernelWaitSema(cond->sleep, 1, &timeout);
	}
	else
		slept = sceKernelWaitSema(cond->sleep, 1, NULL);
	if (slept < 0)
	{
		/* timed out: out of the count, unless a signal came meanwhile and
		left its wake in the semaphore (taken here: this waiter was the
		one it woke) */
		sceKernelWaitSema(cond->guard, 1, NULL);
		if (sceKernelPollSema(cond->sleep, 1) >= 0)
			slept = 0;
		else if (cond->waiters > 0)
			cond->waiters--;
		sceKernelSignalSema(cond->guard, 1);
	}
	pthread_mutex_lock(mutex);
	return slept < 0 ? ETIMEDOUT : 0;
}

int __wrap_pthread_cond_wait(pthread_cond_t *handle, pthread_mutex_t *mutex)
{
	return cond_wait(handle, mutex, NULL);
}

int __wrap_pthread_cond_timedwait(pthread_cond_t *handle, pthread_mutex_t *mutex, const struct timespec *deadline)
{
	return cond_wait(handle, mutex, deadline);
}

int __wrap_pthread_cond_signal(pthread_cond_t *handle)
{
	struct psp_cond *cond = cond_get(handle);

	if (!cond)
		return EINVAL;
	sceKernelWaitSema(cond->guard, 1, NULL);
	if (cond->waiters > 0)
	{
		cond->waiters--;
		sceKernelSignalSema(cond->sleep, 1);
	}
	sceKernelSignalSema(cond->guard, 1);
	return 0;
}

int __wrap_pthread_cond_broadcast(pthread_cond_t *handle)
{
	struct psp_cond *cond = cond_get(handle);

	if (!cond)
		return EINVAL;
	sceKernelWaitSema(cond->guard, 1, NULL);
	if (cond->waiters > 0)
	{
		sceKernelSignalSema(cond->sleep, cond->waiters);
		cond->waiters = 0;
	}
	sceKernelSignalSema(cond->guard, 1);
	return 0;
}

/* ---------- maths, in single precision */

double halo_sin(double x) { return sinf((float)x); }
double halo_cos(double x) { return cosf((float)x); }
double halo_tan(double x) { return tanf((float)x); }
double halo_asin(double x) { return asinf((float)x); }
double halo_acos(double x) { return acosf((float)x); }
double halo_atan(double x) { return atanf((float)x); }
double halo_atan2(double y, double x) { return atan2f((float)y, (float)x); }
double halo_exp(double x) { return expf((float)x); }
double halo_log(double x) { return logf((float)x); }
double halo_log2(double x) { return log2f((float)x); }
double halo_log10(double x) { return log10f((float)x); }
double halo_pow(double x, double y) { return powf((float)x, (float)y); }

/* ---------- no internet play (p2p.c): the campaign build. The multiplayer
build has the engine's p2p layer itself (psp_build.py's INTERNET_UNITS) */

#ifndef PSP_MULTIPLAYER

const unsigned char *p2p_identifier(void) { static unsigned char identifier[16]; return identifier; }
void p2p_hardware_id(char *hex, int size) { if (size > 0) { strncpy(hex, "5053500000000000", size - 1); hex[size - 1] = 0; } }
void p2p_hardware_id_sanitize(char *destination, int size, const char *source)
{ if (size > 0) { strncpy(destination, source ? source : "", size - 1); destination[size - 1] = 0; } }
void p2p_discord_identity(char *id, int id_size, char *name, int name_size)
{ if (id_size > 0) id[0] = 0; if (name_size > 0) name[0] = 0; }
void p2p_discord_sanitize(char *destination, int size, const char *source, int name)
{ (void)name; p2p_hardware_id_sanitize(destination, size, source); }
unsigned long p2p_peer_endpoint_address(unsigned long address) { return address; }
int p2p_outgoing(int stream, int socket, unsigned long *address, unsigned short *port)
{ (void)stream; (void)socket; (void)address; (void)port; return 0; }
int p2p_incoming(int stream, unsigned long *address, unsigned short *port) { (void)stream; (void)address; (void)port; return 0; }
void p2p_socket_port(int socket, int stream, int listening, unsigned short port) { (void)socket; (void)stream; (void)listening; (void)port; }
void p2p_port_taken(int stream, unsigned short port) { (void)stream; (void)port; }
void p2p_socket_closed(int socket, unsigned short port) { (void)socket; (void)port; }
int p2p_broadcast_targets(unsigned short port, unsigned long *addresses, unsigned short *ports, int maximum)
{ (void)port; (void)addresses; (void)ports; (void)maximum; return 0; }
int p2p_send_datagram(unsigned short source_port, unsigned long address, unsigned short port, const void *data, int size)
{ (void)source_port; (void)address; (void)port; (void)data; (void)size; return 0; }
int p2p_broadcast_datagram(unsigned short source_port, unsigned short port, const void *data, int size)
{ (void)source_port; (void)port; (void)data; (void)size; return 0; }
int p2p_peer_address(const unsigned char *peer, unsigned long *address) { (void)peer; (void)address; return 0; }
void p2p_set_game_player_counts(int count, int maximum) { (void)count; (void)maximum; }
const char *p2p_take_clipboard_text(void) { return NULL; }
int p2p_join_invite(const char *text) { (void)text; return 0; }
int p2p_hand_off_invite(void) { return 0; }
void p2p_initialize(unsigned long local_address) { (void)local_address; }
/* (ChupathingyCE's: whether a hosted game is offered on the internet) */
void p2p_set_hosting_allowed(int allowed) { (void)allowed; }
void p2p_set_hosting_public(int public) { (void)public; }
/* (ChupathingyCE's: what a hosted game shows in an invite's listing) */
void p2p_set_game_listing(const char *name, const char *map, const char *gametype, int engine_type, int open,
	int in_progress, int has_teams)
{ (void)name; (void)map; (void)gametype; (void)engine_type; (void)open; (void)in_progress; (void)has_teams; }
#endif

/* ---------- no Android GL host */

int host_gl_has_extension(const char *name) { (void)name; return 0; }
unsigned int host_gl_read_buffer_word(unsigned int buffer, unsigned int offset) { (void)buffer; (void)offset; return 0; }
void host_gl_buffer_write(unsigned int target, unsigned int offset, unsigned int size, const void *data)
{ (void)target; (void)offset; (void)size; (void)data; }
void host_gl_fence_frame(unsigned int slot) { (void)slot; }
void host_gl_wait_frame(unsigned int slot) { (void)slot; }
/* (ChupathingyCE's visibility counters: none counted, so none read) */
void host_gl_read_buffer(unsigned int buffer, unsigned int offset, unsigned int size, void *data)
{ (void)buffer; (void)offset; memset(data, 0, size); }

/* ---------- no TIFF screenshots */

void *TIFFOpen(const char *name, const char *mode) { (void)name; (void)mode; return NULL; }
void TIFFClose(void *tiff) { (void)tiff; }
int TIFFSetField(void *tiff, unsigned int tag, ...) { (void)tiff; (void)tag; return 0; }
int TIFFWriteScanline(void *tiff, void *buffer, unsigned int row, unsigned short sample)
{ (void)tiff; (void)buffer; (void)row; (void)sample; return -1; }

/* ---------- build 105: one thread at a time in newlib's number formatting

pspdev's newlib keeps one shared state for every thread (a single global
_impure_ptr, no per-thread one). Its float-to-text converter (_dtoa_r, under
every %f) keeps scratch memory there and returns a string that the next call
frees. So two threads formatting floats at once corrupt each other: the
heartbeat (priority 0x18) can interrupt the game thread (0x20) in the middle
of a line with %f and format its own. The emulator hit it with two copies
running side by side (a crash in _dtoa_r reading 0x00333033, text used as a
pointer); on the PSP it would be rare and random. The printf engines and
strtod (the same scratch memory) are wrapped (psp_build.py) to take one
lock. It is recursive for the thread that holds it, and after a crash it is
skipped: the crashed thread may hold it, and the crash handler formats text */

#include <stdarg.h>
#include <reent.h>

extern volatile int psp_crashed;
static SceUID format_lock = -1;
static volatile SceUID format_owner = 0;
static volatile int format_depth = 0;

/* called first thing at start-up, while only one thread runs (psp_platform.c) */
void psp_format_lock_init(void)
{
	if (format_lock < 0)
		format_lock = sceKernelCreateSema("halo format", 0, 1, 1, NULL);
}

static int format_take(void)
{
	SceUID self;

	if (format_lock < 0 || psp_crashed)
		return 0;
	self = sceKernelGetThreadId();
	if (format_owner == self)
	{
		format_depth++;
		return 1;
	}
	if (sceKernelWaitSema(format_lock, 1, NULL) < 0)
		return 0;
	format_owner = self;
	format_depth = 1;
	return 1;
}

static void format_give(int taken)
{
	if (!taken)
		return;
	if (--format_depth == 0)
	{
		format_owner = 0;
		sceKernelSignalSema(format_lock, 1);
	}
}

/* build 106: "printfcount" (keep.txt): which format strings the formatting
goes to, the busiest logged by psp_printf_report (the emulator's PC samples
put 4% of b30's tick in the lock taken here) */
#define PRINTF_FORMATS 64
static struct { const char *format; unsigned long calls; } printf_formats[PRINTF_FORMATS];
static int printf_count_on = -1;

static void printf_count(const char *format)
{
	extern int psp_keep(const char *name);
	extern int psp_keep_loaded(void);
	unsigned int slot = ((unsigned int)format >> 2) % PRINTF_FORMATS, probes;

	if (printf_count_on < 0)
	{
		if (!psp_keep_loaded())
			return;
		/* psp_keep formats too: off while it looks */
		printf_count_on = 0;
		printf_count_on = psp_keep("printfcount");
	}
	if (!printf_count_on)
		return;
	for (probes = 0; probes < PRINTF_FORMATS; probes++, slot = (slot + 1) % PRINTF_FORMATS)
		if (!printf_formats[slot].format || printf_formats[slot].format == format)
		{
			printf_formats[slot].format = format;
			printf_formats[slot].calls++;
			return;
		}
}

/* the five busiest formats since the last report, into the log */
void psp_printf_report(void)
{
	extern void platform_log(const char *format, ...);
	int pass, k;

	if (printf_count_on <= 0)
		return;
	for (pass = 0; pass < 5; pass++)
	{
		int best = -1;

		for (k = 0; k < PRINTF_FORMATS; k++)
			if (printf_formats[k].format && printf_formats[k].calls && (best < 0 || printf_formats[k].calls > printf_formats[best].calls))
				best = k;
		if (best < 0)
			break;
		{
			char shown[48];
			int c;

			for (c = 0; c < 47 && printf_formats[best].format[c]; c++)
				shown[c] = printf_formats[best].format[c] < ' ' ? ' ' : printf_formats[best].format[c];
			shown[c] = 0;
			platform_log("printf: %lu calls with \"%s\"", printf_formats[best].calls, shown);
		}
		printf_formats[best].calls = 0;
	}
	for (k = 0; k < PRINTF_FORMATS; k++)
		printf_formats[k].calls = 0;
}

int __real__svfprintf_r(struct _reent *reent, FILE *file, const char *format, va_list arguments);
int __wrap__svfprintf_r(struct _reent *reent, FILE *file, const char *format, va_list arguments)
{
	int taken = format_take(), result;

	printf_count(format);
	result = __real__svfprintf_r(reent, file, format, arguments);

	format_give(taken);
	return result;
}

int __real__vfprintf_r(struct _reent *reent, FILE *file, const char *format, va_list arguments);
int __wrap__vfprintf_r(struct _reent *reent, FILE *file, const char *format, va_list arguments)
{
	int taken = format_take(), result = __real__vfprintf_r(reent, file, format, arguments);

	format_give(taken);
	return result;
}

/* vfprintf, strtod and strtof live in the same objects as the engines they
call, so those calls never pass through the wrappers above: the entry points
themselves are wrapped too (the other printf and scanf calls reach an engine
from another object, and are covered) */
int __real_vfprintf(FILE *file, const char *format, va_list arguments);
int __wrap_vfprintf(FILE *file, const char *format, va_list arguments)
{
	int taken = format_take(), result = __real_vfprintf(file, format, arguments);

	format_give(taken);
	return result;
}

double __real_strtod(const char *text, char **end);
double __wrap_strtod(const char *text, char **end)
{
	int taken = format_take();
	double result = __real_strtod(text, end);

	format_give(taken);
	return result;
}

float __real_strtof(const char *text, char **end);
float __wrap_strtof(const char *text, char **end)
{
	int taken = format_take();
	float result = __real_strtof(text, end);

	format_give(taken);
	return result;
}

double __real__strtod_r(struct _reent *reent, const char *text, char **end);
double __wrap__strtod_r(struct _reent *reent, const char *text, char **end)
{
	int taken = format_take();
	double result = __real__strtod_r(reent, text, end);

	format_give(taken);
	return result;
}

/* ---------- heap accounting (linked with --wrap=malloc,calloc,realloc,memalign):
large requests and every failure are logged, so the log says what the heap
went to */

void *__real_malloc(size_t size);
void *__real_calloc(size_t count, size_t size);
void *__real_realloc(void *pointer, size_t size);
void *__real_memalign(size_t alignment, size_t size);
void platform_log(const char *format, ...);

static unsigned long heap_large_total = 0;

/* build 93: who asks for the heap (bytes asked for since start, by the
calling address; frees aren't counted), reported at the first failure and
on psp_heap_report(): psp-addr2line -f -e halo.elf turns the addresses into
functions */
#define HEAP_CALLERS 96
static struct { void *caller; unsigned long bytes, count; } heap_callers[HEAP_CALLERS];
static int heap_caller_count = 0, heap_failed_reported = 0;

static void heap_caller_add(void *caller, size_t size)
{
	int k;

	for (k = 0; k < heap_caller_count; k++)
		if (heap_callers[k].caller == caller)
			break;
	if (k == heap_caller_count)
	{
		if (heap_caller_count >= HEAP_CALLERS)
			return;
		heap_callers[heap_caller_count].caller = caller;
		heap_caller_count++;
	}
	heap_callers[k].bytes += size;
	heap_callers[k].count++;
}

void psp_heap_report(const char *why)
{
	char line[480];
	int length = 0, shown = 0, k, best;
	static unsigned char done[HEAP_CALLERS];
	struct mallinfo info = mallinfo();

	platform_log("heap (%s): %d KB in use, %d KB free inside, %d KB taken from the system; largest callers (bytes asked since start):",
		why, info.uordblks / 1024, info.fordblks / 1024, info.arena / 1024);
	memset(done, 0, sizeof(done));
	while (shown < 16)
	{
		best = -1;
		for (k = 0; k < heap_caller_count; k++)
			if (!done[k] && (best < 0 || heap_callers[k].bytes > heap_callers[best].bytes))
				best = k;
		if (best < 0)
			break;
		done[best] = 1;
		length += snprintf(line + length, sizeof(line) - length, " %p %lu KB (%lu);", heap_callers[best].caller,
			heap_callers[best].bytes / 1024, heap_callers[best].count);
		if (++shown % 8 == 0)
		{
			platform_log("heap callers:%s", line);
			length = 0;
			line[0] = 0;
		}
	}
	if (length)
		platform_log("heap callers:%s", line);
}

/* build 105: failures are counted for the overlay's warning (psp_debug.c)
and logged one by one only for the first 64; b104 run 1 wrote 15,128 such
lines, which the log then had to carry. The heartbeat's heap line has the
running count */
volatile unsigned long psp_heap_failures = 0;
#define HEAP_FAILURES_LOGGED 64

/* requests asked through psp_heap_try_memalign whose callers have somewhere
else to go (psp_ge.c's level copies wait for the carve): a refusal there is
counted apart, so the overlay's HEAP FULL means a request nothing caught */
volatile unsigned long psp_heap_refusals = 0;

static void heap_map_tag(void *pointer, size_t size, const char *kind, void *caller); /* below */
void psp_heap_map(const char *label); /* below */
static struct heap_map_space *heap_map; /* below */

static void *heap_note(void *result, size_t size, const char *kind, void *caller)
{
	if (result)
		heap_caller_add(caller, size);
	if (!result && !heap_failed_reported)
	{
		heap_failed_reported = 1;
		psp_heap_report("first failure");
	}
	if (!result)
	{
		if (++psp_heap_failures <= HEAP_FAILURES_LOGGED)
			platform_log("heap: %s of %u bytes FAILED for %p (large requests so far %lu KB)%s", kind, (unsigned)size, caller,
				heap_large_total / 1024, psp_heap_failures == HEAP_FAILURES_LOGGED ? "; further failures are only counted" : "");
		/* the map at the moment a large request finds no room: what took the
		space the level change left */
		if (heap_map && size >= 0x4000 && psp_heap_failures <= HEAP_FAILURES_LOGGED)
			psp_heap_map("a failed request");
	}
	else if (size >= 0x4000)
	{
		if (heap_map)
			heap_map_tag(result, size, kind, caller);
		if (size >= 0x10000)
		{
			heap_large_total += size;
			platform_log("heap: %s %u KB from %p (large requests so far %lu KB)", kind, (unsigned)(size / 1024),
				caller, heap_large_total / 1024);
		}
	}
	return result;
}

/* build 105: for the heartbeat. Free space inside the heap split into many
small pieces (allocations made and freed every frame) can leave a large
request failing though plenty is free in all: the pieces' count says
whether that is happening */
void psp_heap_line(char *line, int size)
{
	struct mallinfo info = mallinfo();

	snprintf(line, size, "heap: %d KB in use, %d KB free inside in %d pieces, %d KB taken from the system; %lu requests failed, %lu refused with a fallback",
		info.uordblks / 1024, info.fordblks / 1024, info.ordblks, info.arena / 1024, psp_heap_failures, psp_heap_refusals);
}

/* work marked as the Media Engine's in the emulator (gemesim's psp_me_context)
that asks for the heap: the heap's lock is a system call, which the ME can't
make. Each caller is named once, so it can be moved to the main CPU */
extern volatile unsigned int psp_me_context;
#define HEAP_ME_CALLERS 16
static void *heap_me_callers[HEAP_ME_CALLERS];

static void heap_me_check(void *caller, size_t size, const char *how)
{
	int k;

	unsigned int sp;

	__asm__ ("move %0, $sp" : "=r"(sp));
	if (!psp_me_context || (sp & 0x1FFFFFFFu) < 0x01000000u) /* not on the ME itself: no log from there */
		return;
	/* the context names the thread doing the ME's work on the main CPU; a
	thread that took the CPU from it meanwhile (the cache file reader) asks
	for the heap as its own */
	if ((unsigned int)sceKernelGetThreadId() != psp_me_context)
		return;
	for (k = 0; k < HEAP_ME_CALLERS && heap_me_callers[k]; k++)
		if (heap_me_callers[k] == caller)
			return;
	if (k == HEAP_ME_CALLERS)
		return;
	heap_me_callers[k] = caller;
	{
		extern int psp_me_record_kind; /* psp_ge.c: the call being carried out */

		platform_log("heap: %s of %u bytes in work meant for the Media Engine, from %p (GE layer call kind %d)",
			how, (unsigned int)size, caller, psp_me_record_kind);
	}
}

void *__wrap_malloc(size_t size) { heap_me_check(__builtin_return_address(0), size, "malloc"); return heap_note(__real_malloc(size), size, "malloc", __builtin_return_address(0)); }
void *__wrap_calloc(size_t count, size_t size) { heap_me_check(__builtin_return_address(0), count * size, "calloc"); return heap_note(__real_calloc(count, size), count * size, "calloc", __builtin_return_address(0)); }
void *__wrap_realloc(void *pointer, size_t size) { heap_me_check(__builtin_return_address(0), size, "realloc"); return heap_note(__real_realloc(pointer, size), size, "realloc", __builtin_return_address(0)); }
void *__wrap_memalign(size_t alignment, size_t size) { heap_me_check(__builtin_return_address(0), size, "memalign"); return heap_note(__real_memalign(alignment, size), size, "memalign", __builtin_return_address(0)); }

/* memalign for a caller that copes with a refusal. A success is booked as
memalign's (callers, the map, the large total); a refusal only adds to
psp_heap_refusals. "heapcountall" books refusals as failures again, as
before this existed */
static int heap_count_all = -1;

void *psp_heap_try_memalign(size_t alignment, size_t size)
{
	extern int psp_keep(const char *name);
	extern int psp_keep_loaded(void);
	void *caller = __builtin_return_address(0);
	void *result;

	heap_me_check(caller, size, "memalign");
	result = __real_memalign(alignment, size);
	if (heap_count_all < 0 && psp_keep_loaded())
		heap_count_all = psp_keep("heapcountall");
	if (result || heap_count_all > 0)
		return heap_note(result, size, "memalign", caller);
	psp_heap_refusals++;
	return NULL;
}

/* ---------- the heap's map ("heapmap", keep.txt)

A large request can fail with plenty free inside the heap when no single gap
is large enough. The map says what splits it: when a level change starts
(the old level still in) and once the new level has loaded and settled
(psp_loading.c), newlib's chunks are walked in address order and every gap
and every block of 16 KB or more is logged, at its offset in KB from the
heap's start, with what asked for each block. The engine's blocks
(debug_malloc, behind GlobalAlloc's 16-byte header) carry their own file and
line; the rest are named by the caller recorded as they are made, which
psp-addr2line -f -e halo.elf turns into a function. Newlib's allocator
(dlmalloc 2.6) merges a freed chunk with its neighbours at once, so a chunk
not marked in use is free space as the allocator sees it.

Off, it costs one test on requests of 16 KB or more: its tables come from
the heap only when the word is set */

/* newlib's chunk header (mallocr.c): the size's low bit says whether the
chunk before this one is in use; chunks are 16-byte aligned past it */
struct heap_chunk
{
	unsigned long previous_size, size;
};
#define HEAP_CHUNK_SIZE(chunk) ((chunk)->size & ~3ul)
#define HEAP_CHUNK_AFTER(chunk) ((struct heap_chunk *)((char *)(chunk) + HEAP_CHUNK_SIZE(chunk)))
extern char *__malloc_sbrk_base;
extern struct heap_chunk *__malloc_av_[]; /* [2]: the top chunk */
void __malloc_lock(struct _reent *reent);
void __malloc_unlock(struct _reent *reent);
extern int sce_newlib_heap_kb_size; /* PSP_HEAP_SIZE_KB (psp_platform.c) */

/* the heap's room in KB: free inside it plus what the system hasn't handed
over yet. Walks newlib's free bins, so for rare callers only */
unsigned long psp_heap_room_kb(void)
{
	struct mallinfo info = mallinfo();
	unsigned long untaken = 0;
	char *taken = sbrk(0);

	if (__malloc_sbrk_base != (char *)-1 && taken != (char *)-1)
	{
		char *end = __malloc_sbrk_base + (unsigned long)sce_newlib_heap_kb_size * 1024;

		if (taken < end)
			untaken = end - taken;
	}
	return ((unsigned long)info.fordblks + untaken) / 1024;
}


#define HEAP_MAP_LARGE 0x4000
#define HEAP_MAP_TAGS 256
#define HEAP_MAP_ENTRIES 384
#define HEAP_MAP_BETWEEN 8
/* past a chunk's request: its size field, the rounding to 16 and what
memalign leaves rather than split off */
#define HEAP_MAP_SLACK (4 + 15 + 32)

struct heap_map_entry
{
	unsigned long offset, size;
	const char *file;   /* the engine's record of the block, or NULL */
	unsigned long tag;  /* the engine's line, or the caller's address */
	char kind;          /* 'g' a gap, 't' the top gap, 'd' the engine's, 'm' 'c' 'r' 'a' by request, '?' */
};

struct heap_map_space
{
	struct { void *pointer, *caller; unsigned long size; char kind; } tags[HEAP_MAP_TAGS];
	unsigned int next_tag;
	struct heap_map_entry entries[HEAP_MAP_ENTRIES];
	struct heap_map_entry between[HEAP_MAP_BETWEEN];
};

static int heap_map_fits(unsigned long chunk_size, unsigned long asked)
{
	return asked + 4 <= chunk_size && chunk_size <= asked + HEAP_MAP_SLACK;
}

/* whether a recorded block still looks in use at its size (read without
the allocator's lock: a hint for which entry to give up, no more) */
static int heap_map_tag_live(void *pointer, unsigned long asked)
{
	struct heap_chunk *chunk = (struct heap_chunk *)((char *)pointer - 8);

	if (__malloc_sbrk_base == (char *)-1 || (char *)chunk < __malloc_sbrk_base || chunk >= __malloc_av_[2])
		return 0;
	return heap_map_fits(HEAP_CHUNK_SIZE(chunk), asked) && (HEAP_CHUNK_AFTER(chunk)->size & 1);
}

/* frees aren't seen here, so an entry can outlive its block: the map trusts
one only where it finds a block of that size at that address. When the table
is full, the oldest entry whose block is gone gives way */
static void heap_map_tag(void *pointer, size_t size, const char *kind, void *caller)
{
	struct heap_map_space *space = heap_map;
	unsigned int k, slot = HEAP_MAP_TAGS;
	int interrupts = pspSdkDisableInterrupts();

	for (k = 0; k < HEAP_MAP_TAGS; k++)
		if (space->tags[k].pointer == pointer)
		{
			slot = k;
			break;
		}
	for (k = 0; slot == HEAP_MAP_TAGS && k < HEAP_MAP_TAGS; k++)
	{
		unsigned int at = (space->next_tag + k) % HEAP_MAP_TAGS;

		if (!space->tags[at].pointer || !heap_map_tag_live(space->tags[at].pointer, space->tags[at].size))
			slot = at;
	}
	if (slot == HEAP_MAP_TAGS)
		slot = space->next_tag;
	space->next_tag = (slot + 1) % HEAP_MAP_TAGS;
	space->tags[slot].pointer = pointer;
	space->tags[slot].caller = caller;
	space->tags[slot].size = size;
	space->tags[slot].kind = kind[1] == 'e' ? 'a' : kind[0];
	pspSdkEnableInterrupts(interrupts);
}

/* a source file's name, as the engine recorded it: in the program's image,
which ends where the heap begins */
static int heap_map_text(const char *text, const char *heap)
{
	return (unsigned long)text >= 0x08800000ul && text < heap && text[0] >= ' ' && text[0] < 127;
}

static void heap_map_name(struct heap_map_space *space, struct heap_chunk *chunk, unsigned long size,
	struct heap_map_entry *entry)
{
	const unsigned long *words = (const unsigned long *)(chunk + 1);
	int k;

	entry->file = NULL;
	entry->tag = 0;
	entry->kind = '?';
	/* GlobalAlloc's 16 bytes, then debug_malloc's header: signature, two
	links, size, file, line (debug_memory.c) */
	if (size >= 8 + 16 + 32 + 4 && words[4] == 0x2D2D2D3Eul && words[7] + 16 + 32 + 4 + 4 <= size &&
		heap_map_text((const char *)words[8], __malloc_sbrk_base))
	{
		const char *file = (const char *)words[8], *c;

		for (c = file; *c && c - file < 160; c++)
			if (*c == '\\' || *c == '/')
				file = c + 1;
		entry->file = file;
		entry->tag = words[9];
		entry->kind = 'd';
		return;
	}
	for (k = 0; k < HEAP_MAP_TAGS; k++)
		if (space->tags[k].pointer == (const void *)words && heap_map_fits(size, space->tags[k].size))
		{
			entry->tag = (unsigned long)space->tags[k].caller;
			entry->kind = space->tags[k].kind;
			return;
		}
}

static int heap_map_text_of(char *text, int size, const struct heap_map_entry *entry)
{
	switch (entry->kind)
	{
	case 'g': return snprintf(text, size, " +%luK gap %luK;", entry->offset / 1024, entry->size / 1024);
	case 't': return snprintf(text, size, " +%luK top gap %luK;", entry->offset / 1024, entry->size / 1024);
	case 'd': return snprintf(text, size, " +%luK %luK %.24s:%lu;", entry->offset / 1024, entry->size / 1024, entry->file, entry->tag);
	case '?': return snprintf(text, size, " +%luK %luK ?;", entry->offset / 1024, entry->size / 1024);
	default: return snprintf(text, size, " +%luK %luK %c@%08lx;", entry->offset / 1024, entry->size / 1024, entry->kind, entry->tag);
	}
}

/* entries onto lines under the log's 512 bytes */
static void heap_map_lines(const char *label, const char *heading, const struct heap_map_entry *entries, int count)
{
	char line[480], text[80];
	int length = 0, k;

	for (k = 0; k < count; k++)
	{
		int added = heap_map_text_of(text, sizeof(text), &entries[k]);

		if (length && length + added >= (int)sizeof(line) - 64)
		{
			platform_log("heapmap (%s)%s:%s", label, heading, line);
			length = 0;
		}
		memcpy(line + length, text, added + 1);
		length += added;
	}
	if (length)
		platform_log("heapmap (%s)%s:%s", label, heading, line);
}

/* after keep.txt is read (psp_platform.c) */
void psp_heap_map_start(void)
{
	extern int psp_keep(const char *name);
	struct heap_map_space *space;

	if (heap_map || !psp_keep("heapmap"))
		return;
	space = __real_malloc(sizeof(*space));
	if (!space)
		return;
	memset(space, 0, sizeof(*space));
	heap_map = space;
	platform_log("heapmap: on (%u bytes of tables from the heap)", (unsigned)sizeof(*space));
}

void psp_heap_map(const char *label)
{
	struct heap_map_space *space = heap_map;
	struct heap_chunk *chunk, *first, *top;
	char *base, *end, *taken;
	unsigned long free_total = 0, used_total = 0, top_size = 0, untaken = 0, between_bytes = 0, stopped_size = 0;
	unsigned long gap_offset[2] = { 0, 0 }, gap_size[2] = { 0, 0 }, low, high, stopped_at = ~0ul;
	int gaps = 0, used = 0, entries = 0, more = 0, between = 0, between_count = 0, pass, k;

	if (!space || __malloc_sbrk_base == (char *)-1)
		return;
	__malloc_lock(_REENT);
	base = __malloc_sbrk_base;
	top = __malloc_av_[2];
	end = base + (unsigned long)sce_newlib_heap_kb_size * 1024;
	taken = sbrk(0);
	if (taken != (char *)-1 && taken < end)
		untaken = end - taken;
	first = (struct heap_chunk *)(base + ((16 - (((unsigned long)base + 8) & 15)) & 15));
	for (pass = 0; pass < 2; pass++)
	{
		for (chunk = first; chunk != top; chunk = HEAP_CHUNK_AFTER(chunk))
		{
			unsigned long size = HEAP_CHUNK_SIZE(chunk), offset = (char *)chunk - base;
			int in_use;

			if (size < 16 || (char *)chunk + size > (char *)top)
			{
				stopped_at = offset;
				stopped_size = size;
				break;
			}
			in_use = HEAP_CHUNK_AFTER(chunk)->size & 1;
			if (pass == 1)
			{
				/* what sits between the two largest gaps */
				if (in_use && offset > low && offset < high)
				{
					between_count++;
					between_bytes += size;
					for (k = between; k > 0 && space->between[k - 1].size < size; k--)
						if (k < HEAP_MAP_BETWEEN)
							space->between[k] = space->between[k - 1];
					if (k < HEAP_MAP_BETWEEN)
					{
						space->between[k].offset = offset;
						space->between[k].size = size;
						heap_map_name(space, chunk, size, &space->between[k]);
						if (between < HEAP_MAP_BETWEEN)
							between++;
					}
				}
				continue;
			}
			if (in_use)
			{
				used++;
				used_total += size;
			}
			else
			{
				gaps++;
				free_total += size;
				if (size > gap_size[0])
				{
					gap_size[1] = gap_size[0], gap_offset[1] = gap_offset[0];
					gap_size[0] = size, gap_offset[0] = offset;
				}
				else if (size > gap_size[1])
					gap_size[1] = size, gap_offset[1] = offset;
			}
			if (size < HEAP_MAP_LARGE)
				continue;
			if (entries == HEAP_MAP_ENTRIES)
			{
				more++;
				continue;
			}
			space->entries[entries].offset = offset;
			space->entries[entries].size = size;
			if (in_use)
				heap_map_name(space, chunk, size, &space->entries[entries]);
			else
				space->entries[entries].kind = 'g';
			entries++;
		}
		if (pass == 1)
			break;
		/* the top chunk, and past it what the system hasn't handed over yet:
		one gap, which a request can grow into */
		if (top != first && (char *)top > base && (char *)top < end)
		{
			unsigned long offset = (char *)top - base;

			top_size = HEAP_CHUNK_SIZE(top) + untaken;
			gaps++;
			free_total += top_size;
			if (top_size > gap_size[0])
			{
				gap_size[1] = gap_size[0], gap_offset[1] = gap_offset[0];
				gap_size[0] = top_size, gap_offset[0] = offset;
			}
			else if (top_size > gap_size[1])
				gap_size[1] = top_size, gap_offset[1] = offset;
			if (entries < HEAP_MAP_ENTRIES)
			{
				space->entries[entries].offset = offset;
				space->entries[entries].size = top_size;
				space->entries[entries].kind = 't';
				entries++;
			}
		}
		low = gap_offset[0] < gap_offset[1] ? gap_offset[0] : gap_offset[1];
		high = gap_offset[0] < gap_offset[1] ? gap_offset[1] : gap_offset[0];
	}
	__malloc_unlock(_REENT);

	platform_log("heapmap (%s): %lu KB heap at %08lx; %lu KB free in %d gaps, the largest %lu KB at +%luK, the next %lu KB at +%luK; "
		"%lu KB in use in %d blocks; the top gap %lu KB, %lu KB of it not yet taken from the system",
		label, (end - base) / 1024, (unsigned long)base, free_total / 1024, gaps, gap_size[0] / 1024, gap_offset[0] / 1024,
		gap_size[1] / 1024, gap_offset[1] / 1024, used_total / 1024, used, top_size / 1024, untaken / 1024);
	if (stopped_at != ~0ul)
		platform_log("heapmap (%s): the walk stopped at +%luK, a chunk of %lu bytes: the allocator's records look damaged",
			label, stopped_at / 1024, stopped_size);
	heap_map_lines(label, " gaps and blocks of 16 KB or more", space->entries, entries);
	if (more)
		platform_log("heapmap (%s): %d more of 16 KB or more not listed", label, more);
	platform_log("heapmap (%s): between the two largest gaps, %d blocks in use, %lu KB; the largest %d follow",
		label, between_count, between_bytes / 1024, between);
	heap_map_lines(label, " between", space->between, between);
}

/* "heapwalk": newlib's chunks walked at each game tick and as each frame
comes back from the Media Engine, and the first header that can't be right
logged with where it is and its 64-byte line, so a header put back stale is
seen where it happens rather than at the _malloc_r that later follows it.
Checked: each chunk's size (at least 16, a multiple of 8, inside the heap);
for a free chunk (its next chunk's in-use bit clear), the next chunk's
previous size equal to its own, and its two bin links each a chunk in the
heap or one of newlib's bin heads. It only reads. Off: one test a call */
#define HEAP_WALK_LINES 6
#define HEAP_BIN_WORDS (2 * 128 + 2) /* newlib's av_[] (mallocr.c, NAV 128) */
static int heap_walk_on = -1;
static unsigned long heap_walks = 0, heap_walk_bad = 0, heap_walk_chunks = 0;
static int heap_walk_logged = 0;
static struct heap_chunk *heap_walk_last_bad = NULL;

static int heap_link_ok(unsigned long link, char *base, char *top)
{
	unsigned long bins = (unsigned long)__malloc_av_;

	if (link >= bins - 16 && link < bins + HEAP_BIN_WORDS * sizeof(void *))
		return 1;
	return link >= (unsigned long)base && link < (unsigned long)top && !(link & 7);
}

void psp_heap_walk(const char *where)
{
	extern int psp_keep(const char *name);
	extern int psp_keep_loaded(void);
	extern long psp_game_time_peek(void);
	struct heap_chunk *chunk, *first, *top, *before = NULL;
	char *base, *end;
	const char *why = NULL;
	unsigned long chunks = 0;

	if (heap_walk_on < 0)
	{
		if (!psp_keep_loaded())
			return;
		heap_walk_on = psp_keep("heapwalk") != 0;
		if (heap_walk_on)
			platform_log("heapwalk: on (newlib's chunks walked each tick and as each frame comes back from the Media Engine)");
	}
	if (!heap_walk_on || __malloc_sbrk_base == (char *)-1)
		return;
	__malloc_lock(_REENT);
	base = __malloc_sbrk_base;
	top = __malloc_av_[2];
	end = base + (unsigned long)sce_newlib_heap_kb_size * 1024;
	first = (struct heap_chunk *)(base + ((16 - (((unsigned long)base + 8) & 15)) & 15));
	if ((char *)top < base || (char *)top >= end)
		why = "the top chunk outside the heap";
	else
		for (chunk = first; chunk != top; before = chunk, chunk = HEAP_CHUNK_AFTER(chunk))
		{
			unsigned long size = HEAP_CHUNK_SIZE(chunk);
			struct heap_chunk *next;

			chunks++;
			if (size < 16 || (size & 7) || (char *)chunk + size > (char *)top)
			{
				why = "a size that isn't a chunk's";
				break;
			}
			next = HEAP_CHUNK_AFTER(chunk);
			if (!(next->size & 1))
			{
				const unsigned long *links = (const unsigned long *)(chunk + 1);

				if (next != top && next->previous_size != size)
				{
					why = "a free chunk whose next chunk has another previous size";
					break;
				}
				if (!heap_link_ok(links[0], base, (char *)top) || !heap_link_ok(links[1], base, (char *)top))
				{
					why = "a free chunk's bin link outside the heap and the bins";
					break;
				}
			}
		}
	heap_walks++;
	heap_walk_chunks += chunks;
	if (why)
	{
		heap_walk_bad++;
		if (chunk != heap_walk_last_bad && heap_walk_logged < HEAP_WALK_LINES)
		{
			const unsigned long *line = (const unsigned long *)((unsigned long)chunk & ~63ul);

			heap_walk_logged++;
			platform_log("heapwalk: BAD at %s, game time %ld, walk %lu: %s: chunk %p (heap +%lu) size %08lx previous %08lx links %08lx %08lx; the chunk before %p size %08lx",
				where, psp_game_time_peek(), heap_walks, why, chunk, (unsigned long)((char *)chunk - base),
				chunk->size, chunk->previous_size, ((const unsigned long *)(chunk + 1))[0],
				((const unsigned long *)(chunk + 1))[1], before, before ? before->size : 0ul);
			platform_log("heapwalk: its line %p: %08lx %08lx %08lx %08lx %08lx %08lx %08lx %08lx %08lx %08lx %08lx %08lx %08lx %08lx %08lx %08lx",
				line, line[0], line[1], line[2], line[3], line[4], line[5], line[6], line[7], line[8], line[9],
				line[10], line[11], line[12], line[13], line[14], line[15]);
		}
		heap_walk_last_bad = chunk;
	}
	__malloc_unlock(_REENT);
	if (heap_walks % 1000 == 0)
		platform_log("heapwalk: %lu walks, %lu chunks a walk, %lu found a bad header", heap_walks,
			heap_walk_chunks / heap_walks, heap_walk_bad);
}

#if defined(PSP_METRICS) && PSP_METRICS
/* for the per-map heap figures (psp_metrics.c): the heap's free space and
its largest free block, which a large request needs, by heapmap's walk
above without its tables. The top chunk counts with what the system hasn't
handed over yet, since a request can grow into both */
void psp_heap_survey(unsigned long *free_bytes, unsigned long *largest_bytes, unsigned long *used_bytes)
{
	struct heap_chunk *chunk, *first, *top;
	char *base, *end, *taken;
	unsigned long free_total = 0, used_total = 0, largest = 0, untaken = 0;

	*free_bytes = *largest_bytes = *used_bytes = 0;
	if (__malloc_sbrk_base == (char *)-1)
		return;
	__malloc_lock(_REENT);
	base = __malloc_sbrk_base;
	top = __malloc_av_[2];
	end = base + (unsigned long)sce_newlib_heap_kb_size * 1024;
	taken = sbrk(0);
	if (taken != (char *)-1 && taken < end)
		untaken = end - taken;
	first = (struct heap_chunk *)(base + ((16 - (((unsigned long)base + 8) & 15)) & 15));
	for (chunk = first; chunk != top; chunk = HEAP_CHUNK_AFTER(chunk))
	{
		unsigned long size = HEAP_CHUNK_SIZE(chunk);

		if (size < 16 || (char *)chunk + size > (char *)top)
			break;
		if (HEAP_CHUNK_AFTER(chunk)->size & 1)
			used_total += size;
		else
		{
			free_total += size;
			if (size > largest)
				largest = size;
		}
	}
	if (top != first && (char *)top > base && (char *)top < end)
	{
		unsigned long size = HEAP_CHUNK_SIZE(top) + untaken;

		free_total += size;
		if (size > largest)
			largest = size;
	}
	else
	{
		free_total += untaken;
		if (untaken > largest)
			largest = untaken;
	}
	__malloc_unlock(_REENT);
	*free_bytes = free_total;
	*largest_bytes = largest;
	*used_bytes = used_total;
}
#endif
