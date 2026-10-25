/*
HALO_ME_TEST (build 44, 2026-10-03)

A first run of code on the PSP's Media Engine (its second CPU) on a
PSP-1000: does it start, does it run C compiled into this program (with the
FPU and the C library's maths), how fast does it mix sound compared with the
main CPU, and does the PSP come back cleanly afterwards.

Built on mcidclan's me-core-mapper library (MIT, copyright (c) 2026 mcidclan
(m-c/d, m-cid), https://github.com/mcidclan/psp-media-engine-custom-core),
which loads a small kernel module, resets the Media Engine and runs
meLibOnProcess() on it. This is NOT the game: a separate folder, PSP/GAME/halo_me.

What it does, in order (the screen and ms0:/halo/log/me_test.txt say):
  1. starts the ME; it counts in shared memory (proves it runs)
  2. the ME mixes 16 voices x 512 frames, 200 times; the main CPU times it
  3. the main CPU does the same mix, timed
  4. the ME runs some FPU maths (sinf) and reports a checksum
  5. the ME stops; press X or wait 20 s to exit
A hang at step 1 or 2 means the ME didn't start or faulted: hold the power
switch for 10 s. That result is useful too.
*/

#include <pspdebug.h>
#include <psppower.h>
#include <pspctrl.h>
#include <pspdisplay.h>
#include <pspkernel.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <me-core-mapper/me-core.h>

PSP_MODULE_INFO("halo_me_test", 0, 1, 0);
PSP_HEAP_SIZE_KB(-2048);
PSP_MAIN_THREAD_ATTR(PSP_THREAD_ATTR_VFPU | PSP_THREAD_ATTR_USER);

#define VOICES 16
#define FRAMES 512
#define SAMPLE_WORDS 16384 /* 32 KB of 16-bit samples the voices read */

/* shared, uncached: what the two CPUs say to each other */
meLibSetSharedUncached32(16);
#define me_alive      (meLibSharedMemory[0])  /* the ME counts here */
#define me_command    (meLibSharedMemory[1])  /* 1 mix, 2 maths, 3 stop */
#define me_done       (meLibSharedMemory[2])  /* the command it finished */
#define me_result     (meLibSharedMemory[3])
#define me_rounds     (meLibSharedMemory[4])
#define me_exit       (meLibSharedMemory[5])

/* the sound data and the mix buffer: cache-line aligned, read by both CPUs
through their own caches (the ME invalidates its cache first) */
static short samples[SAMPLE_WORDS] __attribute__((aligned(64)));
static int sums[FRAMES * 2] __attribute__((aligned(64)));
static short output[FRAMES * 2] __attribute__((aligned(64)));

/* the mix: 16.16 fixed-point stepping through each voice's samples with
linear interpolation, left and right gains, as psp_dsound.c does it */
static unsigned int mix_once(int rounds)
{
	unsigned int check = 0;
	int round, voice, frame;

	for (round = 0; round < rounds; round++)
	{
		memset(sums, 0, sizeof(sums));
		for (voice = 0; voice < VOICES; voice++)
		{
			unsigned int position = (voice * 977) << 16, step = 0x8000 + voice * 0x1000;
			int left = 0x2000 + voice * 0x100, right = 0x3000 - voice * 0x80;

			for (frame = 0; frame < FRAMES; frame++)
			{
				unsigned int index = (position >> 16) & (SAMPLE_WORDS - 2);
				int fraction = position & 0xFFFF;
				int s0 = samples[index], s1 = samples[index + 1];
				int sample = s0 + (((s1 - s0) * fraction) >> 16);

				sums[2 * frame] += (sample * left) >> 14;
				sums[2 * frame + 1] += (sample * right) >> 14;
				position += step;
			}
		}
		for (frame = 0; frame < FRAMES * 2; frame++)
		{
			int value = sums[frame];

			output[frame] = (short)(value > 32767 ? 32767 : value < -32768 ? -32768 : value);
			check = check * 31 + (unsigned short)output[frame];
		}
	}
	return check;
}

static unsigned int maths_once(void)
{
	float sum = 0.0f;
	int k;

	for (k = 0; k < 20000; k++)
		sum += sinf(k * 0.001f) * sqrtf((float)k + 1.0f);
	return (unsigned int)(sum * 1000.0f);
}

/* ---------- the Media Engine's side: no kernel calls here, ever */
__attribute__((noinline, aligned(4)))
void meLibOnProcess(void)
{
	meLibDcacheWritebackInvalidateAll();
	for (;;)
	{
		unsigned int command;

		me_alive++;
		command = me_command;
		if (command == 3 || me_exit)
			break;
		if (command && command != me_done)
		{
			unsigned int result = 0;

			/* the samples were written by the main CPU: drop any stale copy */
			meLibDcacheWritebackInvalidateAll();
			if (command == 1)
				result = mix_once(me_rounds);
			else if (command == 2)
				result = maths_once();
			/* the output is the ME's; the main CPU reads it after a
			write-back of the ME's cache */
			meLibDcacheWritebackInvalidateAll();
			me_result = result;
			me_done = command;
		}
		meLibDelayPipeline();
	}
	me_exit = 2;
	meLibHalt();
}

/* ---------- the main CPU's side */
static SceUID report = -1;

