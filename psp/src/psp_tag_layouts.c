/* psp_tag_layouts.c - the PSP's slim tag layouts, from a map of either bake (build 108)

Four tag blocks carry bytes the game never reads on the PSP: a sound
permutation's name and subtitle data, a pitch range's name, an AI squad's
unused fields and a firing position's padding. On a marine level that is
0.4-0.6 MiB of the 9 MiB tag window, and b40 doesn't fit without it
(docs/overnight_b108.md, section 6). So the PSP's structs leave those bytes
out (psp_fixups.py, FIXES_108), and the bake (invader-build -Z) writes the
blocks that way: each element keeps only the byte ranges in the table below,
in order.

A map baked before build 108 has the Xbox's layouts. Its blocks are made slim
here, in place, as soon as its tags are read (cache_files.c, beside
pal_tags_loaded): every element moves down to where the slim layout puts it,
so the block keeps its address and its count and only its tail goes unused.
Nothing reads the tags before that. The geometry pack's header word 10 says
which blocks the bake already made slim (one bit per row of the table;
psp_geo.c); a map without a pack has none.

Built as port/linux/game/pal_tags.c is (psp_build.py), the other place a
map's tags are put right as they load: the game's headers are the real ones,
so the table is checked against the structs the game reads with. */

#include "cseries.h"
#include "ai/ai_scenario_definitions.h"
#include "cache/cache_files.h"
#include "scenario/scenario_definitions.h"
#include "sound/sound_definitions.h"
#include "tag_files/tag_groups.h"

#include <stdlib.h>
#include <string.h>

void platform_log(char const *format, ...);
unsigned long psp_geo_map_formats(void);

/* ---------- the layouts */

enum
{
	PSP_SLIM_PERMUTATIONS,
	PSP_SLIM_PITCH_RANGES,
	PSP_SLIM_SQUADS,
	PSP_SLIM_FIRING_POSITIONS,
	NUMBER_OF_PSP_SLIM_LAYOUTS,

	MAXIMUM_KEPT_RANGES = 3,

	/* where the Xbox's pitch range keeps its permutations block */
	XBOX_PITCH_RANGE_PERMUTATIONS_OFFSET = 60,
};

struct psp_slim_layout
{
	char const *name;
	long xbox_size; /* the Xbox's element */
	long psp_size; /* the PSP's struct */
	struct
	{
		short from, to; /* [from, to) of the Xbox's element */
	} kept[MAXIMUM_KEPT_RANGES];
};

/* the same table as invader-build's (build_workload.cpp, slim_psp_blocks);
the bit for each row is 1 << its index */
static struct psp_slim_layout const psp_slim_layouts[NUMBER_OF_PSP_SLIM_LAYOUTS] =
{
	/* name (32), then the fields, then subtitle data (20) */
	{"sound permutations", 124, sizeof(struct sound_permutation), {{32, 104}}},
	/* name (32), then the fields */
	{"sound pitch ranges", 72, sizeof(struct sound_pitch_range), {{32, 72}}},
	/* unused1 (28 at 48), unused3 (48 at 148) and unused_block (12 at 220) */
	{"AI squads", 232, sizeof(struct squad_definition), {{0, 48}, {76, 148}, {196, 220}}},
	/* unresolved (4 at 16) */
	{"AI firing positions", 24, sizeof(struct firing_position_definition), {{0, 16}, {20, 24}}},
};

/* the kept ranges add up to the PSP's structs, and the fields after a gap
are where the table puts them */
typedef char psp_slim_permutation_size[sizeof(struct sound_permutation) == 104 - 32 ? 1 : -1];
typedef char psp_slim_pitch_range_size[sizeof(struct sound_pitch_range) == 72 - 32 ? 1 : -1];
typedef char psp_slim_pitch_range_permutations[
	offsetof(struct sound_pitch_range, permutations) == XBOX_PITCH_RANGE_PERMUTATIONS_OFFSET - 32 ? 1 : -1];
typedef char psp_slim_squad_size[sizeof(struct squad_definition) == 48 + (148 - 76) + (220 - 196) ? 1 : -1];
typedef char psp_slim_squad_maneuver[offsetof(struct squad_definition, maneuver_squad_index) == 78 - 28 ? 1 : -1];
typedef char psp_slim_squad_move_positions[offsetof(struct squad_definition, move_positions) == 196 - 28 - 48 ? 1 : -1];
typedef char psp_slim_firing_position_size[sizeof(struct firing_position_definition) == 20 ? 1 : -1];
typedef char psp_slim_firing_position_surface[offsetof(struct firing_position_definition, surface_index) == 16 ? 1 : -1];

/* ---------- the blocks to convert */

struct psp_slim_block
{
	byte *address;
	long count;
	short layout;
};

struct psp_slim_list
{
	struct psp_slim_block *blocks; /* NULL while counting */
	long count;
	unsigned long formats;
};

static void psp_slim_add(
	struct psp_slim_list *list,
	struct tag_block const *block,
	short layout)
{
	if (list->formats & (1 << layout))
		return;
	if (block->count <= 0 || !block->address)
		return;
	if (list->blocks)
	{
		list->blocks[list->count].address = (byte *)block->address;
		list->blocks[list->count].count = block->count;
		list->blocks[list->count].layout = layout;
	}
	list->count++;
}

