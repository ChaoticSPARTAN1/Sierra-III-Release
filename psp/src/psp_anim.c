/* psp_anim.c - the animation cache (build 106)

Why: animation frame data is the largest class of tag data on every campaign
map (1.4-2.8 MB after keyframing; b30: 2.44 MB in 859 animations), and the
PSP-1000's 9 MB tag window can't hold it with the rest of a map's tags and
its largest BSP (campaign_window_fit.md). The game reads frame data only
while it evaluates an animation, so a map can be built with it outside its
tags (invader-build -M, with -G): it goes to the front of the map's geometry
pack, and each animation's data pointer becomes 0x0C000000 + its offset there,
as model vertices' do.

This cache turns such a pointer back into bytes in RAM. Unlike the geometry
cache it never answers "not yet": the game's logic needs the right pose this
tick (where a hand holds a weapon, where a head is for a headshot), so a miss
is read at once, on the game thread, from the geometry pack's own descriptor
(psp_geo_pack_read). What the game asks for: a compressed animation's whole
compressed block, or one uncompressed frame (model_animation_definitions.c,
model_animations.c; psp_fixups.py).

Pages: the cache reads whole pages of the pack (8 KB), holding the animation
asked for and its neighbours. An animation graph's animations sit together in
the pack and a unit plays several of one graph, so one read serves the next
few misses. A random Memory Stick read costs about 2 ms however small, 8 KB
about 2.2 ms (PSP-1000, halo_bench v10). The fork lays the animations out
for the page (the header's word 7): one that would cross into the next page
starts on it, so an animation no larger than a page is always one page.
tools/anim_cache_sim.py replays a trace of what the game asked for (keep word
"animtrace") against other sizes: on b30's beach fight, 8 KB pages and 512 KB
miss 214 times in 9 minutes (1.8 MB read, about 0.5 s on a PSP-1000), 384 KB
360 times; under about 300 KB the cache thrashes.

Slots: the cache is an array of page-sized slots. Bytes that span pages are
read into consecutive slots, so they are contiguous in RAM. A slot is reused
least recently used first, never while it holds part of one of the last few
ranges handed out: a caller may still hold a pointer into those (the readers
hold one for the length of one call; aiming_screen_apply holds four frames
of one animation at once).

Where the RAM comes from, in up to two blocks: first the part of the tag
window the map never uses (from the end of its tag data to where its largest
BSP starts; the pack's header says where: b30 leaves 377 KB there), then,
for the rest, a carve from the texture RAM cache next to the geometry
cache's share when the geometry cache took one (campaign maps: their
textures stream anyway), else the heap. A run of pages always lies in one
block. keep.txt: "animNNN" sets the size in KB (default 512), "animheap"
takes the second block from the heap on every map, "animnowindow" leaves
the tag window alone. */
#include <pspkernel.h>
#include <pspiofilemgr.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <malloc.h>
#include "psp_paths.h"   /* ms0:/halo or ms0:/halomp, and the maps */
#include "psp_metrics.h"

void platform_log(const char *format, ...);
int psp_keep(const char *name);
int psp_keep_number(const char *prefix, int fallback);
int psp_geo_pack_read(unsigned int address, void *to, unsigned int size);
void *psp_tex_carve(unsigned int bytes);
unsigned int psp_tex_carved(void);

#define EXTERNAL_BASE 0x0C000000u
#define CACHE_KB_DEFAULT 512
/* the page when the pack doesn't say (a pack laid out end to end) */
#define PAGE_BYTES_DEFAULT 8192
#define SLOTS_MAX 256
/* ranges handed out most recently, whose slots are never reused */
#define RECENT_RANGES 6

/* the pack's animation data: [first, end) external addresses; pages count
from page_base, the page boundary at or before first */
static unsigned int animation_first = 0, animation_end = 0, page_base = 0;
static unsigned int page_bytes = 0, page_shift = 0, page_count = 0;
static short *page_slot = NULL; /* the slot holding each page, -1 none */

/* the slots: block 0's first, then block 1's (either may hold none) */
static unsigned int block_at[2], block_slots[2], slot_count = 0;
static int arena_carved = 0; /* block 1 is carved from the texture RAM cache */
static void *heap_arena = NULL;
static unsigned int heap_arena_bytes = 0;
static int slot_page[SLOTS_MAX]; /* -1: empty */
static unsigned int slot_used[SLOTS_MAX]; /* the lookup that last used it, 0 never */
static unsigned int lookup_stamp = 0;

