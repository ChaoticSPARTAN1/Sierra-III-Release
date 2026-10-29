/* The sleep switch held off while the game runs: a stopgap, with the
maintainer's OK (2026-10-28), until the proper fix for B41 is in.

B41: on b148, a plain sleep from the power switch crashed the PSP-1000 as
it went to sleep, and the crash landed while the firmware was writing its
own settings, which came back corrupted on the next boot. The proper fix
quiets the Media Engine and the GE before the kernel's sleep; until it is
in, the game asks the firmware not to sleep at all. The switch does
nothing while the game runs, and works again in the XMB once the game
exits. Keep word "sleepallowed" turns the hold off, for the proper fix's
own tests.

In a unit of its own, called with no branch at its two call sites, so the
release's profile still fits the code it was counted on. */

#include <psppower.h>

extern int psp_keep(const char *word);
extern void platform_log(const char *format, ...);

static int held;

/* once, after keep.txt is read */
void psp_sleepguard_start(void)
{
	int result;

	if (psp_keep("sleepallowed"))
	{
		platform_log("power: the sleep switch left working (sleepallowed); a sleep may crash the PSP (B41)");
		return;
	}
	result = scePowerLock(0);
	held = result >= 0;
	platform_log("power: the sleep switch held off while the game runs (B41's stopgap; keep word sleepallowed): %08x",
		(unsigned int)result);
}

/* every exit: the XMB has its switch back */
void psp_sleepguard_end(void)
{
	if (!held)
		return;
	held = 0;
	scePowerUnlock(0);
}
