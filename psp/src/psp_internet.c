/* psp_internet.c - internet play's platform answers on the PSP (mpb1)

The multiplayer build compiles the engine's p2p layer as it is
(psp_build.py's INTERNET_UNITS): the public games' listings
(p2p_lobby.c, through MQTT brokers, p2p_signal.c) and the invite tunnel
(p2p.c). Sockets and names come from psp_net.c. This file answers the rest:

- random bytes: internet play makes its keys from posix_random_bytes (an
  X25519 key a run, nonces for every signalling message). The campaign
  build's answer (psp_platform.c) is a Mersenne Twister seeded from the
  32-bit clock, fine for gameplay and no good for keys: anyone who saw the
  public key could try every seed. Here the bytes are ChaCha20's keystream
  under a key hashed (BLAKE2b, Monocypher's, which the p2p layer already
  links) from this PSP's OpenPSID, its Wi-Fi MAC address, the clock since
  boot and the real-time clock, both in microseconds; each call hashes the
  clock in again. The OpenPSID is never sent, so someone who sees the
  traffic can't rebuild the key from what they see. Still not a hardware
  generator [U]: KIRK's (the security chip's) would be, but it is reached
  only from kernel mode
- Discord: the desktop shows a hosted game in Discord; the PSP has none
- whether a file is on the Memory Stick, for the ONLINE GAMES screen's map
  check (psp_online.c: a game unit, whose stdio goes through the engine's
  file layer, which found none of the maps)
- the tunnel thread's priority (psp_internet_thread_priority) */

#include <pspkernel.h>
#include <pspiofilemgr.h>
#include <pspopenpsid.h>
#include <psprtc.h>
#include <pspwlan.h>
#include <pthread.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "monocypher.h"

/* posix.h's types on a 32-bit system (port/linux/src/posix.h) */
typedef unsigned long posix_ulong;

/* ---------- random bytes */

static struct
{
	pthread_mutex_t lock;
	int seeded;
	unsigned char key[32];
	unsigned long long counter;
} random_state = { PTHREAD_MUTEX_INITIALIZER, 0, { 0 }, 0 };

/* the key, hashed from what this PSP alone knows and the clocks (the
previous key in it too once seeded, so each call moves it on) */
static void random_mix(void)
{
	struct
	{
		unsigned char previous[32];
		PspOpenPSID psid;
		unsigned char mac[8];
		SceInt64 since_boot;
		u64 real_time;
		unsigned int stack_address;
	} pool;

	memset(&pool, 0, sizeof(pool));
	memcpy(pool.previous, random_state.key, sizeof(pool.previous));
	if (!random_state.seeded)
	{
		sceOpenPSIDGetOpenPSID(&pool.psid);
		sceWlanGetEtherAddr(pool.mac);
	}
	pool.since_boot = sceKernelGetSystemTimeWide();
	sceRtcGetCurrentTick(&pool.real_time);
	pool.stack_address = (unsigned int)(unsigned long)&pool;
	crypto_blake2b(random_state.key, sizeof(random_state.key), (const unsigned char *)&pool, sizeof(pool));
	crypto_wipe(&pool, sizeof(pool));
	random_state.seeded = 1;
}

void posix_random_bytes(void *buffer, posix_ulong size)
{
	static const unsigned char nonce[8] = { 'S', 'i', 'e', 'r', 'r', 'a', '-', '3' };

	pthread_mutex_lock(&random_state.lock);
	random_mix();
	memset(buffer, 0, size);
	random_state.counter = crypto_chacha20_djb(buffer, buffer, size, random_state.key, nonce, random_state.counter);
	pthread_mutex_unlock(&random_state.lock);
}

/* ---------- no Discord (p2p_discord.c) */

void p2p_discord_update(void)
{
}

void p2p_discord_user(char *id, int id_size, char *name, int name_size)
{
	if (id && id_size > 0)
		id[0] = 0;
	if (name && name_size > 0)
		name[0] = 0;
}

void p2p_discord_set_hosting(const char *secret, int player_count, int maximum_player_count)
{
	(void)secret; (void)player_count; (void)maximum_player_count;
}

