/* psp_nocoop.c: ChupathingyCE's network co-op answered "never".

Both builds compile this file. The campaign build leaves out all the
netcode (psp_campaign.c answers the rest); the multiplayer build leaves out
only the co-op units, unless PSP_MP_COOP=1 builds them back in. Co-op is a
network game on a campaign map, and it needs a host: the multiplayer build
neither hosts nor carries the campaign's maps (2026-10-17: hosting
comes back after the campaign's performance targets, as co-op's
prerequisite). Leaving the units out frees their code and tables. */

#include "cseries.h"
#include "game/game.h"

/* ChupathingyCE plays the campaign over the network (port/linux/game/
network_coop.c, network_actors.c, coop_*.c), and the game calls into it at
every scripted event. Outside a network game each real function returns at
its first test (coop_host(), game_connection(), network_coop_active()); the
answers below are what those tests leave. Most do nothing, but some carry
the single player's own behaviour and must keep it:
- coop_scripts_safe_to_save is the scripts' game_safe_to_save (hs.c): the
  game's own test, or no checkpoint ever saves;
- coop_scripts_player_add_equipment is the scripts' player_add_equipment:
  the player's own equipment, then the followers' (none);
- network_coop_player_collisions and network_coop_player_has_structure_bsp
  are TRUE outside co-op: players collide, and their BSP is theirs. */

#include "game/players.h"
#include "network_coop.h"
#include "coop_enemies.h"
#include "coop_scripts.h"
#include "coop_spectate.h"

struct unit_control_data;

/* the declarations of the callers (sound_manager.c, units.c, actors.c,
damage.c, game_engine.c), which the netcode's files have no header for */
void network_coop_note_sound(short kind, long definition_index, long object_index, real scale);
void network_actors_drive(void);
void network_actors_note_control(long unit_index, struct unit_control_data const *control_data);
void network_actors_note_impulse(long unit_index, short animation_impulse, real_vector2d const *alignment_vector);
void network_actors_note_melee(long unit_index, real_vector2d const *alignment_vector);
void network_actors_note_leap(long unit_index, real_vector2d const *alignment_vector);
void network_actors_note_speech(long unit_index, long sound_definition_index);
void network_actors_note_user_animation(long unit_index, long animation_graph_index, short animation_index,
	boolean interpolate);

/* network_coop.c: never a co-op game */
boolean network_coop_active(void) { return FALSE; }
boolean network_coop_player_collisions(void) { return TRUE; }
boolean network_coop_devices_remote(void) { return FALSE; }
boolean network_coop_player_has_structure_bsp(long player_index) { (void)player_index; return TRUE; }
boolean network_coop_set_players_vitality(long unit_index, boolean maximum, real body, real shield)
{
	(void)unit_index; (void)maximum; (void)body; (void)shield;
	return FALSE;
}
boolean network_coop_skip_offered(void) { return FALSE; }
boolean network_coop_vote_skip(void) { return FALSE; }
void network_coop_skip_done(void) {}
/* (the real one also forgets coop_enemies.c's kept riders: none are kept) */
void network_coop_reverted(long now) { (void)now; }
void network_coop_vehicle_dropped(long vehicle_index, long carrier_index) { (void)vehicle_index; (void)carrier_index; }
/* the host's notes to its clients: no clients */
void network_coop_note_device_snap(short group_index) { (void)group_index; }
void network_coop_note_title(short title_index, real delay) { (void)title_index; (void)delay; }
void network_coop_note_hud(short kind, short value) { (void)kind; (void)value; }
void network_coop_note_player_effect(short kind, real a, real b, real c) { (void)kind; (void)a; (void)b; (void)c; }
void network_coop_note_nav_point(short kind, short nav_index, long target, long marker, real vertical_offset)
{
	(void)kind; (void)nav_index; (void)target; (void)marker; (void)vertical_offset;
}
void network_coop_note_unit_animation(long unit_index, long animation_graph_index, short animation_index,
	boolean interpolate)
{
	(void)unit_index; (void)animation_graph_index; (void)animation_index; (void)interpolate;
}
void network_coop_note_unit_open(long unit_index, boolean open) { (void)unit_index; (void)open; }
void network_coop_note_unit_animation_frame(long unit_index, short frame_index) { (void)unit_index; (void)frame_index; }
void network_coop_note_effect(long effect_definition_index, short cutscene_flag_index)
{
	(void)effect_definition_index; (void)cutscene_flag_index;
}
void network_coop_note_attach(long parent_index, char const *parent_marker_name, long child_index,
	char const *child_marker_name)
{
	(void)parent_index; (void)parent_marker_name; (void)child_index; (void)child_marker_name;
}
void network_coop_note_detach(long parent_index, long child_index) { (void)parent_index; (void)child_index; }
void network_coop_note_object_effect(long effect_definition_index, long object_index, char const *marker_name)
{
	(void)effect_definition_index; (void)object_index; (void)marker_name;
}
void network_coop_note_surface_broken(short breakable_surface_index, real_point3d const *epicenter)
{
	(void)breakable_surface_index; (void)epicenter;
}
void network_coop_note_scenery_animation(long object_index, long animation_graph_index, short animation_index,
	short frame_index)
{
	(void)object_index; (void)animation_graph_index; (void)animation_index; (void)frame_index;
}
void network_coop_note_sound(short kind, long definition_index, long object_index, real scale)
{
	(void)kind; (void)definition_index; (void)object_index; (void)scale;
}

