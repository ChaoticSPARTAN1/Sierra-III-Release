/*
HALO_CRASH.PRX

A small kernel-mode module the game loads at start-up (psp_platform.c). It
installs a handler for unhandled CPU exceptions: when the game crashes, the
handler writes the CPU state (the faulting instruction's address, the
reason, every register and the top of the stack) to ms0:/halo/crash.txt
shows the same on screen, and stops there, instead of the PSP silently switching itself off.
The addresses can then be looked up in the build's symbols.
*/

#include <pspkernel.h>
#include <pspdebug.h>
#include <pspdisplay.h>
#include <pspiofilemgr.h>
#include <string.h>
#include <pspexception.h>

PSP_MODULE_INFO("halo_crash", PSP_MODULE_KERNEL, 1, 0);
PSP_MAIN_THREAD_ATTR(0);

static const char *causes[32] = {
	"interrupt", "TLB modification", "TLB load", "TLB store",
	"unaligned or bad address (load)", "unaligned or bad address (store)", "bus error (instruction)", "bus error (data)",
	"syscall", "breakpoint", "reserved instruction", "coprocessor unusable",
	"overflow", "", "", "", "", "", "", "", "", "", "", "", "", "", "", "", "", "", "", ""
};

static char line[96];
static int file = -1;

static void put(const char *text)
{
	if (file >= 0)
		sceIoWrite(file, text, strlen(text));
}

static void hex(char *out, unsigned int value)
{
	static const char digits[] = "0123456789abcdef";
	int index;

	for (index = 7; index >= 0; index--, value >>= 4)
		out[index] = digits[value & 15];
	out[8] = 0;
}

static void put_hex(const char *label, unsigned int value)
{
	char text[12];

	hex(text, value);
	put(label);
	put(text);
}

/* the screen first: the handler runs inside the exception, where drawing
into the frame buffer works but file I/O may not */
static void show(const char *label, unsigned int value)
{
	char text[12];

	hex(text, value);
	pspDebugScreenPuts(label);
	pspDebugScreenPuts(text);
}

/* build 97: the report drawn at one base: the on-screen log's buffer (shown
while loading), then both of the GE's frame buffers (shown during play: b96's
crashes in play froze on the game's last frame, the report drawn where
nobody could see it) */
static void draw_report(void *base, int clear, PspDebugRegBlock *regs)
{
	unsigned int code = (regs->cause >> 2) & 31;
	unsigned int index, sp = regs->r[29];

	if (clear)
	{
		unsigned int *p = (unsigned int *)base, n;

		for (n = 512 * 24 / 2; n < 512 * 272 / 2; n++)
			p[n] = 0;
	}

	/* draw straight into the frame buffer the game's on-screen log set up
	and the display is showing (psp_platform.c: 16-bit, 512 wide, at
	0x041B0000 in VRAM), with no kernel calls at all: on 6.xx firmware the
	display and GE driver functions an earlier handler called before
	drawing may not be linked, and calling one inside the exception would
	itself fault. pspDebugScreenInitEx with a base and no set-up only
	writes memory */
	pspDebugScreenInitEx(base, PSP_DISPLAY_PIXEL_FORMAT_565, 0);
	pspDebugScreenPuts("Halo PSP crashed: ");
	pspDebugScreenPuts(causes[code]);
	show("\n\nepc      ", regs->epc);
	show("\nra       ", regs->r[31]);
	show("\nbadvaddr ", regs->badvaddr);
	show("\ncause    ", regs->cause);
	show("\nsp       ", sp);
	pspDebugScreenPuts("\n\nregisters r0-r31:\n");
	for (index = 0; index < 32; index++)
	{
		char text[12];

		hex(text, regs->r[index]);
		pspDebugScreenPuts(text);
		pspDebugScreenPuts(index % 6 == 5 ? "\n" : " ");
	}
	pspDebugScreenPuts("\n\nstack:\n");
	if (sp >= 0x08800000 && sp < 0x0A000000 - 48 * 4)
	{
		for (index = 0; index < 48; index++)
		{
			char text[12];

			hex(text, ((unsigned int *)sp)[index]);
			pspDebugScreenPuts(text);
			pspDebugScreenPuts(index % 6 == 5 ? "\n" : " ");
		}
	}
	pspDebugScreenPuts("\nTake a photo of this screen, then hold POWER.\n");
}

