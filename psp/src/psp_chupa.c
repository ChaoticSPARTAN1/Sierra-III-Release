/* psp_chupa.c - what ChupathingyCE's desktop units answer on the PSP, game side (build 110)

ChupathingyCE adds desktop features the PSP build leaves out (psp_build.py's
CHUPA_DESKTOP_UNITS). The game still calls into some of them; each answer
here is what the feature does when it has nothing to do, read from the
real function, so the game takes the path it takes on a desktop without
it. Built only on the ChupathingyCE base, as port/linux/game/pal_tags.c is
(the game's headers). psp_chupa_platform.c answers the platform layer's
calls, psp_campaign.c the co-op and netcode hooks. */

#include "cseries.h"
#include "math/real_math.h"

struct bitmap_data;
struct event_record;
struct widget_instance;

/* ---------- the PC menus (menu_tags.c, menu_functions.c)

ChupathingyCE builds menus written in XML into ui.map's tags as it loads.
The PSP keeps the game's own menus (psp_menu.c draws them), so no tag is
ever one of the PC menus', and every question about one answers "not
ours". The declarations are the callers' (cache_files.c, ui_widget*.c,
players.c), as the files have no header. */

void menu_tags_loaded(char const *map_name);
void menu_tags_unloaded(void);
char const *pc_menus_root_name(void);
char const *pc_menus_screen(char const *name);
boolean pc_menu_tag(long tag_index);
boolean pc_menu_frame_placement(struct bitmap_data const *bitmap, short *x, short *y, short *width, short *height);
boolean pc_menu_text_color(struct widget_instance const *widget, real_rgb_color *rgb);
boolean pc_menu_event_function_invoke(struct widget_instance *widget, struct event_record *event,
	long function_index, boolean *widget_deleted);
void pc_menu_game_data_function_invoke(struct widget_instance *widget, long function);
unsigned char pc_menu_split_players(void);

void menu_tags_loaded(char const *map_name) { (void)map_name; }
void menu_tags_unloaded(void) {}
/* the main menu the game opens: the real one's answer with no menus built
(the Xbox's own, as halo-ce-universal opens it) */
char const *pc_menus_root_name(void) { return "ui\\shell\\main_menu\\main_menu"; }
/* a screen the game opens (the multiplayer pregame and postgame), as itself:
the real one's answer with no menus built */
char const *pc_menus_screen(char const *name) { return name; }
boolean pc_menu_tag(long tag_index) { (void)tag_index; return FALSE; }
boolean pc_menu_frame_placement(struct bitmap_data const *bitmap, short *x, short *y, short *width, short *height)
{
	(void)bitmap; (void)x; (void)y; (void)width; (void)height;
	return FALSE;
}
boolean pc_menu_text_color(struct widget_instance const *widget, real_rgb_color *rgb)
{
	(void)widget; (void)rgb;
	return FALSE;
}
/* (asked only for a PC menu widget's functions, numbered from
PC_MENU_FUNCTION_BASE: none here) */
boolean pc_menu_event_function_invoke(struct widget_instance *widget, struct event_record *event,
	long function_index, boolean *widget_deleted)
{
	(void)widget; (void)event; (void)function_index; (void)widget_deleted;
	return FALSE;
}
void pc_menu_game_data_function_invoke(struct widget_instance *widget, long function) { (void)widget; (void)function; }
/* whether the menus are setting up split screen: the PSP has one player */
unsigned char pc_menu_split_players(void) { return FALSE; }

/* ---------- the tag schema check (tag_validate.c)

ChupathingyCE checks every tag of a map against its group's schema as the
map loads: a bit per byte of the tag window (1.1 MB on the PSP) and a walk
of all its tags. The schemas are the Xbox's layouts, and the PSP's bake
changes them (slim blocks, packed BSP vertices, geometry in the pack), so
the PSP's maps would be refused. They are its own bake of a retail disc, read
as halo-ce-universal reads every map: as they are. */

boolean tag_validate_tags(void *tag_header, long tag_data_size, long file_length, char const *map_name);
boolean tag_validate_structure_bsp(long tag_index, void *base, long size);

boolean tag_validate_tags(void *tag_header, long tag_data_size, long file_length, char const *map_name)
{
	(void)tag_header; (void)tag_data_size; (void)file_length; (void)map_name;
	return TRUE;
}
boolean tag_validate_structure_bsp(long tag_index, void *base, long size)
{
	(void)tag_index; (void)base; (void)size;
	return TRUE;
}

/* ---------- the high-resolution HUD's bitmaps (hud_hires_tags.c)

Its pictures replace the HUD's on a desktop's larger screen; the PSP's
screen is smaller than the Xbox's, and the GE draws the HUD's own */

void hud_hires_tags_loaded(long scenario_index);
void hud_hires_tags_unloaded(void);

void hud_hires_tags_loaded(long scenario_index) { (void)scenario_index; }
void hud_hires_tags_unloaded(void) {}

/* ---------- zlib's inflate, for compressed maps (cache_files_decompress_windows.c)

ChupathingyCE decompresses a compressed map with its own zlib
(port/third_party/zlib, its names prefixed z_). The PSP's maps are baked
uncompressed (mapunpack.py) and nothing is decompressed: a stream can't be
set up, the decompressor's "can't" (Z_STREAM_ERROR). The stream's type is
zlib's; only its address passes through here */

enum
{
	PSP_Z_STREAM_ERROR = -2, /* zlib.h's Z_STREAM_ERROR */
};

int z_inflateInit_(void *stream, char const *version, int stream_size);
int z_inflate(void *stream, int flush);
int z_inflateEnd(void *stream);

int z_inflateInit_(void *stream, char const *version, int stream_size)
{
	(void)stream; (void)version; (void)stream_size;
	return PSP_Z_STREAM_ERROR;
}
int z_inflate(void *stream, int flush) { (void)stream; (void)flush; return PSP_Z_STREAM_ERROR; }
int z_inflateEnd(void *stream) { (void)stream; return PSP_Z_STREAM_ERROR; }