static void say(const char *format, ...)
{
	char line[256];
	va_list arguments;
	int length;

	va_start(arguments, format);
	length = vsnprintf(line, sizeof(line) - 2, format, arguments);
	va_end(arguments);
	pspDebugScreenPrintf("%s\n", line);
	if (report >= 0)
	{
		line[length++] = '\n';
		sceIoWrite(report, line, length);
	}
}

static int wait_done(unsigned int command, unsigned int timeout_us)
{
	unsigned int start = sceKernelGetSystemTimeLow();

	while (me_done != command)
	{
		if (sceKernelGetSystemTimeLow() - start > timeout_us)
			return 0;
		sceKernelDelayThread(200);
	}
	return 1;
}

int main(void)
{
	int table, k;
	unsigned int alive_before, start, me_us, sc_us, sc_check, me_check;
	SceCtrlData pad;

	pspDebugScreenInit();
	sceIoMkdir("ms0:/halo", 0777);
	sceIoMkdir("ms0:/halo/log", 0777);
	report = sceIoOpen("ms0:/halo/log/me_test.txt", PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
	say("Halo PSP: Media Engine test (build 44)");
	say("CPU %d MHz, bus %d MHz", scePowerGetCpuClockFrequencyInt(), scePowerGetBusClockFrequencyInt());
	for (k = 0; k < SAMPLE_WORDS; k++)
		samples[k] = (short)((k * 2654435761u) >> 17);
	sceKernelDcacheWritebackInvalidateAll();

	say("1. starting the Media Engine...");
	table = meLibDefaultInit();
	say("   meLibDefaultInit: %d (%s)", table, table == ME_CORE_IMG_TABLE ? "old generation table: PSP-1000" :
		table == ME_CORE_T2_IMG_TABLE ? "new generation table" : table < 0 ? "ERROR" : "unknown table");
	alive_before = me_alive;
	sceKernelDelayThread(200000);
	say("   alive counter: %u -> %u after 0.2 s (%s)", alive_before, (unsigned)me_alive,
		me_alive != alive_before ? "the ME runs" : "NOT RUNNING");
	if (me_alive == alive_before)
	{
		say("   the ME didn't start; stopping here");
		goto out;
	}

	say("2. ME mixes 16 voices x 512 frames, 200 rounds...");
	me_rounds = 200;
	start = sceKernelGetSystemTimeLow();
	me_command = 1;
	if (!wait_done(1, 20000000))
	{
		say("   TIMED OUT after 20 s (alive %u)", (unsigned)me_alive);
		goto out;
	}
	me_us = sceKernelGetSystemTimeLow() - start;
	sceKernelDcacheWritebackInvalidateAll();
	me_check = me_result;
	say("   ME: %u us for 200 rounds = %u us a round (a 23 ms block of 16 voices); checksum %08x", me_us, me_us / 200, me_check);

	say("3. the main CPU does the same...");
	start = sceKernelGetSystemTimeLow();
	sc_check = mix_once(200);
	sc_us = sceKernelGetSystemTimeLow() - start;
	say("   SC: %u us for 200 rounds = %u us a round; checksum %08x (%s)", sc_us, sc_us / 200, sc_check,
		sc_check == me_check ? "SAME as the ME's" : "DIFFERENT from the ME's");

	say("4. ME runs 20000 sinf/sqrtf...");
	start = sceKernelGetSystemTimeLow();
	me_command = 2;
	if (!wait_done(2, 20000000))
	{
		say("   TIMED OUT (alive %u)", (unsigned)me_alive);
		goto out;
	}
	me_us = sceKernelGetSystemTimeLow() - start;
	me_check = me_result;
	start = sceKernelGetSystemTimeLow();
	sc_check = maths_once();
	sc_us = sceKernelGetSystemTimeLow() - start;
	say("   ME %u us, SC %u us; results %08x / %08x (%s)", me_us, sc_us, me_check, sc_check,
		me_check == sc_check ? "same" : "differ: the ME's FPU may round differently, or lack something");

	say("5. 10 s of both CPUs busy (ME mixing, SC counting)...");
	{
		unsigned int rounds = 0, me_mixes = 0, t0 = sceKernelGetSystemTimeLow();
		volatile unsigned int spin = 0;

		me_rounds = 20;
		while (sceKernelGetSystemTimeLow() - t0 < 10000000)
		{
			me_command = 1;
			me_done = 0;
			while (me_done != 1 && sceKernelGetSystemTimeLow() - t0 < 10000000)
				spin++;
			me_mixes += me_done == 1 ? 20 : 0;
			rounds++;
		}
		say("   ME mixed %u blocks in 10 s while the SC spun %u times; alive %u", me_mixes, (unsigned)spin, (unsigned)me_alive);
	}
out:
	say("stopping the ME...");
	me_command = 3;
	me_exit = 1;
	for (k = 0; k < 20 && me_exit < 2; k++)
		sceKernelDelayThread(100000);
	say("   %s", me_exit >= 2 ? "stopped cleanly" : "did not acknowledge the stop (it may have faulted)");
	say("done. X or 20 s to exit. Then check: does the XMB play music, and does the game run?");
	if (report >= 0)
		sceIoClose(report);
	for (k = 0; k < 200; k++)
	{
		sceCtrlPeekBufferPositive(&pad, 1);
		if (pad.Buttons & PSP_CTRL_CROSS)
			break;
		sceKernelDelayThread(100000);
	}
	sceKernelExitGame();
	return 0;
}
