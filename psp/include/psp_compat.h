/* PSP: small gaps between newlib and what the Linux platform layer
(port/linux/src) expects. Force-included for those files only. */
#ifndef __PSP_COMPAT_H
#define __PSP_COMPAT_H
#include <time.h>
#include <errno.h>
#include <pspthreadman.h>

/* psp_pre.h removes these for the game's own headers, which define them */
#ifndef LONG_MAX
#define LONG_MAX 2147483647L
#endif
#ifndef LONG_MIN
#define LONG_MIN (-LONG_MAX - 1L)
#endif

/* build 75: CLOCK_MONOTONIC from the microsecond counter since boot. The C
library answers every clock with the wall clock (sceKernelLibcGettimeofday),
and with the PSP's clock chip unset (battery out; a PSP-1000, 2026-10-05)
that clock misbehaves: d3d8_gl.c's 60 Hz vertical blank thread, which paces
itself on CLOCK_MONOTONIC, slowed down further and further, and Present,
which waits on it once two frames are queued, went from 30 fps to under 1
(b71-b74, render_frame_present 99% of the frame). CLOCK_REALTIME stays the
wall clock: GetSystemTime wants the date, and WaitForSingleObjectEx's
deadline is compared with pthread's own reading of it (ftime) */
long long sceKernelGetSystemTimeWide(void);
static inline int psp_clock_gettime(clockid_t clock, struct timespec *now)
{
	if (clock == CLOCK_MONOTONIC || clock == CLOCK_MONOTONIC_RAW || clock == CLOCK_MONOTONIC_COARSE)
	{
		unsigned long long microseconds = (unsigned long long)sceKernelGetSystemTimeWide();

		now->tv_sec = (time_t)(microseconds / 1000000ULL);
		now->tv_nsec = (long)(microseconds % 1000000ULL) * 1000L;
		return 0;
	}
	return clock_gettime(clock, now);
}
#define clock_gettime psp_clock_gettime

#ifndef UTIME_NOW
#define UTIME_NOW -2L
#define UTIME_OMIT -1L
#endif

/* msvc_crt.c's floating-point control word is AArch64 code under
HALO_ANDROID; the PSP keeps the default rounding (round to nearest) */
#define __builtin_arm_rsr64(name) 0ULL
#define __builtin_arm_wsr64(name, value) ((void)(value))

#ifndef TIMER_ABSTIME
#define TIMER_ABSTIME 1
#endif
/* sleeps until (TIMER_ABSTIME) or for the given time on that clock */
static inline int clock_nanosleep(clockid_t clock, int flags, const struct timespec *request, struct timespec *remain)
{
	long long microseconds = (long long)request->tv_sec * 1000000 + request->tv_nsec / 1000;

	(void)remain;
	if (flags & TIMER_ABSTIME)
	{
		struct timespec now;

		clock_gettime(clock, &now);
		microseconds -= (long long)now.tv_sec * 1000000 + now.tv_nsec / 1000;
	}
	if (microseconds > 0)
		sceKernelDelayThread((SceUInt)microseconds);
	return 0;
}
#endif
