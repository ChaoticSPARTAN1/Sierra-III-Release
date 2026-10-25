/*
PSP_LIMITS.H

The PSP's copy of the engine's halo_port_limits.h (psp_build.py
force-includes it, and it takes that header's include guard, so the
engine's own is never read): the session limits and the netcode's layout,
as ChupathingyCE has them (its game settings record and netcode version
differ from the Xbox game's).

The Xbox game allows 16 players on at most 4 machines (up to 4 players each
on split screen). The native builds allow 128 players on up to 128
machines; split screen stays at 4 players per machine.

128 is the largest session that fits the game's existing records: player,
machine and team indices are stored in signed chars (0..127 with NONE), and
a finishing place in 7 bits.
*/

#ifndef __HALO_PORT_LIMITS_H
#define __HALO_PORT_LIMITS_H

/* a PSP value, or the other ports' (psp_fixups.py) */
#ifdef HALO_PSP
#define HALO_PSP_OR(psp, other) (psp)
#else
#define HALO_PSP_OR(psp, other) (other)
#endif
/* a 32-bit value at an address that may not be a multiple of 4. The PSP's
CPU faults on unaligned word loads and stores (x86 doesn't care, and
PPSSPP's JIT forgives them; its interpreter reports them); a packed
struct makes GCC use the unaligned-safe lwl/lwr and swl/swr pairs
(psp_fixups.py) */
#ifdef HALO_PSP
struct psp_unaligned_ulong { unsigned long value; } __attribute__((packed));
#define PSP_UNALIGNED_ULONG(address) (((struct psp_unaligned_ulong *)(address))->value)
#else
#define PSP_UNALIGNED_ULONG(address) (*(unsigned long *)(address))
#endif
/* the same for a short and a 64-bit value: a network packet's values sit at
any byte offset (data_encoding.c, psp_fixups.py) */
#ifdef HALO_PSP
struct psp_unaligned_short { short value; } __attribute__((packed));
struct psp_unaligned_int64 { long long value; } __attribute__((packed));
#define PSP_UNALIGNED_SHORT(address) (((struct psp_unaligned_short *)(address))->value)
#define PSP_UNALIGNED_INT64(address) (((struct psp_unaligned_int64 *)(address))->value)
#else
#define PSP_UNALIGNED_SHORT(address) (*(short *)(address))
#define PSP_UNALIGNED_INT64(address) (*(long long *)(address))
#endif
/* build 116: the same for a float, and for three in a row (a point or a
vector) copied to an aligned place. Bungie's compressed animations can hold
their float tables at an address 2 past a multiple of 4: b115 faulted on a10's
opening in animation_get_keyframe_translation (psp_fixups.py, FIXES_116) */
#ifdef HALO_PSP
struct psp_unaligned_real { float value; } __attribute__((packed));
#define PSP_UNALIGNED_REAL(address) (((const struct psp_unaligned_real *)(address))->value)
#else
#define PSP_UNALIGNED_REAL(address) (*(const float *)(address))
#endif
#define PSP_UNALIGNED_REAL3(to, from) do { \
		const char *psp_from_ = (const char *)(from); \
		float *psp_to_ = (float *)(to); \
		psp_to_[0] = PSP_UNALIGNED_REAL(psp_from_); \
		psp_to_[1] = PSP_UNALIGNED_REAL(psp_from_ + 4); \
		psp_to_[2] = PSP_UNALIGNED_REAL(psp_from_ + 8); \
	} while (0)
/* build 116: a see-through shader's fade, packed with its blend function
(shared with psp_ge.c) */
#include "psp/include/psp_seethrough.h"
/* the AI debug records (ai_debug.c) are 6.6 MB for 256 actors and 3.7 MB of
path storage; gameplay only writes them, so on the PSP every actor shares
one record (psp_fixups.py) */
#ifdef HALO_PSP
#define PSP_DEBUG_INDEX(index) (0 * (index))
#define PSP_DEBUG_COUNT(count) 1
#else
#define PSP_DEBUG_INDEX(index) (index)
#define PSP_DEBUG_COUNT(count) (count)
#endif

/* ---------- session limits

The campaign build keeps the Xbox's 16 players on 4 machines: it has one
player, and these size records it still carries (halo-psp-feasibility.md,
2026-10-01). The multiplayer build (psp_build.py's PSP_MULTIPLAYER) takes
the native builds' 128 and 128: the game settings record goes over the wire
as raw bytes, its size checked, so only the same limits can join a PC's
game or let a PC join a PSP's (docs/multiplayer_plan.md, section 3). */
#ifdef PSP_MULTIPLAYER
#define HALO_PORT_MAXIMUM_NETWORK_PLAYERS 128
#define HALO_PORT_MAXIMUM_NETWORK_MACHINES 128
#else
#define HALO_PORT_MAXIMUM_NETWORK_PLAYERS 16
#define HALO_PORT_MAXIMUM_NETWORK_MACHINES 4
#endif

/* ... a game the PSP hosts: at most this many machines (its own included)
and players. The host's own records (its machines' slots, what each late
joiner was started with, where every player was to check what clients
report) are sized for this, not for the session limits above, which a PSP
only meets as a client (psp_fixups.py, FIXES_MULTIPLAYER) */
#define PSP_HOST_MAXIMUM_NETWORK_PLAYERS 16