struct slot_range
{
	unsigned short first, count;
};
static struct slot_range recent[RECENT_RANGES];

/* the range handed out last: a reader asks for the same animation once per
node, so most lookups end here. psp_animation_data (psp_fixups.py,
FIXES_122) makes this same test inline, so a repeat costs no call: these,
repeats, forget_pending and trace are shared with it under the psp_anim_
names. "animlockcall" (keep.txt): every lookup comes here, as before */
#define memo_start psp_anim_memo_start
#define memo_end psp_anim_memo_end
#define memo_at psp_anim_memo_at
#define repeats psp_anim_repeats
#define forget_pending psp_anim_forget_pending
#define trace psp_anim_trace
unsigned int memo_start = 0, memo_end = 0, memo_at = 0;
int psp_anim_inline = 1;

/* what the log reports (since the map opened, and this window's) */
unsigned long repeats = 0;
static unsigned long lookups = 0, hits = 0, misses = 0, reads = 0, read_bytes = 0, evictions = 0,
	failures = 0, no_room = 0, other_threads = 0;
static unsigned long read_us = 0, read_worst_us = 0;
extern volatile unsigned long psp_stick_read_us; /* build 126: every blocking Memory Stick read's time (psp_frametrace.c) */
static unsigned long window_reads = 0, window_us = 0, window_worst_us = 0;
static SceUID game_thread = 0;
/* set by the power callback's thread before the PSP sleeps (a carved arena's
bytes don't survive it); the game thread forgets at its next lookup */
volatile int forget_pending = 0;

/* "animtrace" (keep.txt): the distinct ranges asked for in each frame,
written to log/animtrace.bin for tools/anim_cache_sim.py, which replays them
against other cache sizes, page sizes and pack layouts. Per frame: the word
0xFFFFFFFF, the frame's number, then an address and a size per range. For
the emulator: the memo is off (every lookup is seen) and the file is written
every 64 KB */
#define TRACE_WORDS (16 * 1024)
#define TRACE_SEEN 256
unsigned int *trace = NULL;
static int trace_words = 0, trace_on = -1;
static SceUID trace_file = -1;
static unsigned int *trace_seen = NULL; /* TRACE_SEEN words after the trace's */
static unsigned int trace_frame = 0;

static void trace_flush(void)
{
	if (trace_file >= 0 && trace_words)
		sceIoWrite(trace_file, trace, trace_words * 4);
	trace_words = 0;
}

static void trace_word(unsigned int word)
{
	if (trace_words >= TRACE_WORDS)
		trace_flush();
	trace[trace_words++] = word;
}

static void trace_range(unsigned int address, unsigned int size)
{
	unsigned int k = ((address >> 2) ^ (size << 7)) & (TRACE_SEEN - 1), probes;

	for (probes = 0; probes < TRACE_SEEN; probes++, k = (k + 1) & (TRACE_SEEN - 1))
	{
		if (trace_seen[k] == address)
			return; /* seen this frame (the size is the same for an address) */
		if (!trace_seen[k])
		{
			trace_seen[k] = address;
			trace_word(address);
			trace_word(size);
			return;
		}
	}
}

/* every frame (psp_geo.c's frame end) */
void psp_anim_frame_end(void)
{
	if (!trace)
		return;
	trace_word(0xFFFFFFFFu);
	trace_word(++trace_frame);
	memset(trace_seen, 0, TRACE_SEEN * 4);
	if (trace_frame % 300 == 0)
		trace_flush();
}

