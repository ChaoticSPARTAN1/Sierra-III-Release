/* psp_crash.c - the game's own crash handler (build 102)

halo_crash.prx (psp/crash) catches the CPU's exception, saves the registers
into the block below and returns from the exception into psp_crash_entry,
here, on a stack of its own, in the crashed thread (crash_asm.S says why:
pspsdk's handler returned a user-mode thread onto kernel code, and no crash
in the hardware runs ever showed a report). From here everything the game can do
works: the report is drawn on both of the GE's frame buffers, written to
crash.txt in the build's folder (PSP_DATA_ROOT, psp_paths.h: ms0:/halo, or
ms0:/halomp for the multiplayer build), and the log's last lines are saved.
The other threads go on (the heartbeat keeps logging, the sound may keep
playing); the game thread, if it wasn't the one that crashed, stops at its
next frame (psp_crashed) so the report stays on the screen.

"crashdraw" in keep.txt: the handler is called 20 s into play with the game
thread's registers made up, no exception: checks this half in the emulator,
which has no exception handlers */
#include <pspkernel.h>
#include <pspdebug.h>
#include <pspdisplay.h>
#include <pspiofilemgr.h>
#include <stdio.h>
#include <string.h>
#include "psp_paths.h"   /* ms0:/halo or ms0:/halomp, and the maps */

void platform_log(const char *format, ...);
void psp_log_flush(void);
void psp_log_exiting(void);
int psp_build_number(void);

struct psp_crash_block
{
	unsigned int r[32];
	unsigned int status, lo, hi, badvaddr, cause, epc;
};

volatile int psp_crashed = 0;
static struct psp_crash_block *crash_block = NULL;

static const char *const causes[32] = {
	"interrupt", "TLB modification", "TLB load", "TLB store",
	"bad address (load)", "bad address (store)", "bus error (instruction)", "bus error (data)",
	"syscall", "breakpoint", "reserved instruction", "coprocessor unusable",
	"overflow", "?", "?", "floating point", "?", "?", "?", "?", "?", "?", "?", "?",
	"?", "?", "?", "?", "?", "?", "?", "?"
};

static const char *const names[32] = {
	"zr", "at", "v0", "v1", "a0", "a1", "a2", "a3", "t0", "t1", "t2", "t3", "t4", "t5", "t6", "t7",
	"s0", "s1", "s2", "s3", "s4", "s5", "s6", "s7", "t8", "t9", "k0", "k1", "gp", "sp", "fp", "ra"
};

static int user_word(unsigned int address)
{
	return address >= 0x08800000 && address < 0x0A000000 && !(address & 3);
}

static void draw_report(unsigned int base, const struct psp_crash_block *regs, const char *thread_name)
{
	char line[100];
	unsigned int code = (regs->cause >> 2) & 31, sp = regs->r[29], k;
	volatile unsigned short *pixels = (volatile unsigned short *)(0x40000000u | base);

	/* below the stub's magenta band, a red one, then the text on black */
	for (k = 512 * 8; k < 512 * 24; k++)
		pixels[k] = 0x001F;
	for (k = 512 * 24; k < 512 * 272; k++)
		pixels[k] = 0;
	pspDebugScreenSetBase((u32 *)(0x40000000u | base));
	pspDebugScreenSetXY(0, 3);
	snprintf(line, sizeof(line), "Halo PSP build %d crashed: %s, thread %s\n\n", psp_build_number(), causes[code], thread_name);
	pspDebugScreenPuts(line);
	/* build 103: badvaddr is set only by address errors (the CPU leaves it
	stale for bus errors: b102's crashplay showed 4a00e120); a fault in a
	branch's delay slot (cause bit 31) reports the branch's address */
	snprintf(line, sizeof(line), "epc %08x%s  ra %08x  badvaddr %08x%s\ncause %08x  status %08x  sp %08x\n\n",
		regs->epc, (regs->cause & 0x80000000u) ? " (+4: delay slot)" : "", regs->r[31], regs->badvaddr,
		(code == 6 || code == 7) ? " (stale)" : "", regs->cause, regs->status, sp);
	pspDebugScreenPuts(line);
	for (k = 0; k < 32; k += 4)
	{
		snprintf(line, sizeof(line), "%s %08x  %s %08x  %s %08x  %s %08x\n", names[k], regs->r[k], names[k + 1], regs->r[k + 1],
			names[k + 2], regs->r[k + 2], names[k + 3], regs->r[k + 3]);
		pspDebugScreenPuts(line);
	}
	pspDebugScreenPuts("\nstack:\n");
	if (user_word(sp) && user_word(sp + 4 * 24))
		for (k = 0; k < 24; k += 6)
		{
			const unsigned int *word = (const unsigned int *)sp + k;

			snprintf(line, sizeof(line), "%08x %08x %08x %08x %08x %08x\n", word[0], word[1], word[2], word[3], word[4], word[5]);
			pspDebugScreenPuts(line);
		}
	pspDebugScreenPuts("\nSaved to " PSP_DATA_ROOT "/crash.txt (if the Memory Stick still answers).\nTake a photo of this screen, then hold POWER.\n");
}

