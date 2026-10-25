/* psp_campaign.c - "no network game", for the campaign-only engine (build 107)

The campaign build leaves out the networking, the netcode and the
multiplayer game engines (psp_build.py's MULTIPLAYER_UNITS;
docs/dead_code_inventory.md). Co-op and multiplayer become a branch of their
own, with their own budgets (2026-10-08).

What the campaign still calls in those units is answered here, each the way
the real function answers when there is no network game: no client, no
server, the game's connection local. Those answers are read from the real
code, and each says where it came from. Most only ever run from the
multiplayer menus, which can't start a game without a client or a server.

Multiplayer maps still load the way they are tested (map_name
levels\test\..., which runs no game engine).

The headers are the real ones, so a stub whose signature differs from the
declaration doesn't compile. The netcode's (port/linux/game/network_*.c) have
no header: their declarations are copied from the files that call them. */

#include "cseries.h"
#include "bungie_net/network/transport.h"
#include "bungie_net/network/transport_endpoint_winsock.h"
#include "game/game.h"
#include "game/game_engine.h"
#include "game/game_engine_list.h"
#include "game/player_queues_new.h"
#include "networking/network_client_manager.h"
#include "networking/network_connection.h"
#include "networking/network_game_globals.h"
#include "networking/network_game_manager.h"
#include "networking/network_messages.h"
#include "networking/network_server_manager.h"
#include "networking/telnet_console.h"
#include "objects/damage.h"

/* ---------- script globals (hs_globals_external.c takes their addresses) */

/* network_client_manager.c's and network_connection.c's: set by the console,
read only by the networking */
boolean allow_out_of_sync = FALSE;
boolean global_connection_dont_timeout = FALSE;

/* ---------- the game engines (game_engine_list.c) */

/* No multiplayer engine. game_engine_initialize picks one only for a
multiplayer game variant (game_engine_index above 0), so the campaign's
game_engine stays NULL as before */
struct game_engine *game_engines[8] = { NULL };

/* game_engine_multiplayer_sounds.c's: run only with a game engine */
void game_engine_intialize_queued_sounds(void) {}
void game_engine_play_multiplayer_sound(long sound_index) { (void)sound_index; }
void game_engine_update_multiplayer_sound(void) {}

/* ---------- network_game_globals.c: no client and no server */

boolean create_global_network_game_client(void) { return FALSE; }
boolean create_global_network_game_server(void) { return FALSE; }
void dispose_global_network_game_client(void) {}
void dispose_global_network_game_server(void) {}
struct network_game_client *global_network_game_client_get(void) { return NULL; }
struct network_game_server *global_network_game_server_get(void) { return NULL; }
struct network_game *network_game_get_game(void) { return NULL; }
boolean network_game_is_active(void) { return FALSE; }
boolean network_game_is_splitscreen_local(void) { return FALSE; }
boolean network_game_should_accept_remote_connections(void) { return FALSE; }
void network_game_accept_remote_connections(boolean accept_remote_connections) { (void)accept_remote_connections; }
void network_game_set_quickstart_local(void) {}
void network_game_abort(void) {}
/* game_connection() is never the network client's here */
boolean network_game_distributed_client(void) { return FALSE; }
/* with no client: TRUE, except in a saved film's playback, where the
player's own machine index says (network_game_globals.c's struct and
assert, copied) */
struct local_network_player
{
	byte __unknown0[0x1C];
	boolean machine_index;
};

boolean network_game_player_is_local(struct network_player *player)
{
	if (game_connection() == _game_connection_film_playback)
	{
		match_assert(
			"c:\\halo\\SOURCE\\networking\\network_game_globals.c",
			0x9B,
			player);
		return !((struct local_network_player *)player)->machine_index;
	}
	return TRUE;
}
short network_game_client_get_local_machine_index(void) { return NONE; }
void network_game_client_local_player_quit(short controller_index) { (void)controller_index; }
void network_game_client_request_immediate_start(void) {}
/* main.c calls the frame functions only while the connection is a network
one, which it never is here; TRUE is their "nothing went wrong" */
boolean network_game_client_start_frame(void) { return TRUE; }
boolean network_game_client_end_frame(void) { return TRUE; }
boolean network_game_server_start_frame(void) { return TRUE; }

/* ---------- network_client_manager.c: never given a client */