static void handler(PspDebugRegBlock *regs)
{
	unsigned int code = (regs->cause >> 2) & 31;
	unsigned int index, sp = regs->r[29];

	/* build 101: first, with nothing but stores, a red band across the top
	of both of the GE's frame buffers (16-bit 5650, red in the low bits):
	b100's own crash test froze with nothing on the screen, so a band and no
	text says this handler runs but the text drawing doesn't */
	{
		volatile unsigned short *fb0 = (volatile unsigned short *)0x44000000, *fb1 = (volatile unsigned short *)0x44044000;
		unsigned int k;

		for (k = 0; k < 512 * 24; k++)
			fb0[k] = fb1[k] = 0x001F;
	}
	draw_report((void *)0x441B0000, 0, regs);
	/* the GE's two frame buffers (psp_ge.c: 16-bit, 512 wide, at VRAM 0
	and 0x44000) */
	draw_report((void *)0x44000000, 1, regs);
	draw_report((void *)0x44044000, 1, regs);

	/* then the same to a file, if I/O works here */
	file = sceIoOpen("ms0:/halo/crash.txt", PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
	put("Halo PSP crash\nexception: ");
	put(causes[code]);
	put_hex("\nepc ", regs->epc);
	put_hex("  ra ", regs->r[31]);
	put_hex("  badvaddr ", regs->badvaddr);
	put_hex("  cause ", regs->cause);
	put_hex("  sp ", sp);
	put("\nregisters:");
	for (index = 0; index < 32; index++)
	{
		if (index % 4 == 0)
			put("\n");
		put_hex(" ", regs->r[index]);
	}
	put("\nstack (from sp):");
	if (sp >= 0x08800000 && sp < 0x0A000000 - 256 * 4)
	{
		for (index = 0; index < 256; index++)
		{
			if (index % 8 == 0)
				put("\n");
			put_hex(" ", ((unsigned int *)sp)[index]);
		}
	}
	put("\n");
	if (file >= 0)
		sceIoClose(file);
	for (;;)
		;
}

/* build 102: the exception entry of our own (crash_asm.S), handing over to
the game's handler (psp_crash.c), which the game names when it starts this
module: its arguments are { magic, register block, handler, stack top }, all
in the game's memory. Without them (an older game), pspsdk's handler as
before */
extern void halo_exception(void);
extern unsigned int halo_block, halo_user_handler, halo_user_stack;

struct halo_crash_arguments
{
	unsigned int magic, block, handler, stack_top;
};
#define HALO_CRASH_MAGIC 0x48435231 /* "HCR1" */

static int user_address(unsigned int address)
{
	return address >= 0x08800000 && address < 0x0A000000;
}

int module_start(SceSize args, void *argp)
{
	int result;
	char text[12];
	struct halo_crash_arguments const *given = argp;
	int ours = 0;

	if (args == sizeof(struct halo_crash_arguments) && given && given->magic == HALO_CRASH_MAGIC &&
		user_address(given->block) && user_address(given->handler) && user_address(given->stack_top))
	{
		halo_block = given->block;
		halo_user_handler = given->handler;
		halo_user_stack = given->stack_top & ~15u;
		ours = 1;
		result = sceKernelRegisterDefaultExceptionHandler((void *)((unsigned int)halo_exception | 0x80000000));
	}
	else
		result = pspDebugInstallErrorHandler(handler);
	/* what the kernel said, for the game's log: 0 is installed; 0x8002013a
	would mean the firmware doesn't resolve the call's NID */
	file = sceIoOpen("ms0:/halo/log/crash_handler.txt", PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
	put(ours ? "build 102 handler (into the game's own); " : "pspsdk's handler (no arguments from the game); ");
	hex(text, (unsigned int)result);
	put("sceKernelRegisterDefaultExceptionHandler returned 0x");
	put(text);
	put("\n");
	if (ours)
	{
		put_hex("block ", halo_block);
		put_hex(" handler ", halo_user_handler);
		put_hex(" stack ", halo_user_stack);
		put("\n");
	}
	if (file >= 0)
		sceIoClose(file);
	file = -1;
	return 0;
}

int module_stop(SceSize args, void *argp)
{
	(void)args; (void)argp;
	return 0;
}

/* libpspdebug's exception code links its screen printer, which wants these;
nothing here prints to the screen */
int vsnprintf(char *out, unsigned int size, const char *format, void *arguments)
{
	(void)format; (void)arguments;
	if (size)
		out[0] = 0;
	return 0;
}