/* (the desktop keeps a Discord name's allowed characters; with no Discord,
the text as given, cut to fit) */
void p2p_discord_sanitize(char *destination, int size, const char *source, int name)
{
	(void)name;
	if (destination && size > 0)
		snprintf(destination, (size_t)size, "%s", source ? source : "");
}

/* ---------- the tunnel's thread

p2p.c does all of internet play's network work on one thread: the tunnel,
its KCP streams, the signalling and the lobby. It starts at 0x5c, far below
the game thread's 0x20, and the PSP runs a thread only while every higher
one waits.
- In a game the PSP can't keep up with, the game thread runs one to two and
  a half seconds of catch-up ticks without waiting, and the tunnel can go
  that long without running: the host's messages stopped within seconds of
  the join, and the client gave up (docs/milestone_first_public_game.md). So
  in a game the thread runs at 0x1f: above the game thread, below the
  vertical blank's 0x1e. It waits in select() between packets and KCP's
  clock, so there it takes only what it needs.
- In the menus it stays at 0x5c. While ONLINE GAMES browses, the thread
  checks thousands of signed listings; above the game thread it kept the
  menus' start-up thread (saved profiles and playlists, slow on the stick)
  from ever finishing, and the menus never ran the screen (a hardware
  run, its A/B with tunnelslow).
"tunnelslow" keeps it at 0x5c in games too, for comparing */
#define PSP_TUNNEL_PRIORITY 0x1f
int psp_keep(const char *name);
void platform_log(const char *format, ...);

static SceUID tunnel_thread = -1;
static int tunnel_priority = -1;

/* ---------- KCP's memory

KCP (the tunnel's streams) allocates a block for every segment it holds,
from the heap. A map's load takes the heap to its top while the host keeps
sending, and ikcp_input wrote through the NULL it got back: a bus error on
this thread (two emulator runs). So KCP asks the heap first and,
when the heap has nothing, a reserve of fixed blocks outside the heap, taken
as internet play starts from user RAM the heap never uses (psp_memory.c
keeps OUTSIDE_RESERVE of it free; this takes well under half of what is left
then). ikcp.c's NULL checks (psp_fixups.py #nomemory) cover what is left over.
Keep word kcpreserveN: the reserve in KB (kcpreserve0: none) */
int psp_keep_number(const char *name, int otherwise);

enum
{
	/* a segment's header (56 bytes) and the most one carries (KCP_MTU 1,200
	less its 24-byte header), rounded up */
	KCP_BLOCK_BYTES = 1280,
	KCP_RESERVE_DEFAULT_KB = 64,
	/* psp_memory.c's OUTSIDE_RESERVE: threads' stacks and the like */
	KCP_RESERVE_LEAVES_FREE = 160 * 1024,
};

static char *kcp_reserve_start, *kcp_reserve_end;
static void *kcp_reserve_free;
static int kcp_reserve_used, kcp_reserve_peak, kcp_reserve_logged;
static pthread_mutex_t kcp_reserve_lock = PTHREAD_MUTEX_INITIALIZER;

static void *kcp_malloc(size_t size)
{
	void *block = malloc(size);

	if (block)
		return block;
	pthread_mutex_lock(&kcp_reserve_lock);
	if (size <= KCP_BLOCK_BYTES && kcp_reserve_free)
	{
		block = kcp_reserve_free;
		kcp_reserve_free = *(void **)block;
		if (++kcp_reserve_used > kcp_reserve_peak)
			kcp_reserve_peak = kcp_reserve_used;
	}
	pthread_mutex_unlock(&kcp_reserve_lock);
	if ((!block || kcp_reserve_used == 1) && kcp_reserve_logged < 16)
	{
		kcp_reserve_logged++;
		platform_log("internet play: the heap is full: KCP's %u bytes %s (reserve: %d of its blocks in use, %d at most)",
			(unsigned)size, block ? "from its reserve" : size > KCP_BLOCK_BYTES ? "too many for a reserve block: refused" :
			"refused: the reserve is used up", kcp_reserve_used, kcp_reserve_peak);
	}
	return block;
}