static int write_report(const struct psp_crash_block *regs, const char *thread_name)
{
	char line[200];
	unsigned int code = (regs->cause >> 2) & 31, sp = regs->r[29], k;
	SceUID file = sceIoOpen(PSP_DATA_ROOT "/crash.txt", PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);

	if (file < 0)
		return file;
#define PUT(...) do { int n = snprintf(line, sizeof(line), __VA_ARGS__); sceIoWrite(file, line, n > 0 ? (n < (int)sizeof(line) ? n : (int)sizeof(line) - 1) : 0); } while (0)
	PUT("Halo PSP build %d crash\nexception: %s (code %u)\nthread: %s\n", psp_build_number(), causes[code], code, thread_name);
	PUT("epc %08x  ra %08x  badvaddr %08x  cause %08x  status %08x  sp %08x  lo %08x  hi %08x\n", regs->epc,
		regs->r[31], regs->badvaddr, regs->cause, regs->status, sp, regs->lo, regs->hi);
	PUT("%s%sregisters:", (regs->cause & 0x80000000u) ? "the fault is in the delay slot: the instruction after epc\n" : "",
		(code == 6 || code == 7) ? "bus error: badvaddr is stale (only address errors set it)\n" : "");
	for (k = 0; k < 32; k++)
		PUT("%s %s %08x", k % 4 ? "" : "\n", names[k], regs->r[k]);
	PUT("\nstack (from sp):");
	if (user_word(sp))
		for (k = 0; k < 256 && user_word(sp + 4 * k); k++)
			PUT("%s %08x", k % 8 ? "" : "\n", ((const unsigned int *)sp)[k]);
	PUT("\n");
#undef PUT
	sceIoClose(file);
	return 0;
}

/* crashdraw's picture of the first frame buffer (the emulator's runs keep
the game's shots folder): 480 x 272, 24-bit BMP */
static int crash_draw_testing = 0;

