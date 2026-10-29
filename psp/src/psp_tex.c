/* psp_tex.c: textures for the GE renderer (build 20)

The map's bitmaps are baked offline (tools/texpack.py) into a texture pack,
ms0:/halo/maps/<map>.ptex: swizzled CLUT8 textures with their palettes and
mip chains, at a resolution chosen for the PSP's 272 lines. A texture is
found by the address of its bitmap_data in the tag window, which is fixed
for a map built for the PSP.

Textures live in two caches: the VRAM after the depth buffer (1,232 KB; the
on-screen log there stops once the GE runs), and the 4 MB volatile partition
(or 256 KB of heap without it) for what doesn't fit (the
GE reads either; VRAM is faster and stays off the CPU's bus). Each is filled
like a ring: a new texture goes after the last one, wrapping at the end, and
whatever it lands on is evicted, unless that texture is drawn in the current
frame (then the next cache is tried, or it waits a frame).
Loads are read from the Memory Stick with a budget per frame, so a new view
fills in over a few frames instead of stopping the game; until a texture
arrives its surfaces are drawn in their flat colour. */

#include <pspkernel.h>
#include <pspgu.h>
#include <string.h>
#include <math.h>
#include <stdlib.h>
#include <stdio.h>
#include <malloc.h>
#include <pspsuspend.h>
#include <pspdmac.h>
#include "psp_me.h" /* build 118: PSP_ME_WORK */
#include "psp_paths.h"   /* ms0:/halo or ms0:/halomp, and the maps */
#include "psp_bindblock.h"
#include "psp_metrics.h"

void platform_log(const char *format, ...);
void *tag_get(long group_tag, long tag_index);
int psp_keep(const char *name);
int psp_keep_number(const char *prefix, int fallback);
void psp_tex_light_report(void);

struct pack_entry
{
	unsigned int key;
	unsigned short width, height;
	unsigned char levels, psm, clut, flags;
	unsigned int offset, size;
};

#define VRAM_CACHE_START_FIXED 0x000D0000 /* after two frame buffers, the depth buffer and the motion sensor's 16 KB target (psp_ge.c) */
/* "blipdepth" (psp_ge.c, docs/review_vrbroom.md): the motion sensor's target
drawn in the depth buffer instead, so its 16 KB at 0x0CC000 join the room
shared by the textures and the copies */
#define VRAM_CACHE_START_BLIPDEPTH 0x000CC000
static unsigned int vram_cache_start = VRAM_CACHE_START_FIXED, vram_blip_freed = 0;
#define VRAM_CACHE_START vram_cache_start
#define VRAM_CACHE_END_FULL 0x00200000 /* the end of VRAM: the on-screen log stops once the GE runs (psp_platform.c) */
/* build 93: benchrun keeps VRAM's last 256 KB for its bus tests (the display
list in VRAM, the VRAM-only GE load; psp_ge.c), for the whole run so every
switch has the same texture placement */
#define VRAM_BENCH_BYTES 0x40000
static unsigned int vram_cache_end = VRAM_CACHE_END_FULL;
#define VRAM_CACHE_END vram_cache_end
/* the models' VRAM room by default, in KB: past 600 the texture cache is
too small for b30, which then draws untextured for seconds at a time */
#define VRAM_MODELS_DEFAULT_KB 600
#define VRAM_TEXTURES_LEAST (256 * 1024) /* the least VRAM the texture cache keeps beside the models' room */
/* "vrammodelsN": the top N KB of VRAM given to psp_ge.c's copies of the
most-read model buffers (psp_tex_vram_models) */
static unsigned int vram_models_bytes = 0;
#define LOAD_BUDGET_BYTES (96 * 1024) /* a frame's Memory Stick reads: ~7 ms */
#define STAGING_BYTES (32 * 1024) /* build 68: 32 (was 64): 32 KB of zeroed data back; the Memory Stick gives 14.5 MB/s at 32 KB against 15.7 at 64 (halo_bench v10) */

/* the lightmap page the next level surfaces use (rasterizer.c, psp_fixups.py) */
unsigned int psp_lightmap_key = 0;

/* a texture in its own colours (GU_PSM_5650 to GU_PSM_8888), not indices
into a palette; and the bytes a texel of a texture takes (T8 and direct) */
#define DIRECT(entry) ((entry)->psm <= GU_PSM_8888)
#define TEXEL_BYTES(entry) ((entry)->psm == GU_PSM_8888 ? 4u : DIRECT(entry) ? 2u : 1u)

static SceUID pack = -1;
static struct pack_entry *entries = NULL;
static int entry_count = 0;
/* each pack opened gets a number of its own (bindblocks: a block is its pack's) */
static unsigned int pack_generation = 0;
/* per entry: where it is in the cache (0 if not resident), the last frame
it was drawn in */
static unsigned int *resident_at = NULL, *last_frame = NULL;
static unsigned char *resident_in = NULL;
/* the textures the HUD and the other screen-space draws use (the meters,
the crosshair, the motion sensor, the text): kept as long as they're drawn
within RECENT_FRAMES, whatever the frame's other textures need, and read past
the frame's read budget when missing. The HUD draws last in a frame, so in
the next frame's first pass its textures were last frame's, and when both
caches were full of this frame's (a fight under load), the second pass
evicted them; the HUD then lost parts until they were read again, and at
low frame rates the budget was spent before the HUD's turn. 1: pinned.
"hudpinoff": as before */
static unsigned char *hud_pinned = NULL;
static int hud_pin_on = -1;
static void hud_pin_list_forget(void); /* hudpinme, below */
static unsigned long hud_misses = 0, hud_reads_past_budget = 0, hud_untextured = 0;
int psp_tex_screen_pass = 0; /* psp_ge.c: set while the screen-space draws are made */
/* build 44: binds per entry since the last report, to see which textures
the GE samples most and whether those are the ones in VRAM (the GE reads
VRAM about ten times faster than main memory: YAPSPD 11.7) */
static unsigned short *bind_counts = NULL;
/* build 46: binds per entry over every run on this map, learned into
ms0:/halo/maps/<map>.vram (the earlier runs' totals halved as they load, so
the counts follow how the map is played now); with keep.txt's "vramranked",
the next run's VRAM set is chosen from them instead of by size */
static unsigned int *bind_totals = NULL;
static char map_name[64];
/* per entry: its average colour (ABGR), from the pack's 'TAV1' table
(texpack.py, runtime_reads: worked out as the runtime sampled it until b144,
so nothing on the Media Engine reads a texture for it). averages holds the
ones asked for while the texture was resident, so psp_tex_average_known
answers after it has left; the ME's work writes it, on lines of its own */
static unsigned int *averages = NULL, *baked_averages = NULL;
static int averages_count = 0;
/* directory flags (texpack.py): the change mask's CLUT ends the texture's
data; the CPU reads its palette (the meters), so it's kept in main RAM, which
the Media Engine can reach */
#define CHANGE_MASK 8
#define CPU_READS 16
/* Chief's multipurpose maps: their reflection mask's CLUT just before the
change mask's (psp_tex_reflection_mask) */
#define REFL_MASK 32
/* the light gels' six face colours (psp_tex_gel_faces) */
#define GEL_FACES_MARK 0xFACE
#define GEL_FACES_KEPT 16
static struct
{
	unsigned int key;
	int known;
	float faces[18];
} gel_faces_kept[GEL_FACES_KEPT];
static int gel_faces_next = 0;
/* the light gels' kept cubes (psp_tex_gel_cube): a CLUT8 patch whose entry
255 is GEL_CUBE_MARK holds the six faces stacked, GEL_CUBE_SIDE a side
(texpack.py, gel_cube). The light pass samples one at every lit vertex, so
each is copied out of the uncached texture once, as colours, and kept by key */
#define GEL_CUBE_MARK 0xC0BE
#define GEL_CUBE_SIDE 16
#define GEL_CUBE_TEXELS (6 * GEL_CUBE_SIDE * GEL_CUBE_SIDE)
#define GEL_CUBES_KEPT 4
struct gel_cube
{
	unsigned int key;
	int known;
	unsigned short texels[GEL_CUBE_TEXELS]; /* GU_PSM_5650 */
};
static struct gel_cube gel_cubes_kept[GEL_CUBES_KEPT];
static int gel_cubes_next = 0;
/* the see-through shaders' tints (texpack.py), pairs of (the shader's tag
address, its tint ABGR) sorted by address. Bits of the address: 0, the tint
is half the colour, for the GE's colour doubling; 1, the tint is added to the
map rather than multiplied. A pack without them draws every shader untinted */
static unsigned int *tints = NULL;
static int tint_count = 0;
/* the shaders whose drawn map the bake chose over the runtime's smallest-
scale rule (texpack.py 'TND1'): triples of (the shader's tag address, with
the third word's flags in its low bits; the map's index in the shader's
maps; the tint for the rule's map, for the keep word drawnmapscale) sorted
by address. A pack without them leaves every shader to the rule */
static unsigned int *drawn_maps = NULL;
static int drawn_map_count = 0;
/* the shaders whose combiners read the angle a surface is seen at (texpack.py
'TNA1'): triples of (the shader's tag address with its tint's flags, the tint
face on, the tint edge on) sorted by address, blended per vertex by the angle
(psp_ge.c); without them those shaders draw at the middle angle's tint, so
Cortana's face-on body was as bright as her rim. "noangletint": the middle */
static unsigned int *angle_tints = NULL;
static int angle_tint_count = 0;
/* the see-through shaders drawn from a texture of their own (texpack.py
'TNC1', seethrough.py composite): pairs of (the shader's tag address, the
map the composite is drawn in place of) sorted by address; the texture is
under the shader's address | 1. One map and a tint can't hold a shader whose
look comes from two maps (a thruster's cone from one, its colour from
another). "nocomposite": drawn as one map and a tint, as without them */
static unsigned int *composites = NULL;
static int composite_count = 0;
static int drawn_map_scale(void);
/* build 39: the textures waiting for VRAM until the first frame (preload) */
static unsigned short vram_pending[512];
static int vram_pending_count = 0;

/* a cache: GE addresses [start, end), filled from next; the CPU writes
through write_offset (VRAM's or RAM's uncached view) */
struct arena
{
	unsigned int start, end, next, write_offset;
};
#define RAM_CACHE_BYTES (256 * 1024)
static struct arena arenas[2] = {
	{ 0x04000000 + VRAM_CACHE_START_FIXED, 0x04000000 + VRAM_CACHE_END_FULL, 0x04000000 + VRAM_CACHE_START_FIXED, 0x40000000 },
	{ 0, 0, 0, 0x40000000 } };
static void *ram_cache = NULL;
static unsigned int frame = 1, frame_loaded = 0;
static unsigned long loads = 0, load_bytes = 0, misses = 0, waits = 0, evictions = 0, preloaded = 0;
#define RECENT_FRAMES 60
/* this frame's textures: how many, how many bytes; the largest seen */
static unsigned long frame_set_bytes = 0, frame_set_count = 0, frame_set_peak = 0, frame_set_peak_count = 0;
/* build 67: frames in which a bind found its texture missing and couldn't
read it in time (the surface drew untextured that frame), and the longest run
of such frames in a row */
static unsigned long untextured_frames = 0, untextured_run = 0, untextured_run_worst = 0;
static int frame_untextured = 0;
static unsigned char staging[STAGING_BYTES] __attribute__((aligned(64)));

static int alignment(unsigned int size)
{
	return (size + 63) & ~63u;
}

static void bind_totals_save(void);
static void bind_totals_gather(void);

/* "stripcensus" (docs/review_vrbroom.md): which textures the HUD
binds, and which of them could live in the frame and depth buffers' padding
strips instead of the cache. Each buffer is 512 pixels a row of which 480
show, so pixels 480-511 of every row (64 bytes a row, 272 rows: 17 KB a
buffer) are VRAM that nothing draws or shows. The GE samples a texture with
any buffer width, so a texture fits a strip when a row of it takes at most 64
bytes at a buffer width of 1,024 bytes: T8 up to 64 texels wide, 16-bit up to
32, 32-bit up to 16 (T4's 2,048-texel width is past the GE's 2,044), its
levels stacked down the strip, unswizzled. Its palette can't follow: a
palette is read as one run of bytes. Per entry: the frames it was bound for
the HUD, and the last such frame */
static int strip_census = -1;
static unsigned int *hud_bound = NULL;
static int find(unsigned int key);
long psp_game_time_peek(void);

/* "texlinecheck": a probe of the hazard texlines removes, on any layout. One
word the Media Engine's replay counts on each bind of a resident texture (as
it writes last_frame[] and bind_counts[]), and one word the main CPU counts
on each game tick, which runs while the ME replays (geinflight). With
"texlinesbad" both words share one 64-byte line, as a calloc'd table's edge
line shares one with a heap chunk header; without it each is on a line of
its own, as texlines puts the tables. A third line, the ME's alone, keeps the
ME's count again. After each frame back from the ME: the main CPU's word
should hold its last count, and the ME's its own copy's; a word that doesn't
was put back by the other CPU's write-back of the whole line. The emulator
has no caches: 0 both ways on either layout */
static unsigned int texline_probe[3][16] __attribute__((aligned(64)));
static PSP_MAIN_SIDE int texline_on = -1;
static PSP_MAIN_SIDE unsigned int texline_main_count = 0, texline_checks = 0, texline_main_back = 0,
	texline_me_lost = 0, texline_me_writes = 0, texline_events = 0;
/* the ME's word and its copy (NULL: off); set once by the main CPU, read by the replay */
static PSP_MAIN_SIDE volatile unsigned int *texline_me = NULL, *texline_me_copy = NULL;
static PSP_MAIN_SIDE volatile unsigned int *texline_main = NULL;

/* each game tick, on the main CPU */
void psp_texline_tick(void)
{
	extern int psp_keep_loaded(void);

	if (texline_on < 0)
	{
		if (!psp_keep_loaded())
			return;
		texline_on = psp_keep("texlinecheck") != 0;
		if (texline_on)
		{
			int bad = psp_keep("texlinesbad") != 0;

			memset(texline_probe, 0, sizeof(texline_probe));
			texline_main = bad ? &texline_probe[0][8] : &texline_probe[1][0];
			texline_me_copy = &texline_probe[2][0];
			texline_me = &texline_probe[0][0];
			platform_log("textures: texlinecheck: the ME's bind word and the main CPU's tick word %s",
				bad ? "on one 64-byte line (texlinesbad: the hazard forced)" : "on lines of their own (as texlines)");
		}
	}
	if (texline_main)
		*texline_main = ++texline_main_count;
}

/* the frame back from the ME (psp_ge.c, after its whole-cache write-back) */
void psp_texline_check(void)
{
	unsigned int main_word, me_word, me_copy;

	if (!texline_main)
		return;
	main_word = *texline_main;
	me_word = *texline_me;
	me_copy = *texline_me_copy;
	texline_checks++;
	texline_me_writes = me_copy;
	if (main_word != texline_main_count || me_word != me_copy)
	{
		if (main_word != texline_main_count)
			texline_main_back++;
		if (me_word != me_copy)
			texline_me_lost++;
		if (texline_events++ < 8)
			platform_log("textures: texlinecheck: check %u, game time %ld: the main CPU's word %u (wrote %u), the ME's %u (its copy %u)",
				texline_checks, psp_game_time_peek(), main_word, texline_main_count, me_word, me_copy);
		/* both words as they should be, for the next frame's look */
		*texline_main = texline_main_count;
		*texline_me = me_copy;
	}
	if (texline_checks % 300 == 0)
		platform_log("textures: texlinecheck: %u checks; the main CPU's word put back %u times, the ME's count lost %u times; %u ME writes",
			texline_checks, texline_main_back, texline_me_lost, texline_me_writes);
}