static void kcp_free(void *block)
{
	if ((char *)block >= kcp_reserve_start && (char *)block < kcp_reserve_end)
	{
		pthread_mutex_lock(&kcp_reserve_lock);
		*(void **)block = kcp_reserve_free;
		kcp_reserve_free = block;
		kcp_reserve_used--;
		pthread_mutex_unlock(&kcp_reserve_lock);
		return;
	}
	free(block);
}

/* before the tunnel makes its first stream (this thread's start) */
static void kcp_reserve_begin(void)
{
	void ikcp_allocator(void *(*new_malloc)(size_t), void (*new_free)(void *));
	int count = psp_keep_number("kcpreserve", KCP_RESERVE_DEFAULT_KB) * 1024 / KCP_BLOCK_BYTES, index;
	unsigned long bytes = (unsigned long)count * KCP_BLOCK_BYTES;
	SceUID block;

	ikcp_allocator(kcp_malloc, kcp_free);
	/* (once: internet play starts its thread again after a failed join) */
	if (count <= 0 || kcp_reserve_start)
		return;
	if ((unsigned long)sceKernelTotalFreeMemSize() < bytes + 64 + KCP_RESERVE_LEAVES_FREE ||
		(unsigned long)sceKernelMaxFreeMemSize() < bytes + 64 ||
		(block = sceKernelAllocPartitionMemory(2, "halo kcp reserve", PSP_SMEM_High, bytes + 64, NULL)) < 0)
	{
		platform_log("internet play: no room for KCP's reserve of %lu KB outside the heap (%d KB free there)",
			bytes / 1024, sceKernelTotalFreeMemSize() / 1024);
		return;
	}
	kcp_reserve_start = (char *)(((unsigned long)sceKernelGetBlockHeadAddr(block) + 63) & ~63UL);
	kcp_reserve_end = kcp_reserve_start + bytes;
	for (index = count - 1; index >= 0; index--)
	{
		void **slot = (void **)(kcp_reserve_start + (unsigned long)index * KCP_BLOCK_BYTES);

		*slot = kcp_reserve_free;
		kcp_reserve_free = slot;
	}
	platform_log("internet play: KCP's reserve, %d blocks (%lu KB) outside the heap; %d KB still free there",
		count, bytes / 1024, sceKernelTotalFreeMemSize() / 1024);
}

/* the thread, as it starts (p2p.c's #priority fixup) */
void psp_internet_thread_priority(void)
{
	SceKernelThreadInfo info;

	kcp_reserve_begin();
	memset(&info, 0, sizeof(info));
	info.size = sizeof(info);
	sceKernelReferThreadStatus(0, &info);
	tunnel_thread = sceKernelGetThreadId();
	tunnel_priority = info.currentPriority;
	platform_log("internet play's thread (%s): priority 0x%x; 0x%x in a game%s", info.name, tunnel_priority,
		PSP_TUNNEL_PRIORITY, psp_keep("tunnelslow") ? " (not: tunnelslow)" : "");
}

/* the thread's priority now, for the ONLINE GAMES test's line; -1 before it starts */
int psp_internet_thread_priority_now(void)
{
	SceKernelThreadInfo info;

	if (tunnel_thread < 0)
		return -1;
	memset(&info, 0, sizeof(info));
	info.size = sizeof(info);
	return sceKernelReferThreadStatus(tunnel_thread, &info) < 0 ? -1 : info.currentPriority;
}

/* in a game (1: the screen joined one) or browsing the menus (0) */
void psp_internet_tunnel_priority(int in_game)
{
	int priority = in_game && !psp_keep("tunnelslow") ? PSP_TUNNEL_PRIORITY : tunnel_priority;

	if (tunnel_thread < 0 || priority < 0)
		return;
	if (sceKernelChangeThreadPriority(tunnel_thread, priority) < 0)
		platform_log("internet play's thread: the change to 0x%x was refused", priority);
}

/* ---------- the Memory Stick */

int psp_file_exists(const char *path)
{
	SceIoStat stat;

	return sceIoGetstat(path, &stat) >= 0 && FIO_S_ISREG(stat.st_mode);
}
