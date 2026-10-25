/* psp_geo.c - the geometry cache

Why: a campaign map's tags are 14-17 MB against the PSP-1000's 9 MB tag
window, and 4.5-5.4 MB of that is model vertex and index data, plus 2-4 MB
of the level's render vertices in the BSP. None of it is read by the game's
logic: the renderer draws it, and object lighting looks up three vertices'
texture coordinates now and then. So the map is built without it (the
Invader fork's -G option): the data goes to ms0:/halo/maps/<map>.pgeo, and
every pointer to it in the tags is 0x0C000000 + its offset in that file.

This cache turns such a pointer back into bytes in RAM: psp_geo_lock(address,
size) answers with a resident copy, or NULL and a read queued. Reads are
asynchronous (sceIoReadAsync: the Memory Stick's DMA fills the block while
the CPU goes on; halo_bench v10 on a PSP-1000: a 1 MB read in 65 ms with
97% of the CPU kept, as long as the file's async priority sits above the
game thread's, or the completion never runs). A block that isn't there yet
is skipped for a frame, as the Xbox's texture cache skipped a texture.

The arena is carved from the texture RAM cache (the volatile partition:
reloadable data, given back before the PSP sleeps). Blocks are evicted
oldest first, never one locked this frame or last.

What the renderer must know: a block's address can be reused for another
range after an eviction, so anything it worked out about the bytes there
(the in-place conversion of model vertices, a skinning plan) is tagged with
psp_geo_generation(), which changes with every load into the block. */
#include <pspkernel.h>
#include <pspiofilemgr.h>
#include <stdio.h>
#include <stdlib.h>
#include <malloc.h>
#include <string.h>
#include "psp_me.h" /* psp_on_me */
#include "psp_paths.h"   /* ms0:/halo or ms0:/halomp, and the maps */
#include "psp_metrics.h"

void platform_log(const char *format, ...);
int psp_keep(const char *name);
int psp_keep_number(const char *prefix, int fallback);
/* the animation cache, this pack's second reader (psp_anim.c) */
void psp_anim_close(void);
void psp_anim_map_opened(unsigned int bytes, unsigned int first, unsigned int end, unsigned int page,
	unsigned int window_at, unsigned int window_bytes, int carve_from_textures);
void psp_anim_suspend(void);
void psp_anim_report(void);
void psp_anim_frame_end(void);
unsigned int psp_anim_window_end(void);

#define EXTERNAL_BASE 0x0C000000u
#define HEADER_BYTES 64
#define GEO_ENTRIES 512
#define GEO_PENDING 48
#define RECENT_FRAMES 2
#ifndef ARENA_BYTES
#define ARENA_BYTES (1024 * 1024)
#endif
#define ASYNC_PRIORITY 0x10 /* above the game thread's 0x20 and the audio thread's 0x12 */

struct geo_entry
{
	unsigned int address; /* external */
	unsigned int size;
	unsigned int at; /* resident or loading block, 0 if neither */
	unsigned int last_frame;
	unsigned int generation;
	unsigned char state; /* 0 nothing, 1 queued, 2 loading, 3 resident */
	unsigned char pad[3];
};

static SceUID pack = -1;
static unsigned int pack_size = 0;
static char pack_name[64];
static unsigned int arena_start = 0, arena_end = 0, arena_next = 0;
/* a second stretch of the ring, 0 to 0 when there is none: with "soundgeo",
the main RAM the sound cache no longer takes (psp_dsound.c); otherwise, with
"geowindow", the part of the tag window the map never uses past what the
animation cache took (psp_geo_window). Both are per map, and many maps have
no window part, so the cache never counts on it: without it the cache is the
arena above */
static unsigned int extra_start = 0, extra_end = 0;

/* the end of the stretch that holds start (0 if neither does) */
static unsigned int stretch_end(unsigned int start)
{
	if (start >= arena_start && start < arena_end)
		return arena_end;
	if (start >= extra_start && start < extra_end)
		return extra_end;
	return 0;
}

static unsigned int largest_stretch(void)
{
	return arena_end - arena_start > extra_end - extra_start ? arena_end - arena_start : extra_end - extra_start;
}

static int in_cache(unsigned int a)
{
	return stretch_end(a) != 0;
}

/* the cache's addresses, for "gereads" (psp_ge.c) */
void psp_geo_range(unsigned int *start, unsigned int *end)
{
	*start = arena_start;
	*end = arena_end;
}

int psp_geo_holds(unsigned int address)
{
	return in_cache(address);
}
/* the cache's size when it is carved from the texture RAM cache (a
campaign map): 1 MB, or 1.5 / 2 MB with "geo15" / "geo2" (launch menu). At
1 MB a walk through the Library loaded 1,899 blocks on the PSP, 375 frames
missed one, and the area ahead flashed in and out at transitions
(docs/overnight_b102.md). The asset track's smaller textures leave the
texture cache room to give */
static unsigned int arena_bytes = ARENA_BYTES;
static struct geo_entry entries[GEO_ENTRIES];
static int entry_count = 0;
static unsigned short pending[GEO_PENDING];
static int pending_count = 0;
static int inflight = -1;
static unsigned int frame = 1, generation_counter = 0;
static unsigned long hits = 0, misses = 0, loads = 0, load_bytes = 0, evictions = 0, too_big = 0, failures = 0,
	miss_frames = 0, queue_full = 0, no_room = 0;
static int frame_missed = 0;

#ifdef PSP_RECON
/* "geocluster" (PSP_RECON=1; docs/plan_measurement_round.md, the new
counters): the misses by where the camera was, its BSP and cluster
(structure_visibility_compute names it each frame, psp_fixups.py
FIXES_RECON #geocluster), so a stretch's churn can be put on the map: the
places whose geometry doesn't fit the cache together with their
neighbours' */
#define GEO_PLACES 64
static struct { short bsp, cluster; unsigned long frames, misses, bytes; } geo_places[GEO_PLACES];
static int geo_place = -1, geo_cluster_on = -1;

void psp_geo_camera(short bsp, short cluster)
{
	unsigned int k, n;

	if (geo_cluster_on < 0)
		geo_cluster_on = psp_keep("geocluster") != 0;
	if (!geo_cluster_on)
		return;
	geo_place = -1;
	k = ((unsigned int)(unsigned short)cluster * 31u + (unsigned int)(unsigned short)bsp) % GEO_PLACES;
	for (n = 0; n < GEO_PLACES; n++, k = (k + 1) % GEO_PLACES)
	{
		if (!geo_places[k].frames && !geo_places[k].misses)
		{
			geo_places[k].bsp = bsp;
			geo_places[k].cluster = cluster;
		}
		if (geo_places[k].bsp == bsp && geo_places[k].cluster == cluster)
		{
			geo_place = (int)k;
			geo_places[k].frames++;
			return;
		}
	}
}