/* a table the Media Engine's replay writes (last_frame, bind_counts: a bind
on the ME, psp_ge.c's geinflight): on whole 64-byte lines of its own. The ME
writes back whole lines from its own cache, so a table sharing its first or
last line with a heap neighbour would put that line back as it was at the
hand-over, over the main CPU's newer bytes there: a heap chunk's header,
changed by a malloc or free while the replay ran, comes back stale.
"texlinesold": calloc's 16-byte alignment, as before */
static void *line_calloc(size_t count, size_t size)
{
	size_t bytes = (count * size + 63) & ~(size_t)63;
	void *table;

	if (psp_keep("texlinesold"))
		return calloc(count, size);
	table = memalign(64, bytes ? bytes : 64);
	if (table)
		memset(table, 0, bytes ? bytes : 64);
	return table;
}

static void strip_census_free(void)
{
	free(hud_bound);
	hud_bound = NULL;
}

void psp_tex_hud_note(unsigned int key)
{
	int index;

	if (strip_census < 0)
		strip_census = psp_keep("stripcensus") != 0;
	if (!strip_census || !entries || (index = find(key)) < 0)
		return;
	if (!hud_bound && !(hud_bound = line_calloc(entry_count, 2 * sizeof(unsigned int))))
		return;
	if (hud_bound[2 * index + 1] != frame)
	{
		hud_bound[2 * index]++;
		hud_bound[2 * index + 1] = frame;
	}
}

/* the rows a strip would need for the entry's levels, 0 when it can't fit */
static unsigned int strip_rows(struct pack_entry const *entry)
{
	unsigned int width = entry->width, height = entry->height, rows = 0;
	int level;

	if (entry->psm == GU_PSM_T4 || (entry->psm == GU_PSM_T8 ? width : width * TEXEL_BYTES(entry)) > 64)
		return 0;
	for (level = 0; level < entry->levels; level++)
	{
		rows += height;
		height = height > 1 ? height / 2 : 1;
	}
	return rows <= 272 ? rows : 0;
}

static void strip_census_report(unsigned int frames)
{
	char line[440]; /* the log's lines end at 512 characters */
	int index, length = 0, count = 0, every = 0, fit = 0;
	unsigned int bytes = 0, vram = 0, every_bytes = 0, fit_bytes = 0, fit_vram = 0, fit_palettes = 0, fit_rows = 0;

	if (!hud_bound)
		return;
	for (index = 0; index < entry_count; index++)
	{
		struct pack_entry const *entry = &entries[index];
		unsigned int size = alignment(entry->size), rows = strip_rows(entry), palette;
		int in_vram = resident_at[index] && !resident_in[index];

		if (!hud_bound[2 * index])
			continue;
		palette = DIRECT(entry) ? 0 : (entry->clut == GU_PSM_8888 ? 4 : 2) * (entry->psm == GU_PSM_T4 ? 16 : 256);
		count++;
		bytes += size;
		if (in_vram)
			vram += size;
		if (hud_bound[2 * index] * 10 >= frames * 9)
		{
			every++;
			every_bytes += size;
		}
		if (rows)
		{
			fit++;
			fit_bytes += size - palette;
			fit_palettes += palette;
			fit_rows += rows;
			if (in_vram)
				fit_vram += size;
		}
		if (length < (int)sizeof(line) - 72)
			length += snprintf(line + length, sizeof(line) - length, " %08x %ux%u psm%u l%u %uB %s %u %s;", entry->key,
				entry->width, entry->height, entry->psm, entry->levels, size, resident_at[index] ?
				(resident_in[index] ? "RAM" : "VRAM") : "out", hud_bound[2 * index], rows ? "fits" : "-");
		hud_bound[2 * index] = 0;
	}
	platform_log("textures: stripcensus: %d HUD textures over %u frames, %u KB (%u KB in VRAM); %d bound in 9 frames of 10 or more (%u KB); %d would fit the padding strips (%u KB of texels, %u KB of palettes that can't follow, %u rows of 816; %u KB of them in VRAM now)",
		count, frames, bytes / 1024, vram / 1024, every, every_bytes / 1024, fit, fit_bytes / 1024, fit_palettes / 1024,
		fit_rows, fit_vram / 1024);
	if (length)
		platform_log("textures: stripcensus (key size psm levels bytes place frames fit):%s", line);
}
/* build 89: the heartbeat's in-play save (psp_tex_save_pending) */
static volatile int snapshot_pending = 0, snapshot_writing = 0;

void psp_tex_close(void)
{
	/* build 47: the learned binds saved as the map closes (b46 wrote them
	every 2,700 frames during play: a 40-56 ms pause each time) */
	bind_totals_gather();
	/* build 89: not over the heartbeat's in-play save of the same file */
	{
		int k;

		snapshot_pending = 0;
		for (k = 0; snapshot_writing && k < 100; k++)
			sceKernelDelayThread(2000);
	}
	bind_totals_save();
	if (pack >= 0)
		sceIoClose(pack);
	pack = -1;
	free(entries);
	free(resident_at);
	free(last_frame);
	free(resident_in);
	free(hud_pinned);
	hud_pinned = NULL;
	hud_pin_list_forget();
	free(averages);
	averages = NULL;
	averages_count = 0;
	free(baked_averages);
	baked_averages = NULL;
	memset(gel_faces_kept, 0, sizeof(gel_faces_kept));
	memset(gel_cubes_kept, 0, sizeof(gel_cubes_kept));
	free(tints);
	tints = NULL;
	tint_count = 0;
	free(angle_tints);
	angle_tints = NULL;
	angle_tint_count = 0;
	free(composites);
	composites = NULL;
	composite_count = 0;
	free(drawn_maps);
	drawn_maps = NULL;
	drawn_map_count = 0;
	vram_pending_count = 0;
	entries = NULL;
	resident_at = last_frame = NULL;
	resident_in = NULL;
	free(bind_counts);
	bind_counts = NULL;
	free(bind_totals);
	bind_totals = NULL;
	entry_count = 0;
	strip_census_free();
	/* the sensor's 16 KB, when psp_ge.c draws its target in the depth buffer
	(never under benchrun, whose rooms are fixed) */
	{
		extern int psp_ge_blip_room_freed(void);

		vram_blip_freed = !psp_keep("benchrun") && psp_ge_blip_room_freed() ? VRAM_CACHE_START_FIXED - VRAM_CACHE_START_BLIPDEPTH : 0;
		vram_cache_start = VRAM_CACHE_START_FIXED - vram_blip_freed;
		arenas[0].start = 0x04000000 + VRAM_CACHE_START;
	}
	/* build 103: and 256 KB below that for the level's vertex copies
	(psp_ge.c's setting 25, benchrun's "level vertices in VRAM") */
	if (psp_keep("benchrun"))
		vram_cache_end = VRAM_CACHE_END_FULL - VRAM_BENCH_BYTES - 0x40000;
	/* build 115: "vramtexN" (big_levers.md section 0, the swap's first step):
	the VRAM texture cache cut to N KB, the rest of VRAM left empty, so the
	textures past it come from the RAM cache instead. The A/B for whether the
	GE reads texels from main RAM at no cost on b30, as it did on Blood Gulch
	(gebusy's time by kind is the measure; the caches hold less in all, so
	more textures stream, which iostats shows apart) */
	else
	{
		int kb = psp_keep_number("vramtex", 0);

		if (kb >= 64 && (unsigned int)kb * 1024 < VRAM_CACHE_END_FULL - VRAM_CACHE_START)
		{
			vram_cache_end = VRAM_CACHE_START + (unsigned int)kb * 1024;
			platform_log("textures: vramtex: the VRAM cache cut to %d KB (of %u)", kb, (VRAM_CACHE_END_FULL - VRAM_CACHE_START) / 1024);
		}
	}
	/* build 116: "vrammodelsN" (psp_ge.c, the model buffers' VRAM copies):
	the top N KB of VRAM leave the texture cache for the models, the VRAM
	swap's second step (b115 test 3 [M]: textures from main RAM cost the GE
	~1.5 ms a frame; the models' reads are most of the rest). Not with
	benchrun, whose rooms are up there.
	Build 117: 600 KB by default (b116 test 2 [M]: the models' GE time 31%
	less in b30's cinematic and 44% less in the flight after it, the flight
	16% faster; the maintainer's call, 2026-10-12). "vrammodels0" turns it off */
	vram_models_bytes = 0;
	if (!psp_keep("benchrun"))
	{
		int kb = psp_keep_number("vrammodels", VRAM_MODELS_DEFAULT_KB);

		/* at least 256 KB stay the textures': a 512 x 512 indexed map with its
		levels (341 KB) can still sit in VRAM, and below that the set's best
		third, worth most of its binds, would no longer fit */
		/* blipdepth's 16 KB go to the copies' room: a KB of VRAM holding
		geometry takes ~5.6 times the bus reads off the ticks that a KB holding
		textures does (docs/research/vram_budget.md section 3), and the
		texture cache keeps its size, so the same textures fit as without it */
		if (kb >= 64 && (unsigned int)kb * 1024 + vram_blip_freed <= VRAM_CACHE_END_FULL - VRAM_CACHE_START - VRAM_TEXTURES_LEAST)
		{
			vram_models_bytes = (unsigned int)kb * 1024 + vram_blip_freed;
			if (vram_cache_end > VRAM_CACHE_END_FULL - vram_models_bytes)
				vram_cache_end = VRAM_CACHE_END_FULL - vram_models_bytes;
			platform_log("textures: vrammodels: the top %u KB of VRAM for the model buffers; the texture cache %u KB",
				vram_models_bytes / 1024, (vram_cache_end - VRAM_CACHE_START) / 1024);
		}
		else if (!kb)
			platform_log("textures: vrammodels0: the model buffers stay in main RAM (as b116 without the word)");
		else
			/* said, not silently left off (the emulator's vrammodels800 run) */
			platform_log("textures: vrammodels%d left off: 64 KB up to %u KB (the textures keep %u)", kb,
				(VRAM_CACHE_END_FULL - VRAM_CACHE_START - VRAM_TEXTURES_LEAST) / 1024, VRAM_TEXTURES_LEAST / 1024);
	}
	arenas[0].end = 0x04000000 + VRAM_CACHE_END;
	arenas[0].next = arenas[0].start;
	/* the VRAM it freed, per map */
	if (vram_blip_freed)
		platform_log("textures: blipdepth: the motion sensor's target in the depth buffer; %u KB freed at %06x, to the %s (%u KB; the texture cache %u KB)",
			vram_blip_freed / 1024, VRAM_CACHE_START, vram_models_bytes ? "copies' room" : "texture cache",
			vram_models_bytes ? vram_models_bytes / 1024 : (VRAM_CACHE_END - VRAM_CACHE_START) / 1024,
			(VRAM_CACHE_END - VRAM_CACHE_START) / 1024);
	arenas[1].next = arenas[1].start;
}

/* build 116: the VRAM given to the model buffers' copies ("vrammodelsN"):
its size, and its start as a GE address (0 bytes: none) */
unsigned int psp_tex_vram_models(unsigned int *start)
{
	*start = 0x04000000 + VRAM_CACHE_END_FULL - vram_models_bytes;
	return vram_models_bytes;
}

/* the RAM cache: the 4 MB volatile partition outside the game's 24 MB when the
system lends it (it is the sleep save area, so it is given back before the
PSP sleeps and its textures are reloaded after: they are only a cache), or
256 KB of the heap (build 22) */
static int volatile_locked = 0;

/* build 67: "texsqueeze" caps the RAM cache at 1 MB and "texvramonly" leaves
it empty, so a map that otherwise fits the caches whole (Blood Gulch: 5 MB in
1.2 MB of VRAM and the 4 MB partition, 0 evictions in b64's run) has to
stream textures from the Memory Stick during play. The first measure of
in-play streaming on hardware: loads, waits, evictions, the worst frame's
reading, and how many frames drew something untextured meanwhile */
#define SQUEEZED_BYTES (1024 * 1024)
static int squeezed = 0;
/* build 68: the geometry cache's share at the end of the block (psp_geo.c) */
static unsigned int ram_cache_bytes = 0, carved_bytes = 0;
/* the first 32 KB of the RAM block hold the button glyph sheet (psp_ge.c,
glyph_sheet), outside the texture cache. In VRAM the sheet cut the models'
room (vrammodels' check then refused its default); here it
costs the RAM cache 32 KB of its 4 MB. The block goes back to the system
while the PSP sleeps, so each acquisition has a generation, and psp_ge.c
unpacks the sheet again when it changes */
#define GLYPH_ROOM_BYTES (32 * 1024)
static unsigned int ram_cache_generation = 0;
/* after the glyph sheet, geinflight's record buffer when "inflightroom"
asks for it there (psp_ge.c, psp_ge_inflight_room_bytes): asked once, as
the block is first taken, so no texture is ever in it; only in the
volatile partition (the heap's 256 KB block keeps it all for textures) */
unsigned int psp_ge_inflight_room_bytes(void);
static int inflight_room_asked = 0;
static unsigned int inflight_room_bytes = 0;

static unsigned int front_bytes(void)
{
	return GLYPH_ROOM_BYTES + (volatile_locked ? inflight_room_bytes : 0);
}

static void ram_cache_acquire(void)
{
	void *block = NULL;
	int size = 0, given;

	if (sceKernelVolatileMemTryLock(0, &block, &size) == 0 && block && size > 0)
	{
		volatile_locked = 1;
		ram_cache = block;
	}
	else
	{
		size = RAM_CACHE_BYTES;
		ram_cache = memalign(64, size);
	}
	given = size;
	ram_cache_bytes = (unsigned int)size;
	if (!inflight_room_asked)
	{
		inflight_room_asked = 1;
		inflight_room_bytes = (psp_ge_inflight_room_bytes() + 63) & ~63u;
		if (inflight_room_bytes > ram_cache_bytes / 4)
			inflight_room_bytes = 0;
	}
	if (!squeezed)
		squeezed = psp_keep("texvramonly") ? 2 : psp_keep("texsqueeze") ? 1 : -1;
	if (squeezed == 1 && size > SQUEEZED_BYTES)
		size = SQUEEZED_BYTES;
	else if (squeezed == 2)
		size = 0;
	if ((unsigned int)size > ram_cache_bytes - carved_bytes)
		size = (int)(ram_cache_bytes - carved_bytes);
	if ((unsigned int)size < front_bytes())
		size = (int)front_bytes();
	if (ram_cache)
	{
		ram_cache_generation++;
		arenas[1].start = arenas[1].next = (unsigned int)ram_cache + front_bytes();
		arenas[1].end = (unsigned int)ram_cache + size;
		sceKernelDcacheWritebackInvalidateRange(ram_cache, given);
	}
	platform_log("textures: RAM cache %d KB (%s%s%s; the glyph sheet's 32 KB before it, then %u KB of geinflight's records)",
		(int)(size - front_bytes()) / 1024, volatile_locked ? "volatile partition" : "heap",
		squeezed == 1 ? ", squeezed by keep.txt" : squeezed == 2 ? ", left empty by keep.txt" : "",
		carved_bytes ? ", the rest the geometry cache's" : "", (front_bytes() - GLYPH_ROOM_BYTES) / 1024);
}

/* build 68: bytes at the end of the RAM block for the geometry cache (0 gives
them back). Called as a map's tags load, before its texture pack is read in,
so no texture is in the carved part; the block is acquired if it isn't yet */
void *psp_tex_carve(unsigned int bytes)
{
	int index;

	if (!ram_cache)
		ram_cache_acquire();
	if (!ram_cache || bytes > ram_cache_bytes - front_bytes())
		return NULL;
	carved_bytes = bytes;
	/* the cache runs to the carve, wherever the last map's carve left it: a
	smaller carve gives the room back (a level's 1-2 MB, then the main
	menu's own pack's size), as no carve at all always did */
	{
		unsigned int top = (unsigned int)ram_cache + ram_cache_bytes - carved_bytes;

		if (squeezed == 1 && top > arenas[1].start + SQUEEZED_BYTES)
			top = arenas[1].start + SQUEEZED_BYTES;
		else if (squeezed == 2)
			top = arenas[1].start;
		arenas[1].end = top;
	}
	if (arenas[1].next > arenas[1].end)
		arenas[1].next = arenas[1].start;
	/* anything resident past the end is forgotten */
	for (index = 0; index < entry_count; index++)
		if (resident_at && resident_at[index] && resident_in[index] == 1 &&
			resident_at[index] + alignment(entries[index].size) > arenas[1].end)
			resident_at[index] = 0;
	return bytes ? (void *)((unsigned int)ram_cache + ram_cache_bytes - carved_bytes) : NULL;
}

