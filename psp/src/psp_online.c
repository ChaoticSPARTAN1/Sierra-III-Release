/* psp_online.c - the Multiplayer menu's ONLINE GAMES screen on the PSP (mpb1)

The public games on the internet, and a join of one. The engine adds an
ONLINE GAMES item to the Xbox's Multiplayer menu (interface/ui_widget.c,
built with HALO_GAME_BROWSER in the multiplayer build: psp_build.py's
ONLINE_GAMES_UNITS) and hands its screen the menu's frames: drawing, input,
whether it's up. On the desktop that screen (port/linux/game/browser_screen.c)
reads the community's web game list over HTTPS and draws with its own fonts
in OpenGL. The PSP has neither, so this is its own screen:

- The list is internet play's own (p2p_lobby.c): every public game's host
  publishes a signed listing through MQTT brokers, plain TCP, which the
  desktop's in-game Server Browser reads too. No TLS, no web server.
- It's drawn with the game's own tools, as the virtual keyboard is: the
  menus' fonts (ui\small_ui, ui\large_ui) and flat quads, in the menus'
  640x480.
- A game is joined as browser_screen.c joins one: its invite joined (the
  tunnel to its host, p2p.c), the network client searching as System Link's
  does, and the host's game joined once it's advertised through the tunnel
  (network_game_client_join_invite_host), then its pregame lobby opened.

What a PSP can join (psp_join_blocker): an open game, without a password
(not yet), on one of the thirteen Xbox maps baked onto this stick. That's
the first form of the PSP rating the plan calls for (multiplayer_plan.md):
a game the PSP can't play is shown dimmed with the reason, never joined.

Controls: up and down pick a game, left and right turn the page, cross
joins (A), square refreshes (X), circle goes back (B; while a host is
waited for, it stops waiting). */

#include "cseries.h"
#include "cseries/cseries_windows.h"
#include "cutscene/cinematics.h"
#include "input/input.h"
#include "interface/event_manager.h"
#include "interface/player_ui.h"
#include "interface/ui_widget.h"
#include "main/main.h"
#include "networking/network_game_globals.h"
#include "rasterizer/rasterizer.h"
#include "saved games/player_profile.h"
#include "saved games/saved_game_files.h"
#include "tag_files/tag_groups.h"
#include "text/draw_string.h"
#include "game/game.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#include <pspiofilemgr.h>

#include "p2p.h"
#include "../include/psp_paths.h"

/* ---------- constants */

enum
{
	/* (event_manager.c's event types, which it keeps to itself) */
	ONLINE_EVENT_LEFT_STICK = 1,
	ONLINE_EVENT_BUTTON = 3,

	MAXIMUM_GAMES = 64,
	ROWS_PER_PAGE = 8,
	/* a picked game's host answers this soon, or it's given up on (the
	tunnel's hole punching tries for a while: p2p.c) */
	CONNECT_TIMEOUT = 30000,
	/* the screen takes no cross this soon after it opens (the menu's press
	that opened it) */
	OPEN_SETTLE = 600,
	/* a status line's life */
	STATUS_DURATION = 6000,
	/* whether a map is on the stick, asked again this often */
	MAP_CHECK_INTERVAL = 5000,
	/* the log's account of internet play while the screen is up */
	LOG_INTERVAL = 10000,
	/* (the whole list every third account) */
	LOGGED_GAMES = MAXIMUM_GAMES,
};

/* the layout, in the menus' 640x480 */
enum
{
	LIST_X = 37, LIST_Y = 70, LIST_WIDTH = 566, LIST_HEAD = 22, LIST_ROW = 24,
	DETAIL_Y = 290, DETAIL_HEIGHT = 130,
	COLUMN_NAME = 47, COLUMN_MAP = 262, COLUMN_TYPE = 382, COLUMN_PLAYERS = 540, COLUMN_PING = 596,
	FOOTER_Y = 438,
};