/* ... and a multiplayer build that hosts nothing (2026-10-17: hosting
and ad hoc come back after the campaign's performance targets, as co-op's
prerequisite; PSP_MP_HOST=1, or PSP_MP_COOP=1, builds them back in). It
joins games only, so the host's records are one slot each
(psp_fixups.py, FIXES_MPCLIENT) */
#if defined(PSP_MULTIPLAYER) && !defined(PSP_MP_HOST)
#define PSP_MP_CLIENT_ONLY 1
#define PSP_HOST_MAXIMUM_NETWORK_MACHINES 1
#else
#define PSP_HOST_MAXIMUM_NETWORK_MACHINES 16
#endif

/* a host polls its listening socket and one socket per machine; the Xbox's
Winsock headers default to 64 (the prefix headers define FD_SETSIZE from
this before any of them is read) */
#define HALO_PORT_FD_SETSIZE 256

/* ---------- memory capacity for these limits (game state and pools) */

/* capacity: Xbox values (psp_limits.h) */
#include "psp_capacity.h"

/* ---------- struct network_game layout

The game settings record (struct network_game) is declared separately in
several networking and interface units; its layout follows from the limits.
Every copy checks its offsets against these values. The Xbox values (4
machines, 16 players) are 0x226 and 0x434. */

#define HALO_PORT_NETWORK_MACHINE_SIZE 0x44
#define HALO_PORT_NETWORK_PLAYER_SIZE 0x20
#define HALO_PORT_NETWORK_GAME_MACHINES_OFFSET 0x114
#define HALO_PORT_NETWORK_GAME_PLAYER_COUNT_OFFSET \
	(HALO_PORT_NETWORK_GAME_MACHINES_OFFSET + HALO_PORT_MAXIMUM_NETWORK_MACHINES * HALO_PORT_NETWORK_MACHINE_SIZE)
#define HALO_PORT_NETWORK_GAME_PLAYERS_OFFSET (HALO_PORT_NETWORK_GAME_PLAYER_COUNT_OFFSET + 2)
#define HALO_PORT_NETWORK_GAME_PLAYERS_END \
	(HALO_PORT_NETWORK_GAME_PLAYERS_OFFSET + HALO_PORT_MAXIMUM_NETWORK_PLAYERS * HALO_PORT_NETWORK_PLAYER_SIZE)
#define HALO_PORT_NETWORK_GAME_RANDOM_SEED_OFFSET (HALO_PORT_NETWORK_GAME_PLAYERS_END + 2)
/* (ChupathingyCE: then the gametype's PC options, struct game_variant_options:
0x1C bytes) */
#define HALO_PORT_NETWORK_GAME_VARIANT_OPTIONS_OFFSET (HALO_PORT_NETWORK_GAME_PLAYERS_END + 0xA)
#define HALO_PORT_NETWORK_GAME_LOCAL_DATA_OFFSET (HALO_PORT_NETWORK_GAME_PLAYERS_END + 0x26)
#define HALO_PORT_NETWORK_GAME_SIZE (HALO_PORT_NETWORK_GAME_PLAYERS_END + 0x2A)

/* ---------- system link protocol

The native builds' messages differ from the Xbox game's (longer arrays, the
game settings record in fragments), so they search for games with their own
protocol version and never see the Xbox game's, or it theirs. */

#define HALO_PORT_NETWORK_GAME_MESSAGE_VERSION 2

/* The native builds' network code has a version of its own (an unsigned
16-bit number): machines of different versions cannot play together, and a
client does not join a host of another version, but tells the player which
is newer (network_client_manager.c). A host advertises it, with its netcode,
in its game's advertisement's reserved bytes (network_server_message_handler.c),
which hosts built before there was a version send as zeros: version 0.
Raise it with any change to what the machines send each other. The PSP's
numbers are the engine pin's (ChupathingyCE v0.7.5d: 29, joining 11 to 30):
move them with the pin, since the PSP compiles that pin's netcode */
#define HALO_PORT_NETWORK_VERSION 29
/* ... the versions whose hosts a client joins (ChupathingyCE: delta.h's
table, DELTA_LEGACY_VERSIONS); code asks the functions, not the numbers */
#define HALO_PORT_NETWORK_VERSION_MINIMUM 11
#define HALO_PORT_NETWORK_VERSION_MAXIMUM 30
int delta_legacy_announce(void);
int delta_legacy_minimum(void);
int delta_legacy_maximum(void);
/* ... the advertisement's reserved bytes: the version (a little-endian word),
then flags */
#define HALO_PORT_ADVERTISED_VERSION_OFFSET 0
#define HALO_PORT_ADVERTISED_FLAGS_OFFSET 2
/* ... the host plays the distributed netcode (always, since the lockstep
netcode was removed; hosts of version 4 built before then may not) */
#define HALO_PORT_ADVERTISED_DISTRIBUTED_FLAG 0x01
/* ... the game is under way (loading, playing or over), not in its lobby
(ChupathingyCE) */
#define HALO_PORT_ADVERTISED_IN_PROGRESS_FLAG 0x02

/* a message header's 12-bit length allows messages of up to 0xFFF bytes,
header included; the per-tick update of 128 players is 3,857 */
#define HALO_PORT_MAXIMUM_NETWORK_MESSAGE_SIZE 0x1000

/* the packet codec's limit on a decoded or encoded packet; the per-tick
update of 128 players decodes to 0x1010 bytes */
#define HALO_PORT_NETWORK_PACKET_SIZE 0x1100

/* the game settings record (HALO_PORT_NETWORK_GAME_SIZE, 13,092 bytes at 128
machines and players) does not fit one message; it is sent in pieces of
this many bytes (4 pieces), each 3,594 bytes on the wire */
#define HALO_PORT_NETWORK_GAME_SETTINGS_FRAGMENT_SIZE 0xE00

#endif /* __HALO_PORT_LIMITS_H */
