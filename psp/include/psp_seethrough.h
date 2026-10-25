/*
PSP_SEETHROUGH.H

Build 116: a see-through shader's fade, packed above its blend function in
the short the transparent queue already carries. The engine side packs it
(psp_fixups.py, psp_transparent_map; psp_limits.h includes this header for
the game's units) and psp_ge.c unpacks it (it includes this header by path:
the plain-C units have no -I for psp/include).

  bits 0-3   the blend function (0 alpha blend, 1 multiply, 2 double
             multiply, 3 add, 4 subtract, 5 min, 6 max, 7 alpha-multiply-add)
  bits 4-5   the framebuffer fade mode: 0 none, 1 fade when perpendicular,
             2 fade when parallel
  bit 6      "don't fade with active camouflage" (chicago extra flags, bit 0)
  bits 7-14  how much the fade source takes away (0: none, 255: all)
  bit 15     one-sided: the shader isn't two-sided, so its back faces are
             culled, as the Xbox culls them (D3DCULL_CCW unless the
             shader's two-sided flag is set)

A plain blend function (nothing above bit 3) is full strength, no fade, so
every queue entry made before build 116 reads the same as before.
*/
#ifndef __PSP_SEETHROUGH_H
#define __PSP_SEETHROUGH_H

#define PSP_SEETHROUGH_PACK(blend, mode, fade, keep_with_camouflage) \
	((short)(((blend) & 15) | (((mode) & 3) << 4) | ((keep_with_camouflage) ? 64 : 0) | \
	((255 - (int)((((fade) < 0.0f ? 0.0f : (fade) > 1.0f ? 1.0f : (fade)) * 255.0f) + 0.5f)) << 7)))
#define PSP_SEETHROUGH_BLEND(packed) ((packed) & 15)
#define PSP_SEETHROUGH_MODE(packed) (((packed) >> 4) & 3)
#define PSP_SEETHROUGH_KEEP_WITH_CAMOUFLAGE(packed) (((packed) & 64) != 0)
#define PSP_SEETHROUGH_FADE(packed) ((float)(255 - (((packed) >> 7) & 255)) / 255.0f)
#define PSP_SEETHROUGH_ONE_SIDED_BIT 0x8000
#define PSP_SEETHROUGH_ONE_SIDED(packed) (((packed) & PSP_SEETHROUGH_ONE_SIDED_BIT) != 0)

/* build 116 (b113 #16): a meter shader's parameters (a plasma rifle's heat
gauge, the Warthog's speedometer), worked out on the engine side the way the
Xbox fills its four combiner stages (psp_fixups.py, psp_transparent_map) and
passed to the GE just before the part is queued (psp_ge_seethrough_meter).
The layout is the game's rasterizer_meter_parameters, which the HUD's meters
already pass (psp_ge.c, struct game_meter), so one palette routine serves
both. Colours are ARGB. In the HUD's terms:
  gradient_min_color  the empty-to-full colour's low end; its alpha is where
                      the flash starts (stage 0)
  gradient_max_color  the high end
  background_color    the part past the value (stage 3)
  flash_color         added past the flash's start; its alpha is the value,
                      where the background begins (stage 2)
  tint_color          the blend's constant colour
  gradient            how fast the colour runs along the meter (1: once) */
struct psp_seethrough_meter
{
	unsigned int gradient_min_color, gradient_max_color, background_color, flash_color;
	unsigned char flash_color_is_negative, tint_mode_2, pad[2];
	unsigned int tint_color;
	float gradient;
};

#endif /* __PSP_SEETHROUGH_H */