static void geo_places_report(void)
{
	char line[480];
	int length, shown, k;

	if (geo_cluster_on <= 0)
		return;
	length = snprintf(line, sizeof(line), "geometry by the camera's place (geocluster; bsp:cluster misses, KB asked, frames there), the most missed first:");
	for (shown = 0; shown < 12 && length < (int)sizeof(line) - 40; shown++)
	{
		int best = -1;

		for (k = 0; k < GEO_PLACES; k++)
			if (geo_places[k].frames && (best < 0 || geo_places[k].misses > geo_places[best].misses))
				best = k;
		if (best < 0 || !geo_places[best].misses)
			break;
		length += snprintf(line + length, sizeof(line) - length, " %d:%d %lu %lu %lu;", geo_places[best].bsp, geo_places[best].cluster,
			geo_places[best].misses, geo_places[best].bytes / 1024, geo_places[best].frames);
		geo_places[best].frames = 0; /* shown: out of the running */
	}
	platform_log("%s", line);
	memset(geo_places, 0, sizeof(geo_places));
	geo_place = -1;
}
#endif
static unsigned int read_start_us = 0;
static unsigned long read_us = 0, read_worst_us = 0;
extern volatile unsigned long psp_stick_read_us; /* build 126: every blocking Memory Stick read's time (psp_frametrace.c) */

int psp_geo_open_count(void) { return pack >= 0; }

/* the open map's bake formats (its pack's header word 10; 0 for a
map without a pack): which tag blocks are already in the PSP's slim layouts
(psp_tag_layouts.c) */
static unsigned long map_formats = 0;

unsigned long psp_geo_map_formats(void) { return map_formats; }

/* bit 4: the level's rendered vertices are packed without the binormal and
tangent, which the port never reads (tools/levelslim.py): 24 bytes each, the
uv right after the normal */
#define PGEO_LEVEL_SLIM 0x10u

/* the bytes of one of the level's rendered vertices in the pack */
unsigned int psp_geo_level_stride(void) { return map_formats & PGEO_LEVEL_SLIM ? 24u : 32u; }

/* the map's layout, from its first model part's vertex descriptor: a map
built with its geometry outside (invader-build -G) keeps 6 bytes a level
vertex in the tags (three int16 about the material's centroid, times a scale
in the unused uncompressed vertex data's size field), its leaf surface
references as two u16 and its surface index lists as u16; one built as
before keeps the Xbox's 32-byte vertices and 32-bit indices. Read by the
game's own readers of level data (structures.c, structure_visibility.c,
object_lights.c; psp_fixups.py). Set at map open whether or not the pack
file is there */
long psp_structure_vertex_stride = 32;

static int alignment(unsigned int size)
{
	return (size + 63) & ~63u;
}

/* ---------- "memcensus" (docs/memory_census.md): what size would stop the
churn. Two counts, read at each 300-frame report:
- the working set: the bytes of every block wanted in the last 1, 5, 10 and
  30 s of frames (an entry's last_frame), whether or not it is resident;
- each read of a block read before, by the bytes the cache read between its
  last read and this one. The arena is used as a ring, so a block survives
  until about the cache's size has been read after it: a re-read at distance
  d would have been a hit with a cache larger than d. Re-reads under the
  cache's own size were lost to its gaps and the blocks it may not evict
  (the policy's share, not the size's).
Off: one test per read from the Memory Stick */
static int census_on = -1;
#define CENSUS_BUCKETS 7
static unsigned int census_read_at[GEO_ENTRIES]; /* load_bytes + 1 at an entry's last read; 0: never read */
static unsigned long census_rereads[CENSUS_BUCKETS], census_first = 0, census_forgotten = 0;
static const char *const census_bucket_names[CENSUS_BUCKETS] = {
	"under the cache's size", "+256 KB", "+512 KB", "+1 MB", "+2 MB", "+4 MB", "more" };

/* the entries by external address, chained in buckets (the
emulator's PC samples: 3% of b30's beach fight in finding an entry, a scan
of the 300-odd resident ones for each of the frame's few hundred locks) */
#define GEO_HASH 1024
static short hash_head[GEO_HASH]; /* an entry's index + 1, 0: none */
static short hash_next[GEO_ENTRIES];
/* the entry psp_geo_lock handed out last: psp_geo_generation and
psp_geo_address_of are asked about it right after, by the renderer */
static int last_locked = -1;

static unsigned int hash_of(unsigned int address)
{
	return ((address >> 6) * 2654435761u) >> 22;
}

static void hash_insert(int index)
{
	unsigned int bucket = hash_of(entries[index].address);

	hash_next[index] = hash_head[bucket];
	hash_head[bucket] = (short)(index + 1);
}

static void hash_remove(int index)
{
	short *link = &hash_head[hash_of(entries[index].address)];

	while (*link)
	{
		if (*link == index + 1)
		{
			*link = hash_next[index];
			return;
		}
		link = &hash_next[*link - 1];
	}
}

static struct geo_entry *find(unsigned int address)
{
	short at = hash_head[hash_of(address)];

	while (at)
	{
		if (entries[at - 1].address == address)
			return &entries[at - 1];
		at = hash_next[at - 1];
	}
	return NULL;
}

void psp_ge_show3_reuse(int kind, unsigned int stamp); /* psp_ge.c: geshow3check */

/* the frame counter, for geshow3check's stamps (psp_ge.c) */
unsigned int psp_geo_frame_now(void)
{
	return frame;
}

static int protected_entry(const struct geo_entry *e)
{
	return e->state == 2 || (e->at && e->last_frame + RECENT_FRAMES > frame);
}

/* room in the arena for size bytes: at the ring position, the start, or
after a protected block; whatever unprotected lies in the chosen space is
evicted. 0 if nothing fits */
static int fits(unsigned int start, unsigned int size)
{
	unsigned int end = start + size;
	int index;

	if (end > stretch_end(start))
		return 0;
	for (index = 0; index < entry_count; index++)
	{
		const struct geo_entry *e = &entries[index];

		if (e->at && protected_entry(e) && e->at < end && e->at + alignment(e->size) > start)
			return 0;
	}
	return 1;
}

/* The ring passes over blocks drawn in the last geo_keep_frames frames (a
second chance), up to GEO_SKIP_STEPS times, before it evicts by position
alone. Without it the ring evicted whatever it reached, oldest loaded first
however often drawn: once a level's working set neared the cache's size,
each wrap of the ring (about every 20 s on a10 and b30 on the PSP-1000)
threw out blocks still in view, read them again within frames, and drew
them late (0.7-1.6 MB a wrap). "geofifo" (or "geokeep0"): as before */
#define GEO_SKIP_STEPS 16
static int geo_keep_frames = -1;
static unsigned long skips = 0, skip_fallbacks = 0;