/* colours (0xAARRGGBB, as draw_quad takes them) */
enum
{
	COLOR_BACKGROUND = 0xFF050C1A,
	COLOR_HEADER = 0xFF0A1A36,
	COLOR_RULE = 0xFF2A62C8,
	COLOR_PANEL = 0xFF081530,
	COLOR_LIST_HEAD = 0xFF123266,
	COLOR_ROW_SELECTED = 0xFF2052B0,
	COLOR_ROW_RULE = 0xFF16294A,
	COLOR_TITLE = 0xFF3D8BFF,
	COLOR_HEAD = 0xFF7FB0FF,
	COLOR_TEXT = 0xFFE6EEFC,
	COLOR_DIM = 0xFF8FA6C8,
	COLOR_BLOCKED = 0xFFF08A4B,
	COLOR_READY = 0xFF7BE08A,
	COLOR_PROMPT = 0xFF4AA3FF,
};

/* the game types, short, as players say them (the listing's engine index) */
static char const *const engine_names[] = { "Game", "CTF", "Slayer", "Oddball", "King", "Race" };

/* the Xbox's thirteen multiplayer maps: their files and their names in the menus */
static char const *const map_names[][2] =
{
	{ "beavercreek", "Battle Creek" }, { "bloodgulch", "Blood Gulch" }, { "boardingaction", "Boarding Action" },
	{ "carousel", "Derelict" }, { "chillout", "Chill Out" }, { "damnation", "Damnation" },
	{ "hangemhigh", "Hang 'Em High" }, { "longest", "Longest" }, { "prisoner", "Prisoner" },
	{ "putput", "Chiron TL-34" }, { "ratrace", "Rat Race" }, { "sidewinder", "Sidewinder" }, { "wizard", "Wizard" },
};

/* the campaign's levels, which a host's network co-op game is on */
static char const *const campaign_maps[] =
{
	"a10", "a30", "a50", "b30", "b40", "c10", "c20", "c40", "d20", "d40",
};

/* ---------- globals */

static struct
{
	boolean active;
	short selected;
	short count;
	struct p2p_listing games[MAXIMUM_GAMES];
	char status[96];
	unsigned long status_time;
	unsigned long opened_time;
	/* a game picked: its invite and name, while its host's game is waited for */
	boolean connecting;
	char connecting_invite[P2P_LISTING_INVITE_SIZE];
	char connecting_name[P2P_LISTING_NAME_SIZE + 1];
	unsigned char connecting_identifier[6];
	unsigned long connecting_time;
	/* which of the thirteen maps are on the stick, and when that was asked */
	boolean map_present[NUMBEROF(map_names)];
	unsigned long maps_checked_time;
	boolean maps_checked;
	unsigned long logged_time;
	long accounts;
	/* browser_screen_process's calls, for the hands-free test's line */
	unsigned long processed;
} online;

/* (network_client_manager.c: the game whose host's identifier the invite
starts with, joined once it's advertised; 1 joining, 0 not yet, -1 can't) */
long network_game_client_join_invite_host(char const *invite);
/* (ui_widget_event_handler_functions.c: the network searching for games,
as System Link's list starts it) */
boolean ui_online_games_start_network(void);
/* (the platform's log, port/linux/src/platform.h) */
void platform_log(const char *format, ...);
/* (psp_fixups.py's FIXES_INTERNET: the brokers' states, and the listings
heard and why any were turned away) */
void psp_signal_report(char *text, int size);
void psp_lobby_report(char *text, int size);
void psp_tunnel_report(char *text, int size);
void psp_tunnel_sizes(char *text, int size);
void psp_net_select_report(char *text, int size);
int psp_internet_thread_priority_now(void);
/* (psp_internet.c: whether a file is on the Memory Stick) */
int psp_file_exists(const char *path);
/* (psp_internet.c: the tunnel's thread above the game thread in a game) */
void psp_internet_tunnel_priority(int in_game);
struct halo_ui_pointer;

/* ---------- private code */

static void set_status(
	char const *text)
{
	csstrncpy(online.status, text, sizeof(online.status) - 1);
	online.status[sizeof(online.status) - 1] = 0;
	online.status_time = system_milliseconds();
}

