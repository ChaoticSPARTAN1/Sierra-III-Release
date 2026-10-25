/* psp_watch.c - instruments that watch the game's own state and log what
changes (build 113). Each is a keep word, off unless asked for, and costs
nothing while off but the test of its word.

"flipwatch": the vehicle flip prompt (b113_candidates.md #10). The sequence seen
across several builds: a vehicle flips, the player walks up, the prompt to
flip it appears, goes before he can press, and never comes back however he
walks around it. The prompt is worked out every tick from four conditions
(players.c, player_examine_nearby_vehicle): the vehicle not dead, tipped past
the game globals' flipping angle, its upending flag clear, no driver. So one
of them turns off and stays off. This logs, for each vehicle the player's
search looks at, every change in those four plus whether it is at rest and
whether the port's sleep skipped its update; the player's action, its target
and the action button; the flip itself; and the vehicle physics' upending
(the torque that rights it, at most 30 ticks, which then clears the flag).
The calls come from players.c and vehicles.c (psp_fixups.py, FIXES_113) and
the sleep from objects.c's psp_object_update. */
#include <stdio.h>
#include <string.h>

void platform_log(const char *format, ...);
int psp_keep(const char *name);
long psp_game_time_peek(void);

#define FLIPWATCH_VEHICLES 8
#define FLIPWATCH_SLEEPERS 16

struct flipwatch_vehicle
{
	long index;            /* the vehicle's object index, NONE (-1) for a free slot */
	const char *name;      /* its tag's name (tag data: lives as long as the map) */
	long last_examined;    /* game time the player's search last looked at it */
	int in_reach;          /* looked at last tick */
	int dead, tipped, upending, driver, at_rest, asleep;
};

/* the watch runs in the ticks, while the Media Engine may be drawing a frame
(geinflight): its words go with the main CPU's (psp_me.h's PSP_MAIN_SIDE), off
the ME's cache lines */
#define FLIPWATCH_MAIN_SIDE __attribute__((section(".data.psp_main_side")))
static FLIPWATCH_MAIN_SIDE int flipwatch_enabled = -1;
static FLIPWATCH_MAIN_SIDE struct flipwatch_vehicle flipwatch_vehicles[FLIPWATCH_VEHICLES];
static FLIPWATCH_MAIN_SIDE struct { long index, tick; } flipwatch_sleepers[FLIPWATCH_SLEEPERS];
static FLIPWATCH_MAIN_SIDE int flipwatch_next_sleeper = 0;
static FLIPWATCH_MAIN_SIDE long flipwatch_action_result = -2, flipwatch_action_object = -2;
static FLIPWATCH_MAIN_SIDE int flipwatch_action_bit = -1;

int psp_flipwatch_on(void)
{
	if (flipwatch_enabled < 0)
	{
		int k;

		flipwatch_enabled = psp_keep("flipwatch");
		for (k = 0; k < FLIPWATCH_VEHICLES; k++)
			flipwatch_vehicles[k].index = -1;
		for (k = 0; k < FLIPWATCH_SLEEPERS; k++)
			flipwatch_sleepers[k].index = -1;
		if (flipwatch_enabled)
			platform_log("flipwatch: on (vehicles near the player: the flip prompt's conditions, logged as they change)");
	}
	return flipwatch_enabled;
}

/* objects.c: the sleep skipped this vehicle's update this tick */
void psp_flipwatch_asleep(long object_index)
{
	int k;

	if (flipwatch_enabled <= 0)
		return;
	for (k = 0; k < FLIPWATCH_SLEEPERS && flipwatch_sleepers[k].index != object_index; k++)
		;
	if (k == FLIPWATCH_SLEEPERS)
	{
		k = flipwatch_next_sleeper;
		flipwatch_next_sleeper = (flipwatch_next_sleeper + 1) % FLIPWATCH_SLEEPERS;
		flipwatch_sleepers[k].index = object_index;
	}
	flipwatch_sleepers[k].tick = psp_game_time_peek();
}

static int flipwatch_was_asleep(long object_index, long now)
{
	int k;

	for (k = 0; k < FLIPWATCH_SLEEPERS; k++)
		if (flipwatch_sleepers[k].index == object_index)
			return flipwatch_sleepers[k].tick >= now - 1;
	return 0;
}

static struct flipwatch_vehicle *flipwatch_slot(long vehicle_index, const char *name)
{
	int k, oldest = 0;

	for (k = 0; k < FLIPWATCH_VEHICLES; k++)
		if (flipwatch_vehicles[k].index == vehicle_index)
			return &flipwatch_vehicles[k];
	for (k = 0; k < FLIPWATCH_VEHICLES; k++)
	{
		if (flipwatch_vehicles[k].index == -1)
			break;
		if (flipwatch_vehicles[k].last_examined < flipwatch_vehicles[oldest].last_examined)
			oldest = k;
	}
	if (k == FLIPWATCH_VEHICLES)
		k = oldest;
	memset(&flipwatch_vehicles[k], 0, sizeof(flipwatch_vehicles[k]));
	flipwatch_vehicles[k].index = vehicle_index;
	flipwatch_vehicles[k].name = name;
	flipwatch_vehicles[k].dead = -1; /* nothing logged yet */
	return &flipwatch_vehicles[k];
}