static int kept_entry(const struct geo_entry *e)
{
	return protected_entry(e) || (e->at && e->last_frame + (unsigned int)geo_keep_frames > frame);
}

/* where the ring goes on once at has passed its stretch: from the arena to
the second stretch when there is one, else back to the arena's start */
static unsigned int ring_after(unsigned int at)
{
	if (extra_start && at >= arena_start && at <= arena_end)
		return extra_start;
	return arena_start;
}

static unsigned int allocate_skipping(unsigned int size)
{
	unsigned int at = arena_next;
	int steps, index;

	for (steps = 0; steps < GEO_SKIP_STEPS; steps++)
	{
		unsigned int past = 0;

		/* past a stretch's end the ring goes on at the other's start, as
		allocate's does; a block no stretch holds is left to allocate */
		if (!stretch_end(at) || at + size > stretch_end(at))
			at = ring_after(at);
		if (at + size > stretch_end(at))
			at = ring_after(at);
		if (at + size > stretch_end(at))
			break;
		for (index = 0; index < entry_count; index++)
		{
			const struct geo_entry *e = &entries[index];
			unsigned int end;

			if (!e->at || e->at >= at + size || e->at + alignment(e->size) <= at || !kept_entry(e))
				continue;
			end = e->at + alignment(e->size);
			if (end > past)
				past = end;
		}
		if (!past)
			return at;
		skips++;
		at = past;
	}
	skip_fallbacks++;
	return 0;
}

static unsigned int allocate(unsigned int size)
{
	unsigned int start = 0;
	int index;

	if (!arena_start || size > largest_stretch())
		return 0;
	if (geo_keep_frames < 0)
		geo_keep_frames = psp_keep("geofifo") ? 0 : psp_keep_number("geokeep", 60);
	if (geo_keep_frames > 0)
		start = allocate_skipping(size);
	if (start)
		;
	/* the ring runs on from the end of one stretch to the start of the other */
	else if (fits(arena_next, size))
		start = arena_next;
	else if (extra_start && arena_next >= arena_start && arena_next <= arena_end && fits(extra_start, size))
		start = extra_start;
	else if (fits(arena_start, size))
		start = arena_start;
	else if (extra_start && fits(extra_start, size))
		start = extra_start;
	else
		for (index = 0; index < entry_count && !start; index++)
		{
			unsigned int candidate;

			if (!entries[index].at || !protected_entry(&entries[index]))
				continue;
			candidate = entries[index].at + alignment(entries[index].size);
			if (fits(candidate, size))
				start = candidate;
		}
	if (!start)
		return 0;
	for (index = 0; index < entry_count; index++)
	{
		struct geo_entry *e = &entries[index];

		if (e->at && e->at < start + size && e->at + alignment(e->size) > start)
		{
			psp_ge_show3_reuse(2, e->last_frame); /* geshow3check */
			e->at = 0;
			e->state = 0;
			evictions++;
		}
	}
	arena_next = start + size;
	return start;
}

static unsigned int cache_bytes(void)
{
	return (arena_end - arena_start) + (extra_end - extra_start);
}

/* a block has been read (before load_bytes counts it) */
static void census_read(const struct geo_entry *e)
{
	int index = (int)(e - entries), bucket;
	unsigned int distance, size;

	if (census_on <= 0)
		return;
	if (!census_read_at[index])
		census_first++;
	else
	{
		distance = (unsigned int)load_bytes - (census_read_at[index] - 1);
		size = cache_bytes();
		bucket = distance < size ? 0 : distance < size + 256 * 1024 ? 1 : distance < size + 512 * 1024 ? 2 :
			distance < size + 1024 * 1024 ? 3 : distance < size + 2048 * 1024 ? 4 : distance < size + 4096 * 1024 ? 5 : 6;
		census_rereads[bucket]++;
	}
	census_read_at[index] = (unsigned int)load_bytes + 1;
}

/* the read in flight has finished with this result */
static void complete(SceInt64 result)
{
	struct geo_entry *e;

	e = &entries[inflight];
	inflight = -1;
	read_us += sceKernelGetSystemTimeLow() - read_start_us;
	if ((unsigned int)result == e->size && e->at)
	{
		/* whichever way the kernel filled the block (the card's DMA, or a copy
		through the cache), the CPU's view is made to agree with memory */
		sceKernelDcacheWritebackInvalidateRange((void *)e->at, e->size);
		e->state = 3;
		e->generation = ++generation_counter;
		e->last_frame = frame;
		census_read(e);
		loads++;
		load_bytes += e->size;
	}
	else
	{
		if (failures++ < 8)
			platform_log("geometry: reading %u bytes at %08x failed (%ld)", e->size, e->address, (long)result);
		e->at = 0;
		e->state = 0;
	}
}

/* the read in flight, if it has finished */
static void poll(void)
{
	SceInt64 result = 0;

	if (inflight < 0)
		return;
	if (sceIoPollAsync(pack, &result) != 0)
		return;
	complete(result);
}

/* a synchronous read of size bytes at an external address, for
the animation cache (psp_anim.c), whose data shares this pack (invader-build
-M). On the game thread, like the geometry cache's own reads; a geometry read
in flight ("geoasync") is finished first. The bytes read, or a negative error */
/* the seek and its read under one hold of the stick's lock (psp_trace.c);
"geolockeach": the lock taken per call, as before */
void psp_stick_hold(SceUID fd);
void psp_stick_release(void);
static int geo_lock_each = -1;
static int geo_holding = 0;

static void stick_hold_begin(void)
{
	if (geo_lock_each < 0)
		geo_lock_each = psp_keep("geolockeach") != 0;
	geo_holding = !geo_lock_each;
	if (geo_holding)
		psp_stick_hold(pack);
}

static void stick_hold_end(void)
{
	if (geo_holding)
		psp_stick_release();
	geo_holding = 0;
}

int psp_geo_pack_read(unsigned int address, void *to, unsigned int size)
{
	if (pack < 0 || address < EXTERNAL_BASE || address - EXTERNAL_BASE + size > pack_size)
		return -1;
	if (inflight >= 0)
	{
		SceInt64 result = 0;

		sceIoWaitAsync(pack, &result);
		complete(result);
	}
	{
		int got;

		stick_hold_begin();
		got = sceIoLseek32(pack, HEADER_BYTES + (address - EXTERNAL_BASE), PSP_SEEK_SET) < 0 ? -1 :
			sceIoRead(pack, to, size);
		stick_hold_end();
		return got;
	}
}