/* a listed map's index among the thirteen, or NONE (a custom map: its file
name, or a family's, file@ce) */
static short stock_map_index(
	char const *map)
{
	short index;

	for (index = 0; index < NUMBEROF(map_names); index++)
	{
		if (!strcmp(map, map_names[index][0]))
			return index;
	}
	return NONE;
}

/* which of the thirteen maps this stick has (the multiplayer build reads
them from the campaign build's folder, psp_paths.h) */
static void check_maps(
	void)
{
	short index;

	if (online.maps_checked && system_milliseconds() - online.maps_checked_time < MAP_CHECK_INTERVAL)
		return;
	for (index = 0; index < NUMBEROF(map_names); index++)
	{
		char path[96];

		snprintf(path, sizeof(path), PSP_MAPS_ROOT "/%s.map", map_names[index][0]);
		online.map_present[index] = (boolean)psp_file_exists(path);
	}
	if (!online.maps_checked)
	{
		char text[160];
		long used = 0;

		text[0] = 0;
		for (index = 0; index < NUMBEROF(map_names); index++)
		{
			if (online.map_present[index] && used < (long)sizeof(text) - 20)
				used += snprintf(text + used, sizeof(text) - (size_t)used, " %s", map_names[index][0]);
		}
		platform_log("online games: maps on this stick (%s):%s", PSP_MAPS_ROOT, used ? text : " none");
	}
	online.maps_checked = TRUE;
	online.maps_checked_time = system_milliseconds();
}

/* why this PSP can't join a game, or NULL when it can */
static char const *psp_join_blocker(
	struct p2p_listing const *game)
{
	short map = stock_map_index(game->map);

	if (map == NONE)
	{
		short level;

		for (level = 0; level < NUMBEROF(campaign_maps); level++)
		{
			if (!strcmp(game->map, campaign_maps[level]))
				return "Co-op: not on the PSP yet";
		}
		return "Custom map: not on the PSP yet";
	}
	if (!online.map_present[map])
		return "Map not on this Memory Stick";
	if (game->locked)
		return "Password games: not yet";
	if (!game->open)
		return "Not accepting players";
	if (game->failed)
		return "Couldn't reach its host";
	return NULL;
}

static char const *map_display_name(
	char const *map)
{
	short index = stock_map_index(map);

	return index != NONE ? map_names[index][1] : map;
}

static void type_name(
	struct p2p_listing const *game,
	char *text,
	long size)
{
	char const *engine = game->engine_type < NUMBEROF(engine_names) ? engine_names[game->engine_type] : "Game";

	/* (Capture the Flag is played in teams alone) */
	snprintf(text, (size_t)size, "%s%s", game->has_teams && game->engine_type != 1 ? "Team " : "", engine);
}

/* the lobby's games (it orders them: the most players first); the
selection stays on its game */
static void fetch_games(
	void)
{
	unsigned char identifier[6];
	boolean had_selection = online.selected >= 0 && online.selected < online.count;
	short index;

	if (had_selection)
		csmemcpy(identifier, online.games[online.selected].identifier, sizeof(identifier));
	online.count = (short)p2p_lobby_games(online.games, MAXIMUM_GAMES);
	/* (one row per game: the lobby lists a game heard under several
	network versions once, at the newest, p2p_listing_older_copy) */
	for (index = 0; had_selection && index < online.count; index++)
	{
		if (!csmemcmp(online.games[index].identifier, identifier, sizeof(identifier)))
			online.selected = index;
	}
	if (online.selected >= online.count)
		online.selected = (short)MAX(0, online.count - 1);
}

