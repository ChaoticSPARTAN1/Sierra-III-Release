/* psp_bindblock.h - a texture's bind as GE words (psp_tex.c, "bindblocks").

The words psp_tex_bind sends through pspgu (sceGuClutMode, sceGuClutLoad,
sceGuTexMode, one sceGuTexImage a level, sceGuTexFilter; pspsdk's TexMode and
TexImage each end with a TexFlush), written into a block the list CALLs, so
the GE gets the same state. A header of its own, with no PSP calls, so a
host test can compare it with pspsdk's own functions word for word. */
#ifndef PSP_BINDBLOCK_H
#define PSP_BINDBLOCK_H

#include <pspgu.h>

/* pspsdk's getExp: log2 of a power of two up to 512 */
static inline unsigned int bind_block_exponent(unsigned int value)
{
	return 31 - __builtin_clz(value & 0x3FF);
}

/* fills w with the bind and a RET; returns the words written (8 + 4 a level,
at most 8 levels, + 1). address: where the palette starts, the levels after
it, as the pack lays them out */
static inline int bind_block_fill(unsigned int *w, int clut, int psm, int levels, unsigned int width,
	unsigned int height, unsigned int address, unsigned int palette_bytes)
{
	int n = 0, level;

	/* sceGuClutMode(clut, 0, 0xFF, 0); sceGuClutLoad(palette_bytes / 32, address) */
	w[n++] = (0xC5u << 24) | (((unsigned int)clut | (0xFFu << 8)) & 0xFFFFFFu);
	w[n++] = (0xB0u << 24) | (address & 0xFFFFFFu);
	w[n++] = (0xB1u << 24) | ((address >> 8) & 0xF0000u);
	w[n++] = (0xC4u << 24) | ((palette_bytes / 32) & 0xFFFFFFu);
	/* sceGuTexMode(psm, levels - 1, 0, 1) and its flush */
	w[n++] = (0xC2u << 24) | ((((unsigned int)(levels - 1) << 16) | 1u) & 0xFFFFFFu);
	w[n++] = (0xC3u << 24) | ((unsigned int)psm & 0xFFFFFFu);
	w[n++] = 0xCBu << 24;
	address += palette_bytes;
	for (level = 0; level < levels && level < 8; level++)
	{
		unsigned int bytes = psm == GU_PSM_T4 ? (width + 1) / 2 : width;
		unsigned int row = ((bytes < 16 ? 16 : bytes) + 15) & ~15u, rows = (height + 7) & ~7u;
		unsigned int tbw = psm == GU_PSM_T4 ? row * 2 : row;

		/* sceGuTexImage(level, width, height, tbw, address) and its flush */
		w[n++] = ((0xA0u + (unsigned int)level) << 24) | (address & 0xFFFFFFu);
		w[n++] = ((0xA8u + (unsigned int)level) << 24) | ((((address >> 8) & 0x0F0000u) | tbw) & 0xFFFFFFu);
		w[n++] = ((0xB8u + (unsigned int)level) << 24) |
			(((bind_block_exponent(height) << 8) | bind_block_exponent(width)) & 0xFFFFFFu);
		w[n++] = 0xCBu << 24;
		address += row * rows;
		width = width > 1 ? width / 2 : 1;
		height = height > 1 ? height / 2 : 1;
	}
	/* sceGuTexFilter(levels > 1 ? GU_LINEAR_MIPMAP_LINEAR : GU_LINEAR, GU_LINEAR), then back */
	w[n++] = (0xC6u << 24) | (((unsigned int)(levels > 1 ? GU_LINEAR_MIPMAP_LINEAR : GU_LINEAR) | (GU_LINEAR << 8)) & 0xFFFFFFu);
	w[n++] = 0x0Bu << 24;
	return n;
}

#endif