/* starts the next queued read, if none is in flight and the block has room */
/* synchronous reads unless keep.txt says "geoasync"; at most
SYNC_FRAME_BUDGET bytes a frame (about 12 ms at the Memory Stick's measured
16 MB/s) */
static int async_reads = 0;
static void *heap_arena = NULL;
/* the arena carved from the texture RAM cache (campaign maps) */
static int arena_carved = 0;
/* the texture caches' size: VRAM 1,216 KB and RAM 4,096 KB */
#define TEXTURE_CACHES_BYTES (5312 * 1024)
static unsigned long window_reads = 0, window_us = 0, window_worst_us = 0;
#define SYNC_FRAME_BUDGET (192 * 1024)
static unsigned int frame_read_bytes = 0;
static unsigned long frame_read_us = 0, frame_read_worst_us = 0;

/* R4: a draw whose geometry isn't resident is read for at once, past the
frame's share, while the frame has read under twice the share. Left out
instead, an object vanishes for a frame or two as it comes into view (seen
on the PSP approaching b30's landing pad: docs/hardware_b116.md, test 1),
and the Xbox never leaves one out. The frame pays for the read instead: at
the 1.7 MB/s the PSP read geometry in that run, the second share is up to
~110 ms [M, U]. Keep word "geoshare": such a draw is left out for the frame */
#define SYNC_FRAME_CAP (2 * SYNC_FRAME_BUDGET)
static int geo_share_only = -1;
static unsigned long past_share_reads = 0, past_share_bytes = 0;

static void pending_remove(int k)
{
	memmove(pending + k, pending + k + 1, (pending_count - k - 1) * sizeof(pending[0]));
	pending_count--;
}

/* reads entry e into the arena now, on this thread: 1 when it is resident
after, 0 when there was no room (it stays as it was), -1 when the read
failed (it goes back to state 0) */
static int read_entry_now(struct geo_entry *e)
{
	unsigned int at = allocate(alignment(e->size)), start, took;
	int got;

	if (!at)
	{
		no_room++;
		return 0;
	}
	/* the previous tenant's lines, if any are still dirty in the cache, go
	back to memory now rather than over the new bytes later */
	sceKernelDcacheWritebackInvalidateRange((void *)at, alignment(e->size));
	start = sceKernelGetSystemTimeLow();
	stick_hold_begin();
	got = sceIoLseek32(pack, HEADER_BYTES + (e->address - EXTERNAL_BASE), PSP_SEEK_SET) < 0 ? -1 :
		sceIoRead(pack, (void *)at, e->size);
	stick_hold_end();
	if (got != (int)e->size)
	{
		if (failures++ < 8)
			platform_log("geometry: reading %u bytes at %08x failed (%d)", e->size, e->address, got);
		e->state = 0;
		return -1;
	}
	sceKernelDcacheWritebackInvalidateRange((void *)at, e->size);
	e->at = at;
	e->state = 3;
	e->generation = ++generation_counter;
	e->last_frame = frame;
	census_read(e);
	loads++;
	load_bytes += e->size;
	frame_read_bytes += e->size;
	took = sceKernelGetSystemTimeLow() - start;
	read_us += took;
	frame_read_us += took;
	psp_stick_read_us += took; /* build 126: frametrace (the asynchronous reads, which overlap other work, aren't counted) */
	window_reads++;
	window_us += took;
	if (took > window_worst_us)
		window_worst_us = took;
	PSP_METRICS_STICK(PSP_METRICS_GEOMETRY, e->size, took);
	return 1;
}

static void pump(void)
{
	while (inflight < 0 && pending_count > 0)
	{
		struct geo_entry *e = &entries[pending[0]];
		unsigned int at;

		if (e->state != 1)
		{
			/* dropped meanwhile */
			pending_remove(0);
			continue;
		}
		/* this frame's read share first, then the room: allocating first
		(evicting) and only then finding the share spent would evict a
		block for nothing on each further miss that frame */
		if (!async_reads && frame_read_bytes >= SYNC_FRAME_BUDGET)
			return;
		if (!async_reads)
		{
			/* read now, on this thread. With these reads asynchronous, every
			frame took 2.5 or 5 s on the PSP, all of it in the frame's end
			(render_frame_present 97%), where the texture cache's own reads
			(synchronous, the same Memory Stick) wait behind them
			(docs/streaming.md §2). "geoasync" in keep.txt brings the
			asynchronous reads back */
			if (read_entry_now(e) == 0)
				return; /* everything is in use this frame: try again next frame */
			pending_remove(0);
			continue;
		}
		at = allocate(alignment(e->size));
		if (!at)
		{
			/* everything is in use this frame: try again next frame */
			no_room++;
			return;
		}
		pending_remove(0);
		/* the previous tenant's lines, if any are still dirty in the cache,
		go back to memory now rather than over the new bytes later */
		sceKernelDcacheWritebackInvalidateRange((void *)at, alignment(e->size));
		if (sceIoLseek32(pack, HEADER_BYTES + (e->address - EXTERNAL_BASE), PSP_SEEK_SET) < 0 ||
			sceIoReadAsync(pack, (void *)at, e->size) < 0)
		{
			if (failures++ < 8)
				platform_log("geometry: couldn't start reading %u bytes at %08x", e->size, e->address);
			e->state = 0;
			continue;
		}
		e->at = at;
		e->state = 2;
		inflight = (int)(e - entries);
		read_start_us = sceKernelGetSystemTimeLow();
	}
}

/* R4: e, wanted by a draw this frame and not read by the pump,
read now past the frame's share; 1 when it is resident after */
static int read_past_share(struct geo_entry *e)
{
	int k;

	if (geo_share_only < 0)
		geo_share_only = psp_keep("geoshare");
	if (geo_share_only || async_reads || frame_read_bytes >= SYNC_FRAME_CAP || (e->state != 0 && e->state != 1))
		return 0;
	if (read_entry_now(e) != 1)
		return 0;
	for (k = 0; k < pending_count; k++)
		if (pending[k] == (unsigned short)(e - entries))
		{
			pending_remove(k);
			break;
		}
	past_share_reads++;
	past_share_bytes += e->size;
	return 1;
}

static struct geo_entry *entry_new(unsigned int address, unsigned int size)
{
	struct geo_entry *e = NULL;
	int index;

	if (entry_count < GEO_ENTRIES)
		e = &entries[entry_count++];
	else
	{
		/* the entry least recently used that isn't loading or queued */
		for (index = 0; index < GEO_ENTRIES; index++)
		{
			struct geo_entry *candidate = &entries[index];

			if (candidate->state == 1 || candidate->state == 2)
				continue;
			if (!e || candidate->last_frame < e->last_frame)
				e = candidate;
		}
		if (!e)
			return NULL;
		if (e->at)
		{
			psp_ge_show3_reuse(2, e->last_frame); /* geshow3check */
			evictions++;
		}
		hash_remove((int)(e - entries));
		if (census_on > 0 && census_read_at[e - entries])
			census_forgotten++; /* its next read will look like a first one */
	}
	census_read_at[e - entries] = 0;
	memset(e, 0, sizeof(*e));
	e->address = address;
	e->size = size;
	hash_insert((int)(e - entries));
	if (last_locked == (int)(e - entries))
		last_locked = -1;
	return e;
}