/* the first local player, with the profile System Link's Start would pick:
the one last used, else the first saved (browser_screen.c's
first_player_profile and join_first_player) */
static void join_first_player(
	void)
{
	struct player_profile profile;
	long profile_index = player_ui_get_player1_last_used_profile_index();

	player_ui_local_player_joined_multiplayer_game(0);
	if (profile_index == NONE || !TEST_FLAG(profile_index, _saved_game_file_index_valid_bit))
	{
		long profile_indices[100];
		word profile_count = NUMBEROF(profile_indices);

		player_profiles_enumerate_available_to_local_player_index(0, &profile_count, profile_indices, FALSE);
		profile_index = profile_count ? profile_indices[0] : NONE;
	}
	if (profile_index != NONE && TEST_FLAG(profile_index, _saved_game_file_index_valid_bit) &&
		player_profile_get(profile_index, &profile))
	{
		player_ui_set_active_player_profile(0, profile_index, &profile);
	}
}

/* a game picked: its invite joined (the tunnel to its host), then its game
joined once advertised through it (wait_for_host) */
static void join_selected(
	void)
{
	struct p2p_listing const *game;
	char const *blocker;

	if (online.selected < 0 || online.selected >= online.count)
		return;
	game = &online.games[online.selected];
	blocker = psp_join_blocker(game);
	if (blocker)
	{
		set_status(blocker);
		return;
	}
	if (!global_network_game_client_get())
	{
		if (!create_global_network_game_client())
		{
			set_status("Couldn't start the network.");
			return;
		}
		game_connection_set(_game_connection_network_client);
	}
	join_first_player();
	if (!p2p_join_invite(game->invite))
	{
		set_status("Internet play is off (network.online in config.toml).");
		return;
	}
	online.connecting = TRUE;
	/* (the tunnel's handshake with the host needs its thread now; the
	menus' start-up thread has finished by the time a game can be chosen) */
	psp_internet_tunnel_priority(1);
	csstrncpy(online.connecting_invite, game->invite, sizeof(online.connecting_invite) - 1);
	online.connecting_invite[sizeof(online.connecting_invite) - 1] = 0;
	csstrncpy(online.connecting_name, game->name, sizeof(online.connecting_name) - 1);
	online.connecting_name[sizeof(online.connecting_name) - 1] = 0;
	csmemcpy(online.connecting_identifier, game->identifier, sizeof(online.connecting_identifier));
	online.connecting_time = system_milliseconds();
	platform_log("online games: joining %s (%s, %s)", game->name, game->map, game->gametype);
}

/* the picked game's host: its game joined once it's advertised, and its
lobby opened (an invite reads "halo://join/<host><token>": the host's part
is what network_game_client_join_invite_host matches) */
static void wait_for_host(
	void)
{
	char const *invite = online.connecting_invite;
	long joined;

	if (!strncmp(invite, "halo://join/", 12))
		invite += 12;
	joined = network_game_client_join_invite_host(invite);
	if (joined > 0)
	{
		platform_log("online games: joined %s", online.connecting_name);
		online.connecting = FALSE;
		online.active = FALSE;
		p2p_lobby_browse(FALSE);
		psp_internet_tunnel_priority(1);
		ui_widgets_close_all();
		ui_widget_load_by_name_or_tag(
			"ui\\shell\\main_menu\\multiplayer_type_select\\connected\\pregame\\connected_pregame_screen",
			NONE, NULL, NONE, NONE, NONE, NONE);
	}
	else if (joined < 0)
	{
		online.connecting = FALSE;
		set_status("That game can't be joined from this version.");
	}
	else if (system_milliseconds() - online.connecting_time > CONNECT_TIMEOUT)
	{
		online.connecting = FALSE;
		p2p_lobby_mark_failed(online.connecting_identifier);
		platform_log("online games: %s's host didn't answer", online.connecting_name);
		set_status("The host didn't answer.");
	}
}

static void step(
	short move)
{
	online.selected = (short)PIN(online.selected + move, 0, MAX(0, online.count - 1));
}