boolean network_game_client_add_player(struct network_game_client *client, short local_player_index)
{
	(void)client; (void)local_player_index;
	return FALSE;
}
boolean network_game_client_advertised_game_compatible(struct network_game_client *client,
	struct network_advertised_game const *game, boolean tell)
{
	(void)client; (void)game; (void)tell;
	return FALSE;
}
boolean network_game_client_advertised_game_is_valid(struct network_advertised_game *advertised_game)
{
	(void)advertised_game;
	return FALSE;
}
struct network_advertised_game *network_game_client_get_available_games(struct network_game_client *client)
{
	(void)client;
	return NULL;
}
struct network_game *network_game_client_get_game(struct network_game_client *client) { (void)client; return NULL; }
struct network_machine *network_game_client_get_machine(struct network_game_client *client) { (void)client; return NULL; }
short network_game_client_get_machine_index(struct network_game_client *client) { (void)client; return NONE; }
short network_game_client_get_seconds_to_game_start(struct network_game_client *client) { (void)client; return 0; }
short network_game_client_get_state(struct network_game_client *client, short *state_data)
{
	(void)client; (void)state_data;
	return 0;
}
boolean network_game_client_initiate_join_game(struct network_game_client *client, struct network_advertised_game *game,
	struct network_join_parameters *join_parameters, struct transport_address *address)
{
	(void)client; (void)game; (void)join_parameters; (void)address;
	return FALSE;
}
boolean network_game_client_request_remove_player(struct network_game_client *client, struct network_player *player)
{
	(void)client; (void)player;
	return FALSE;
}
boolean network_game_client_request_start_time_change(struct network_game_client *client, short request_type)
{
	(void)client; (void)request_type;
	return FALSE;
}
boolean network_game_client_server_has_started_game(struct network_game_client *client) { (void)client; return FALSE; }
boolean network_game_client_update_local_player_data(struct network_game_client *client, struct network_player *player)
{
	(void)client; (void)player;
	return FALSE;
}

/* ---------- network_server_manager.c: never given a server */

boolean network_game_server_ban_player(char const *text) { (void)text; return FALSE; }
void network_game_server_change_game_variant(struct network_game_server *server, struct game_variant *variant)
{
	(void)server; (void)variant;
}
void network_game_server_change_map_name(struct network_game_server *server, char const *map_name)
{
	(void)server; (void)map_name;
}
short network_game_server_matching_player_names(char const *text, char (*names)[NETWORK_GAME_SERVER_NAME_TEXT_SIZE],
	short maximum_count)
{
	(void)text; (void)names; (void)maximum_count;
	return 0;
}
void network_game_server_open_game(struct network_game_server *server) { (void)server; }
void network_game_server_pause_countdown(struct network_game_server *server, boolean pause_countdown)
{
	(void)server; (void)pause_countdown;
}
boolean network_game_server_reset_to_pregame(struct network_game_server *server) { (void)server; return FALSE; }
void network_game_server_switch_to_postgame(struct network_game_server *server) { (void)server; }
void network_game_server_update_ticks(struct network_game_server *server, short tick_count)
{
	(void)server; (void)tick_count;
}
void network_game_generate_join_game_token(byte *join_token) { (void)join_token; }

/* ---------- network_messages.c, network_game_manager.c, telnet_console.c */

/* the packet definitions only the networking reads */
void initialize_network_game_packets(void) {}
/* a log line of the networking's own */
void network_event(char *format, ...) { (void)format; }
/* network_game_manager.c's, whole: it reads only the player. Its machine
count (MAXIMUM_NETWORK_MACHINE_COUNT there) is local to that file; it is
HALO_PORT_MAXIMUM_NETWORK_MACHINES */
boolean network_player_is_valid(struct network_player *player)
{
	return player &&
		player->controller_index >= 0 &&
		player->controller_index < MAXIMUM_LOCAL_PLAYERS &&
		player->machine_index >= 0 &&
		(long)player->machine_index < HALO_PORT_MAXIMUM_NETWORK_MACHINES;
}
/* the Xbox's nickname; the PSP has none */
void xbox_set_machine_name(char const *machine_name) { (void)machine_name; }
/* the remote console over a socket: the PSP opens none */
void telnet_console_initialize(void) {}
void telnet_console_dispose(void) {}
void telnet_console_process(void) {}
void telnet_console_print(char *string) { (void)string; }

/* ---------- bungie_net/network: no network (game.c ignores the results) */

short transport_initialize(void) { return _transport_error_none; }
short transport_dispose(void) { return _transport_error_none; }
/* the real one asks for the Ethernet link: the menus then grey out system link */
boolean transport_network_available(void) { return FALSE; }
void transport_client_start(XNADDR const *xnaddr, XNKEY const *key, XNKID const *key_id, word port,
	struct transport_address *address)
{
	(void)xnaddr; (void)key; (void)key_id; (void)port; (void)address;
}

/* ---------- the port's netcode (port/linux/game/network_*.c) */

/* damage.c's declarations */
boolean network_damage_deals(struct damage_data const *damage, long object_index, short node_index,
	short region_index, short material_index, real_vector3d const *object_normal, boolean authorized);
boolean network_damage_replaying_kill(void);
void network_damage_player_effect(long player_index, struct damage_data const *damage, real total_damage);
void network_damage_aftermath(long object_index, struct damage_data const *damage, unsigned long being_damaged_flags,
	real shield_damage, real body_damage, real body_damage_multiplier, short body_part, short node_index,
	short region_index, short material_index, long victim_player_index);