/* network_actors.c: a host's AI sent to its clients, a client's driven from
them: neither here */
void network_actors_drive(void) {}
void network_actors_note_control(long unit_index, struct unit_control_data const *control_data)
{
	(void)unit_index; (void)control_data;
}
void network_actors_note_impulse(long unit_index, short animation_impulse, real_vector2d const *alignment_vector)
{
	(void)unit_index; (void)animation_impulse; (void)alignment_vector;
}
void network_actors_note_melee(long unit_index, real_vector2d const *alignment_vector)
{
	(void)unit_index; (void)alignment_vector;
}
void network_actors_note_leap(long unit_index, real_vector2d const *alignment_vector)
{
	(void)unit_index; (void)alignment_vector;
}
void network_actors_note_speech(long unit_index, long sound_definition_index)
{
	(void)unit_index; (void)sound_definition_index;
}
void network_actors_note_user_animation(long unit_index, long animation_graph_index, short animation_index,
	boolean interpolate)
{
	(void)unit_index; (void)animation_graph_index; (void)animation_index; (void)interpolate;
}

/* coop_enemies.c: no extra enemies for extra players. The positions are
asked only for an extra enemy */
short coop_enemies_extra_count(long encounter_index, short count) { (void)encounter_index; (void)count; return 0; }
boolean coop_enemies_spread_position(real_point3d const *origin, short number, long *taken, real_point3d *position)
{
	(void)origin; (void)number; (void)taken; (void)position;
	return FALSE;
}
boolean coop_enemies_fallback_position(real_point3d const *origin, short number, real_point3d *position)
{
	(void)origin; (void)number; (void)position;
	return FALSE;
}
void coop_enemies_rider_seated(long vehicle_index, long unit_index) { (void)vehicle_index; (void)unit_index; }
boolean coop_enemies_rider_unseated(long vehicle_index, long unit_index)
{
	(void)vehicle_index; (void)unit_index;
	return FALSE;
}

/* coop_scripts.c: the scripts as written. No player follows another, and
a list's players are waited for as the scripts say */
boolean coop_scripts_any_player_will_do(long object_list_index) { (void)object_list_index; return FALSE; }
void coop_scripts_gather_in_volume(short trigger_volume_index, long object_list_index)
{
	(void)trigger_volume_index; (void)object_list_index;
}
void coop_scripts_teleport_followers(long unit_index) { (void)unit_index; }
void coop_scripts_suspend_followers(long unit_index, boolean suspended) { (void)unit_index; (void)suspended; }
void coop_scripts_exit_followers(long unit_index, long vehicle_index) { (void)unit_index; (void)vehicle_index; }
void coop_scripts_board_followers(long unit_index, long vehicle_index, char const *seat_name)
{
	(void)unit_index; (void)vehicle_index; (void)seat_name;
}
/* the real one's first test, when no network co-op host is there to ask further */
boolean coop_scripts_safe_to_save(void) { return game_safe_to_save(); }
/* the real one's first line; then each follower's (none) */
void coop_scripts_player_add_equipment(long unit_index, short starting_profile_index, boolean reset_equipment)
{
	player_add_equipment(unit_index, starting_profile_index, reset_equipment);
}

/* coop_spectate.c: a dead player's camera and text in co-op: never */
boolean coop_spectating(void) { return FALSE; }
long coop_spectate_unit(short local_player_index) { (void)local_player_index; return NONE; }
void coop_spectate_camera(struct dead_camera *camera) { (void)camera; }
void coop_spectate_draw(short local_player_index) { (void)local_player_index; }
void coop_skip_vote_draw(short local_player_index) { (void)local_player_index; }


#ifdef PSP_MULTIPLAYER
/* network_distributed.c's calls, which only the multiplayer build compiles.
A co-op host never sends to this PSP (it joins only multiplayer maps), so
its co-op messages are never handled: an entry size of 0 passes the size
check and the handler drops the message. */
void network_actors_new_game(void) {}
void network_actors_host_tick(void) {}
void network_coop_new_game(void) {}
void network_coop_host_tick(void) {}
void network_coop_client_tick(void) {}
void network_coop_note_player_structure_bsp(short player_index, short structure_bsp_index)
{
	(void)player_index; (void)structure_bsp_index;
}
word network_actors_entry_size(void) { return 0; }
word network_actors_damage_entry_size(void) { return 0; }
word network_coop_presentation_entry_size(void) { return 0; }
word network_coop_event_entry_size(void) { return 0; }
word network_coop_skip_vote_entry_size(void) { return 0; }
word network_coop_device_group_entry_size(void) { return 0; }
word network_coop_device_state_entry_size(void) { return 0; }
word network_coop_object_names_entry_size(void) { return 0; }
word network_coop_object_transform_entry_size(void) { return 0; }
word network_coop_object_look_entry_size(void) { return 0; }
word network_coop_screen_effect_entry_size(void) { return 0; }
void network_actors_handle_states(void const *entries, short count) { (void)entries; (void)count; }
void network_actors_handle_damage(void const *entries, short count) { (void)entries; (void)count; }
void network_coop_handle_presentation(void const *entries, long host_time) { (void)entries; (void)host_time; }
void network_coop_handle_events(void const *entries, short count) { (void)entries; (void)count; }
void network_coop_handle_skip_vote(long machine_index, void const *entries) { (void)machine_index; (void)entries; }
void network_coop_handle_device_groups(void const *entries, short count) { (void)entries; (void)count; }
void network_coop_handle_device_states(void const *entries, short count) { (void)entries; (void)count; }
void network_coop_handle_object_names(void const *entries) { (void)entries; }
void network_coop_handle_object_transforms(void const *entries, short count) { (void)entries; (void)count; }
void network_coop_handle_object_looks(void const *entries, short count) { (void)entries; (void)count; }
void network_coop_handle_screen_effect(void const *entries, short count) { (void)entries; (void)count; }
#endif