static void press(
	short button)
{
	switch (button)
	{
	case _gamepad_binary_button_dpad_up: step(-1); break;
	case _gamepad_binary_button_dpad_down: step(1); break;
	case _gamepad_binary_button_dpad_left: step(-ROWS_PER_PAGE); break;
	case _gamepad_binary_button_dpad_right: step(ROWS_PER_PAGE); break;
	case _gamepad_analog_button_a:
		if (!online.connecting && system_milliseconds() - online.opened_time > OPEN_SETTLE)
			join_selected();
		break;
	case _gamepad_analog_button_x:
		p2p_lobby_refresh();
		online.maps_checked = FALSE;
		set_status("Refreshing");
		break;
	case _gamepad_analog_button_b:
		if (online.connecting)
			online.connecting = FALSE;
		else
		{
			online.active = FALSE;
			p2p_lobby_browse(FALSE);
		}
		break;
	default: break;
	}
}

/* ---------- drawing */

static void quad(
	short x0,
	short y0,
	short x1,
	short y1,
	pixel32 color)
{
	rectangle2d bounds;

	bounds.x0 = x0;
	bounds.y0 = y0;
	bounds.x1 = x1;
	bounds.y1 = y1;
	draw_quad(&bounds, color);
}

/* text on one line in a font (NONE: not drawn), from x (justification 0:
its left; 1: its right) with its top at y */
static void text(
	long font,
	short justification,
	short x,
	short y,
	pixel32 color,
	char const *string)
{
	union real_argb_color real_color;
	rectangle2d bounds;

	if (font == NONE || !string || !string[0])
		return;
	real_color.alpha = (real)((color >> 24) & 0xFF) / 255.0f;
	real_color.red = (real)((color >> 16) & 0xFF) / 255.0f;
	real_color.green = (real)((color >> 8) & 0xFF) / 255.0f;
	real_color.blue = (real)(color & 0xFF) / 255.0f;
	draw_string_set_draw_mode(font, NONE, justification, 0, &real_color);
	bounds.y0 = y;
	bounds.y1 = (short)(y + 40);
	bounds.x0 = justification == 1 ? 0 : x;
	bounds.x1 = justification == 1 ? x : 640;
	rasterizer_draw_string(&bounds, &bounds, NULL, 0, string);
}

/* what internet play is doing, for the log: the brokers, the lobby's
listings, this PSP's clock (a listing's time is checked against it), and
the games shown with what this PSP makes of each */
static void log_state(
	void)
{
	char text[320];
	time_t now = time(NULL);
	struct tm const *utc = gmtime(&now);
	short index;

	psp_signal_report(text, sizeof(text));
	platform_log("online games: brokers: %s", text);
	psp_lobby_report(text, sizeof(text));
	platform_log("online games: lobby: %s", text);
	if (utc)
	{
		platform_log("online games: this PSP's clock: %04d-%02d-%02d %02d:%02d:%02d UTC", utc->tm_year + 1900,
			utc->tm_mon + 1, utc->tm_mday, utc->tm_hour, utc->tm_min, utc->tm_sec);
	}
	platform_log("online games: %d public games", online.count);
	if (++online.accounts % 3 != 1)
		return;
	for (index = 0; index < online.count && index < LOGGED_GAMES; index++)
	{
		struct p2p_listing const *game = &online.games[index];
		char const *blocker = psp_join_blocker(game);

		platform_log("online games:   %s | %s | %s | %d/%d | ping %d | %s", game->name, game->map, game->gametype,
			game->player_count, game->maximum_player_count, game->ping, blocker ? blocker : "joinable");
	}
}

/* ---------- public code */

boolean browser_screen_active(
	void)
{
	return online.active;
}

/* the screen opened (the Multiplayer menu's ONLINE GAMES, interface/ui_widget.c) */
void browser_screen_open(
	void)
{
	online.active = TRUE;
	online.selected = 0;
	online.status[0] = 0;
	online.connecting = FALSE;
	online.maps_checked = FALSE;
	online.opened_time = online.logged_time = system_milliseconds();
	event_manager_flush();
	/* the network searching, as System Link's list starts it (a game left
	behind ended), and the lobby gathering the public games */
	if (!ui_online_games_start_network())
		set_status("Couldn't start the network.");
	p2p_lobby_browse(TRUE);
	psp_internet_tunnel_priority(0);
	fetch_games();
}