void *psp_geo_lock(unsigned int address, unsigned int size)
{
	struct geo_entry *e;

	if (pack < 0 || address < EXTERNAL_BASE || address - EXTERNAL_BASE + size > pack_size || !size)
		return NULL;
	e = find(address);
	if (e && e->state == 3 && e->size >= size)
	{
		e->last_frame = frame;
		hits++;
		last_locked = (int)(e - entries);
		return (void *)e->at;
	}
	if (e && e->size < size && (e->state == 3 || e->state == 0))
	{
		/* asked for more than was read (a material's lightmap vertices after
		its own, say): read again, the longer way */
		if (e->state == 3)
			evictions++;
		e->at = 0;
		e->state = 0;
		e->size = size;
	}
	misses++;
	frame_missed = 1;
#ifdef PSP_RECON
	if (geo_place >= 0)
	{
		geo_places[geo_place].misses++;
		geo_places[geo_place].bytes += size;
	}
#endif
	if (inflight >= 0)
	{
		/* it may be the one just read */
		poll();
		if (e && e->state == 3)
		{
			e->last_frame = frame;
			last_locked = (int)(e - entries);
			return (void *)e->at;
		}
	}
	if (alignment(size) > largest_stretch())
	{
		if (too_big++ < 8)
			platform_log("geometry: %u bytes at %08x is more than the cache's %u", size, address, largest_stretch());
		return NULL;
	}
	if (!e)
		e = entry_new(address, size);
	if (!e)
		return NULL;
	if (e->state == 1 && e->size < size)
		e->size = size; /* still queued: read the longer way */
	if (e->state == 0)
	{
		if (pending_count >= GEO_PENDING)
		{
			if (read_past_share(e))
			{
				last_locked = (int)(e - entries);
				return (void *)e->at;
			}
			queue_full++;
			return NULL;
		}
		pending[pending_count++] = (unsigned short)(e - entries);
		e->state = 1;
		e->last_frame = frame;
	}
	pump();
	if (!async_reads && e->state == 1)
		read_past_share(e); /* R4: still queued after the pump */
	if (!async_reads && e->state == 3 && e->size >= size)
	{
		e->last_frame = frame;
		last_locked = (int)(e - entries);
		return (void *)e->at;
	}
	return NULL;
}

/* the resident entry holding ptr, or -1. The renderer asks this for every
model part it draws (psp_geo_generation, psp_geo_address_of), so a scan of
every entry each time is too slow: scanning, the emulator's cache model put
6% of the drawing phase's data misses on the 12 KB entry table (b30's beach,
docs/overnight_b108.md). So: the last block locked, then a small table by address (64 slots: the parts
of a frame mostly repeat), then the scan, which fills the table. A slot is
checked before it is believed (the entry may have been evicted or reused
since), and only one resident block holds any arena address, so the answer
is the scan's */
#define AT_SLOTS 64
/* one table a CPU, each on cache lines of its own. The renderer
asks this from the Media Engine too (gemeoverlap), while the main CPU locks
blocks for the next calls; a table both wrote would share cache lines with
the main CPU's words, and the ME's write-back would undo the main CPU's */
static short at_slot_cpu[2][AT_SLOTS] __attribute__((aligned(64))); /* an entry's index + 1, 0: none */

static int entry_holds(int index, unsigned int at)
{
	const struct geo_entry *e = &entries[index];

	return e->at && e->state == 3 && at >= e->at && at < e->at + e->size;
}

/* The resident blocks in address order, searched by halving when the slot
table doesn't know a pointer. A frame asks about more pointers than the 64
slots hold (each pass of each part, its vertices and its indices: b30 draws
~300 parts a frame), so most questions used to end in the scan, ~350 entries
of 24 bytes: the drawing phase's most-missed data in the cache model
(docs/ge_audit.md). Each key is a block's start in 64-byte units above the
arena (blocks are aligned to 64) over its entry's index, so the 2 KB of keys
stay in a few cache lines and the search reads no entry until its answer.
The keys are a snapshot, made again only after the resident set has changed
(a load or an eviction moves the stamp), at most once a frame. An answer is
believed only once entry_holds confirms it, so a stale snapshot can only
send a question on to the scan, never give a wrong entry: the same answers
as the scan. Main CPU only (the ME's work keeps the scan). "geoscan": the
scan as before; "geoindexcheck": both, answers compared in the report */
#define PLACE_INDEX_BITS 9 /* GEO_ENTRIES = 512 */
static unsigned int by_place[GEO_ENTRIES];
static int by_place_count = -1, place_index_on = -1, place_index_check = 0;
static unsigned int by_place_stamp = 0, by_place_frame = 0;
static unsigned long place_slot_answers = 0, place_index_answers = 0, place_scans = 0, place_builds = 0,
	place_checked = 0, place_differed = 0;

static unsigned int place_stamp(void)
{
	return generation_counter * 2654435761u + (unsigned int)evictions * 40503u + (unsigned int)entry_count;
}

static void by_place_build(void)
{
	int index, n = 0, gap, i, j;

	for (index = 0; index < entry_count; index++)
		if (entries[index].at && entries[index].state == 3)
			by_place[n++] = ((entries[index].at - arena_start) >> 6) << PLACE_INDEX_BITS | (unsigned int)index;
	/* Shell sort (Ciura's gaps): no calls, no heap */
	{
		static const int gaps[] = { 132, 57, 23, 10, 4, 1 };
		int g;

		for (g = 0; g < (int)(sizeof(gaps) / sizeof(gaps[0])); g++)
			for (gap = gaps[g], i = gap; i < n; i++)
			{
				unsigned int key = by_place[i];

				for (j = i; j >= gap && by_place[j - gap] > key; j -= gap)
					by_place[j] = by_place[j - gap];
				by_place[j] = key;
			}
	}
	by_place_count = n;
	by_place_stamp = place_stamp();
	by_place_frame = frame;
	place_builds++;
}

/* the entry whose block starts last at or before at, if it holds at; -1 to scan */
static int by_place_find(unsigned int at)
{
	unsigned int units = (at - arena_start) >> 6;
	int low = 0, high, index;

	if (by_place_count < 0 || by_place_stamp != place_stamp())
	{
		if (by_place_count >= 0 && by_place_frame == frame)
			return -1;
		by_place_build();
	}
	high = by_place_count - 1;
	if (high < 0 || (by_place[0] >> PLACE_INDEX_BITS) > units)
		return -1;
	while (low < high)
	{
		int middle = (low + high + 1) >> 1;

		if ((by_place[middle] >> PLACE_INDEX_BITS) <= units)
			low = middle;
		else
			high = middle - 1;
	}
	index = (int)(by_place[low] & ((1u << PLACE_INDEX_BITS) - 1));
	return entry_holds(index, at) ? index : -1;
}