/* players.c, player_examine_nearby_vehicle: the player's search found this
vehicle near him this tick. up_k: the vehicle's up vector's height;
tipped_below: the prompt's limit (the flip is offered while up_k is at or
below it) */
void psp_flipwatch_examine(long vehicle_index, const char *name, int dead, float up_k, float tipped_below,
	int upending, int driver, int at_rest)
{
	struct flipwatch_vehicle *vehicle;
	long now = psp_game_time_peek();
	int tipped = up_k <= tipped_below, asleep = flipwatch_was_asleep(vehicle_index, now);

	if (flipwatch_enabled <= 0)
		return;
	vehicle = flipwatch_slot(vehicle_index, name);
	if (vehicle->dead != dead || vehicle->tipped != tipped || vehicle->upending != upending ||
		vehicle->driver != driver || vehicle->at_rest != at_rest || vehicle->asleep != asleep || !vehicle->in_reach)
	{
		platform_log("flipwatch: t=%ld %s #%08lx %s: prompt %s | dead %s, up.k %.2f (offered at or below %.2f), "
			"upending %s, driver %s, at rest %s, asleep %s",
			now, name ? name : "?", (unsigned long)vehicle_index, vehicle->in_reach ? "changed" : "in reach",
			!dead && tipped && !upending && !driver ? "OFFERED" : "not offered", dead ? "yes" : "no", up_k, tipped_below,
			upending ? "SET" : "clear", driver ? "yes" : "none", at_rest ? "yes" : "no", asleep ? "yes" : "no");
	}
	vehicle->dead = dead;
	vehicle->tipped = tipped;
	vehicle->upending = upending;
	vehicle->driver = driver;
	vehicle->at_rest = at_rest;
	vehicle->asleep = asleep;
	vehicle->in_reach = 1;
	vehicle->last_examined = now;
}

/* players.c, every tick before the player's action is handled: his action,
its object, the action button; and vehicles the search stopped finding */
void psp_flipwatch_player(long action_result, long action_object, int action_bit)
{
	long now = psp_game_time_peek();
	int k;

	if (flipwatch_enabled <= 0)
		return;
	if (action_result != flipwatch_action_result || action_object != flipwatch_action_object ||
		action_bit != flipwatch_action_bit)
		platform_log("flipwatch: t=%ld player action %ld on #%08lx, action button %s", now, action_result,
			(unsigned long)action_object, action_bit ? "DOWN" : "up");
	flipwatch_action_result = action_result;
	flipwatch_action_object = action_object;
	flipwatch_action_bit = action_bit;
	for (k = 0; k < FLIPWATCH_VEHICLES; k++)
	{
		struct flipwatch_vehicle *vehicle = &flipwatch_vehicles[k];

		if (vehicle->index != -1 && vehicle->in_reach && vehicle->last_examined < now - 1)
		{
			platform_log("flipwatch: t=%ld %s #%08lx out of reach (the search stopped finding it)", now,
				vehicle->name ? vehicle->name : "?", (unsigned long)vehicle->index);
			vehicle->in_reach = 0;
		}
	}
}

/* the vehicle just flipped, watched by the sleep (objects.c) for 60 ticks */
static FLIPWATCH_MAIN_SIDE long flipwatch_watch_index = -1, flipwatch_watch_until = 0;

long psp_flipwatch_watched(void)
{
	if (flipwatch_enabled <= 0 || flipwatch_watch_index == -1)
		return -1;
	if (psp_game_time_peek() > flipwatch_watch_until)
		flipwatch_watch_index = -1;
	return flipwatch_watch_index;
}

/* players.c, player_handle_action: the flip itself */
void psp_flipwatch_flip(long vehicle_index, int approach_type)
{
	if (flipwatch_enabled <= 0)
		return;
	platform_log("flipwatch: t=%ld FLIP on #%08lx (approach %d): upending flag set", psp_game_time_peek(),
		(unsigned long)vehicle_index, approach_type);
	flipwatch_watch_index = vehicle_index;
	flipwatch_watch_until = psp_game_time_peek() + 60;
}

/* vehicles.c, vehicle_update: the torque that rights it starts, and the flag
clears (after the torque, or at once if the vehicle isn't tipped) */
void psp_flipwatch_upending_start(long vehicle_index, float up_k)
{
	if (flipwatch_enabled > 0)
		platform_log("flipwatch: t=%ld #%08lx upending: torque starts (up.k %.2f)", psp_game_time_peek(),
			(unsigned long)vehicle_index, up_k);
}

void psp_flipwatch_upending_end(long vehicle_index, int ticks, int type, float up_k)
{
	if (flipwatch_enabled > 0)
		platform_log("flipwatch: t=%ld #%08lx upending flag cleared after %d ticks of torque (type %d, up.k %.2f)",
			psp_game_time_peek(), (unsigned long)vehicle_index, ticks, type, up_k);
}