void browser_screen_process(
	void)
{
	struct event_record event;

	online.processed++;
	check_maps();
	fetch_games();
	if (system_milliseconds() - online.logged_time >= LOG_INTERVAL)
	{
		online.logged_time = system_milliseconds();
		log_state();
	}
	if (online.connecting)
		wait_for_host();
	while (online.active && get_next_event(&event, NONE))
	{
		if (event.type == ONLINE_EVENT_BUTTON)
			press(event.data.button.index);
		else if (event.type == ONLINE_EVENT_LEFT_STICK)
		{
			if (event.data.stick.y == SHORT_MAX)
				step(-1);
			else if (event.data.stick.y == SHORT_MIN)
				step(1);
			else if (event.data.stick.x == SHORT_MIN)
				step(-ROWS_PER_PAGE);
			else if (event.data.stick.x == SHORT_MAX)
				step(ROWS_PER_PAGE);
		}
	}
	/* (the widgets behind take nothing while the screen is up) */
	event_manager_flush();
}

void browser_screen_render(
	void)
{
	long small_font = tag_loaded('font', "ui\\small_ui");
	long large_font = tag_loaded('font', "ui\\large_ui");
	struct p2p_listing const *selected = online.count ? &online.games[online.selected] : NULL;
	short page_first = (short)(online.selected / ROWS_PER_PAGE * ROWS_PER_PAGE);
	short row;
	long players = 0, index;
	char line[160];

	for (index = 0; index < online.count; index++)
		players += online.games[index].player_count;

	/* the screen and its header */
	quad(0, 0, 640, 480, COLOR_BACKGROUND);
	quad(0, 0, 640, 60, COLOR_HEADER);
	quad(0, 60, 640, 61, COLOR_RULE);
	text(large_font, 0, 37, 16, COLOR_TITLE, "ONLINE GAMES");
	snprintf(line, sizeof(line), "%d GAMES   %ld PLAYERS", online.count, players);
	text(small_font, 1, 603, 26, COLOR_DIM, line);

	/* the list */
	quad(LIST_X, LIST_Y, LIST_X + LIST_WIDTH, LIST_Y + LIST_HEAD + ROWS_PER_PAGE * LIST_ROW, COLOR_PANEL);
	quad(LIST_X, LIST_Y, LIST_X + LIST_WIDTH, LIST_Y + LIST_HEAD, COLOR_LIST_HEAD);
	text(small_font, 0, COLUMN_NAME, LIST_Y + 3, COLOR_HEAD, "SERVER");
	text(small_font, 0, COLUMN_MAP, LIST_Y + 3, COLOR_HEAD, "MAP");
	text(small_font, 0, COLUMN_TYPE, LIST_Y + 3, COLOR_HEAD, "TYPE");
	text(small_font, 1, COLUMN_PLAYERS, LIST_Y + 3, COLOR_HEAD, "PLAYERS");
	text(small_font, 1, COLUMN_PING, LIST_Y + 3, COLOR_HEAD, "PING");
	if (!online.count)
	{
		text(small_font, 0, COLUMN_NAME, LIST_Y + LIST_HEAD + 8, COLOR_DIM,
			"Looking for public games...");
	}
	for (row = 0; row < ROWS_PER_PAGE && page_first + row < online.count; row++)
	{
		struct p2p_listing const *game = &online.games[page_first + row];
		short y = (short)(LIST_Y + LIST_HEAD + row * LIST_ROW);
		pixel32 color = psp_join_blocker(game) ? COLOR_DIM : COLOR_TEXT;

		if (page_first + row == online.selected)
			quad(LIST_X, y, LIST_X + LIST_WIDTH, (short)(y + LIST_ROW), COLOR_ROW_SELECTED);
		else
			quad(LIST_X, (short)(y + LIST_ROW - 1), LIST_X + LIST_WIDTH, (short)(y + LIST_ROW), COLOR_ROW_RULE);
		text(small_font, 0, COLUMN_NAME, (short)(y + 4), color, game->name);
		text(small_font, 0, COLUMN_MAP, (short)(y + 4), color, map_display_name(game->map));
		type_name(game, line, sizeof(line));
		text(small_font, 0, COLUMN_TYPE, (short)(y + 4), color, line);
		snprintf(line, sizeof(line), "%d/%d", game->player_count, game->maximum_player_count);
		text(small_font, 1, COLUMN_PLAYERS, (short)(y + 4), color, line);
		if (game->ping >= 0)
			snprintf(line, sizeof(line), "%d", game->ping);
		else
			snprintf(line, sizeof(line), "-");
		text(small_font, 1, COLUMN_PING, (short)(y + 4), color, line);
	}

	/* the picked game: what it is, and whether this PSP can join it */
	quad(LIST_X, DETAIL_Y, LIST_X + LIST_WIDTH, DETAIL_Y + DETAIL_HEIGHT, COLOR_PANEL);
	if (selected)
	{
		char const *blocker = psp_join_blocker(selected);

		text(large_font, 0, COLUMN_NAME, DETAIL_Y + 8, COLOR_TEXT, selected->name);
		type_name(selected, line, sizeof(line));
		snprintf(line + strlen(line), sizeof(line) - strlen(line), " on %s   %d of %d players%s%s",
			map_display_name(selected->map), selected->player_count, selected->maximum_player_count,
			selected->in_progress ? "   under way" : "   in its lobby", selected->dedicated ? "   dedicated server" : "");
		text(small_font, 0, COLUMN_NAME, DETAIL_Y + 44, COLOR_DIM, line);
		if (online.connecting)
		{
			snprintf(line, sizeof(line), "Reaching %s's host...", online.connecting_name);
			text(small_font, 0, COLUMN_NAME, DETAIL_Y + 72, COLOR_PROMPT, line);
		}
		else
			text(small_font, 0, COLUMN_NAME, DETAIL_Y + 72, blocker ? COLOR_BLOCKED : COLOR_READY,
				blocker ? blocker : "Ready to join on this PSP");
	}
	if (online.status[0] && system_milliseconds() - online.status_time < STATUS_DURATION)
		text(small_font, 0, COLUMN_NAME, DETAIL_Y + 100, COLOR_TEXT, online.status);

	/* what the buttons do */
	text(small_font, 0, LIST_X, FOOTER_Y, COLOR_PROMPT,
		online.connecting ? "CIRCLE  stop waiting" : "CROSS  join      SQUARE  refresh      CIRCLE  back");
}

