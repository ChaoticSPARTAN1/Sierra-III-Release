/* all_levels.c - the release's ready-made profile with every campaign level
open (psp_build.py, seed_profile).

Never linked into the game. psp_build.py compiles it with the flags of the
engine's player_profile.c and reads psp_seed_profile's bytes out of the
object, so the profile is laid out by the engine's own headers: a field
added, moved or resized in struct player_profile moves here too, and the
checks below stop the build when the file around it no longer fits.

The values are a new profile's (player_profile_new), with every difficulty
of every level finished, as config.toml's game.unlock_all_levels marks a new
profile. PSP_SEED_NAME and PSP_SEED_LOOK_SENSITIVITY come from psp_build.py:
the name is the profile's slot (its save folder is named from it), and the
look sensitivity is player_profile.c's own DEFAULT_LOOK_SENSITIVITY, which no
header carries. */
#include "cseries/cseries.h"
#include "game/game.h"
#include "saved games/player_profile.h"
#include "saved games/saved_game_files.h"

#include <xtl.h>

#define PSP_SEED_ALL_DIFFICULTIES ((byte)((1 << NUMBER_OF_GAME_DIFFICULTY_LEVELS) - 1))

/* the flags are a byte per level, a bit per difficulty */
typedef char psp_seed_difficulties_fit[NUMBER_OF_GAME_DIFFICULTY_LEVELS <= 8 ? 1 : -1];
/* the profile, its checksum and the rest of the file's one block */
typedef char psp_seed_block_fits[
	sizeof(struct player_profile) + sizeof(XCALCSIG_SIGNATURE) <= SAVED_GAME_FILE_BLOCK_SIZE ? 1 : -1];
typedef char psp_seed_name_fits[sizeof(PSP_SEED_NAME) / sizeof(wchar_t) <= MAXIMUM_PLAYER_PROFILE_NAME_LENGTH ? 1 : -1];

const struct player_profile psp_seed_profile =
{
	.player_name = PSP_SEED_NAME,
	.primary_color_index = NONE,
	.flags = 0,
	.single_player_map_flags =
	{
		PSP_SEED_ALL_DIFFICULTIES, PSP_SEED_ALL_DIFFICULTIES, PSP_SEED_ALL_DIFFICULTIES,
		PSP_SEED_ALL_DIFFICULTIES, PSP_SEED_ALL_DIFFICULTIES, PSP_SEED_ALL_DIFFICULTIES,
		PSP_SEED_ALL_DIFFICULTIES, PSP_SEED_ALL_DIFFICULTIES, PSP_SEED_ALL_DIFFICULTIES,
		PSP_SEED_ALL_DIFFICULTIES,
	},
	.last_single_player_map_played = 0,
	.controller_settings =
	{
		.button_preset = _button_preset_standard,
		.joystick_preset = _joystick_preset_standard,
		.look_sensitivity = PSP_SEED_LOOK_SENSITIVITY,
		.invert_look = FALSE,
		.vibration_disabled = FALSE,
		.flight_stick_aircraft_controls = FALSE,
		.autocenter = FALSE,
		.ingame_help_disabled = FALSE,
	},
};
typedef char psp_seed_every_level[NUMBER_OF_SINGLE_PLAYER_LEVELS == 10 ? 1 : -1];

/* what psp_build.py checks the bytes against: the profile's size, the file's
block, the checksum's size */
const unsigned short psp_seed_sizes[3] =
{
	sizeof(struct player_profile), SAVED_GAME_FILE_BLOCK_SIZE, sizeof(XCALCSIG_SIGNATURE)
};