/* the glyph sheet's room at the RAM block's start (NULL without a block) and
the block's generation: a new one means the sheet must be unpacked again */
void *psp_tex_glyph_room(unsigned int *generation)
{
	if (!ram_cache)
		ram_cache_acquire();
	*generation = ram_cache_generation;
	return ram_cache;
}

/* the same, never asking for a block: for the Media Engine's work, which
can't make the system calls that asking takes */
void *psp_tex_glyph_room_held(unsigned int *generation)
{
	*generation = ram_cache_generation;
	return ram_cache;
}

/* geinflight's record buffer in the front room (NULL without one: no
volatile partition, asleep, or the room not asked for) and the block's
generation: records begun in an older one were in the block the system had
while the PSP slept */
void *psp_tex_inflight_room(unsigned int *generation)
{
	*generation = ram_cache_generation;
	if (!volatile_locked || !ram_cache || !inflight_room_bytes)
		return NULL;
	return (void *)((unsigned int)ram_cache + GLYPH_ROOM_BYTES);
}

/* the lowest carved address while the volatile partition is lent (NULL while
it isn't: asleep, or the heap's block instead), and the block's generation.
The level carve (psp_ge.c) is carved last, so it starts there; a new
generation says the block went back to the system and came again */
void *psp_tex_carve_now(unsigned int *generation)
{
	*generation = ram_cache_generation;
	if (!volatile_locked || !ram_cache || !carved_bytes)
		return NULL;
	return (void *)((unsigned int)ram_cache + ram_cache_bytes - carved_bytes);
}

/* the RAM the texture cache itself has now, for the level carve's log */
unsigned int psp_tex_ram_textures(void)
{
	return arenas[1].end > arenas[1].start ? arenas[1].end - arenas[1].start : 0;
}

/* build 106: the bytes carved now (the geometry cache's, then the animation
cache's on top: psp_anim.c carves this plus its own) */
unsigned int psp_tex_carved(void)
{
	return carved_bytes;
}

/* the PSP is about to sleep (psp_platform.c's power callback): the volatile
partition goes back to the system, and its textures are forgotten */
void psp_tex_suspend(void)
{
	int index;

	if (!volatile_locked)
		return;
	for (index = 0; index < entry_count; index++)
		if (resident_in && resident_at[index] && resident_in[index] == 1)
			resident_at[index] = 0;
	arenas[1].start = arenas[1].end = arenas[1].next = 0;
	sceKernelVolatileMemUnlock(0);
	volatile_locked = 0;
	ram_cache = NULL;
}

/* awake again: the partition back, empty */
void psp_tex_resume(void)
{
	if (!ram_cache)
		ram_cache_acquire();
}

static int load(int index);
static int load_into(int index, int only);
static void preload_vram(void);

/* build 112: textures that work marked as the Media Engine's needed and
couldn't load (the ME makes no system calls; gemesim, psp_ge.c): the main
CPU reads them at the frame's end (psp_tex_frame_end), within the frame's
budget, so they are there for the next frame. Until then their surfaces are
drawn in their flat colour, as for any texture still on its way */
extern volatile unsigned int psp_me_context;
#define WANTED_MAX 64
static unsigned short wanted[WANTED_MAX];
static int wanted_count = 0;
static unsigned long wanted_total = 0, wanted_dropped = 0;

static void want(int index)
{
	int k;

	for (k = 0; k < wanted_count; k++)
		if (wanted[k] == index)
			return;
	if (wanted_count < WANTED_MAX)
	{
		wanted[wanted_count++] = (unsigned short)index;
		wanted_total++;
	}
	else
		wanted_dropped++;
}
static int screen_free;

/* build 38: the whole pack read in at the start when it fits the caches
(Blood Gulch's 5.1 MB in 1.2 MB of VRAM and the 4 MB partition): the ring
of build 33 still evicted textures last drawn over two seconds ago to make
room when the cache could have held everything, and in the tank fight they
came back from the Memory Stick (216 evictions). Largest first, so the big
ones get VRAM; anything that doesn't fit is read when first drawn, as before */
/* an ordering of entries, scratch for preload() and the 900-frame report */
static unsigned short order[1024];

static void preload(int ranked)
{
	unsigned int total = 0, room, start = sceKernelGetSystemTimeLow();
	int index, loaded = 0, order_count = 0, k;

	for (index = 0; index < entry_count; index++)
		total += (entries[index].size + 63) & ~63u;
	room = (VRAM_CACHE_END - VRAM_CACHE_START) + (arenas[1].end - arenas[1].start);
	/* build 39: a pack a little over the caches (Blood Gulch's with the HUD
	at full size: 5322 KB of 5312) is still read in as far as it goes, the
	few left over read when first drawn; one far over is read as drawn */
	if (total > room + room / 8 || entry_count > 1024 || !arenas[1].start)
	{
		platform_log("textures: %u KB doesn't fit the caches' %u KB: read as drawn", total / 1024, room / 1024);
		return;
	}
	if (ranked)
	{
		/* build 46 ("vramranked"): by binds per byte of VRAM, with a
		texture's worth counted as binds x min(size, 16 KB): a bigger texture
		gives the GE more to read a draw, but a draw reads one mip level of
		it, not the whole chain. A guess to be measured against "largest
		first" (the GE's own time is the test: "waited for the GE"). Those
		never bound sort last, largest first, as before */
		static float density[1024];

		for (index = 0; index < entry_count; index++)
		{
			unsigned int size = alignment(entries[index].size);

			density[index] = bind_totals[index] ? (float)bind_totals[index] * (size < 16384 ? size : 16384) / size : 0.0f;
			for (k = order_count; k > 0 && (density[order[k - 1]] < density[index] ||
				(density[order[k - 1]] == density[index] && entries[order[k - 1]].size < entries[index].size)); k--)
				order[k] = order[k - 1];
			order[k] = (unsigned short)index;
			order_count++;
		}
	}
	else
	{
		/* build 79: "vramcold" fills VRAM smallest first, so the large
		textures (the level's, covering most of the screen) are drawn from
		RAM: the worst case of texturing from RAM, measured against the
		usual order to learn what VRAM is worth (a screen-size scratch for
		anti-aliasing would have to come out of it) */
		int cold = psp_keep("vramcold");

		if (cold)
			platform_log("textures: vramcold: VRAM filled smallest first; the large textures are drawn from RAM");
	for (index = 0; index < entry_count; index++)
	{
		/* by size, largest first (an insertion sort: a few hundred) */
		for (k = order_count; k > 0 && (cold ? entries[order[k - 1]].size > entries[index].size :
			entries[order[k - 1]].size < entries[index].size); k--)
			order[k] = order[k - 1];
		order[k] = (unsigned short)index;
		order_count++;
	}
	}
	/* build 39: the largest go to VRAM, but only once the GE draws: until
	then the start-up log on the screen is drawn into VRAM, over the cache
	(build 38's first frames: the level's textures were that log's text). So
	now the rest go to RAM, and the VRAM set is read at the first frame's end */
	{
		unsigned int vram = VRAM_CACHE_END - VRAM_CACHE_START, used = 0;

		vram_pending_count = 0;
		for (k = 0; k < order_count; k++)
		{
			unsigned int size = (entries[order[k]].size + 63) & ~63u;

			if (used + size <= vram && vram_pending_count < 512 && !(entries[order[k]].flags & CPU_READS))
			{
				used += size;
				vram_pending[vram_pending_count++] = order[k];
				continue;
			}
			if (load_into(order[k], 1))
			{
				/* counted as drawn now, so the next one's room never evicts it */
				last_frame[order[k]] = frame;
				loaded++;
				preloaded++;
			}
		}
	}
	frame_loaded = 0;
	platform_log("textures: %d of %d read in at the start (%u KB) in %u ms; %d more into VRAM at the first frame (chosen %s)",
		loaded, entry_count, total / 1024, (sceKernelGetSystemTimeLow() - start) / 1000, vram_pending_count,
		ranked ? "by learned binds per byte" : "largest first");
	if (screen_free && vram_pending_count)
		preload_vram();
}

/* the VRAM set, once the GE has the screen (psp_tex_frame_end) */
static void preload_vram(void)
{
	unsigned int start = sceKernelGetSystemTimeLow();
	int k, loaded = 0;

	for (k = 0; k < vram_pending_count; k++)
		if (!resident_at[vram_pending[k]] && load(vram_pending[k]))
		{
			last_frame[vram_pending[k]] = frame;
			loaded++;
			preloaded++;
		}
	platform_log("textures: %d of %d read into VRAM at the first frame in %u ms", loaded, vram_pending_count,
		(sceKernelGetSystemTimeLow() - start) / 1000);
	vram_pending_count = 0;
	frame_loaded = 0;
}

/* build 46: the learned bind counts (see bind_totals). The file: "VRM1",
the entry count, the sum of the entries' keys (so a re-baked pack starts
over), then a count per entry */
static unsigned int key_sum(void)
{
	unsigned int sum = 0;
	int index;

	for (index = 0; index < entry_count; index++)
		sum += entries[index].key;
	return sum;
}

static int bind_totals_load(void)
{
	char path[128];
	unsigned int header[3];
	SceUID file;
	int ok = 0, index;

	snprintf(path, sizeof(path), PSP_MAPS_ROOT "/%s.vram", map_name);
	file = sceIoOpen(path, PSP_O_RDONLY, 0);
	if (file < 0)
	{
		/* build 89: a save whose rename didn't happen (vram_write) */
		snprintf(path, sizeof(path), PSP_MAPS_ROOT "/%s.vram.tmp", map_name);
		file = sceIoOpen(path, PSP_O_RDONLY, 0);
	}
	if (file < 0)
		return 0;
	if (sceIoRead(file, header, sizeof(header)) == sizeof(header) && header[0] == 0x314D5256 /* VRM1 */ &&
		header[1] == (unsigned int)entry_count && header[2] == key_sum() &&
		sceIoRead(file, bind_totals, entry_count * sizeof(unsigned int)) == (int)(entry_count * sizeof(unsigned int)))
		ok = 1;
	sceIoClose(file);
	if (!ok)
	{
		memset(bind_totals, 0, entry_count * sizeof(unsigned int));
		platform_log("textures: %s is from another pack: learning again", path);
		return 0;
	}
	for (index = 0; index < entry_count; index++)
		bind_totals[index] /= 2;
	return 1;
}

/* this census period's binds into the totals (the census does the same) */
static void bind_totals_gather(void)
{
	int index;

	if (!bind_totals || !bind_counts)
		return;
	for (index = 0; index < entry_count; index++)
	{
		bind_totals[index] += bind_counts[index];
		bind_counts[index] = 0;
	}
}