/* (the menus' pointer: the PSP has none) */
void browser_screen_pointer(
	struct halo_ui_pointer const *pointer)
{
	(void)pointer;
}

/* ---------- the hands-free test (keep words onlinelist, onlinetest, onlinedoneN)

For an emulator run, which has no one to press buttons: once the main
menu has been up a few seconds the screen opens by itself (and logs, as it
always does while up). With onlinetest, the first game it can join is
joined, as cross would. onlinedoneN ends the game N seconds after the
screen opened, so an emulator run ends at its question (the menu's game
time stands still, so exitatN never comes). Never on a PSP: a game that
quits by itself leaves the PSP hung. Called every frame (network_test.c's
network_test_update, psp_fixups.py) */

enum
{
	TEST_OPEN_DELAY = 5000,
	/* the lobby gathers what the brokers kept, then what hosts publish again */
	TEST_JOIN_DELAY = 30000,
};

int psp_keep(const char *name);
int psp_keep_number(const char *name, int otherwise);
void psp_exit_game(const char *why);

/* keep word onlinejoin=<prefix>: onlinetest joins only a game whose name
starts with it, so a run can be pointed at servers whose owner agreed to
tests (Milenko's are named "[C] ..."). keep.txt is read here because the
platform's keep words carry yes/no and numbers, not text; the value ends at
a space or a comma, as the words do, so an underscore in it stands for a
space ("onlinejoin=[C]_Slayer") */
static void test_join_prefix(char *prefix, int size)
{
	static const char word[] = "onlinejoin=";
	char text[512];
	const char *found;
	int length = 0, count;
	/* (the firmware's own calls, as keep_load reads the file: the game's
	fopen would take ms0:/ for an Xbox path under its data folder) */
	SceUID file = sceIoOpen(PSP_DATA_ROOT "/keep.txt", PSP_O_RDONLY, 0);

	prefix[0] = '\0';
	if (file >= 0)
	{
		length = sceIoRead(file, text, sizeof(text) - 1);
		sceIoClose(file);
	}
	text[length > 0 ? length : 0] = '\0';
	found = strstr(text, word);
	if (!found)
		return;
	found += sizeof(word) - 1;
	for (count = 0; count < size - 1 && found[count] && !strchr(" \t\r\n,", found[count]); count++)
		prefix[count] = found[count] == '_' ? ' ' : found[count];
	prefix[count] = '\0';
}