/* game_engine.c's */
void network_distributed_player_killed(long *killing_player_index, long *killing_object_index,
	long dead_player_index, boolean *friendly_fire);
boolean network_damage_killer_score(long player_index, long *score);
/* game.c's, game_time.c's, players.c's, objects.c's, main.c's */
void network_distributed_new_game(void);
void network_objects_placed(void);
void network_distributed_tick(void);
void network_distributed_player_picked_up(long player_index, short kind, long definition_index, short count);
long network_objects_new_object_index(void);
boolean network_objects_creating_host_object(void);
boolean network_objects_may_delete(long object_index);
void network_test_update(boolean main_menu_loaded, real seconds);

/* the machine that isn't a network client deals its own damage (the real
one's last line) */
boolean network_damage_deals(struct damage_data const *damage, long object_index, short node_index,
	short region_index, short material_index, real_vector3d const *object_normal, boolean authorized)
{
	(void)damage; (void)object_index; (void)node_index; (void)region_index; (void)material_index;
	(void)object_normal; (void)authorized;
	return TRUE;
}
/* only a client replays the host's kills */
boolean network_damage_replaying_kill(void) { return FALSE; }
boolean network_damage_killer_score(long player_index, long *score) { (void)player_index; (void)score; return FALSE; }
/* the host's records of damage for its clients: no host */
void network_damage_player_effect(long player_index, struct damage_data const *damage, real total_damage)
{
	(void)player_index; (void)damage; (void)total_damage;
}
void network_damage_aftermath(long object_index, struct damage_data const *damage, unsigned long being_damaged_flags,
	real shield_damage, real body_damage, real body_damage_multiplier, short body_part, short node_index,
	short region_index, short material_index, long victim_player_index)
{
	(void)object_index; (void)damage; (void)being_damaged_flags; (void)shield_damage; (void)body_damage;
	(void)body_damage_multiplier; (void)body_part; (void)node_index; (void)region_index; (void)material_index;
	(void)victim_player_index;
}
/* a kill's killer as the host saw it: neither host nor client, so the
caller's own stand */
void network_distributed_player_killed(long *killing_player_index, long *killing_object_index,
	long dead_player_index, boolean *friendly_fire)
{
	(void)killing_player_index; (void)killing_object_index; (void)dead_player_index; (void)friendly_fire;
}
void network_distributed_player_picked_up(long player_index, short kind, long definition_index, short count)
{
	(void)player_index; (void)kind; (void)definition_index; (void)count;
}
/* the real one clears the netcode's own state, and the player queues'
distributed inputs (player_queues_new.c), which only the netcode sets:
cleared as before, so a new game starts from the same queues */
void network_distributed_new_game(void)
{
	update_queues_distributed_reset();
}
/* neither host nor client: the real one sends nothing and flushes nothing */
void network_distributed_tick(void) {}
void network_objects_placed(void) {}
/* NONE: the object goes wherever the game puts it (only a client's are placed) */
long network_objects_new_object_index(void) { return NONE; }
boolean network_objects_creating_host_object(void) { return FALSE; }
/* not a distributed client: any object may be deleted */
boolean network_objects_may_delete(long object_index) { (void)object_index; return TRUE; }
/* the automated system-link tests: none */
void network_test_update(boolean main_menu_loaded, real seconds) { (void)main_menu_loaded; (void)seconds; }

/* ---------- the netcode's other answers outside a network game

network_objects_damage_animation keeps the animation it is given. */

/* the declarations of the callers (actors.c, damage.c, game_engine.c),
which the netcode's files have no header for */
short network_objects_damage_animation(long unit_index, short type, short animation_index);
void network_damage_note_grenade(long unit_index, short grenade_type);
long distributed_player_ping(short player_index);
struct network_game *network_game_server_get_game(struct network_game_server *server);

/* network_objects.c, network_damage.c, network_distributed.c */
/* (only a host queues it for its clients) */
short network_objects_damage_animation(long unit_index, short type, short animation_index)
{
	(void)unit_index; (void)type;
	return animation_index;
}
void network_damage_note_grenade(long unit_index, short grenade_type) { (void)unit_index; (void)grenade_type; }
/* no machine to have a ping to: the scoreboard shows none */
long distributed_player_ping(short player_index) { (void)player_index; return NONE; }

/* network_server_manager.c: never given a server (the callers ask
global_network_game_server_get first, or run only on a co-op host) */
struct network_game *network_game_server_get_game(struct network_game_server *server) { (void)server; return NULL; }
boolean network_game_server_kick_player(char const *text) { (void)text; return FALSE; }
void network_game_server_port_cooperative_won(char const *next_map) { (void)next_map; }