/* build 89: <map>.vram written whole or not at all: to <map>.vram.tmp,
every write checked, then renamed over the old file (the loader takes the
.tmp if the rename didn't happen). b46-b88 truncated the file first and
reported success without checking the writes, so a failed or interrupted
save wiped what had been learned (review). The bytes written, or < 0 */
static int vram_write(const char *map, const unsigned int *totals, unsigned int entries, unsigned int sum)
{
	char path[128], temp[136];
	unsigned int header[3];
	SceUID file;
	int ok = 0;

	snprintf(path, sizeof(path), PSP_MAPS_ROOT "/%s.vram", map);
	snprintf(temp, sizeof(temp), "%s.tmp", path);
	file = sceIoOpen(temp, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
	if (file < 0)
		return file;
	header[0] = 0x314D5256;
	header[1] = entries;
	header[2] = sum;
	if (sceIoWrite(file, header, sizeof(header)) == (int)sizeof(header) &&
		sceIoWrite(file, totals, entries * sizeof(unsigned int)) == (int)(entries * sizeof(unsigned int)))
		ok = 1;
	sceIoClose(file);
	if (!ok)
	{
		sceIoRemove(temp);
		return -1;
	}
	sceIoRemove(path);
	if (sceIoRename(temp, path) < 0)
		return -2;
	return (int)(sizeof(header) + entries * sizeof(unsigned int));
}

/* build 88: the learned binds saved DURING play. The save at the map's
close and at HOME (b47) never reached the Memory Stick on hardware: every
log from b70 to b87 says "not yet saved", so the VRAM set has been largest
first since the b68 packs replaced b46's file, and b87's bench found 90% of
the texture binds served from RAM (b46, ranked: 88% from VRAM). Here the
game thread copies the totals at a census (cheap: a few KB) once the map
has been played for 1,800 frames and then every 9,000, and the heartbeat
thread writes the copy (psp_tex_save_pending): the Memory Stick's 40-56 ms
(b46) land on that thread, not on a frame */
static unsigned int *snapshot = NULL, snapshot_capacity = 0, snapshot_entries = 0, snapshot_sum = 0;
static char snapshot_name[64];
static unsigned int map_open_frame = 0, last_snapshot_frame = 0, snapshots_taken = 0;

static void learned_snapshot(void)
{
	if (!bind_totals || !map_name[0] || snapshot_pending || entry_count <= 0)
		return;
	if (frame - map_open_frame < 1800)
		return;
	if (snapshots_taken && frame - last_snapshot_frame < 9000)
		return;
	if (snapshot_capacity < (unsigned int)entry_count)
	{
		free(snapshot);
		snapshot = malloc(entry_count * sizeof(unsigned int));
		snapshot_capacity = snapshot ? (unsigned int)entry_count : 0;
		if (!snapshot)
			return;
	}
	memcpy(snapshot, bind_totals, entry_count * sizeof(unsigned int));
	snapshot_entries = (unsigned int)entry_count;
	snapshot_sum = key_sum();
	snprintf(snapshot_name, sizeof(snapshot_name), "%s", map_name);
	last_snapshot_frame = frame;
	snapshots_taken++;
	snapshot_pending = 1;
}

/* the heartbeat thread (psp_platform.c), every heartbeat */
void psp_tex_save_pending(void)
{
	unsigned int start;
	int written;

	if (!snapshot_pending)
		return;
	start = sceKernelGetSystemTimeLow();
	snapshot_writing = 1;
	written = vram_write(snapshot_name, snapshot, snapshot_entries, snapshot_sum);
	snapshot_writing = 0;
	if (written > 0)
		platform_log("textures: learned binds saved during play to %s.vram (%d bytes) in %u ms; the next launch fills VRAM by them",
			snapshot_name, written, (sceKernelGetSystemTimeLow() - start) / 1000);
	else
		platform_log("textures: learned binds NOT saved to %s.vram (%08x: %s)", snapshot_name, (unsigned int)written,
			written == -1 ? "a write failed" : written == -2 ? "the rename failed; the .tmp will be read" : "couldn't open the file");
	snapshot_pending = 0;
}

/* the game is quitting (HOME; psp_platform.c's exit callback) */
void psp_tex_save_learned(void)
{
	bind_totals_gather();
	bind_totals_save();
}

static void bind_totals_save(void)
{
	unsigned int start = sceKernelGetSystemTimeLow();
	int written;

	if (!bind_totals || !map_name[0] || entry_count <= 0)
		return;
	written = vram_write(map_name, bind_totals, (unsigned int)entry_count, key_sum());
	platform_log("textures: learned binds written to %s.vram (%d%s) in %u ms", map_name, written,
		written > 0 ? " bytes" : ": FAILED", (sceKernelGetSystemTimeLow() - start) / 1000);
}

/* The packs a map may use, in the order they are tried. Build 102: the
asset track ("assettrack", launch menu), the map's <map>_track.ptex when
there is one, the same textures baked another way (lightmaps at half size),
so one game runs either data set; "assetdetail", <map>_detail.ptex, a pack
baked with some classes at full size (texpack.py --full-detail fp,...).
Before them, the main menu's ui_showcase.ptex (texpack.py --showcase: every
map at its own size, the objects and the sky in their own colours), unless
"noshowcase": the menu is the hardware's showcase (CLAUDE.md), and the pack
is read on ui.map alone, so the levels never pay for it. "showcasepal":
ui_showcase_pal.ptex instead, the same sizes on 256-colour palettes.
Last, the usual <map>.ptex. Returns 0 for a candidate whose keep word is off */
#define PACK_CANDIDATES 4
static int pack_candidate(const char *name, int which, char *path, int size, const char **word)
{
	static const char *const words[2] = { "assettrack", "assetdetail" }, *const suffixes[2] = { "_track", "_detail" };

	*word = NULL;
	if (which == 0)
	{
		if (strcmp(name, "ui") != 0 || psp_keep("noshowcase"))
			return 0;
		*word = psp_keep("showcasepal") ? "showcasepal" : "showcase";
		snprintf(path, size, PSP_MAPS_ROOT "/%s_%s.ptex", name, psp_keep("showcasepal") ? "showcase_pal" : "showcase");
	}
	else if (which < 3)
	{
		if (!psp_keep(words[which - 1]))
			return 0;
		*word = words[which - 1];
		snprintf(path, size, PSP_MAPS_ROOT "/%s%s.ptex", name, suffixes[which - 1]);
	}
	else
		snprintf(path, size, PSP_MAPS_ROOT "/%s.ptex", name);
	return 1;
}

/* build 105: the size of the pack psp_tex_map_opened will open, for the
geometry cache to place itself before the textures are opened (psp_geo.c).
A failed stat is tried twice more: in b104 run 1 it most likely failed for
lack of file descriptors, put the geometry cache in the heap and starved
the game of memory. Returns the size, or the last error (negative) */
long psp_tex_pack_bytes(const char *name, char *path, int size)
{
	long result = 0x80010002; /* not there */
	int which, attempt;
	const char *word;
	SceIoStat stat;

	for (which = 0; which < PACK_CANDIDATES; which++)
	{
		if (!pack_candidate(name, which, path, size, &word))
			continue;
		for (attempt = 0; attempt < 3; attempt++)
		{
			int got = sceIoGetstat(path, &stat);

			if (got >= 0)
				return (long)stat.st_size;
			result = got;
			if ((unsigned)got == 0x80010002)
				break;
			sceKernelDelayThread(20000);
		}
	}
	return result < 0 ? result : -1;
}

/* a map's tags were loaded: its texture pack, if there is one */
/* maps opened so far: caches keyed on a map's data (lightplan's surface
plans, psp_fixups.py) start again when it moves */
PSP_MAIN_SIDE unsigned long psp_tex_map_opens = 0;

void psp_tex_map_opened(const char *name)
{
	char path[128];
	unsigned int header[3];
	SceUID file = -1;
	int which;

	psp_tex_map_opens++;

	if (!name || !name[0])
		return;
	psp_tex_close();
	for (which = 0; which < PACK_CANDIDATES && file < 0; which++)
	{
		const char *word;

		if (!pack_candidate(name, which, path, sizeof(path), &word))
			continue;
		file = sceIoOpen(path, PSP_O_RDONLY, 0);
		if (word)
			platform_log("textures: %s: %s", word, file >= 0 ? path : "no such pack for this map: the usual one");
	}
	if (file < 0)
	{
		platform_log("textures: no %s (bake it with tools/texpack.py); surfaces stay flat", path);
		return;
	}
	if (sceIoRead(file, header, sizeof(header)) != sizeof(header) || header[0] != 0x31585450 /* PTX1 */ ||
		header[2] != header[1] * sizeof(struct pack_entry))
	{
		platform_log("textures: %s is not a texture pack", path);
		sceIoClose(file);
		return;
	}
	entries = malloc(header[2]);
	resident_at = calloc(header[1], sizeof(unsigned int));
	last_frame = line_calloc(header[1], sizeof(unsigned int));
	resident_in = calloc(header[1], 1);
	hud_pinned = calloc(header[1], 1);
	bind_counts = line_calloc(header[1], sizeof(unsigned short));
	free(averages);
	averages = line_calloc(header[1], sizeof(unsigned int));
	averages_count = averages ? (int)header[1] : 0;
	if (!ram_cache)
		ram_cache_acquire();
	if (!entries || !resident_at || !last_frame || !resident_in || sceIoRead(file, entries, header[2]) != (int)header[2])
	{
		platform_log("textures: can't read %s's directory", path);
		sceIoClose(file);
		psp_tex_close();
		return;
	}
	pack = file;
	entry_count = header[1];
	pack_generation++;
	{
		/* the tints, right after the directory when the pack has them
		('TNT1', a count, the pairs), then the drawn maps ('TND1', a count,
		the triples); the textures' data is read by offset, so packs without
		them read as before */
		unsigned int table[2];
		int tables = 0;

		if (sceIoRead(file, table, sizeof(table)) == (int)sizeof(table) && table[0] == 0x31544E54 /* TNT1 */ &&
			table[1] <= 4096)
		{
			tables = 1;
			if (table[1] > 0 && (tints = malloc(table[1] * 8)) != NULL)
			{
				if (sceIoRead(file, tints, table[1] * 8) == (int)(table[1] * 8))
					tint_count = (int)table[1];
				else
				{
					free(tints);
					tints = NULL;
					tables = 0;
				}
			}
		}
		if (tables && sceIoRead(file, table, sizeof(table)) == (int)sizeof(table) &&
			table[0] == 0x31444E54 /* TND1 */ && table[1] <= 4096)
		{
			tables = 2;
			if (table[1] > 0 && (drawn_maps = malloc(table[1] * 12)) != NULL)
			{
				if (sceIoRead(file, drawn_maps, table[1] * 12) == (int)(table[1] * 12))
					drawn_map_count = (int)table[1];
				else
				{
					free(drawn_maps);
					drawn_maps = NULL;
					tables = 0;
				}
			}
		}
		if (tables == 2 && sceIoRead(file, table, sizeof(table)) == (int)sizeof(table) &&
			table[0] == 0x31414E54 /* TNA1 */ && table[1] <= 4096)
		{
			tables = 3;
			if (table[1] > 0 && (angle_tints = malloc(table[1] * 12)) != NULL)
			{
				if (sceIoRead(file, angle_tints, table[1] * 12) == (int)(table[1] * 12))
					angle_tint_count = (int)table[1];
				else
				{
					free(angle_tints);
					angle_tints = NULL;
					tables = 0;
				}
			}
		}
		if (tables == 3 && sceIoRead(file, table, sizeof(table)) == (int)sizeof(table) &&
			table[0] == 0x31434E54 /* TNC1 */ && table[1] <= 4096)
		{
			tables = 4;
			if (table[1] > 0 && (composites = malloc(table[1] * 8)) != NULL)
			{
				if (sceIoRead(file, composites, table[1] * 8) == (int)(table[1] * 8))
					composite_count = (int)table[1];
				else
				{
					free(composites);
					composites = NULL;
					tables = 0;
				}
			}
			else if (table[1] > 0)
				tables = 0;
		}
		/* the averages ('TAV1', one a texture in directory order), since bake
		format 2; a pack without them has no averages: models' reflections
		stay out of their ambient and the flashlight's cone untinted */
		if (tables == 4 && sceIoRead(file, table, sizeof(table)) == (int)sizeof(table) &&
			table[0] == 0x31564154 /* TAV1 */ && table[1] == header[1] &&
			(baked_averages = malloc(table[1] * 4)) != NULL &&
			sceIoRead(file, baked_averages, table[1] * 4) != (int)(table[1] * 4))
		{
			free(baked_averages);
			baked_averages = NULL;
		}
		if (!baked_averages)
			platform_log("textures: no averages in this pack (baked before bake format 2): bake again with this build's tools");
		platform_log("textures: see-through tints for %d shaders%s; drawn maps chosen by the bake for %d%s", tint_count,
			tint_count ? "" : " (a pack baked before build 116: none)", drawn_map_count,
			drawn_map_count ? (drawn_map_scale() ? " (drawnmapscale: the smallest scale's instead)" : "") :
			" (none in this pack: the smallest scale's for all)");
		platform_log("textures: see-through composites for %d shaders%s", composite_count,
			composite_count && psp_keep("nocomposite") ? " (nocomposite: one map and a tint instead)" : "");
	}
	if (angle_tint_count)
		platform_log("textures: tints by angle for %d shaders%s", angle_tint_count,
			psp_keep("noangletint") ? " (noangletint: the middle angle's)" : "");
	platform_log("textures: %s, %d textures; caches %d KB of VRAM and %d KB of RAM", path, entry_count,
		(VRAM_CACHE_END - VRAM_CACHE_START) / 1024, (int)(arenas[1].end - arenas[1].start) / 1024);
	{
		int learned = 0, ranked;

		snprintf(map_name, sizeof(map_name), "%s", name);
		map_open_frame = frame;    /* build 88: the in-play save counts from here */
		snapshots_taken = 0;
		bind_totals = calloc(entry_count, sizeof(unsigned int));
		if (bind_totals)
			learned = bind_totals_load();
		/* build 47: by learned binds whenever they exist (b46's A/B: 13% ->
		88% of binds from VRAM, nothing visible changes); "vramlargest" in
		keep.txt goes back to largest first */
		ranked = learned && !psp_keep("vramlargest");
		platform_log("textures: learned binds %s; VRAM set %s", learned ? "read" : "not yet saved (learning this run; saved when the map closes or the game quits)",
			ranked ? "by binds per byte" : learned ? "largest first (vramlargest)" : "largest first until learned");
		preload(ranked);
	}
}

/* the keep word "drawnmapscale": every shader on the runtime's own rule,
drawn with the tint the bake worked out for that rule's map */
static int drawn_map_scale(void)
{
	static int on = -1;

	if (on < 0)
		on = psp_keep("drawnmapscale");
	return on;
}

/* the map a see-through shader's composite is drawn in place of, or -1 when
it has none (or "nocomposite", or "drawnmapscale": the composite is laid out
on the bake's choice of map, so it goes with that choice) */
int psp_tex_has(unsigned int key);

static int composite_map(unsigned int shader)
{
	static int off = -1;
	int low = 0, high = composite_count - 1;

	if (off < 0)
		off = psp_keep("nocomposite");
	if (off || drawn_map_scale())
		return -1;
	while (composites && low <= high)
	{
		int middle = (low + high) / 2;

		if (composites[2 * middle] == shader)
			return psp_tex_has(shader | 1) ? (int)composites[2 * middle + 1] : -1;
		if (composites[2 * middle] < shader)
			low = middle + 1;
		else
			high = middle - 1;
	}
	return -1;
}

/* the key of a see-through shader's composite when it has one, else key (the
map's own) */
unsigned int psp_tex_composite_key(unsigned int shader, unsigned int key)
{
	return composite_map(shader) >= 0 ? shader | 1 : key;
}

/* a shader's place in the pack's drawn maps, or -1 */
static int drawn_map_find(unsigned int shader)
{
	int low = 0, high = drawn_map_count - 1;

	while (drawn_maps && low <= high)
	{
		int middle = (low + high) / 2;
		unsigned int address = drawn_maps[3 * middle] & ~3u;

		if (address == shader)
			return middle;
		if (address < shader)
			low = middle + 1;
		else
			high = middle - 1;
	}
	return -1;
}

/* the map the bake chose for a see-through shader to draw (its index in the
shader's maps), or -1 for the runtime's smallest-scale rule */
int psp_tex_shader_map(unsigned int shader)
{
	int found = composite_map(shader);

	if (found >= 0)
		return found;
	found = drawn_map_scale() ? -1 : drawn_map_find(shader);
	return found < 0 ? -1 : (int)drawn_maps[3 * found + 1];
}

/* a see-through shader's tint (ABGR) by its tag address, white when the
pack has none for it, and its flags (bit 0 doubled, bit 1 added). Keep word
"notint": always white */
unsigned int psp_tex_shader_tint(unsigned int shader, int *flags)
{
	static int off = -1;
	int low = 0, high = tint_count - 1, found;

	if (flags)
		*flags = 0;
	if (off < 0)
		off = psp_keep("notint");
	/* a composite is the shader's whole colour already */
	if (off || composite_map(shader) >= 0)
		return 0xFFFFFFFF;
	if (drawn_map_scale() && (found = drawn_map_find(shader)) >= 0)
	{
		if (flags)
			*flags = (int)(drawn_maps[3 * found] & 3);
		return drawn_maps[3 * found + 2];
	}
	while (tints && low <= high)
	{
		int middle = (low + high) / 2;
		unsigned int address = tints[2 * middle] & ~3u;

		if (address == shader)
		{
			if (flags)
				*flags = (int)(tints[2 * middle] & 3);
			return tints[2 * middle + 1];
		}
		if (address < shader)
			low = middle + 1;
		else
			high = middle - 1;
	}
	return 0xFFFFFFFF;
}

/* a see-through shader's tints face on and edge on (ABGR, with the flags of
its psp_tex_shader_tint), or 0 when it has none (or "noangletint", "notint",
"drawnmapscale") */
int psp_tex_shader_angle_tints(unsigned int shader, unsigned int *face, unsigned int *edge)
{
	static int off = -1;
	int low = 0, high = angle_tint_count - 1;

	if (!angle_tints)
		return 0;
	if (off < 0)
		off = psp_keep("noangletint") || psp_keep("notint") || drawn_map_scale();
	while (!off && low <= high)
	{
		int middle = (low + high) / 2;
		unsigned int address = angle_tints[3 * middle] & ~3u;

		if (address == shader)
		{
			*face = angle_tints[3 * middle + 1];
			*edge = angle_tints[3 * middle + 2];
			return 1;
		}
		if (address < shader)
			low = middle + 1;
		else
			high = middle - 1;
	}
	return 0;
}

static int find(unsigned int key)
{
	/* after b116: where the last keys were found, by a few bits of the key
	(a draw asks for its texture 2-3 times: bind, size, residency); a place
	is used only if its entry still has that key, so nothing goes stale
	when a map's pack replaces the entries (an independent review) */
	/* build 118: one table a CPU, each on cache lines of its own: the
	Media Engine binds (gemeoverlap) while the main CPU asks for textures
	too (object lighting's lightmap samples) */
	static int recent_cpu[2][64] __attribute__((aligned(64)));
	int *recent = recent_cpu[PSP_ME_WORK() ? 1 : 0]; /* (the emulator's simulated ME work too, for its write trace) */
	int slot = (int)((key >> 4) & 63), low = 0, high = entry_count - 1, at = recent[slot];

	if (at < entry_count && entries[at].key == key)
		return at;
	while (low <= high)
	{
		int middle = (low + high) / 2;

		if (entries[middle].key == key)
		{
			recent[slot] = middle;
			return middle;
		}
		if (entries[middle].key < key)
			low = middle + 1;
		else
			high = middle - 1;
	}
	return -1;
}

/* room for size bytes in an arena without evicting any texture drawn in the
last `recent` frames (1: this frame only) */
static int protected_at(int index, int which, unsigned int recent)
{
	return resident_at[index] && resident_in[index] == which &&
		(last_frame[index] + recent > frame || (hud_pinned && hud_pinned[index] && last_frame[index] + RECENT_FRAMES > frame));
}

static unsigned int fits(int which, unsigned int start, unsigned int size, unsigned int recent)
{
	unsigned int end = start + size;
	int index;

	if (end > arenas[which].end)
		return 0;
	for (index = 0; index < entry_count; index++)
	{
		unsigned int at = resident_at[index];

		if (protected_at(index, which, recent) && at < end && at + alignment(entries[index].size) > start)
			return 0;
	}
	return 1;
}

/* at the ring position, the arena's start, or just after a protected
texture; whatever unprotected is in the chosen space is evicted */
void psp_ge_show3_reuse(int kind, unsigned int stamp); /* psp_ge.c: geshow3check */

/* the frame counter, for geshow3check's stamps (psp_ge.c) */
unsigned int psp_tex_frame_now(void)
{
	return frame;
}

static unsigned int allocate(int which, unsigned int size, unsigned int recent)
{
	struct arena *arena = &arenas[which];
	unsigned int start = 0, candidate;
	int index;

	if (!arena->start || size > arena->end - arena->start)
		return 0;
	if (fits(which, arena->next, size, recent))
		start = arena->next;
	else if (fits(which, arena->start, size, recent))
		start = arena->start;
	else
	{
		for (index = 0; index < entry_count && !start; index++)
		{
			if (!protected_at(index, which, recent))
				continue;
			candidate = resident_at[index] + alignment(entries[index].size);
			if (fits(which, candidate, size, recent))
				start = candidate;
		}
	}
	if (!start)
		return 0;
	for (index = 0; index < entry_count; index++)
	{
		unsigned int at = resident_at[index];

		if (at && resident_in[index] == which && at < start + size && at + alignment(entries[index].size) > start)
		{
			psp_ge_show3_reuse(0, last_frame[index]); /* geshow3check */
			resident_at[index] = 0;
			evictions++;
		}
	}
	arena->next = start + size;
	return start;
}

static unsigned long read_us = 0, frame_read_us = 0, frame_read_worst = 0;
extern volatile unsigned long psp_stick_read_us; /* build 126: every blocking Memory Stick read's time (psp_frametrace.c) */
/* build 73: this heartbeat window's reads, for the log's io: line */
static PSP_MAIN_SIDE unsigned long window_reads = 0, window_us = 0, window_worst_us = 0; /* build 118: the heartbeat resets them */
/* every read, deferral and wait so far: the loading screen's settle waits
for it to stop moving (psp_loading.c) */
unsigned long psp_tex_load_count(void)
{
	return loads + misses + waits;
}

/* frames that drew something untextured so far: "transwalk" reads it across
a BSP switch (psp_fixups.py, FIXES_TRANSITIONS) */
unsigned long psp_tex_untextured_frames(void)
{
	return untextured_frames;
}

void psp_tex_io_window(unsigned long *reads, unsigned long *us, unsigned long *worst_us)
{
	*reads = window_reads;
	*us = window_us;
	*worst_us = window_worst_us;
	window_reads = window_us = window_worst_us = 0;
}
static int dma_failures = 0;

/* only: -1 either cache (VRAM first), 0 VRAM only, 1 RAM only */
static int load_into(int index, int only);

static int load(int index)
{
	return load_into(index, entries[index].flags & CPU_READS ? 1 : -1);
}

extern volatile int psp_ge_show_busy; /* psp_ge.c */

static int load_into(int index, int only)
{
	struct pack_entry const *entry = &entries[index];
	unsigned int at = 0, done = 0, read_start;
	int which = 0;

	/* build 33: a texture drawn in the last two seconds isn't evicted while
	either cache (VRAM, then the RAM partition) has room elsewhere. Before,
	VRAM was a ring that evicted last frame's textures while the 4 MB in RAM
	sat mostly empty, and on the PSP surfaces vanished and came back as their
	textures were read from the Memory Stick again */
	if (only != 1)
		at = allocate(which = 0, alignment(entry->size), RECENT_FRAMES);
	if (!at && only != 0)
		at = allocate(which = 1, alignment(entry->size), RECENT_FRAMES);
	/* the frame the GE may still be drawing (geshow2: shown off the game
	thread) keeps its textures too */
	if (!at && only != 1)
		at = allocate(which = 0, alignment(entry->size), psp_ge_show_busy ? 2 : 1);
	if (!at && only != 0)
		at = allocate(which = 1, alignment(entry->size), psp_ge_show_busy ? 2 : 1);
	if (!at)
	{
		waits++;
		return 0;
	}
	read_start = sceKernelGetSystemTimeLow();
	if (sceIoLseek32(pack, entry->offset, PSP_SEEK_SET) != (int)entry->offset)
		return 0;
	while (done < entry->size)
	{
		unsigned int part = entry->size - done > STAGING_BYTES ? STAGING_BYTES : entry->size - done;

		if (sceIoRead(pack, staging, part) != (int)part)
		{
			platform_log("textures: reading texture %d failed", index);
			return 0;
		}
		/* build 46: by DMA (2.6 to 3 times memcpy's speed into VRAM:
		halo_bench v9), the staging buffer written back from the CPU's cache
		first; the cache never holds the caches' lines (written only through
		the uncached view, and written back and invalidated when acquired), so
		nothing stale is written over the copy later. memcpy through the
		uncached view if the DMA call fails */
		sceKernelDcacheWritebackRange(staging, part);
		if (dma_failures || sceDmacMemcpy((void *)(at + done), staging, part) < 0)
		{
			if (!dma_failures++)
				platform_log("textures: sceDmacMemcpy failed; copying with the CPU");
			memcpy((void *)((at + done) | arenas[which].write_offset), staging, part);
		}
		done += part;
	}
		/* build 35: the Memory Stick's time, measured (about 1.2 MB/s expected) */
	read_us += sceKernelGetSystemTimeLow() - read_start;
	frame_read_us += sceKernelGetSystemTimeLow() - read_start;
	psp_stick_read_us += sceKernelGetSystemTimeLow() - read_start; /* build 126: frametrace */
	{
		unsigned long took = sceKernelGetSystemTimeLow() - read_start;

		window_reads++;
		window_us += took;
		if (took > window_worst_us)
			window_worst_us = took;
		PSP_METRICS_STICK(PSP_METRICS_TEXTURES, entry->size, took);
	}
	resident_at[index] = at;
	resident_in[index] = (unsigned char)which;
	loads++;
	load_bytes += entry->size;
	frame_loaded += entry->size;
	return 1;
}

/* the bitmap_data of a bitmap tag's bitmap: the key a texture is found by */
unsigned int psp_tex_bitmap_key(long bitmap_tag_index, int bitmap)
{
	char *group;
	long count;
	char *address;

	if (bitmap_tag_index == -1 || !entries)
		return 0;
	group = tag_get(0x6269746D /* bitm */, bitmap_tag_index);
	if (!group)
		return 0;
	count = *(long *)(group + 96);
	address = *(char **)(group + 100);
	if (bitmap < 0 || bitmap >= count || !address)
		return 0;
	return (unsigned int)(address + 48 * bitmap);
}

/* how many bitmaps a bitmap tag holds (a numeric shader's digits; build 25) */
int psp_tex_bitmap_count(long bitmap_tag_index)
{
	char *group;

	if (bitmap_tag_index == -1)
		return 0;
	group = tag_get(0x6269746D /* bitm */, bitmap_tag_index);
	return group ? (int)*(long *)(group + 96) : 0;
}

/* build 102: benchrun's stand-in texture (psp_ge.c's setting 23): 1, every
bind; 2, the lightmaps' only. A 16 x 16 8-bit texture and its palette at the
start of benchrun's VRAM room (contents don't matter: the colours are
whatever is there), so the GE reads no texels from main RAM */
int psp_tex_standin = 0;
static int binding_lightmap = 0;
int psp_tex_bind(unsigned int key, int *has_alpha);
#define STANDIN_ADDRESS (0x04000000 + 0x001C0000)

static int bind_standin(struct pack_entry const *entry, int *has_alpha)
{
	sceGuClutMode(GU_PSM_5650, 0, 0xFF, 0);
	sceGuClutLoad(256 * 2 / 32, (void *)STANDIN_ADDRESS);
	sceGuTexMode(GU_PSM_T8, 0, 0, 1);
	sceGuTexImage(0, 16, 16, 16, (void *)(STANDIN_ADDRESS + 512));
	sceGuTexFilter(GU_LINEAR, GU_LINEAR);
	if (has_alpha)
		*has_alpha = entry->flags & 1;
	return 1;
}

int psp_tex_bind_lightmap(unsigned int key)
{
	int bound;

	binding_lightmap = 1;
	bound = psp_tex_bind(key, NULL);
	binding_lightmap = 0;
	return bound;
}

/* build 114: "statememo": a bind of the texture the GE already has is left
out of the list. A bind is about 28 command words (the palette, the mode,
four a mip level, the filter); b113's Test 3 put the model parts' binds,
state and lighting at 8-12 ms a frame on the PSP. The memo is the texture
and its address, for this frame only, and every other texture-state call
clears it: the five functions are wrapped (psp_build.py links them with
--wrap), and the bind itself calls the real ones. Never in the Media
Engine's work (its writes go to its own cache). The bind's bookkeeping
(residency, this frame's set, the counts) runs either way */
void __real_sceGuTexImage(int mipmap, int width, int height, int tbw, const void *tbp);
void __real_sceGuTexMode(int tpsm, int maxmips, int mc, int swizzle);
void __real_sceGuClutLoad(int num_blocks, const void *cbp);
void __real_sceGuClutMode(unsigned int cpsm, unsigned int shift, unsigned int mask, unsigned int csa);
void __real_sceGuTexFilter(int min, int mag);
static unsigned int memo_key = 0, memo_address = 0, memo_frame = 0;
static int memo_on = -1;
unsigned long psp_tex_binds_memo = 0, psp_tex_binds_sent = 0;

void __wrap_sceGuTexImage(int mipmap, int width, int height, int tbw, const void *tbp)
{
	memo_key = 0;
	__real_sceGuTexImage(mipmap, width, height, tbw, tbp);
}
void __wrap_sceGuTexMode(int tpsm, int maxmips, int mc, int swizzle)
{
	memo_key = 0;
	__real_sceGuTexMode(tpsm, maxmips, mc, swizzle);
}
void __wrap_sceGuClutLoad(int num_blocks, const void *cbp)
{
	memo_key = 0;
	__real_sceGuClutLoad(num_blocks, cbp);
}
void __wrap_sceGuClutMode(unsigned int cpsm, unsigned int shift, unsigned int mask, unsigned int csa)
{
	memo_key = 0;
	__real_sceGuClutMode(cpsm, shift, mask, csa);
}
void __wrap_sceGuTexFilter(int min, int mag)
{
	memo_key = 0;
	__real_sceGuTexFilter(min, mag);
}

/* "bindblocks": a resident texture's bind words (its palette, mode, levels
and filter: 8 words and 4 a level) written once into a block of their own and
CALLed, instead of sent by a dozen pspgu calls a bind. The PSP-1000 puts the
model parts' binds at 2.6 ms a frame on b30's flight (probes), ~11 us a
bind; leaving 8% of them out (statememo, b114) saved
nothing measurable, so the cost is in each bind. The words are pspgu's own
(pspsdk's sceGuClutMode, ClutLoad, TexMode, TexImage, TexFilter, flushes
included), so the GE gets the same state; the draw hash follows the CALL and
proves it. The only pspgu state the calls keep (the context's texture_mode)
nothing reads. It is the texture slot table phase E needs, in a form the GE
reads: a block holds where its texture sits, so it is good while that holds
(the texture's index and address), and a slot is reused only after
BIND_BLOCK_QUARANTINE frames, as a list the GE is still drawing may CALL it.
Never in the Media Engine's work. A block is written back from the data cache
once, when it's made */
#define BIND_BLOCK_WORDS 48 /* 8 + 4 a level (8 at most) + RET, to whole cache lines */
/* b30's beach binds more than 96 textures in two frames (lab bindblk1: up to
150 binds a frame found every slot in use) */
#define BIND_BLOCKS 192
#define BIND_BLOCK_QUARANTINE 2
struct bind_block
{
	int index;
	unsigned int address, frame;
};
static int bind_blocks_on = -1;
static unsigned int (*bind_block_words)[BIND_BLOCK_WORDS] = NULL;
static struct bind_block bind_block_meta[BIND_BLOCKS];
static short *bind_block_of = NULL; /* per entry: its block, or -1 */
static int bind_block_entries = 0;
static unsigned int bind_block_generation = 0;
static unsigned long bind_blocks_called = 0, bind_blocks_made = 0, bind_blocks_full = 0;
static unsigned int bind_blocks_full_frame = 0;

/* the block for this entry at this address, made if need be; NULL when
every slot is in use or still quarantined */
static const unsigned int *bind_block(int index, struct pack_entry const *entry, unsigned int address,
	unsigned int palette_bytes)
{
	int slot, oldest = -1;
	unsigned int *w;

	if (bind_block_generation != pack_generation || bind_block_entries != entry_count)
	{
		/* a new pack: the entries' blocks forgotten */
		free(bind_block_of);
		bind_block_of = malloc(entry_count * sizeof(*bind_block_of));
		if (!bind_block_of)
		{
			bind_block_entries = 0;
			return NULL;
		}
		memset(bind_block_of, 0xFF, entry_count * sizeof(*bind_block_of));
		/* -1 free; -2 the last pack's, free once out of quarantine (a list
		still in flight may CALL it) */
		for (slot = 0; slot < BIND_BLOCKS; slot++)
			bind_block_meta[slot].index = bind_block_entries && bind_block_meta[slot].index != -1 ? -2 : -1;
		bind_block_entries = entry_count;
		bind_block_generation = pack_generation;
		bind_blocks_full_frame = 0;
	}
	slot = bind_block_of[index];
	if (slot >= 0 && bind_block_meta[slot].index == index && bind_block_meta[slot].address == address)
	{
		bind_block_meta[slot].frame = frame;
		return bind_block_words[slot];
	}
	/* every slot was in use at this frame's last look, and none comes out
	of quarantine until the frame changes: no search */
	if (bind_blocks_full_frame == frame)
	{
		bind_blocks_full++;
		return NULL;
	}
	/* a free slot, else the longest unused one out of quarantine */
	for (slot = 0; slot < BIND_BLOCKS; slot++)
	{
		if (bind_block_meta[slot].index == -1)
		{
			oldest = slot;
			break;
		}
		if (bind_block_meta[slot].frame + BIND_BLOCK_QUARANTINE > frame)
			continue;
		if (oldest < 0 || bind_block_meta[slot].frame < bind_block_meta[oldest].frame)
			oldest = slot;
	}
	if (oldest < 0)
	{
		bind_blocks_full++;
		bind_blocks_full_frame = frame;
		return NULL;
	}
	slot = oldest;
	if (bind_block_meta[slot].index != -1)
		psp_ge_show3_reuse(1, bind_block_meta[slot].frame); /* geshow3check */
	if (bind_block_meta[slot].index >= 0 && bind_block_meta[slot].index < bind_block_entries &&
		bind_block_of[bind_block_meta[slot].index] == slot)
		bind_block_of[bind_block_meta[slot].index] = -1;
	w = bind_block_words[slot];
	bind_block_fill(w, entry->clut, entry->psm, entry->levels, entry->width, entry->height, address, palette_bytes);
	sceKernelDcacheWritebackRange(w, BIND_BLOCK_WORDS * 4);
	bind_block_meta[slot].index = index;
	bind_block_meta[slot].address = address;
	bind_block_meta[slot].frame = frame;
	bind_block_of[index] = (short)slot;
	bind_blocks_made++;
	return w;
}

/* makes the texture the GE's current one; 0 if it isn't there (yet) */
#ifdef PSP_RECON
/* "txptcensus" (recon; psp_tex_pretouch): which textures the replay bound this frame */
static int txc_on = -1;
static struct pack_entry const *txc_for = NULL;
static unsigned int *txc_bound_at = NULL;
#define TXC_BOUND(index) do { if (txc_on > 0 && txc_for == entries) txc_bound_at[index] = frame; } while (0)
#else
#define TXC_BOUND(index) do { } while (0)
#endif
int psp_tex_bind(unsigned int key, int *has_alpha)
{
	int index, level;
	struct pack_entry const *entry;
	unsigned int palette_bytes, address, start, width, height;

	if (!key || !entries)
		return 0;
	index = find(key);
	if (index < 0)
		return 0;
	TXC_BOUND(index);
	entry = &entries[index];
	if (psp_tex_standin == 1 || (psp_tex_standin == 2 && binding_lightmap))
		return bind_standin(entry, has_alpha);
	if (hud_pin_on < 0)
		hud_pin_on = !psp_keep("hudpinoff");
	if (!resident_at[index])
	{
		int screen = psp_tex_screen_pass, past_budget = screen && hud_pin_on > 0;

		misses++;
		if (PSP_ME_WORK())
		{
			want(index); /* build 112 */
			frame_untextured = 1;
			return 0;
		}
		if (screen)
		{
			hud_misses++;
			if (frame_loaded >= LOAD_BUDGET_BYTES && past_budget)
				hud_reads_past_budget++;
		}
		if ((frame_loaded >= LOAD_BUDGET_BYTES && !past_budget) || !load(index))
		{
			if (screen)
				hud_untextured++;
			frame_untextured = 1;
			return 0;
		}
	}
	if (psp_tex_screen_pass && hud_pin_on > 0 && hud_pinned && !PSP_ME_WORK())
		hud_pinned[index] = 1;
	/* stored only when it changes: a texture marked before the Media Engine
	binds it (mechunk) leaves the line holding its word unwritten by the ME,
	while the main CPU may write the words beside it */
	if (last_frame[index] != frame)
	{
		frame_set_bytes += alignment(entry->size);
		frame_set_count++;
		last_frame[index] = frame;
	}
	if (bind_counts && bind_counts[index] < 65535)
		bind_counts[index]++;
	if (texline_me && PSP_ME_WORK())
	{
		(*texline_me)++;
		(*texline_me_copy)++;
	}
	/* 8-bit (256 colours) or 4-bit (16) indices into a 16- or 32-bit
	palette; or a texture in its own colours (the showcase's, ui.map:
	texpack.py --showcase), with no palette */
	palette_bytes = DIRECT(entry) ? 0 : (entry->clut == GU_PSM_8888 ? 4 : 2) * (entry->psm == GU_PSM_T4 ? 16 : 256);
	address = resident_at[index];
	/* build 114: statememo (above) */
	if (memo_on < 0)
		memo_on = psp_keep("statememo") || psp_keep("memotex"); /* memotex: this half alone */
	if (memo_on > 0 && !PSP_ME_WORK() && key == memo_key && address == memo_address && frame == memo_frame)
	{
		psp_tex_binds_memo++;
		if (has_alpha)
			*has_alpha = entry->flags & 1;
		return 1;
	}
	psp_tex_binds_sent++;
	if (bind_blocks_on < 0)
	{
		bind_blocks_on = psp_keep("bindblocks");
		if (bind_blocks_on && !(bind_block_words = memalign(64, BIND_BLOCKS * sizeof(*bind_block_words))))
			bind_blocks_on = 0;
	}
	if (bind_blocks_on && !PSP_ME_WORK())
	{
		const unsigned int *block = bind_block(index, entry, address, palette_bytes);

		if (block)
		{
			/* BASE and CALL, as pspgu's sceGuCallList (without its stall update) */
			sceGuSendCommandi(0x10, ((unsigned int)block >> 8) & 0xF0000);
			sceGuSendCommandi(0x0A, (unsigned int)block & 0xFFFFFF);
			bind_blocks_called++;
			if (has_alpha)
				*has_alpha = entry->flags & 1;
			memo_key = key;
			memo_address = address;
			memo_frame = frame;
			return 1;
		}
	}
	start = address; /* where the texture starts (address moves past the palette and levels) */
	if (palette_bytes)
	{
		__real_sceGuClutMode(entry->clut, 0, 0xFF, 0);
		__real_sceGuClutLoad(palette_bytes / 32, (void *)address);
	}
	__real_sceGuTexMode(entry->psm, entry->levels - 1, 0, 1);
	address += palette_bytes;
	width = entry->width;
	height = entry->height;
	for (level = 0; level < entry->levels; level++)
	{
		unsigned int bytes = entry->psm == GU_PSM_T4 ? (width + 1) / 2 : width * TEXEL_BYTES(entry);
		unsigned int row = ((bytes < 16 ? 16 : bytes) + 15) & ~15u, rows = (height + 7) & ~7u;

		/* the buffer width is in pixels */
		__real_sceGuTexImage(level, width, height, entry->psm == GU_PSM_T4 ? row * 2 : row / TEXEL_BYTES(entry),
			(void *)address);
		address += row * rows;
		width = width > 1 ? width / 2 : 1;
		height = height > 1 ? height / 2 : 1;
	}
	__real_sceGuTexFilter(entry->levels > 1 ? GU_LINEAR_MIPMAP_LINEAR : GU_LINEAR, GU_LINEAR);
	if (has_alpha)
		*has_alpha = entry->flags & 1;
	/* the GE now has this texture (never remembered from the ME's work) */
	if (PSP_ME_WORK())
		memo_key = 0;
	else
	{
		memo_key = key;
		memo_address = start;
		memo_frame = frame;
	}
	return 1;
}

/* the size a texture was baked at (texture coordinates in the GE's 2D mode
are in texels); 0 if it isn't in the pack */
/* a resident texture's 32-bit palette, read through the uncached view (for
the HUD's meters, whose look is worked out per palette entry; psp_ge.c) */
int psp_tex_palette(unsigned int key, const unsigned int **palette, int *count)
{
	int index = key && entries ? find(key) : -1;

	if (index < 0 || !resident_at[index] || DIRECT(&entries[index]) || entries[index].clut != GU_PSM_8888)
		return 0;
	*palette = (const unsigned int *)(resident_at[index] | 0x40000000);
	*count = entries[index].psm == GU_PSM_T4 ? 16 : 256;
	return 1;
}

/* build 116: a texture brought into memory without binding it (no GE
state sent): a model meter's palette is worked out from its map's when the
part is queued, before the flush binds the map. Counted as used this frame,
as a bind is. 1 when it is resident */
int psp_tex_resident(unsigned int key)
{
	int index = key && entries ? find(key) : -1;

	if (index < 0)
		return 0;
	if (!resident_at[index])
	{
		misses++;
		if (PSP_ME_WORK())
		{
			want(index);
			return 0;
		}
		if (frame_loaded >= LOAD_BUDGET_BYTES || !load(index))
			return 0;
	}
	if (last_frame[index] != frame)
	{
		frame_set_bytes += alignment(entries[index].size);
		frame_set_count++;
	}
	last_frame[index] = frame;
	return 1;
}

/* "texpretouch": a texture brought in as its draw is written down for the
Media Engine's replay (psp_ge.c, the GE_DEFER records), so the replay's bind
finds it there. The ME can't read the Memory Stick: a bind there that finds
its texture missing only asks for it, the main CPU reads it at the frame's
end, and every texture's first sight drew a frame (or more) untextured.
Records are written while no replay runs and in the frame (this file's
frame) the replay binds in, so a texture marked here is protected from the
rest of the frame's loads as a bind would protect it. A texture already
resident is only marked: that keeps it from being evicted by a later
record's load before the replay draws it. A load the frame's budget or room
refuses is left to the replay's bind, which counts it.
A mark holds room a bind can't then get, and records name textures the
replay never binds (a culled run, a far surface's detail maps), so: a kind
the replay binds only sometimes is marked only if it's already resident
(resident_only: it costs no load and evicts nothing); and nothing more is
marked once this frame's set nears the caches' room ("txptceilN", percent,
95 by default: b30's beach marked 2,559 KB of 2,632 without it, and loads
then waited for room; at 85, b30's flight stopped marking models' maps and
drew them white for up to 47 frames running, against 13 at 95). Past the ceiling the replay's binds ask for what's
missing, as without texpretouch */
#define PRETOUCH_KINDS 18
static PSP_MAIN_SIDE unsigned long pretouch_calls = 0, pretouch_loads = 0, pretouch_refused = 0, pretouch_ceiling_stops = 0,
	pretouch_not_resident = 0;
/* the last texture each record kind asked for, and its frame: a level draw
names its base and its lightmap in turn, so one memo for all kinds missed
every time; a kind's runs of one texture come in a row */
static PSP_MAIN_SIDE unsigned int pretouch_key[PRETOUCH_KINDS], pretouch_frame[PRETOUCH_KINDS];
static PSP_MAIN_SIDE int pretouch_percent = -1;
/* psp_ge.c's mechunk: the frame is replayed by the ME while it is written
down, so a texture its published records bind must be marked (kept for the
frame) before the ME binds it. While psp_ge.c sets psp_tex_chunking, a
texture touched only if resident (a detail or change map) is marked when it
is, and any texture left unmarked (not resident, past the budget or the
ceiling) sets psp_tex_chunk_missed: psp_ge.c publishes no more that frame */
PSP_MAIN_SIDE int psp_tex_chunking = 0, psp_tex_chunk_missed = 0;
/* "mechunkpassold" (a test): a resident-only texture not marked, as without mechunk */
static PSP_MAIN_SIDE int chunk_pass_old = -1;

#ifdef PSP_RECON
/* "txptcensus" (recon): per record kind, the textures marked in a frame
against those the replay then bound, and those a filter passed over that
the replay bound anyway (what the filters cost in looks) */
static const char *const txc_names[PRETOUCH_KINDS] = { "levelbase", "lightmap", "leveldetail", "modelbase", "change",
	"glow", "modeldetail", "sprite", "contrail", "widget", "seethrough", "levelseethrough", "screen", "zoom", "light",
	"decal", "blip", "blipsend" };
static int txc_wait_frames = 0;
static unsigned int *txc_marked_at = NULL, *txc_skipped_at = NULL;
static unsigned char *txc_kind = NULL;
static unsigned long txc_waits_seen = 0, txc_frames = 0;
static unsigned long txc_marked[PRETOUCH_KINDS], txc_marked_kb[PRETOUCH_KINDS], txc_unbound[PRETOUCH_KINDS],
	txc_unbound_kb[PRETOUCH_KINDS], txc_skipped_bound[PRETOUCH_KINDS], txc_skipped[PRETOUCH_KINDS], txc_unannounced = 0;

static int txc_ready(void)
{
	if (txc_on < 0)
		txc_on = psp_keep("txptcensus") != 0;
	if (!txc_on || !entries)
		return 0;
	if (txc_for != entries)
	{
		free(txc_marked_at);
		free(txc_skipped_at);
		free(txc_bound_at);
		free(txc_kind);
		txc_marked_at = calloc(entry_count, sizeof(unsigned int));
		txc_skipped_at = calloc(entry_count, sizeof(unsigned int));
		txc_bound_at = line_calloc(entry_count, sizeof(unsigned int));
		txc_kind = calloc(entry_count, 1);
		txc_for = entries;
		if (!txc_marked_at || !txc_skipped_at || !txc_bound_at || !txc_kind)
		{
			txc_on = 0;
			return 0;
		}
	}
	return 1;
}

static void txc_note(int index, int kind, int marked)
{
	if (!txc_ready())
		return;
	if (marked)
		txc_marked_at[index] = frame;
	else
		txc_skipped_at[index] = frame;
	txc_kind[index] = (unsigned char)kind;
}

static void txc_frame_end(void)
{
	unsigned long marked[PRETOUCH_KINDS] = { 0 }, unbound_kb[PRETOUCH_KINDS] = { 0 }, bound[PRETOUCH_KINDS] = { 0 };
	unsigned long unannounced = 0;
	int index, k, waited;
	char line[600];
	int at = 0;

	line[0] = 0;

	if (txc_on <= 0 || txc_for != entries || !entries)
		return;
	for (index = 0; index < entry_count; index++)
	{
		int was_bound = txc_bound_at[index] == frame;

		k = txc_kind[index];
		if (txc_marked_at[index] == frame)
		{
			marked[k]++;
			txc_marked[k]++;
			txc_marked_kb[k] += alignment(entries[index].size) / 1024;
			if (was_bound)
				bound[k]++;
			else
			{
				unbound_kb[k] += alignment(entries[index].size);
				txc_unbound[k]++;
				txc_unbound_kb[k] += alignment(entries[index].size) / 1024;
			}
		}
		else if (txc_skipped_at[index] == frame)
		{
			txc_skipped[k]++;
			if (was_bound)
				txc_skipped_bound[k]++;
		}
		else if (was_bound)
			unannounced++;
	}
	txc_unannounced += unannounced;
	txc_frames++;
	waited = waits != txc_waits_seen;
	txc_waits_seen = waits;
	if (waited && txc_wait_frames < 8)
	{
		txc_wait_frames++;
		for (k = 0; k < PRETOUCH_KINDS && at < (int)sizeof(line) - 48; k++)
			if (marked[k])
				at += snprintf(line + at, sizeof(line) - at, " %s %lu/%lu/%luKB", txc_names[k], marked[k], bound[k],
					unbound_kb[k] / 1024);
		platform_log("textures: txptcensus: frame %u waited for room (%lu in all); set %lu KB; marked/bound/unbound KB by kind:%s; bound unannounced %lu",
			frame, waits, frame_set_bytes / 1024, line, unannounced);
	}
	if (frame % 300 == 0)
	{
		at = 0;
		line[0] = 0;
		for (k = 0; k < PRETOUCH_KINDS && at < (int)sizeof(line) - 56; k++)
			if (txc_marked[k] || txc_skipped[k])
				at += snprintf(line + at, sizeof(line) - at, " %s %lu/%lu/%lu/%lu", txc_names[k], txc_marked_kb[k],
					txc_unbound_kb[k], txc_skipped[k], txc_skipped_bound[k]);
		platform_log("textures: txptcensus: %lu frames; by kind marked KB/unbound KB/passed over/passed over but bound:%s; bound unannounced %lu",
			txc_frames, line, txc_unannounced);
	}
}
#else
#define txc_note(index, kind, marked) do { } while (0)
#endif

/* resident_only: 1 marks the texture only if it's resident; 2 (the census
alone) notes a key a filter passed over */
/* "hudpinme" (on by default; "hudpinmeold": as before): resident_only 3 is
the HUD's and the motion sensor's records (psp_ge.c, GE_PRETOUCH_HUD). With
the Media Engine replaying the frame (gemeon), the HUD's binds run on the ME,
where the pin above is never set, and the HUD records come last, after the
ceiling below has stopped marking: under a full texture cache the meters
and the sensor were drawn untextured or not at all while the log's HUD line
(main-CPU binds only) said none were missing (GROUNDTEX's test bakes,
groundtex1/2: b30's HUD quads -10%, c20's nearly all). Here, on
the main CPU as the record is written, they're pinned as a main-CPU bind
pins them, read past the frame's budget, and marked past the ceiling (the
HUD's set is small) */
static PSP_MAIN_SIDE int hud_pin_me = -1;
static PSP_MAIN_SIDE unsigned long hud_pretouched = 0, hud_pretouch_past_ceiling = 0, hud_pretouch_refused = 0;
/* the pinned textures, so a frame's start reads back any the cache lost:
there, before the frame's own records, last frame's textures may give way
(hudpin1: under a full cache 2,337 of the HUD's loads found no room once
the frame's own set filled both caches, and the meters still went) */
#define HUD_PIN_LIST 96
static PSP_MAIN_SIDE int hud_pin_list[HUD_PIN_LIST], hud_pin_count = 0;
/* the frame each was last asked for: a texture refused room never gets a
drawn frame (last_frame), so that can't say it's still wanted */
static PSP_MAIN_SIDE unsigned int hud_pin_asked[HUD_PIN_LIST];
static PSP_MAIN_SIDE unsigned long hud_frame_start_loads = 0, hud_frame_start_refused = 0;

static void hud_pin_list_forget(void)
{
	hud_pin_count = 0;
}

/* the room the pins hold (the master: what the pin keeps from the rest) */
static unsigned int hud_pin_kb(int resident)
{
	unsigned int bytes = 0;
	int k;

	for (k = 0; k < hud_pin_count && entries; k++)
		if (!resident || resident_at[hud_pin_list[k]])
			bytes += alignment(entries[hud_pin_list[k]].size);
	return bytes / 1024;
}

/* the HUD's pinned textures the cache lost, read back at the frame's first
record (psp_ge.c's pretouch has carried out any frame still in flight by
then, so nothing it binds is evicted), before the frame's own set fills
the caches and while last frame's textures can give way */
static PSP_MAIN_SIDE unsigned int hud_readback_frame = ~0u;

static void hud_read_back(void)
{
	int k;

	hud_readback_frame = frame;
	for (k = 0; k < hud_pin_count; k++)
	{
		int index = hud_pin_list[k];

		if (resident_at[index] || hud_pin_asked[k] + RECENT_FRAMES <= frame)
			continue;
		if (load(index))
			hud_frame_start_loads++;
		else
			hud_frame_start_refused++;
	}
}

void psp_tex_pretouch(unsigned int key, int kind, int resident_only)
{
	int index, hud = 0;
	unsigned int size, room;

	if (!key || !entries || PSP_ME_WORK() || (unsigned int)kind >= PRETOUCH_KINDS)
		return;
	if (hud_pin_me > 0 && hud_readback_frame != frame)
		hud_read_back();
	if (pretouch_frame[kind] == frame && pretouch_key[kind] == key)
		return;
	index = find(key);
	if (index < 0)
		return;
	pretouch_key[kind] = key;
	pretouch_frame[kind] = frame;
	if (resident_only == 3)
	{
		if (hud_pin_me < 0)
			hud_pin_me = !psp_keep("hudpinmeold");
		if (hud_pin_on < 0)
			hud_pin_on = !psp_keep("hudpinoff");
		hud = hud_pin_me > 0 && hud_pin_on > 0;
		resident_only = 0;
		if (hud)
			hud_pretouched++;
	}
	/* (a texture touched only if resident and not resident isn't missed: the
	ME binding it draws untextured and asks for it, as the replay always has,
	touching nothing the main CPU writes) */
	if (chunk_pass_old < 0)
		chunk_pass_old = psp_keep("mechunkpassold");
	if (resident_only == 2 && psp_tex_chunking && resident_at[index] && !chunk_pass_old)
		resident_only = 1;
	else if (resident_only == 2)
	{
		txc_note(index, kind, 0);
		return;
	}
	pretouch_calls++;
	if (hud && hud_pinned)
	{
		int k;

		if (!hud_pinned[index] && hud_pin_count < HUD_PIN_LIST)
			hud_pin_list[hud_pin_count++] = index;
		hud_pinned[index] = 1;
		for (k = 0; k < hud_pin_count; k++)
			if (hud_pin_list[k] == index)
			{
				hud_pin_asked[k] = frame;
				break;
			}
	}
	if (resident_at[index] && last_frame[index] == frame)
		return; /* marked already, by a bind or another kind */
	size = alignment(entries[index].size);
	if (pretouch_percent < 0)
		pretouch_percent = psp_keep_number("txptceil", 95);
	room = (VRAM_CACHE_END - VRAM_CACHE_START) + (arenas[1].end - arenas[1].start);
	if (frame_set_bytes + size > room / 100 * (unsigned int)pretouch_percent && hud)
		hud_pretouch_past_ceiling++;
	else if (frame_set_bytes + size > room / 100 * (unsigned int)pretouch_percent)
	{
		pretouch_ceiling_stops++;
		psp_tex_chunk_missed = 1;
		txc_note(index, kind, 0);
		return;
	}
	if (!resident_at[index])
	{
		if (resident_only)
		{
			pretouch_not_resident++;
			psp_tex_chunk_missed = 1;
			txc_note(index, kind, 0);
			return;
		}
		if ((frame_loaded >= LOAD_BUDGET_BYTES && !hud) || !load(index))
		{
			if (hud)
				hud_pretouch_refused++;
			pretouch_refused++;
			psp_tex_chunk_missed = 1;
			txc_note(index, kind, 0);
			return;
		}
		misses++; /* as a bind's load counts: the log's waits stay misses - loads */
		pretouch_loads++;
	}
	frame_set_bytes += size;
	frame_set_count++;
	last_frame[index] = frame;
	txc_note(index, kind, 1);
}

/* a resident texture's 16-bit (5650) palette, the same way (the change
colour's mask, in a multipurpose map's blue; build 31) */
int psp_tex_palette16(unsigned int key, const unsigned short **palette, int *count)
{
	int index = key && entries ? find(key) : -1;

	if (index < 0 || !resident_at[index] || DIRECT(&entries[index]) || entries[index].clut != GU_PSM_5650)
		return 0;
	*palette = (const unsigned short *)(resident_at[index] | 0x40000000);
	*count = entries[index].psm == GU_PSM_T4 ? 16 : 256;
	return 1;
}

/* a resident texture's average colour (ABGR), from the bake (the
flashlight's added light, which can't take a second texture: build 36) */
int psp_tex_average(unsigned int key, unsigned int *abgr)
{
	int index = key && entries ? find(key) : -1;

	if (index < 0 || !resident_at[index] || !averages || !baked_averages || !baked_averages[index])
		return 0;
	if (!averages[index])
		averages[index] = baked_averages[index];
	*abgr = averages[index];
	return 1;
}

/* a resident multipurpose map's change mask (texpack.py, change_mask): the
address of its CLUT, the data's last bytes, for the GE to load; NULL when the
map isn't resident or the pack has no mask for it. Nothing is read */
const void *psp_tex_change_mask(unsigned int key, int *count)
{
	int index = key && entries ? find(key) : -1;

	if (index < 0 || !resident_at[index] || !(entries[index].flags & CHANGE_MASK))
		return NULL;
	*count = entries[index].psm == GU_PSM_T4 ? 16 : 256;
	return (const void *)(resident_at[index] + entries[index].size - *count * 4);
}

/* a resident multipurpose map's reflection mask (texpack.py,
reflection_mask): the CLUT just before its change mask, for Chief's sheen
pass; NULL when the map isn't resident or the pack keeps none for it */
const void *psp_tex_reflection_mask(unsigned int key, int *count)
{
	int index = key && entries ? find(key) : -1;

	if (index < 0 || !resident_at[index] || !(entries[index].flags & REFL_MASK) || !(entries[index].flags & CHANGE_MASK))
		return NULL;
	*count = entries[index].psm == GU_PSM_T4 ? 16 : 256;
	return (const void *)(resident_at[index] + entries[index].size - *count * 8);
}

/* a texture's average once worked out, even after it has left memory;
else worked out now, bringing it in without binding it if need be (a model
reflection's cube map patch and multipurpose map, psp_ge.c). 0 when it
can't be had this frame */
int psp_tex_average_known(unsigned int key, unsigned int *abgr)
{
	int index = key && entries ? find(key) : -1;

	if (index < 0)
		return 0;
	if (averages_count == entry_count && averages[index])
	{
		*abgr = averages[index];
		return 1;
	}
	if (!resident_at[index] && !psp_tex_resident(key))
		return 0;
	return psp_tex_average(key, abgr);
}

/* a light's gel patch's six face colours (red, green, blue 0-1 each, in
the cube's face order: +X, -X, +Y, -Y, +Z, -Z), from palette entries 1-6
where entry 15 marks them (texpack.py, cube_patch); kept per key, so a
gel that has left memory isn't brought back each frame. 0 when the patch
carries none or can't be had this frame. A patch that keeps the cube too
(psp_tex_gel_cube) has them in the same entries */

int psp_tex_gel_faces(unsigned int key, float *faces)
{
	int index = key && entries ? find(key) : -1, k;
	const unsigned short *palette;

	if (index < 0)
		return 0;
	for (k = 0; k < GEL_FACES_KEPT; k++)
		if (gel_faces_kept[k].key == key)
		{
			if (gel_faces_kept[k].known)
				memcpy(faces, gel_faces_kept[k].faces, sizeof(gel_faces_kept[k].faces));
			return gel_faces_kept[k].known;
		}
	if (!resident_at[index] && !psp_tex_resident(key))
		return 0;
	k = gel_faces_next;
	gel_faces_next = (gel_faces_next + 1) % GEL_FACES_KEPT;
	gel_faces_kept[k].key = key;
	gel_faces_kept[k].known = 0;
	if (DIRECT(&entries[index]) || entries[index].clut != GU_PSM_5650)
		return 0;
	palette = (const unsigned short *)(resident_at[index] | 0x40000000);
	if (entries[index].psm == GU_PSM_T4 ? palette[15] != GEL_FACES_MARK :
		entries[index].psm != GU_PSM_T8 || palette[255] != GEL_CUBE_MARK)
		return 0;
	for (index = 0; index < 6; index++)
	{
		unsigned int c = palette[1 + index];

		gel_faces_kept[k].faces[3 * index] = (float)(c & 31) / 31.0f;
		gel_faces_kept[k].faces[3 * index + 1] = (float)((c >> 5) & 63) / 63.0f;
		gel_faces_kept[k].faces[3 * index + 2] = (float)(c >> 11) / 31.0f;
	}
	gel_faces_kept[k].known = 1;
	memcpy(faces, gel_faces_kept[k].faces, sizeof(gel_faces_kept[k].faces));
	return 1;
}

/* a light's gel cube (texpack.py, gel_cube) for psp_tex_gel_cube_at, or
NULL when its patch keeps none ("nogelcube" asks for none; then the faces'
means shape the light, psp_tex_gel_faces) or can't be had this frame */
const void *psp_tex_gel_cube(unsigned int key)
{
	int index = key && entries ? find(key) : -1, k, t;
	const unsigned short *palette;
	const unsigned char *texels;
	struct gel_cube *cube;

	if (index < 0)
		return NULL;
	for (k = 0; k < GEL_CUBES_KEPT; k++)
		if (gel_cubes_kept[k].key == key)
			return gel_cubes_kept[k].known ? &gel_cubes_kept[k] : NULL;
	if (!resident_at[index] && !psp_tex_resident(key))
		return NULL;
	cube = &gel_cubes_kept[gel_cubes_next];
	gel_cubes_next = (gel_cubes_next + 1) % GEL_CUBES_KEPT;
	cube->key = key;
	cube->known = 0;
	if (DIRECT(&entries[index]) || entries[index].clut != GU_PSM_5650 || entries[index].psm != GU_PSM_T8 ||
		entries[index].width != GEL_CUBE_SIDE || entries[index].height != 6 * GEL_CUBE_SIDE)
		return NULL;
	palette = (const unsigned short *)(resident_at[index] | 0x40000000);
	if (palette[255] != GEL_CUBE_MARK)
		return NULL;
	/* a row of 16 one-byte texels is one swizzle block's width, so the
	swizzled level is in plain row order */
	texels = (const unsigned char *)((resident_at[index] + 256 * 2) | 0x40000000);
	for (t = 0; t < GEL_CUBE_TEXELS; t++)
		cube->texels[t] = palette[texels[t]];
	cube->known = 1;
	return cube;
}

/* the gel's colour (0-1 a channel) in direction x, y, z of its frame: the
face of the largest axis and the place on it as a cube map lookup takes
them (Direct3D's faces, as the Xbox's), the four nearest texels blended,
clamped at the face's edges */
void psp_tex_gel_cube_at(const void *gel, float x, float y, float z, float *rgb)
{
	const struct gel_cube *cube = (const struct gel_cube *)gel;
	float ax = x < 0.0f ? -x : x, ay = y < 0.0f ? -y : y, az = z < 0.0f ? -z : z;
	float major, s, t, u, v, fu, fv, w[4];
	int face, u0, v0, u1, v1, k, at[4];

	if (ax >= ay && ax >= az)
	{
		major = ax;
		face = x >= 0.0f ? 0 : 1;
		s = x >= 0.0f ? -z : z;
		t = -y;
	}
	else if (ay >= az)
	{
		major = ay;
		face = y >= 0.0f ? 2 : 3;
		s = x;
		t = y >= 0.0f ? z : -z;
	}
	else
	{
		major = az;
		face = z >= 0.0f ? 4 : 5;
		s = z >= 0.0f ? x : -x;
		t = -y;
	}
	if (major < 1e-12f)
	{
		rgb[0] = rgb[1] = rgb[2] = 0.0f;
		return;
	}
	u = (s / major + 1.0f) * (0.5f * GEL_CUBE_SIDE) - 0.5f;
	v = (t / major + 1.0f) * (0.5f * GEL_CUBE_SIDE) - 0.5f;
	if (u < 0.0f)
		u = 0.0f;
	if (v < 0.0f)
		v = 0.0f;
	if (u > GEL_CUBE_SIDE - 1)
		u = GEL_CUBE_SIDE - 1;
	if (v > GEL_CUBE_SIDE - 1)
		v = GEL_CUBE_SIDE - 1;
	u0 = (int)u;
	v0 = (int)v;
	u1 = u0 < GEL_CUBE_SIDE - 1 ? u0 + 1 : u0;
	v1 = v0 < GEL_CUBE_SIDE - 1 ? v0 + 1 : v0;
	fu = u - (float)u0;
	fv = v - (float)v0;
	face *= GEL_CUBE_SIDE * GEL_CUBE_SIDE;
	at[0] = face + v0 * GEL_CUBE_SIDE + u0;
	at[1] = face + v0 * GEL_CUBE_SIDE + u1;
	at[2] = face + v1 * GEL_CUBE_SIDE + u0;
	at[3] = face + v1 * GEL_CUBE_SIDE + u1;
	w[0] = (1.0f - fu) * (1.0f - fv);
	w[1] = fu * (1.0f - fv);
	w[2] = (1.0f - fu) * fv;
	w[3] = fu * fv;
	rgb[0] = rgb[1] = rgb[2] = 0.0f;
	for (k = 0; k < 4; k++)
	{
		unsigned int c = cube->texels[at[k]];

		rgb[0] += w[k] * (float)(c & 31);
		rgb[1] += w[k] * (float)((c >> 5) & 63);
		rgb[2] += w[k] * (float)(c >> 11);
	}
	rgb[0] *= 1.0f / 31.0f;
	rgb[1] *= 1.0f / 63.0f;
	rgb[2] *= 1.0f / 31.0f;
}

/* ---------- build 102: object lighting from the lightmaps

Halo lights an object from the level under it: a ray down finds the
surface, and the lightmap's colour there (sample_lightmap) sets the object's
ambient and its two distant lights' strength, the surface's texture colour
(sample_diffuse_texture) the second light's tint. On the Xbox both read the
bitmap's pixels in the texture cache (bitmap_2d_get_pixel); here the pixels
are in this pack, the game's bitmaps have none, and the read answered
"none", white: every object, characters, vehicles and scenery alike, lit as
if it stood in full light (b101: "the lighting seems to be ignored by
characters and vehicle models, as well as some rocks"). So the samples come
from here: the texel itself when the texture is resident (exact: the
lightmap's colours through its palette); else, while the map loads (static
scenery is lit once, then), straight from the pack, one byte and one palette
entry; in play at most LIGHT_READS_PER_FRAME such reads a frame, past that
the page's last colour */
#define LIGHT_READS_PER_FRAME 2
#define LIGHT_PAGES 64
static struct { unsigned int key, argb; } light_last[LIGHT_PAGES];
static unsigned int light_reads_frame = 0, light_reads_this_frame = 0;
static unsigned long light_exact = 0, light_read = 0, light_last_used = 0, light_white = 0, light_read_us = 0;

static unsigned int argb_from_palette(int clut, unsigned int entry)
{
	unsigned int r, g, b, a = 255;

	if (clut == GU_PSM_8888)
	{
		r = entry & 0xFF; g = (entry >> 8) & 0xFF; b = (entry >> 16) & 0xFF; a = entry >> 24;
	}
	else
	{
		r = (entry & 31) * 255 / 31; g = ((entry >> 5) & 63) * 255 / 63; b = ((entry >> 11) & 31) * 255 / 31;
	}
	return (a << 24) | (r << 16) | (g << 8) | b;
}

/* the byte offset of texel (x, y) in a swizzled top level, and the nibble */
static unsigned int texel_offset(struct pack_entry const *entry, int x, int y, int *nibble)
{
	unsigned int bytes = entry->psm == GU_PSM_T4 ? (entry->width + 1) / 2 : entry->width;
	unsigned int row = ((bytes < 16 ? 16 : bytes) + 15) & ~15u;
	unsigned int xb = entry->psm == GU_PSM_T4 ? (unsigned int)x / 2 : (unsigned int)x;

	*nibble = entry->psm == GU_PSM_T4 ? (x & 1) : -1;
	return ((unsigned int)(y / 8) * (row / 16) + xb / 16) * 128 + (unsigned int)(y % 8) * 16 + xb % 16;
}

static void light_remember(unsigned int key, unsigned int argb)
{
	unsigned int slot = (key >> 4) % LIGHT_PAGES;

	light_last[slot].key = key;
	light_last[slot].argb = argb;
}

/* the texel at (u, v) of a texture's top level, nearest, wrapped (as
bitmap_2d_get_pixel samples a lightmap) as the Xbox's 0xAARRGGBB; 0 if it
can't be had (the caller keeps the game's own answer) */
int psp_tex_light_texel(unsigned int key, float u, float v, unsigned int *argb)
{
	int index = key && entries ? find(key) : -1;
	struct pack_entry const *entry;
	int x, y, nibble;
	unsigned int palette_bytes, at, value, palette_entry = 0, slot;

	if (index < 0)
		return 0;
	entry = &entries[index];
	if (entry->psm != GU_PSM_T8 && entry->psm != GU_PSM_T4)
		return 0;
	x = (int)floorf((float)entry->width * u);
	y = (int)floorf((float)entry->height * v);
	x = ((x % entry->width) + entry->width) % entry->width;
	y = ((y % entry->height) + entry->height) % entry->height;
	palette_bytes = (entry->clut == GU_PSM_8888 ? 4 : 2) * (entry->psm == GU_PSM_T4 ? 16 : 256);
	at = texel_offset(entry, x, y, &nibble);
	if (resident_at[index])
	{
		value = ((const unsigned char *)((resident_at[index] + palette_bytes) | 0x40000000))[at];
		if (nibble >= 0)
			value = (value >> (4 * nibble)) & 15;
		palette_entry = entry->clut == GU_PSM_8888 ? ((const unsigned int *)(resident_at[index] | 0x40000000))[value]
			: ((const unsigned short *)(resident_at[index] | 0x40000000))[value];
		*argb = argb_from_palette(entry->clut, palette_entry);
		light_exact++;
		light_remember(key, *argb);
		return 1;
	}
	if (light_reads_frame != frame)
	{
		light_reads_frame = frame;
		light_reads_this_frame = 0;
	}
	if (pack >= 0 && (frame - map_open_frame < 2 || light_reads_this_frame < LIGHT_READS_PER_FRAME))
	{
		unsigned char byte;
		unsigned int start = sceKernelGetSystemTimeLow(), entry_bytes = entry->clut == GU_PSM_8888 ? 4 : 2;

		light_reads_this_frame++;
		if (sceIoLseek32(pack, entry->offset + palette_bytes + at, PSP_SEEK_SET) == (int)(entry->offset + palette_bytes + at)
			&& sceIoRead(pack, &byte, 1) == 1)
		{
			value = nibble >= 0 ? (byte >> (4 * nibble)) & 15 : byte;
			if (sceIoLseek32(pack, entry->offset + value * entry_bytes, PSP_SEEK_SET) == (int)(entry->offset + value * entry_bytes)
				&& sceIoRead(pack, &palette_entry, entry_bytes) == (int)entry_bytes)
			{
				*argb = argb_from_palette(entry->clut, palette_entry);
				light_read++;
				light_read_us += sceKernelGetSystemTimeLow() - start;
				light_remember(key, *argb);
				return 1;
			}
		}
	}
	slot = (key >> 4) % LIGHT_PAGES;
	if (light_last[slot].key == key)
	{
		*argb = light_last[slot].argb;
		light_last_used++;
		/* the page's last colour stands in: not this point's own (B30) */
		{
			extern int psp_light_miss;

			psp_light_miss = 1;
		}
		return 1;
	}
	light_white++;
	{
		extern int psp_light_miss;

		psp_light_miss = 1;
	}
	return 0;
}

/* the texel at (u, v) of a resident texture's top level, nearest, wrapped,
as 0xAARRGGBB; 0 if the texture isn't resident (no Memory Stick read, and
none of psp_tex_light_texel's counts or remembered colours: a HUD mask's
texels, after build 116) */
int psp_tex_texel(unsigned int key, float u, float v, unsigned int *argb)
{
	int index = key && entries ? find(key) : -1;
	struct pack_entry const *entry;
	int x, y, nibble;
	unsigned int palette_bytes, at, value, palette_entry;

	if (index < 0 || !resident_at[index])
		return 0;
	entry = &entries[index];
	if (entry->psm != GU_PSM_T8 && entry->psm != GU_PSM_T4)
		return 0;
	x = (int)floorf((float)entry->width * u);
	y = (int)floorf((float)entry->height * v);
	x = ((x % entry->width) + entry->width) % entry->width;
	y = ((y % entry->height) + entry->height) % entry->height;
	palette_bytes = (entry->clut == GU_PSM_8888 ? 4 : 2) * (entry->psm == GU_PSM_T4 ? 16 : 256);
	at = texel_offset(entry, x, y, &nibble);
	value = ((const unsigned char *)((resident_at[index] + palette_bytes) | 0x40000000))[at];
	if (nibble >= 0)
		value = (value >> (4 * nibble)) & 15;
	palette_entry = entry->clut == GU_PSM_8888 ? ((const unsigned int *)(resident_at[index] | 0x40000000))[value]
		: ((const unsigned short *)(resident_at[index] | 0x40000000))[value];
	*argb = argb_from_palette(entry->clut, palette_entry);
	return 1;
}

/* a texture's average colour as 0xAARRGGBB, for the surface colour under
an object (the Xbox reads a small mip level: lod 0.3); worked out once the
texture is resident, and kept */
int psp_tex_light_average(unsigned int key, unsigned int *argb)
{
	int index = key && entries ? find(key) : -1;
	unsigned int abgr;

	if (index < 0)
		return 0;
	if (averages_count == entry_count && averages && averages[index])
		abgr = averages[index];
	else if (!psp_tex_average(key, &abgr))
		return 0;
	*argb = (abgr & 0xFF00FF00) | ((abgr & 0xFF) << 16) | ((abgr >> 16) & 0xFF);
	return 1;
}

void psp_tex_light_report(void)
{
	/* object_lights.c and render_objects.c (psp_fixups.py: FIXES_36, FIXES_B30) */
	extern unsigned long psp_object_light_samples_missed, psp_light_retries;

	if (light_exact + light_read + light_last_used + light_white + psp_object_light_samples_missed)
		platform_log("object lighting: lightmap samples %lu from resident textures, %lu read from the pack (%lu us in all), %lu the page's last colour, %lu white (nothing to go on); %lu reads with no floor copies; objects sampled again after a miss %lu",
			light_exact, light_read, light_read_us, light_last_used, light_white, psp_object_light_samples_missed, psp_light_retries);
}

/* whether the pack has a texture under this key (build 36: a model's base map
with its detail map folded in is under the base's key | 1) */
int psp_tex_has(unsigned int key)
{
	return key && entries && find(key) >= 0;
}

/* a level (senv) base map's key with its bump map's relief baked in, when
the pack has one (texpack.py, apply_relief: under the base's key | 2), else
the key itself. The Xbox lights a level's bump map per pixel; the GE can't,
so the bake folds the relief into the base, lit from one fixed direction.
The bake filters the bump over each base texel's footprint; sampled one
bump texel per base texel, it aliased into vertical streaks along a30's
stretched cliff walls. Keep word "norelief": no relief */
unsigned int psp_relief_key(unsigned int key)
{
	static int on = -1;

	if (on < 0)
		on = !psp_keep("norelief");
	return key && on && psp_tex_has(key | 2) ? key | 2 : key;
}

int psp_tex_size(unsigned int key, int *width, int *height)
{
	int index = key && entries ? find(key) : -1;

	if (index < 0)
		return 0;
	*width = entries[index].width;
	*height = entries[index].height;
	return 1;
}

/* the end of a frame: a new load budget, and a report every 300 frames */
/* psp_ge.c, once the GE has the screen and the on-screen log has stopped:
the VRAM set goes in before the first frame's draws (at the first frame's
end, those draws had already read some of it into VRAM on their own, in no
order, and two of the set no longer fit) */
void psp_tex_screen_free(void)
{
	screen_free = 1;
	if (vram_pending_count && entries)
		preload_vram();
}

void psp_tex_frame_end(void)
{
	/* build 112: what the Media Engine's work wanted, read now */
	if (wanted_count && entries)
	{
		int k;

		for (k = 0; k < wanted_count; k++)
			if (!resident_at[wanted[k]] && frame_loaded < LOAD_BUDGET_BYTES)
				load(wanted[k]);
		wanted_count = 0;
	}
	if (vram_pending_count && entries)
		preload_vram();
#ifdef PSP_RECON
	txc_frame_end();
#endif
	if (frame_set_bytes > frame_set_peak)
	{
		frame_set_peak = frame_set_bytes;
		frame_set_peak_count = frame_set_count;
	}
	frame_set_bytes = frame_set_count = 0;
	if (frame_untextured)
	{
		untextured_frames++;
		if (++untextured_run > untextured_run_worst)
			untextured_run_worst = untextured_run;
	}
	else
		untextured_run = 0;
	frame_untextured = 0;
	if (entries && frame % 300 == 0)
		platform_log("textures: %lu loaded (%lu KB), %lu requests waited for the next frame, %lu for room; %lu evicted; largest frame %lu textures, %lu KB; %lu frames drew something untextured (longest run %lu)",
			loads, load_bytes / 1024, misses - (loads - preloaded), waits, evictions, frame_set_peak_count, frame_set_peak / 1024,
			untextured_frames, untextured_run_worst);
	if (entries && frame % 300 == 0)
		platform_log("textures, the HUD's and the screen's (%s): %lu missing when drawn (%lu read past the frame's budget), %lu drawn untextured; recorded for the Media Engine (%s): %lu marked, %lu past the ceiling, %lu refused room; read back at a frame's start %lu, refused %lu (%d pinned, %u KB, %u KB of them resident)",
			hud_pin_on > 0 ? "pinned" : "hudpinoff", hud_misses, hud_reads_past_budget, hud_untextured,
			hud_pin_me > 0 ? "hudpinme" : hud_pin_me == 0 ? "hudpinmeold" : "none yet", hud_pretouched, hud_pretouch_past_ceiling,
			hud_pretouch_refused, hud_frame_start_loads, hud_frame_start_refused, hud_pin_count, hud_pin_kb(0), hud_pin_kb(1));
	if (entries && frame % 300 == 0)
		psp_tex_light_report();
	if (entries && frame % 300 == 0 && bind_blocks_on > 0)
	{
		platform_log("textures: bindblocks: %lu binds a frame CALLed a ready block, %lu blocks made a frame, %lu times every slot in use (last 300 frames)",
			bind_blocks_called / 300, bind_blocks_made / 300, bind_blocks_full);
		bind_blocks_called = bind_blocks_made = bind_blocks_full = 0;
	}
	if (entries && frame % 300 == 0 && pretouch_calls)
		platform_log("textures: texpretouch: %lu textures touched as their draws were written down, %lu read then, %lu left to the replay (budget or room), %lu at the ceiling, %lu not resident (resident only)",
			pretouch_calls, pretouch_loads, pretouch_refused, pretouch_ceiling_stops, pretouch_not_resident);
	if (entries && frame % 300 == 0 && (wanted_total || wanted_dropped))
		platform_log("textures: %lu wanted by the Media Engine's work and read at the frame's end, %lu more than the list held (gemesim)",
			wanted_total, wanted_dropped);
	if (frame_read_us > frame_read_worst)
		frame_read_worst = frame_read_us;
	frame_read_us = 0;
	if (entries && frame % 300 == 0 && read_us)
		platform_log("textures: reading took %lu ms in all (%lu KB/s); the worst frame spent %lu ms reading",
			read_us / 1000, (unsigned long)((unsigned long long)load_bytes * 1000000ULL / 1024 / read_us), frame_read_worst / 1000);
	if (entries && bind_counts && frame % 900 == 450)
	{
		/* the twelve most bound textures and where they live (build 44) */
		char line[640];
		int length = 0, k, index, vram_binds = 0, ram_binds = 0, vram_bytes = 0, ram_bytes = 0;

		/* build 67: the VRAM set in thirds by binds per byte over the period,
		so a run says what the least useful third of VRAM carries: the third
		the display list (384 KB) or hot CPU data would displace */
		{
			int order_count = 0, k2, third = 0;
			unsigned int third_bytes[3] = { 0, 0, 0 }, third_binds[3] = { 0, 0, 0 }, cumulative = 0, vram_total = 0;

			for (index = 0; index < entry_count; index++)
			{
				float density;

				if (!resident_at[index] || resident_in[index])
					continue;
				vram_total += alignment(entries[index].size);
				density = (float)bind_counts[index] / (float)alignment(entries[index].size);
				for (k2 = order_count; k2 > 0; k2--)
				{
					float other = (float)bind_counts[order[k2 - 1]] / (float)alignment(entries[order[k2 - 1]].size);

					if (other >= density)
						break;
					order[k2] = order[k2 - 1];
				}
				order[k2] = (unsigned short)index;
				order_count++;
			}
			for (k2 = 0; k2 < order_count; k2++)
			{
				unsigned int size = alignment(entries[order[k2]].size);

				while (third < 2 && cumulative >= vram_total * (third + 1) / 3)
					third++;
				third_bytes[third] += size;
				third_binds[third] += bind_counts[order[k2]];
				cumulative += size;
			}
			platform_log("textures: VRAM by binds per byte, best third %u binds in %u KB, middle %u in %u KB, worst %u in %u KB (%d textures resident)",
				third_binds[0], third_bytes[0] / 1024, third_binds[1], third_bytes[1] / 1024, third_binds[2], third_bytes[2] / 1024,
				order_count);
		}
		for (k = 0; k < 12; k++)
		{
			int best = -1;

			for (index = 0; index < entry_count; index++)
				if (bind_counts[index] && (best < 0 || bind_counts[index] > bind_counts[best]))
					best = index;
			if (best < 0 || length > (int)sizeof(line) - 64)
				break;
			length += snprintf(line + length, sizeof(line) - length, " %08x %ux%u %s %u;", entries[best].key, entries[best].width,
				entries[best].height, resident_at[best] ? (resident_in[best] ? "RAM" : "VRAM") : "out", bind_counts[best]);
			if (bind_totals)
				bind_totals[best] += bind_counts[best];
			bind_counts[best] = 0;
		}
		for (index = 0; index < entry_count; index++)
		{
			if (resident_at[index] && !resident_in[index])
			{
				vram_binds += bind_counts[index];
				vram_bytes += entries[index].size;
			}
			else if (resident_at[index])
			{
				ram_binds += bind_counts[index];
				ram_bytes += entries[index].size;
			}
			if (bind_totals)
				bind_totals[index] += bind_counts[index];
			bind_counts[index] = 0;
		}
		platform_log("textures: most bound over 900 frames (key size place binds):%s; the rest: VRAM %d binds (%d KB resident), RAM %d binds (%d KB)",
			line, vram_binds, vram_bytes / 1024, ram_binds, ram_bytes / 1024);
		learned_snapshot();
	}
	if (entries && hud_bound && frame % 900 == 600)
		strip_census_report(900);
	frame++;
	frame_loaded = 0;
}