static int resident_entry(const void *ptr)
{
	unsigned int at = (unsigned int)ptr, slot = (at >> 6) % AT_SLOTS;
	int me = PSP_ME_WORK();
	short *at_slot = at_slot_cpu[me ? 1 : 0]; /* (the emulator's simulated ME work too, for its write trace) */
	int index, found = -1;

	if (!in_cache(at))
		return -1;
	if (last_locked >= 0 && entry_holds(last_locked, at))
		return last_locked;
	if (at_slot[slot] && entry_holds(at_slot[slot] - 1, at))
	{
		if (!me)
			place_slot_answers++;
		return at_slot[slot] - 1;
	}
	if (place_index_on < 0)
	{
		place_index_on = !psp_keep("geoscan") && arena_end - arena_start <= (65536u << 6);
		place_index_check = psp_keep("geoindexcheck");
	}
	if (place_index_on && !me)
	{
		found = by_place_find(at);
		if (found >= 0)
		{
			place_index_answers++;
			if (!place_index_check)
			{
				at_slot[slot] = (short)(found + 1);
				return found;
			}
		}
	}
	if (!me)
		place_scans++;
	for (index = 0; index < entry_count; index++)
		if (entry_holds(index, at))
		{
			if (found >= 0)
			{
				place_checked++;
				if (found != index)
					place_differed++;
			}
			at_slot[slot] = (short)(index + 1);
			return index;
		}
	if (found >= 0)
	{
		place_checked++;
		place_differed++;
	}
	return -1;
}

/* the generation of the block holding ptr: changes with every load into
it, so the renderer knows whether what it worked out about the bytes there
still holds. 0: not the cache's (tag data as before) */
unsigned int psp_geo_generation(const void *ptr)
{
	int index = resident_entry(ptr);

	return index >= 0 ? entries[index].generation : 0;
}

/* the external address of the resident bytes at ptr (the lasting identity
of what's there), 0 if ptr isn't the cache's */
unsigned int psp_geo_address_of(const void *ptr)
{
	int index = resident_entry(ptr);

	return index >= 0 ? entries[index].address + ((unsigned int)ptr - entries[index].at) : 0;
}

int psp_geo_external(unsigned int address)
{
	return pack >= 0 && address >= EXTERNAL_BASE && address < EXTERNAL_BASE + pack_size;
}

static void __attribute__((noinline, cold)) census_report(void)
{
	/* frames: about 1, 5, 10 and 30 s at 30 fps; at the frame rates the
	PSP draws the busy maps (5-15 fps) they span 2-6 times as long */
	static const unsigned int spans[4] = { 30, 150, 300, 900 };
	unsigned int set[4] = { 0, 0, 0, 0 };
	char line[400];
	int index, k, at;

	for (index = 0; index < entry_count; index++)
		for (k = 0; k < 4; k++)
			if (entries[index].last_frame && entries[index].last_frame + spans[k] > frame)
				set[k] += alignment(entries[index].size);
	platform_log("memcensus: geometry working set, KB wanted in the last frames: %u KB in 30 frames; %u KB in 150 frames; %u KB in 300 frames; %u KB in 900 frames (the cache %u KB; %d entries of %d)",
		set[0] / 1024, set[1] / 1024, set[2] / 1024, set[3] / 1024, cache_bytes() / 1024, entry_count, GEO_ENTRIES);
	at = snprintf(line, sizeof(line), "memcensus: geometry re-reads by bytes read since the block was last read (beyond the cache's size):");
	for (k = 0; k < CENSUS_BUCKETS && at < (int)sizeof(line) - 1; k++)
		at += snprintf(line + at, sizeof(line) - at, " %s: %lu;", census_bucket_names[k], census_rereads[k]);
	if (at < (int)sizeof(line) - 1)
		snprintf(line + at, sizeof(line) - at, " first reads %lu, entries forgotten %lu (since the map began)", census_first, census_forgotten);
	platform_log("%s", line);
}

void psp_geo_frame_end(void)
{
	if (async_reads)
		poll();
	frame_read_bytes = 0;
	pump();
	if (frame_read_us > frame_read_worst_us)
		frame_read_worst_us = frame_read_us;
	frame_read_us = 0;
	if (frame_missed)
		miss_frames++;
	frame_missed = 0;
	psp_anim_frame_end();
	if (pack >= 0 && frame % 300 == 0)
	{
		unsigned int resident = 0, resident_bytes = 0;
		int index;

		for (index = 0; index < entry_count; index++)
			if (entries[index].state == 3)
			{
				resident++;
				resident_bytes += entries[index].size;
			}
		/* R4: the draws left out for a frame because their
		vertices weren't read yet (psp_fixups.py, psp_geometry_missing): a
		far object that vanishes for a few frames as you approach */
		extern unsigned long psp_geometry_draws_missed;

		platform_log("geometry: %lu hits, %lu misses (%lu frames missed something, %lu draws left out); %lu loaded (%lu KB), %lu evicted, %lu waited for room, %lu for the queue; %u blocks resident (%u KB of %u); reading %lu ms in all, worst frame %lu ms (%s); read past the frame's share for a draw %lu (%lu KB)",
			hits, misses, miss_frames, psp_geometry_draws_missed, loads, load_bytes / 1024, evictions, no_room, queue_full,
			resident, resident_bytes / 1024, cache_bytes() / 1024, read_us / 1000, frame_read_worst_us / 1000,
			async_reads ? "asynchronous" : "synchronous", past_share_reads, past_share_bytes / 1024);
		if (census_on > 0)
			census_report();
		platform_log("geometry: resident lookups (the renderer's) by the slot table %lu, by the index %lu, scanned %lu; index made %lu times (%s); geoindexcheck: %lu compared, %lu differed",
			place_slot_answers, place_index_answers, place_scans, place_builds,
			place_index_on > 0 ? "on" : place_index_on == 0 ? "off: geoscan" : "not asked yet", place_checked, place_differed);
#ifdef PSP_RECON
		geo_places_report();
#endif
		platform_log("geometry: the ring passed %lu blocks drawn in the last %d frames, and %lu times found none older",
			skips, geo_keep_frames, skip_fallbacks);
		psp_anim_report();
	}
	frame++;
}

static void forget_all(void)
{
	int index;

	if (inflight >= 0)
	{
		SceInt64 result;

		sceIoWaitAsync(pack, &result);
		inflight = -1;
	}
	for (index = 0; index < entry_count; index++)
	{
		entries[index].at = 0;
		entries[index].state = 0;
	}
	pending_count = 0;
	entry_count = 0;
	arena_next = arena_start;
	memset(hash_head, 0, sizeof(hash_head));
	memset(at_slot_cpu, 0, sizeof(at_slot_cpu));
	last_locked = -1;
	by_place_count = -1;
}