static void trace_open(void)
{
	if (trace_on < 0)
		trace_on = psp_keep("animtrace");
	if (!trace_on || trace)
		return;
	trace = calloc(TRACE_WORDS + TRACE_SEEN, 4);
	if (!trace)
		return;
	trace_seen = trace + TRACE_WORDS;
	trace_file = sceIoOpen(PSP_DATA_ROOT "/log/animtrace.bin", PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
	platform_log("animations: tracing every range asked for to log/animtrace.bin (%s)", trace_file >= 0 ? "open" : "couldn't open it");
}

/* a lookup that can't be served (no pack, a failed read): zeroes, so the
game goes on with a wrong pose rather than reading freed memory; logged.
A failure bigger than the block takes a new, bigger block rather than moving
the old one (b106 reallocated it), since a caller may still hold the old
one. The old ones are freed when the pack closes, when no lookup is held */
#define FAILURE_BLOCKS 8
static void *failure_blocks[FAILURE_BLOCKS];
static unsigned int failure_block_count = 0;
static unsigned int failure_bytes = 0;

int psp_anim_active(void)
{
	return slot_count != 0;
}

static unsigned int slot_address(unsigned int slot)
{
	return slot < block_slots[0] ? block_at[0] + (slot << page_shift) : block_at[1] + ((slot - block_slots[0]) << page_shift);
}

/* next tick's poses on the Media Engine (psp_fixups.py FIXES_131 #poseahead):
while a job is out the ME reads this cache's pages through psp_anim_on_me, so
nothing that moves a page (a read into a slot, forgetting them all) happens
until the job is collected: objects.c sets this to its collector */
void (*psp_anim_me_settle)(void) = NULL;

static void settle_me(void)
{
	if (psp_anim_me_settle)
		psp_anim_me_settle();
}

/* the range's bytes if every page of it is resident, side by side in one
block; NULL otherwise. Reads the page table only: safe on the Media Engine
while this CPU leaves the table alone (settle_me) */
void *psp_anim_resident(unsigned long address, unsigned long size)
{
	unsigned int first_page, last_page, count, k;
	int slot;

	if (!slot_count || !page_slot || address < animation_first || address + size > animation_end || !size)
		return NULL;
	first_page = (address - page_base) >> page_shift;
	last_page = (address + size - 1 - page_base) >> page_shift;
	count = last_page - first_page + 1;
	slot = page_slot[first_page];
	for (k = 1; slot >= 0 && k < count; k++)
		if (page_slot[first_page + k] != slot + (int)k)
			slot = -1;
	if (slot < 0 || ((unsigned int)slot < block_slots[0] && (unsigned int)slot + count > block_slots[0]))
		return NULL;
	return (void *)(slot_address((unsigned int)slot) + (address - (page_base + (first_page << page_shift))));
}

/* psp_animation_data on the Media Engine (or in the emulator's simulation of
it): the resident bytes, never a read. A range not resident (the main CPU
checks before it lists a pose, so only by a fault) is noted in a word of the
ME's own and answered with zeros, so the decode runs to its end (with zeros
for offsets and counts it reads only the first few hundred bytes); the job
then marks that pose unmade */
unsigned int psp_anim_me_missed[16] __attribute__((aligned(64)));
static const unsigned char me_zeros[4096] __attribute__((aligned(64)));

void *psp_anim_on_me(unsigned long address, unsigned long size)
{
	void *at = psp_anim_resident(address, size);

	if (at)
		return at;
	psp_anim_me_missed[0]++;
	return (void *)me_zeros;
}

static void forget_all(void)
{
	unsigned int index;

	settle_me();

	if (page_slot)
		for (index = 0; index < page_count; index++)
			page_slot[index] = -1;
	for (index = 0; index < SLOTS_MAX; index++)
	{
		slot_page[index] = -1;
		slot_used[index] = 0;
	}
	memset(recent, 0, sizeof(recent));
	memo_start = memo_end = memo_at = 0;
}

static void *failed(unsigned int address, unsigned int size, const char *why)
{
	if (failures++ < 8)
		platform_log("animations: %u bytes at %08x: %s", size, address, why);
	if (size > failure_bytes && failure_block_count < FAILURE_BLOCKS)
	{
		/* at least double, so a few blocks cover any size */
		unsigned int bytes = size > 2 * failure_bytes ? size : 2 * failure_bytes;
		void *block = malloc(bytes);

		if (block)
		{
			failure_blocks[failure_block_count++] = block;
			failure_bytes = bytes;
		}
	}
	if (!failure_block_count)
		return NULL;
	/* the newest (biggest) block; still smaller than size only if malloc failed */
	memset(failure_blocks[failure_block_count - 1], 0, failure_bytes);
	return failure_blocks[failure_block_count - 1];
}

static void free_failure_blocks(void)
{
	while (failure_block_count)
		free(failure_blocks[--failure_block_count]);
	failure_bytes = 0;
}

static int protected_slot(unsigned int slot)
{
	int index;

	for (index = 0; index < RECENT_RANGES; index++)
		if (recent[index].count && slot >= recent[index].first && slot < (unsigned int)recent[index].first + recent[index].count)
			return 1;
	return 0;
}

/* the range just handed out joins the protected ones (if it isn't the newest
already) */
static void remember(unsigned int first, unsigned int count)
{
	if (recent[0].count && recent[0].first == first && recent[0].count == count)
		return;
	memmove(recent + 1, recent, sizeof(recent) - sizeof(recent[0]));
	recent[0].first = (unsigned short)first;
	recent[0].count = (unsigned short)count;
}

/* count consecutive slots for a new run of pages: the window whose most
recently used slot is the oldest, empty slots counting as never used, and
no window holding a protected slot. -1: none */
static int choose_slots(unsigned int count)
{
	int best = -1;
	unsigned int best_age = 0xFFFFFFFFu, start, k;

	if (count > slot_count)
		return -1;
	for (start = 0; start + count <= slot_count; start++)
	{
		unsigned int newest = 0;
		int usable = 1;

		if (start < block_slots[0] && start + count > block_slots[0])
			continue; /* a run lies in one block */
		for (k = 0; k < count && usable; k++)
		{
			if (protected_slot(start + k))
				usable = 0;
			else if (slot_used[start + k] > newest)
				newest = slot_used[start + k];
		}
		if (usable && newest < best_age)
		{
			best = (int)start;
			best_age = newest;
			if (!newest)
				break; /* all empty: can't do better */
		}
	}
	return best;
}

static void read_pages(unsigned int first_page, unsigned int count, unsigned int slot, unsigned int address, unsigned int size)
{
	unsigned int bytes = count << page_shift, from = page_base + (first_page << page_shift), k;
	unsigned int start = sceKernelGetSystemTimeLow(), took;
	void *to = (void *)slot_address(slot);
	int got;

	/* the old tenants out of the page table first (and duplicates of these
	pages elsewhere: one copy of a page is mapped at a time) */
	for (k = 0; k < count; k++)
	{
		int old = slot_page[slot + k];

		if (old >= 0)
		{
			page_slot[old] = -1;
			evictions++;
		}
		slot_page[slot + k] = -1;
		if (page_slot[first_page + k] >= 0)
		{
			slot_page[page_slot[first_page + k]] = -1;
			page_slot[first_page + k] = -1;
		}
	}
	if (from + bytes > animation_end)
		bytes = animation_end - from;
	/* any dirty lines of the slots' last tenants go to memory now, not over
	the new bytes later */
	sceKernelDcacheWritebackInvalidateRange(to, count << page_shift);
	{
		/* build 115: "iostats" counts these reads as the animations' (psp_trace.c) */
		void psp_io_kind_push_animations(void);
		void psp_io_kind_pop(void);

		psp_io_kind_push_animations();
		got = psp_geo_pack_read(from, to, bytes);
		psp_io_kind_pop();
	}
	took = sceKernelGetSystemTimeLow() - start;
	read_us += took;
	psp_stick_read_us += took; /* build 126: frametrace */
	PSP_METRICS_STICK(PSP_METRICS_ANIMATIONS, bytes, took);
	window_us += took;
	reads++;
	window_reads++;
	if (took > read_worst_us)
		read_worst_us = took;
	if (took > window_worst_us)
		window_worst_us = took;
	if (got != (int)bytes)
	{
		if (failures++ < 8)
			platform_log("animations: reading %u bytes at %08x (for %u at %08x) failed (%d)", bytes, from, size, address, got);
		return;
	}
	read_bytes += bytes;
	sceKernelDcacheWritebackInvalidateRange(to, bytes);
	for (k = 0; k < count; k++)
	{
		slot_page[slot + k] = (int)(first_page + k);
		page_slot[first_page + k] = (short)(slot + k);
	}
}

/* size bytes at the external address: a pointer to them in RAM, valid until
RECENT_RANGES more ranges have been asked for */
void *psp_anim_lock(unsigned long address, unsigned long size)
{
	unsigned int first_page, last_page, count, k;
	int slot;

	if (address >= memo_start && address + size <= memo_end && !forget_pending && !trace)
	{
		repeats++;
		return (void *)(memo_at + (address - memo_start));
	}
	lookups++;
	if (trace)
		trace_range((unsigned int)address, (unsigned int)size);
	if (forget_pending)
	{
		forget_pending = 0;
		forget_all();
	}
	if (sceKernelGetThreadId() != game_thread && other_threads++ < 4)
		platform_log("animations: a lookup from thread %x, not the game's (%x)", (unsigned)sceKernelGetThreadId(), (unsigned)game_thread);
	if (!slot_count)
		return failed(address, size, "no animation cache (the map's geometry pack isn't open?)");
	if (address < animation_first || address + size > animation_end || !size)
		return failed(address, size, "outside the pack's animation data");
	first_page = (address - page_base) >> page_shift;
	last_page = (address + size - 1 - page_base) >> page_shift;
	count = last_page - first_page + 1;
	lookup_stamp++;
	slot = page_slot[first_page];
	for (k = 1; slot >= 0 && k < count; k++)
		if (page_slot[first_page + k] != slot + (int)k)
			slot = -1;
	/* build 107: and in one block. Pages read one at a time can sit in the
	first block's last slot and the second block's first, numbered one after
	the other but not side by side in memory: a range across them was taken
	as a hit and read past the first block's end. Not seen to happen (a10's
	crash was the BSP fill: FIXES_107 cache_files.c#bspfill), but ranges of
	several pages are common: a10's cinematics are up to 32 KB, b30's
	animations up to 15 KB */
	if (slot >= 0 && (unsigned int)slot < block_slots[0] && (unsigned int)slot + count > block_slots[0])
		slot = -1;
	if (slot >= 0)
		hits++;
	else
	{
		misses++;
		settle_me();
		slot = choose_slots(count);
		if (slot < 0)
		{
			no_room++;
			return failed(address, size, "no room (every slot holds a range still in use)");
		}
		read_pages(first_page, count, (unsigned int)slot, (unsigned int)address, (unsigned int)size);
		if (page_slot[first_page] != slot)
			return failed(address, size, "the read failed");
	}
	for (k = 0; k < count; k++)
		slot_used[slot + k] = lookup_stamp;
	remember((unsigned int)slot, count);
	memo_start = page_base + (first_page << page_shift);
	memo_end = memo_start + (count << page_shift);
	if (memo_end > animation_end)
		memo_end = animation_end;
	memo_at = slot_address((unsigned int)slot);
	return (void *)(memo_at + (address - memo_start));
}

/* ---------- the geometry cache's calls (psp_geo.c): the pack opened, closed */

void psp_anim_close(void)
{
	if (arena_carved)
	{
		/* the geometry cache carves again for the next map */
		arena_carved = 0;
	}
	block_at[0] = block_at[1] = block_slots[0] = block_slots[1] = 0;
	slot_count = 0;
	free(page_slot);
	page_slot = NULL;
	page_count = 0;
	animation_first = animation_end = page_base = 0;
	memo_start = memo_end = memo_at = 0;
	free_failure_blocks();
}

/* where its block in the tag window ends (0: none), so the geometry cache
can take what lies past it (psp_geo.c, "geowindow") */
unsigned int psp_anim_window_end(void)
{
	return block_slots[0] ? block_at[0] + (block_slots[0] << page_shift) : 0;
}

/* the pack holds animation data: bytes of it in [first, end) (offsets from
the pack's data, as its header gives them), laid out for page (0: not laid
out for one); window_at and window_bytes, the tag window's unused part (0:
none); carve_from_textures when the geometry cache's own share came from the
texture RAM cache */
void psp_anim_map_opened(unsigned int bytes, unsigned int first, unsigned int end, unsigned int page,
	unsigned int window_at, unsigned int window_bytes, int carve_from_textures)
{
	unsigned int cache_bytes, second_bytes;
	void *block = NULL;

	psp_anim_close();
	forget_pending = 0;
	game_thread = sceKernelGetThreadId();
	psp_anim_inline = !psp_keep("animlockcall");
	repeats = lookups = hits = misses = reads = read_bytes = evictions = failures = no_room = other_threads = 0;
	read_us = read_worst_us = 0;
	if (!bytes || end <= first)
		return;
	if (page < 4096 || page > 32768 || (page & (page - 1)))
		page = PAGE_BYTES_DEFAULT;
	for (page_shift = 12; (1u << page_shift) < page; page_shift++)
		;
	page_bytes = 1u << page_shift;
	cache_bytes = (unsigned int)psp_keep_number("anim", CACHE_KB_DEFAULT) * 1024;
	/* at least 128 KB: the largest animation of the maps measured is 32 KB
	(a10's cinematics; b30's is 15 KB), and a few more ranges are kept while
	one is read */
	if (cache_bytes < 128 * 1024)
		cache_bytes = 128 * 1024;
	if (cache_bytes > SLOTS_MAX * page_bytes)
		cache_bytes = SLOTS_MAX * page_bytes;
	cache_bytes &= ~(page_bytes - 1);
	animation_first = EXTERNAL_BASE + first;
	animation_end = EXTERNAL_BASE + end;
	page_base = EXTERNAL_BASE + (first & ~(page_bytes - 1));
	page_count = ((animation_end - page_base) + page_bytes - 1) >> page_shift;
	page_slot = malloc(page_count * sizeof(page_slot[0]));
	if (!page_slot)
	{
		platform_log("animations: no room for the page table (%u pages)", page_count);
		page_count = 0;
		return;
	}
	/* block 0: the tag window's unused part, a whole number of pages from a
	page-aligned start (none if under 64 KB is left) */
	if (window_bytes && !psp_keep("animnowindow"))
	{
		unsigned int start = (window_at + page_bytes - 1) & ~(page_bytes - 1), stop = window_at + window_bytes;
		unsigned int slots = stop > start ? (stop - start) >> page_shift : 0;

		if (slots > cache_bytes >> page_shift)
			slots = cache_bytes >> page_shift;
		if (slots << page_shift >= 64 * 1024)
		{
			block_at[0] = start;
			block_slots[0] = slots;
		}
	}
	/* block 1: the rest */
	second_bytes = cache_bytes - (block_slots[0] << page_shift);
	if (second_bytes && carve_from_textures && !psp_keep("animheap"))
	{
		unsigned int before = psp_tex_carved();

		block = psp_tex_carve(before + second_bytes);
		arena_carved = block != NULL;
	}
	if (second_bytes && !block)
	{
		if (heap_arena && heap_arena_bytes != second_bytes)
		{
			free(heap_arena);
			heap_arena = NULL;
		}
		if (!heap_arena)
		{
			heap_arena = memalign(64, second_bytes);
			heap_arena_bytes = heap_arena ? second_bytes : 0;
		}
		block = heap_arena;
	}
	if (block)
	{
		block_at[1] = (unsigned int)block;
		block_slots[1] = second_bytes >> page_shift;
	}
	slot_count = block_slots[0] + block_slots[1];
	if (slot_count << page_shift < 128 * 1024)
	{
		platform_log("animations: no RAM for the cache (%u KB found of %u)", (slot_count << page_shift) / 1024, cache_bytes / 1024);
		block_slots[0] = block_slots[1] = slot_count = 0;
		free(page_slot);
		page_slot = NULL;
		return;
	}
	forget_all();
	trace_open();
	platform_log("animations: %u KB of frame data in the pack (%u KB with its padding, laid out for %u KB pages); cache %u KB in pages of %u KB: %u KB at %08x (the tag window's unused end), %u KB at %08x (%s)",
		bytes / 1024, (end - first) / 1024, page / 1024, (slot_count << page_shift) / 1024, page_bytes / 1024,
		(block_slots[0] << page_shift) / 1024, block_at[0], (block_slots[1] << page_shift) / 1024, block_at[1],
		!block_slots[1] ? "none" : arena_carved ? "from the texture RAM cache" : "heap");
}

/* the PSP is about to sleep: a carved arena goes back to the system with the
volatile partition, so what's in it is forgotten (read again on use) */
void psp_anim_suspend(void)
{
	if (arena_carved)
		forget_pending = 1;
}

/* every 300 frames, from the geometry cache's report (psp_geo.c) */
void psp_anim_report(void)
{
	static unsigned int last_stamp = 0;
	unsigned int used = 0, resident = 0, index;

	if (!slot_count)
		return;
	for (index = 0; index < slot_count; index++)
	{
		if (slot_page[index] >= 0)
			resident++;
		if (slot_used[index] > last_stamp)
			used++;
	}
	last_stamp = lookup_stamp;
	platform_log("animations: %lu lookups (%lu more of the same range); %lu hits, %lu misses, %lu reads (%lu KB) in %lu ms, slowest %lu ms; %lu evicted, %lu without room, %lu failed; %u of %u pages resident, %u used since the last report",
		lookups, repeats, hits, misses, reads, read_bytes / 1024, read_us / 1000, read_worst_us / 1000, evictions, no_room,
		failures, resident, slot_count, used);
}

/* this heartbeat window's reads, for the log's io: line (psp_platform.c) */
void psp_anim_io_window(unsigned long *count, unsigned long *us, unsigned long *worst_us)
{
	*count = window_reads;
	*us = window_us;
	*worst_us = window_worst_us;
	window_reads = window_us = window_worst_us = 0;
}
