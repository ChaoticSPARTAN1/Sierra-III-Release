/* psp_loading.h: the loading screen between the menu and a level, and
between levels (docs/plan_loading_screen.md). psp_loading.c keeps the
load's progress and time; the GE layer draws it (psp_ge_loading_*, the
renderer session's, linked weakly so the load runs without them). */
#ifndef PSP_LOADING_H
#define PSP_LOADING_H

struct psp_loading_view
{
	float progress;     /* 0..1, smoothed per unit of time, not per frame */
	float brightness;   /* the Xbox's sin(progress * pi)^0.9 */
	int phase;          /* 0 loading (over the captured frame), 1 settling (over the level's first view) */
	const void *captured; /* the frame shown when the load began, as psp_ge_loading_capture kept it; NULL on a cold start */
	const char *notice;   /* a line of text over the screen (the boot's hint), or NULL */
};

/* The load itself (FIXES_LOADING in psp_fixups.py calls these). */
void psp_loading_begin(const char *map_name);
void psp_loading_expect(unsigned long bytes);
void psp_loading_progress(unsigned long bytes);
void psp_loading_end(void);
int psp_loading_active(void);

/* The settle after the load (main.c's psp_main_settle draws its frames). */
int psp_loading_settle_begin(void);
int psp_loading_settle_frame(void);
void psp_loading_settle_veil(void);

/* The boot screen, from the program's start to the main menu (psp_platform.c,
main.c's boot path; the GE layer's psp_ge_boot_start, ge_initialize). */
void psp_loading_boot_begin(void);
void psp_loading_boot_menu_begin(void);
void psp_loading_boot_loaded(void);
void psp_loading_boot_frame(void);
int psp_loading_holds_input(void);
void psp_loading_boot_screen_stop(void);

/* The GE layer's side, weak: absent, nothing is drawn (as before). */
void psp_ge_loading_frame(const struct psp_loading_view *view) __attribute__((weak));
void psp_ge_loading_veil(const struct psp_loading_view *view) __attribute__((weak));
const void *psp_ge_loading_capture(void) __attribute__((weak));
int psp_ge_boot_start(void) __attribute__((weak));

#endif