void psp_geo_close(void)
{
	psp_anim_close();
	if (pack >= 0)
	{
		forget_all();
		sceIoClose(pack);
		pack = -1;
		platform_log("geometry: %s closed", pack_name);
	}
	pack_size = 0;
	arena_start = arena_end = arena_next = 0;
	extra_start = extra_end = 0;
}

/* the tag window's part the map never uses, past what the animation cache
took: "geowindow" makes it the cache's second stretch (64 KB or more) unless
"soundgeo" has it; the census says where it lies either way */
static void psp_geo_window(unsigned int window_at, unsigned int window_bytes, unsigned int tag_data_end, unsigned int window_end)
{
	unsigned int animation_end = psp_anim_window_end(), start, end;

	start = animation_end > window_at ? animation_end : window_at;
	start = (start + 63) & ~63u;
	end = window_bytes ? (window_at + window_bytes) & ~63u : 0;
	if (census_on > 0)
		platform_log("memcensus: tag window: the tags end at %08x (%u KB into it), the window at %08x; the span no BSP of this map uses %u KB at %08x; the animation cache took %u KB of it; free span %u KB",
			tag_data_end, (tag_data_end - 0x09600000u) / 1024, window_end, window_bytes / 1024, window_at,
			animation_end > window_at ? (animation_end - window_at) / 1024 : 0, end > start ? (end - start) / 1024 : 0);
	if (end <= start || end - start < 64 * 1024 || !psp_keep("geowindow"))
		return;
	/* the ring has one second stretch, and "soundgeo" took it first */
	if (extra_end > extra_start)
	{
		platform_log("geometry: geowindow: %u KB at %08x not used: the cache's second stretch is soundgeo's",
			(end - start) / 1024, start);
		return;
	}
	extra_start = start;
	extra_end = end;
	platform_log("geometry: geowindow: %u KB more at %08x (the tag window's part this map never uses): the cache %u KB",
		(end - start) / 1024, start, cache_bytes() / 1024);
}

/* a map's tags were loaded: its geometry pack, if it was built with one.
Called before the texture pack opens, so the texture cache's RAM share is
settled first */
void psp_geo_map_opened(const char *name, unsigned int map_crc, const void *first_vertex_descriptor,
	unsigned int tag_data_end, unsigned int window_end)
{
	char path[128];
	unsigned int header[16];
	void *arena;
	int outside = first_vertex_descriptor && ((const unsigned int *)first_vertex_descriptor)[1] >= EXTERNAL_BASE;

	/* the load trace, from here to two seconds of play (psp_trace.c) */
	{
		extern void psp_trace_start(const char *why);

		psp_trace_start(name);
	}
	psp_geo_close();
	psp_structure_vertex_stride = outside ? 6 : 32;
	/* the bake's formats, from the pack (below) */
	map_formats = 0;
	if (!name || !name[0])
		return;
	if (!outside)
		return;
	snprintf(path, sizeof(path), PSP_MAPS_ROOT "/%s.pgeo", name);
	pack = sceIoOpen(path, PSP_O_RDONLY, 0);
	if (pack < 0)
	{
		platform_log("geometry: the map keeps its geometry outside its tags, and there is no %s: nothing will be drawn", path);
		return;
	}
	if (sceIoRead(pack, header, sizeof(header)) != sizeof(header) || header[0] != 0x4F454750 /* PGEO */ || header[1] != 1)
	{
		platform_log("geometry: %s is not a geometry pack", path);
		sceIoClose(pack);
		pack = -1;
		return;
	}
	if (map_crc && header[2] != map_crc)
		platform_log("geometry: %s was built for another version of the map (crc %08x, map %08x): expect garbage",
			path, header[2], map_crc);
	pack_size = header[3];
	/* header word 10, the bake's formats: bits 0-3, the tag
	blocks invader-build -Z made slim (psp_tag_layouts.c) */
	map_formats = header[10];
	platform_log("geometry: %s: bake formats %lx", path, map_formats);
	snprintf(pack_name, sizeof(pack_name), "%s", name);
	async_reads = psp_keep("geoasync");
	if (async_reads)
		sceIoChangeAsyncPriority(pack, ASYNC_PRIORITY);
	frame_read_bytes = 0;
	frame_read_us = frame_read_worst_us = 0;
	/* from the heap, not carved from the texture cache: carved, Blood
	Gulch's 5 MB of textures no longer fit the RAM cache and are read from
	the Memory Stick as drawn, mid-frame, every frame (1.7-5 s a frame at
	the frame's end on the PSP). Blood Gulch's heap has 1.5 MB to spare (its
	peak was 3.4 of 5.0 MB) */
	/* a map whose texture pack is larger than the texture caches (every
campaign map: 9-14 MB against 5.3 MB) streams its textures as drawn anyway,
so the geometry cache takes its 1 MB from the texture RAM cache and leaves
the heap: from the heap, The Library runs out of heap at its first frame. A
pack that fits (Blood Gulch's) keeps the heap's, for the reason above */
	/* the size comes from the pack the textures will actually use (the
	asset track's, when chosen), and a size that can't be had is logged and
	counts as "too big": a wrong guess that way costs texture caching, the
	other way it starves the heap (it has, on the PSP) */
	{
		char texture_path[128];
		long texture_bytes;
		extern void *psp_tex_carve(unsigned int bytes);
		extern long psp_tex_pack_bytes(const char *name, char *path, int size);

		texture_bytes = psp_tex_pack_bytes(name, texture_path, sizeof(texture_path));
		if (texture_bytes < 0)
			platform_log("geometry: couldn't size %s (0x%08x): taken as larger than the texture caches",
				texture_path, (unsigned)texture_bytes);
		arena = NULL;
		if (!psp_keep("geoheap") && (texture_bytes < 0 || texture_bytes > TEXTURE_CACHES_BYTES))
		{
			if (heap_arena)
			{
				free(heap_arena);
				heap_arena = NULL;
			}
			arena_bytes = psp_keep("geo2") ? 2048 * 1024 : psp_keep("geo15") ? 1536 * 1024 : ARENA_BYTES;
#ifndef PSP_MULTIPLAYER
			/* "heaproom256" (psp_ge.c): the level copies' room that leaves
			the heap is carved below this cache, and comes from here when
			it is over 1 MB */
			{
				extern unsigned int psp_heaproom_bytes(void);

				if (arena_bytes > ARENA_BYTES && psp_heaproom_bytes())
					arena_bytes -= 256 * 1024;
			}
#endif
			/* the main menu, the showcase (CLAUDE.md): its whole pack (ui's
			is about 130 KB) and 64 KB to spare, so the rest of the 1 MB is
			left to the textures it draws at full size */
			{
				extern int psp_showcase_map;
				unsigned int whole = ((pack_size + 0xFFFFu) & ~0xFFFFu) + 0x10000u;

				if (psp_showcase_map && whole < arena_bytes)
					arena_bytes = whole;
			}
			arena = psp_tex_carve(arena_bytes);
			arena_carved = arena != NULL;
		}
		if (!arena)
		{
			if (arena_carved)
			{
				psp_tex_carve(0);
				arena_carved = 0;
			}
			arena_bytes = ARENA_BYTES;
			if (!heap_arena)
				heap_arena = memalign(64, ARENA_BYTES);
			arena = heap_arena;
		}
	}
	if (!arena)
	{
		platform_log("geometry: no RAM for the cache; %s closed", path);
		sceIoClose(pack);
		pack = -1;
		pack_size = 0;
		return;
	}
	arena_start = arena_next = (unsigned int)arena;
	arena_end = arena_start + arena_bytes;
	{
		extern void *psp_sound_spare(unsigned long *bytes);
		extern int psp_showcase_map;
		unsigned long spare_bytes;
		void *spare = psp_sound_spare(&spare_bytes);

		/* (the main menu keeps its cache as it is) */
		if (spare && !psp_showcase_map)
		{
			extra_start = (unsigned int)spare;
			extra_end = extra_start + (unsigned int)spare_bytes;
			platform_log("geometry: and %lu KB at %08x that the sound cache doesn't take (soundgeo)", spare_bytes / 1024, extra_start);
		}
	}
	hits = misses = loads = load_bytes = evictions = too_big = failures = miss_frames = queue_full = no_room = 0;
	if (census_on < 0)
		census_on = psp_keep("memcensus") != 0;
	memset(census_read_at, 0, sizeof(census_read_at));
	memset(census_rereads, 0, sizeof(census_rereads));
	census_first = census_forgotten = 0;
	read_us = read_worst_us = 0;
	platform_log("geometry: %s, %u KB external; cache %u KB at %08x (%s)", path, pack_size / 1024, arena_bytes / 1024, arena_start,
		arena_carved ? "from the texture RAM cache: its textures stream anyway" : "heap");
	{
		/* words 8-9: the tag window's part the map never uses (the bake's
		measure over its BSPs), checked to lie between the tags' end and the
		window's */
		unsigned int window_at = header[8], window_bytes = header[9];

		if (window_bytes && (window_at < tag_data_end || window_at + window_bytes > window_end))
		{
			platform_log("animations: the pack's unused window part (%u KB at %08x) isn't between the tags' end (%08x) and the window's (%08x): not used",
				window_bytes / 1024, window_at, tag_data_end, window_end);
			window_at = window_bytes = 0;
		}
		/* a map built with its animations outside (invader-build -M)
		keeps their frame data at the front of this pack (the header's words 4-7:
		bytes, first and end offsets, the page it is laid out for), read through
		the animation cache, which takes the window's part first */
		if (header[4] && header[6] > header[5] && header[6] <= pack_size)
			psp_anim_map_opened(header[4], header[5], header[6], header[7], window_at, window_bytes, arena_carved);
		psp_geo_window(window_at, window_bytes, tag_data_end, window_end);
	}
	/* after the animation cache's share: the level copies' room past their
	arena, carved below it as this cache's was (psp_ge.c) */
	{
		extern void psp_ge_level_carve(int carve_from_textures);

		psp_ge_level_carve(arena_carved);
	}
	{
		/* the stencil bake, keyed by this pack's addresses */
		extern void psp_ge_stencils_open(const char *name, unsigned int map_crc);

		psp_ge_stencils_open(name, map_crc);
	}
}