static void test_shot(void)
{
	static unsigned char row[480 * 3];
	unsigned char header[54];
	unsigned int image = 480 * 3 * 272, size = 54 + image, value;
	const volatile unsigned short *frame = (const volatile unsigned short *)0x44000000;
	int x, y;
	SceUID file = sceIoOpen(PSP_DATA_ROOT "/shots/crashdraw.bmp", PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);

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

/* where halo_crash.prx's stub returns to: a0 is the register block */
void psp_crash_entry(struct psp_crash_block *regs)
{
	SceKernelThreadInfo info;
	const char *thread_name = "?";

	psp_crashed = 1;
	/* build 104: every lock wait limited from here on (the crashed thread
	may hold the log's lock; psp_trace.c's locks don't wait after a crash) */
	psp_log_exiting();
	memset(&info, 0, sizeof(info));
	info.size = sizeof(info);
	if (sceKernelReferThreadStatus(sceKernelGetThreadId(), &info) >= 0)
		thread_name = info.name;
	/* the screen first (nothing in it can wait on anything), then the file */
	{
		/* the boot screen's thread would draw over the report */
		extern void psp_ge_boot_freeze(void);

		psp_ge_boot_freeze();
	}
	draw_report(0x04000000, regs, thread_name);
	draw_report(0x04044000, regs, thread_name);
	{
		/* build 104: while loading, the screen shows the log's own buffer
		(VRAM 0x1B0000), not the GE's: the report goes there too, and the log
		stops drawing over it (b97 did this; b102's handler didn't) */
		extern int psp_screen_log_showing(void);
		extern void psp_screen_log_freeze(void);

		if (psp_screen_log_showing())
		{
			psp_screen_log_freeze();
			draw_report(0x041B0000, regs, thread_name);
		}
	}
	{
		/* build 103: the handle held back since start-up, for crash.txt (b102
		run 3's crash.txt couldn't be opened: the kernel was out of handles) */
		extern void psp_handles_release(void);
		extern void psp_handles_dump(const char *why);
		int written, k;
		char line[420];
		int length;

		psp_handles_release();
		written = write_report(regs, thread_name);
		if (written < 0)
			platform_log("CRASH: %s in thread %s at epc %08x%s (ra %08x, badvaddr %08x); crash.txt NOT written (0x%08x)",
				causes[(regs->cause >> 2) & 31], thread_name, regs->epc, (regs->cause & 0x80000000u) ? " (delay slot)" : "",
				regs->r[31], regs->badvaddr, (unsigned)written);
		else
			platform_log("CRASH: %s in thread %s at epc %08x%s (ra %08x, badvaddr %08x); crash.txt written",
				causes[(regs->cause >> 2) & 31], thread_name, regs->epc, (regs->cause & 0x80000000u) ? " (delay slot)" : "",
				regs->r[31], regs->badvaddr);
		/* the registers in the log too, so a crash.txt that couldn't be
		written loses nothing */
		length = snprintf(line, sizeof(line), "CRASH registers:");
		for (k = 1; k < 32 && length < (int)sizeof(line) - 16; k++)
			length += snprintf(line + length, sizeof(line) - length, " %s %08x", names[k], regs->r[k]);
		platform_log("%s", line);
		psp_handles_dump("at the crash");
	}
	/* the log's lock and the flush's wait at most 100 ms each from here: the
	crashed thread may have held them */
	psp_log_exiting();
	psp_log_flush();
	if (crash_draw_testing)
		test_shot();
	for (;;)
		sceKernelSleepThread();
}

/* the game thread's frame end: once something crashed, it stops drawing over the report */
void psp_crash_stop_if_crashed(void)
{
	if (psp_crashed)
		for (;;)
			sceKernelSleepThread();
}

/* halo_crash.prx's start arguments (psp_platform.c's load_crash_handler):
the block and a 16 KB stack from the system's free memory, so neither comes
off the game's heap */
struct halo_crash_arguments
{
	unsigned int magic, block, handler, stack_top;
};

int psp_crash_arguments(void *out, int size)
{
	struct halo_crash_arguments *arguments = out;
	SceUID memory;
	char *base;

	if (size != (int)sizeof(*arguments))
		return 0;
	memory = sceKernelAllocPartitionMemory(2, "halo crash", PSP_SMEM_High, 16384 + 256, NULL);
	if (memory < 0)
		return 0;
	base = sceKernelGetBlockHeadAddr(memory);
	memset(base, 0, 256);
	crash_block = (struct psp_crash_block *)base;
	arguments->magic = 0x48435231; /* "HCR1" */
	arguments->block = (unsigned int)base;
	arguments->handler = (unsigned int)psp_crash_entry;
	arguments->stack_top = ((unsigned int)base + 256 + 16384 - 16) & ~15u;
	return 1;
}

/* keep.txt's "crashdraw": the handler's half alone, with made-up registers */
void psp_crash_draw_test(void)
{
	static struct psp_crash_block fake;
	unsigned int k;

	for (k = 0; k < 32; k++)
		fake.r[k] = 0x11111111u * (k & 15);
	fake.r[29] = (unsigned int)&k & ~3u;
	fake.r[31] = (unsigned int)psp_crash_draw_test;
	fake.epc = (unsigned int)psp_crash_draw_test;
	fake.badvaddr = 4;
	fake.cause = 5 << 2;
	platform_log("crashdraw: the crash handler's report with made-up registers (no exception)");
	crash_draw_testing = 1;
	psp_crash_entry(&fake);
}