void psp_online_test_update(
	boolean main_menu_loaded)
{
	static int mode = -1, done_seconds;
	static char join_prefix[32];
	static unsigned long menu_time, opened_time, test_logged_time;
	static boolean opened, join_tried;
	unsigned long now = system_milliseconds();

	if (mode < 0)
	{
		mode = psp_keep("onlinetest") ? 2 : psp_keep("onlinelist") ? 1 : 0;
		done_seconds = psp_keep_number("onlinedone", 0);
		if (mode == 2)
		{
			test_join_prefix(join_prefix, (int)sizeof(join_prefix));
			if (join_prefix[0])
				platform_log("online games: test: joining only a game named \"%s...\" (onlinejoin=)", join_prefix);
		}
	}
	if (!mode)
		return;
	if (opened && done_seconds > 0 && now - opened_time >= (unsigned long)done_seconds * 1000)
	{
		log_state();
		psp_exit_game("onlinedone");
		return;
	}
	if (!opened)
	{
		if (!main_menu_loaded)
		{
			menu_time = 0;
			return;
		}
		if (!menu_time)
			menu_time = now;
		if (now - menu_time < TEST_OPEN_DELAY)
			return;
		platform_log("online games: opening the screen (%s)", mode == 2 ? "onlinetest" : "onlinelist");
		browser_screen_open();
		opened = TRUE;
		opened_time = now;
		return;
	}
	/* (the screen's own lines come from browser_screen_process; this one
	says whether the menus are calling it at all) */
	if (now - test_logged_time >= LOG_INTERVAL)
	{
		char text[640];

		test_logged_time = now;
		platform_log("online games: test: screen %s, processed %lu times, %d games", online.active ? "open" : "closed",
			online.processed, online.count);
		psp_tunnel_report(text, sizeof(text));
		platform_log("online games: tunnel: %s", text);
		psp_tunnel_sizes(text, sizeof(text));
		platform_log("online games: tunnel: %s", text);
		psp_net_select_report(text, sizeof(text));
		platform_log("online games: tunnel: its thread at 0x%x; %s", psp_internet_thread_priority_now(), text);
	}
	if (!online.active)
		return;
	if (mode == 2 && !join_tried && now - opened_time >= TEST_JOIN_DELAY)
	{
		short index;

		short pass;

		/* a dedicated server first: its port is usually open, where a
		player's home router may not let the tunnel through */
		index = online.count;
		for (pass = 0; pass < 2 && index == online.count; pass++)
		{
			for (index = 0; index < online.count; index++)
			{
				struct p2p_listing const *game = &online.games[index];

				if (!psp_join_blocker(game) && (pass || game->dedicated) &&
					(!join_prefix[0] || !strncmp(game->name, join_prefix, strlen(join_prefix))))
					break;
			}
		}
		if (index < online.count)
		{
			join_tried = TRUE;
			online.selected = index;
			join_selected();
		}
	}
}

/* ---------- no moderation (a dedicated server's, moderation_screen.c) */

boolean moderation_screen_active(
	void)
{
	return FALSE;
}

void moderation_screen_update(
	boolean menu_open)
{
	(void)menu_open;
}

void moderation_screen_process(
	void)
{
}

void moderation_screen_render(
	void)
{
}