/* the PSP is about to sleep: the volatile partition goes back to the system
(psp_tex_suspend), and the blocks in it are forgotten */
/* a read in flight finished, so its descriptor can be closed */
void psp_geo_settle(void)
{
	if (pack >= 0 && inflight >= 0)
	{
		SceInt64 result;

		sceIoWaitAsync(pack, &result);
		poll();
	}
}

void psp_geo_suspend(void)
{
	/* the arena is in the heap, which sleep keeps: only a read in flight is
	finished first */
	psp_geo_settle();
	/* carved from the texture RAM cache, the arena goes back to the
	system with the volatile partition: its blocks are forgotten (the renderer
	re-reads what it draws) */
	if (arena_carved)
		forget_all();
	psp_anim_suspend();
	{
		extern void psp_ge_level_carve_suspend(void);

		psp_ge_level_carve_suspend();
	}
}

void psp_geo_resume(void)
{
}

/* this heartbeat window's Memory Stick reads for geometry, for the
log's io: line (psp_platform.c) */
void psp_geo_io_window(unsigned long *reads, unsigned long *us, unsigned long *worst_us)
{
	*reads = window_reads;
	*us = window_us;
	*worst_us = window_worst_us;
	window_reads = window_us = window_worst_us = 0;
}

/* ---------- the renderer's and object lighting's way in */

struct vertex_buffer
{
	short type;
	unsigned short pad;
	long count;
	long offset;
	void *base_address;
	void *hardware_format;
};
struct triangle_buffer
{
	short type;
	unsigned short pad;
	long count;
	void *base_address;
	void *hardware_format;
};
long rasterizer_geometry_get_vertex_size(short type);

/* a vertex buffer's bytes: the cache's copy when the map keeps them outside,
the tag data otherwise (the D3D shim's lock is Data itself: physical and
virtual addresses are the same on the PSP). NULL: not there this frame */
void *psp_vertex_lock(const struct vertex_buffer *buffer)
{
	unsigned int address;

	if (!buffer || !buffer->hardware_format)
		return NULL;
	/* the hardware format is the 12-byte descriptor {Common, Data, Lock}; Data
	is the pointer the map gave */
	address = ((const unsigned int *)buffer->hardware_format)[1];
	if (psp_geo_external(address))
		return psp_geo_lock(address, (unsigned int)buffer->count * (unsigned int)rasterizer_geometry_get_vertex_size(buffer->type));
	return (void *)address;
}

/* a level vertex buffer's address as the map gives it, in the tags or in the
pack: psp_ge.c's level paths draw from their own copies of a material, and
take the pack's bytes through the cache only while building them */
void *psp_level_vertices(const struct vertex_buffer *buffer)
{
	if (!buffer || !buffer->hardware_format)
		return NULL;
	return (void *)((const unsigned int *)buffer->hardware_format)[1];
}

void *psp_index_lock(const struct triangle_buffer *buffer)
{
	unsigned int address, count;

	if (!buffer || !buffer->hardware_format)
		return NULL;
	address = ((const unsigned int *)buffer->hardware_format)[1];
	count = buffer->type == 1 /* precompiled strip */ ? (unsigned int)buffer->count + 2 : 3 * (unsigned int)buffer->count;
	if (psp_geo_external(address))
		return psp_geo_lock(address, count * 2);
	return (void *)address;
}