/* every block of the four kinds, read in the layouts the map has now
(before any of them is converted) */
static void psp_slim_collect(
	struct psp_slim_list *list)
{
	struct tag_iterator iterator;
	long tag_index;
	boolean pitch_ranges_slim = (list->formats & (1 << PSP_SLIM_PITCH_RANGES)) != 0;

	list->count = 0;
	tag_iterator_new(&iterator, SOUND_DEFINITION_TAG);
	while ((tag_index = tag_iterator_next(&iterator)) != NONE)
	{
		struct sound_definition *sound = tag_get(SOUND_DEFINITION_TAG, tag_index);
		byte *pitch_range = (byte *)sound->pitch_ranges.address;
		long index;

		psp_slim_add(list, &sound->pitch_ranges, PSP_SLIM_PITCH_RANGES);
		for (index = 0; pitch_range && index < sound->pitch_ranges.count; index++)
		{
			struct tag_block const *permutations = pitch_ranges_slim ?
				&((struct sound_pitch_range *)pitch_range)[index].permutations :
				(struct tag_block const *)(pitch_range + index * psp_slim_layouts[PSP_SLIM_PITCH_RANGES].xbox_size +
					XBOX_PITCH_RANGE_PERMUTATIONS_OFFSET);

			psp_slim_add(list, permutations, PSP_SLIM_PERMUTATIONS);
		}
	}
	tag_iterator_new(&iterator, SCENARIO_TAG);
	while ((tag_index = tag_iterator_next(&iterator)) != NONE)
	{
		struct scenario *scenario = tag_get(SCENARIO_TAG, tag_index);
		long index;

		for (index = 0; index < scenario->ai_encounters.count; index++)
		{
			struct encounter_definition *encounter = TAG_BLOCK_GET_ELEMENT(&scenario->ai_encounters, index,
				struct encounter_definition);

			psp_slim_add(list, &encounter->squads, PSP_SLIM_SQUADS);
			psp_slim_add(list, &encounter->firing_positions, PSP_SLIM_FIRING_POSITIONS);
		}
	}
}

/* by address, the longest first where two share one */
static int psp_slim_block_compare(
	void const *a,
	void const *b)
{
	struct psp_slim_block const *first = a;
	struct psp_slim_block const *second = b;

	if (first->address != second->address)
		return first->address < second->address ? -1 : 1;
	return (second->count > first->count) - (second->count < first->count);
}

/* one block's elements, each down to its slim place: element i's kept bytes
go to i * psp_size, never past where they were, so front to back nothing is
overwritten before it has moved */
static void psp_slim_convert(
	struct psp_slim_block const *block)
{
	struct psp_slim_layout const *layout = &psp_slim_layouts[block->layout];
	long index;

	for (index = 0; index < block->count; index++)
	{
		byte *from = block->address + index * layout->xbox_size;
		byte *to = block->address + index * layout->psp_size;
		short range;

		for (range = 0; range < MAXIMUM_KEPT_RANGES && layout->kept[range].to; range++)
		{
			short size = layout->kept[range].to - layout->kept[range].from;

			memmove(to, from + layout->kept[range].from, size);
			to += size;
		}
	}
}

/* ---------- public code */

/* a map's tags were read: its blocks in the PSP's layouts (cache_files.c) */
void psp_tag_layouts_loaded(
	void)
{
	struct psp_slim_list list;
	long converted[NUMBER_OF_PSP_SLIM_LAYOUTS];
	long index;
	unsigned long known = (1 << NUMBER_OF_PSP_SLIM_LAYOUTS) - 1;

	list.blocks = NULL;
	list.formats = psp_geo_map_formats();
	if (list.formats & ~known)
		platform_log("tags: the bake's formats %08lx include ones this build doesn't know: expect garbage", list.formats);
	if ((list.formats & known) == known)
	{
		platform_log("tags: slim layouts from the bake");
		return;
	}
	psp_slim_collect(&list);
	if (!list.count)
		return;
	list.blocks = malloc(list.count * sizeof(*list.blocks));
	if (!list.blocks)
	{
		/* the game can't read these blocks as they are: expect garbage */
		platform_log("tags: no memory to convert %ld blocks to the PSP's layouts", list.count);
		return;
	}
	psp_slim_collect(&list);
	/* a block two tags share (invader-build keeps identical data once) is
	converted once */
	qsort(list.blocks, list.count, sizeof(*list.blocks), psp_slim_block_compare);
	csmemset(converted, 0, sizeof(converted));
	for (index = 0; index < list.count; index++)
	{
		if (index > 0 && list.blocks[index].address == list.blocks[index - 1].address)
			continue;
		psp_slim_convert(&list.blocks[index]);
		converted[list.blocks[index].layout] += list.blocks[index].count;
	}
	free(list.blocks);
	platform_log("tags: Xbox layouts made slim in place (bake formats %lx): %ld %s, %ld %s, %ld %s, %ld %s",
		list.formats,
		converted[0], psp_slim_layouts[0].name, converted[1], psp_slim_layouts[1].name,
		converted[2], psp_slim_layouts[2].name, converted[3], psp_slim_layouts[3].name);
}
