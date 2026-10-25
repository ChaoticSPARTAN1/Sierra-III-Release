/* psp_net.c - the multiplayer build's network: Wi-Fi, and the sockets xnet.c uses

ChupathingyCE's xnet.c (the Xbox's Winsock and XNet, over BSD sockets)
reaches the system through posix.h's posix_socket_* calls, which Linux
answers in posix_net.c. This file is the PSP's answer, built only into the
multiplayer build (psp_build.py's PSP_MULTIPLAYER); the campaign build keeps
psp_platform.c's "no network". Each call returns -1 on failure with the
Winsock error code from posix_socket_last_error(), as posix.h says.

What differs from Linux:
- The firmware's network stack isn't running until a game loads it and
  joins an access point. Its modules are loaded and the stack started at
  start-up (psp_net_start), as soon as the game state and the tag window
  have their places (psp_memory.c): the modules take user RAM outside the
  heap, so they go before the caches that also live there, and they can't
  land where those two fixed blocks must go. The game's transport, which
  starts with the game, asks for this machine's address (xnet.c's
  XNetGetTitleXnAddr, unless network.address names one), and that joins an
  access point, blocking for the few seconds it takes (the emulator: 6 s);
  otherwise the first socket the game opens does. It then stays joined
  until the game quits: the game closes and opens its sockets again between
  the lobby and a game, and joining again would cost seconds each time.
- The access point is one of the network connections saved in the PSP's
  settings: the first, or keep.txt's "netprofileN". The PSP-1000's Wi-Fi is
  802.11b, and takes WEP and WPA; WPA2 only through ARK-4's wpa2 setting
  (multiplayer_recon.md, section 1.4; build_mp1.md, Test 0).
- The stack takes every socket's buffers from one pool, sceNetInit's (the
  engine's 1 MB a socket is a PC's; see NET_POOL_KB below).
- The PSP's sockaddr is BSD's: a length byte, then a one-byte family. The
  game's is Winsock's: a two-byte family. The port and the address sit at
  the same offsets in both, so an address is converted by its first two
  bytes, both ways (as posix_net.c does for macOS).
- libcglue's traps (multiplayer_recon.md, section 1.3): ioctl(FIONBIO) does
  nothing, so non-blocking goes through fcntl's O_NONBLOCK; select() is a
  polling loop (the game asks with no wait); there's no getifaddrs, so this
  machine's address comes from the access point's connection (apctl).
- The socket options the game sets (SOL_SOCKET's REUSEADDR, BROADCAST,
  SNDBUF, RCVBUF; TCP_NODELAY) have Winsock's numbers on the PSP too, so
  they pass through as they are.

Keep word "adhoc" (mp5): no access point. The PSPs join one ad hoc group and
reach each other directly; every call below goes to psp_net_adhoc.c, which
answers it with the firmware's ad hoc sockets. Chosen once, at start-up: the
two need different modules, and only one set is loaded.

The last error is kept per thread, as Winsock's is: the game's thread and
internet play's (p2p.c) both make socket calls, and one shared variable let
one thread's select clear the WSAEWOULDBLOCK the other's connect had just
set, which internet play takes as a failed connection. The PSP's toolchain
has no thread-local storage worth trusting on the Allegrex, so each thread
gets a slot by its thread ID (error_slot). */

#include <pspkernel.h>
#include <pspnet.h>
#include <pspnet_apctl.h>
#include <pspnet_inet.h>
#include <pspnet_resolver.h>
#include <psputility.h>
#include <pspwlan.h>

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdio.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

#include "psp_net.h"

/* (psp_chupa_platform.c: an address as the log may show it) */
const char *log_address_ipv4(unsigned long network_address, unsigned short network_port, char *text, int size);

/* posix.h's types on a 32-bit system (port/linux/src/posix.h) */
typedef long posix_long;
typedef unsigned long posix_ulong;

enum
{
	/* the game's thread, internet play's, and a few spare */
	ERROR_SLOTS = 8,
};

static struct
{
	SceUID thread;
	int error;
} error_slots[ERROR_SLOTS];
static pthread_mutex_t error_slots_lock = PTHREAD_MUTEX_INITIALIZER;

/* the calling thread's last error. A thread past the slots shares the last
slot, which is only as wrong as the one shared variable was */
static int *error_slot(void)
{
	SceUID thread = sceKernelGetThreadId();
	int index;

	for (index = 0; index < ERROR_SLOTS; index++)
	{
		if (error_slots[index].thread == thread)
			return &error_slots[index].error;
	}
	/* a thread's first call: a slot claimed for good, under a lock (the
	Allegrex has no atomic compare-and-swap) */
	pthread_mutex_lock(&error_slots_lock);
	for (index = 0; index < ERROR_SLOTS - 1 && error_slots[index].thread; index++)
		;
	if (index < ERROR_SLOTS - 1)
		error_slots[index].thread = thread;
	pthread_mutex_unlock(&error_slots_lock);
	return &error_slots[index].error;
}
#define last_error (*error_slot())

/* keep word "adhoc": every call goes to psp_net_adhoc.c (decided at
start-up, psp_net_start). Ad hoc play needs a PSP that hosts, so a build
that hosts nothing (no PSP_MP_HOST: psp_limits.h's PSP_MP_CLIENT_ONLY) has
none, and the linker drops psp_net_adhoc.c's code */
#ifdef PSP_MP_HOST
static int adhoc_mode;
#else
enum { adhoc_mode = 0 };
#endif

/* an ad hoc call's answer (minus a Winsock code on failure) as this file's */
static int adhoc_result(int result)
{
	if (result < 0)
	{
		last_error = -result;
		return -1;
	}
	last_error = 0;
	return result;
}

/* a failed call's error code. libcglue copies the firmware's error into
newlib's errno, and on the PSP that is one variable for every thread: the
tunnel's thread, above the game thread, writes it on every accept with
nothing waiting (every ~10 ms in a game), so a read that would block could
come back here as errno 0 or 2, which the game takes as its connection
broken ("_transport_error_endpoint_io"). The firmware's own code is asked
again right after the call instead (keep word libcerrno: errno as before),
and the first 64 calls where the two differ are logged, accept's left out:
its errno says 2 where the firmware says EAGAIN on every pass, which filled
the 64 lines before a read could show (two emulator runs) */
static int call_error(const char *call)
{
	static int keep = -1, differ_logged;
	int library = errno, firmware;

	if (keep < 0)
		keep = psp_keep("libcerrno");
	if (keep)
		return library;
	firmware = sceNetInetGetErrno();
	if (!firmware)
		return library;
	if (firmware != library && differ_logged < 64 && strcmp(call, "posix_socket_accept"))
	{
		SceKernelThreadInfo info;

		differ_logged++;
		memset(&info, 0, sizeof(info));
		info.size = sizeof(info);
		sceKernelReferThreadStatus(0, &info);
		platform_log("network: %s failed: errno %d, the firmware's code %d (taken); thread %s, %u ms",
			call, library, firmware, info.name, sceKernelGetSystemTimeLow() / 1000);
	}
	return firmware;
}

/* the error of a failed call as Winsock's code; -1. A code this doesn't
know is logged each time, the first 32, with the call and the thread (its
numbering is the firmware's, which libcglue passes through as it is) */
static int fail_in(const char *call)
{
	static int unknown_logged;
	int code = call_error(call);

	switch (code)
	{
	case EINTR: last_error = WSAEINTR; break;
	case EINVAL: last_error = WSAEINVAL; break;
	case EBADF: last_error = WSAEBADF; break;
	case EACCES: case EPERM: last_error = WSAEACCES; break;
	case EFAULT: last_error = WSAEFAULT; break;
	case EMFILE: case ENFILE: last_error = WSAEMFILE; break;
	case EAGAIN: last_error = WSAEWOULDBLOCK; break;
	case EINPROGRESS: last_error = WSAEINPROGRESS; break;
	case EALREADY: last_error = WSAEALREADY; break;
	case ENOTSOCK: last_error = WSAENOTSOCK; break;
	case EDESTADDRREQ: last_error = WSAEDESTADDRREQ; break;
	case EMSGSIZE: last_error = WSAEMSGSIZE; break;
	case EPROTOTYPE: last_error = WSAEPROTOTYPE; break;
	case ENOPROTOOPT: last_error = WSAENOPROTOOPT; break;
	case EPROTONOSUPPORT: last_error = WSAEPROTONOSUPPORT; break;
	case EOPNOTSUPP: last_error = WSAEOPNOTSUPP; break;
	case EAFNOSUPPORT: last_error = WSAEAFNOSUPPORT; break;
	case EADDRINUSE: last_error = WSAEADDRINUSE; break;
	case EADDRNOTAVAIL: last_error = WSAEADDRNOTAVAIL; break;
	case ENETDOWN: last_error = WSAENETDOWN; break;
	case ENETUNREACH: last_error = WSAENETUNREACH; break;
	case ENETRESET: last_error = WSAENETRESET; break;
	case ECONNABORTED: last_error = WSAECONNABORTED; break;
	case ECONNRESET: case EPIPE: last_error = WSAECONNRESET; break;
#ifdef ESHUTDOWN /* (newlib names it only with its Linux extensions) */
	case ESHUTDOWN: last_error = WSAESHUTDOWN; break;
#endif
	case EHOSTDOWN: last_error = WSAEHOSTDOWN; break;
	case ENOBUFS: case ENOMEM: last_error = WSAENOBUFS; break;
	case EISCONN: last_error = WSAEISCONN; break;
	case ENOTCONN: last_error = WSAENOTCONN; break;
	case ETIMEDOUT: last_error = WSAETIMEDOUT; break;
	case ECONNREFUSED: last_error = WSAECONNREFUSED; break;
	case EHOSTUNREACH: last_error = WSAEHOSTUNREACH; break;
	default:
		last_error = WSAEINVAL;
		{
			int error = code;

			if (unknown_logged < 32)
			{
				SceKernelThreadInfo info;

				unknown_logged++;
				memset(&info, 0, sizeof(info));
				info.size = sizeof(info);
				sceKernelReferThreadStatus(0, &info);
				platform_log("network: %s failed with errno %d, which this doesn't know (taken as WSAEINVAL); thread %s, %u ms",
					call, error, info.name, sceKernelGetSystemTimeLow() / 1000);
			}
		}
		break;
	}
	return -1;
}

static int succeed_in(int result, const char *call)
{
	if (result < 0)
		return fail_in(call);
	last_error = 0;
	return result;
}

/* each call's name for fail_in's log */
#define fail() fail_in(__func__)
#define succeed(result) succeed_in((result), __func__)

/* ---------- Wi-Fi

Following pspsdk's net/simple sample, as the C&C port's net_psp.cpp does:
the firmware's network modules are loaded (COMMON, then INET) and the stack
is started (sceNetInit, sceNetInetInit, sceNetApctlInit), once, at
start-up (psp_net_start); later, with the WLAN switch on,
sceNetApctlConnect joins a saved connection, whose state is polled until the
PSP has an address (wifi_join). A failed join leaves the stack running and
is tried again later; a stack that didn't start isn't (its modules' RAM may
since have gone to the caches). */

enum
{
	/* mp3: the stack's buffer pool (sceNetInit's), and the most one socket
	may hold, in KB. Every socket's buffers come from that one pool, and the
	engine asks 1 MB a socket (transport_endpoint_winsock.c, sized for a
	PC). mp2's Test 1 on the PSP: the first socket asked was refused
	(WSAENOBUFS), and a few seconds into the game, after a frame that spent
	3 s reading the Memory Stick, nothing was sent or received again: a
	socket holding what arrived meanwhile can empty a 128 KB pool, and then
	the stack has no buffer for anything [U]. Capped, a full socket drops
	what arrives, as UDP may. Keep words netpoolN and netbufN (KB; netbuf0:
	each socket as the engine asks) */
	NET_POOL_KB = 256,
	SOCKET_BUFFER_KB = 32,
	/* scanning, authenticating, then DHCP: past this the access point is
	out of range or refusing the PSP */
	JOIN_TIMEOUT_US = 20 * 1000 * 1000,
	/* after a failed join, the sockets asked for meanwhile fail at once
	(each join can take its whole timeout) */
	JOIN_RETRY_US = 10 * 1000 * 1000,
	/* the stack's own two threads (sceNetInit's callout and netintr: its
	timers, and what moves arrived packets from the driver into the
	sockets): above the game thread (0x20) and internet play's tunnel
	(0x1f in a game), under the cache file thread (0x1c). The PSP shares no
	time between threads of one priority, so at the game thread's own
	priority they waited out every catch-up frame (0.5-2.5 s in a public
	game): arrived packets overflowed the driver's queue, and sent ones left
	in bursts (build_mpb4.md). Their work is short. Keep word netprioN
	(decimal; netprio32: the game thread's) */
	NET_THREAD_PRIORITY = 0x1d,
};

static struct
{
	int common_loaded, inet_loaded, net_started, inet_started, apctl_started;
	/* psp_net_start has run (started or not); and "not started" logged by a join */
	int start_tried, start_failure_logged;
	int connected;
	/* when the last join failed (sceKernelGetSystemTimeLow); 0: none */
	unsigned int failed_at;
	/* a join has failed this run, so internet play may be off (wifi_join) */
	int join_failed;
	/* this machine's address and its network's broadcast address, network
	byte order */
	unsigned long address, broadcast;
} wifi;

/* the firmware's DNS resolver (posix_resolve_ipv4): started once, at the
first name looked up */
static pthread_once_t resolver_once = PTHREAD_ONCE_INIT;
static int resolver_result = -1;

static const char *apctl_state_name(int state)
{
	switch (state)
	{
	case PSP_NET_APCTL_STATE_DISCONNECTED: return "disconnected";
	case PSP_NET_APCTL_STATE_SCANNING: return "scanning";
	case PSP_NET_APCTL_STATE_JOINING: return "joining";
	case PSP_NET_APCTL_STATE_GETTING_IP: return "getting an address";
	case PSP_NET_APCTL_STATE_GOT_IP: return "connected";
	case PSP_NET_APCTL_STATE_EAP_AUTH: return "authenticating";
	case PSP_NET_APCTL_STATE_KEY_EXCHANGE: return "exchanging keys";
	default: return "unknown";
	}
}

/* the access point left (or a join that didn't finish given up), the stack
kept */
static void wifi_leave(void)
{
	int state = PSP_NET_APCTL_STATE_DISCONNECTED;

	if (wifi.apctl_started && sceNetApctlGetState(&state) >= 0 && state != PSP_NET_APCTL_STATE_DISCONNECTED)
	{
		unsigned int start = sceKernelGetSystemTimeLow();

		/* (the disconnection given a moment to finish) */
		sceNetApctlDisconnect();
		while (sceNetApctlGetState(&state) >= 0 && state != PSP_NET_APCTL_STATE_DISCONNECTED &&
			sceKernelGetSystemTimeLow() - start < 1000 * 1000)
		{
			sceKernelDelayThread(10 * 1000);
		}
	}
	if (wifi.connected)
		platform_log("network: disconnected");
	wifi.connected = 0;
	wifi.address = wifi.broadcast = 0;
}

/* the stack stopped and the modules unloaded (a start that failed part way) */
static void wifi_stop(void)
{
	wifi_leave();
	if (resolver_result >= 0)
		sceNetResolverTerm();
	resolver_result = -1;
	if (wifi.apctl_started)
		sceNetApctlTerm();
	if (wifi.inet_started)
		sceNetInetTerm();
	if (wifi.net_started)
		sceNetTerm();
	if (wifi.inet_loaded)
		sceUtilityUnloadNetModule(PSP_NET_MODULE_INET);
	if (wifi.common_loaded)
		sceUtilityUnloadNetModule(PSP_NET_MODULE_COMMON);
	wifi.apctl_started = wifi.inet_started = wifi.net_started = wifi.inet_loaded = wifi.common_loaded = 0;
}

/* polls the connection until it has an address (0), fails or times out */
static int wifi_wait_for_address(void)
{
	unsigned int start = sceKernelGetSystemTimeLow();
	int last_state = -1;
	int left_disconnected = 0;

	for (;;)
	{
		int state = PSP_NET_APCTL_STATE_DISCONNECTED;
		int result = sceNetApctlGetState(&state);

		if (result < 0)
		{
			platform_log("network: can't read the connection's state (0x%08x)", (unsigned int)result);
			return result;
		}
		if (state != last_state)
		{
			platform_log("network: the access point: %s", apctl_state_name(state));
			last_state = state;
		}
		if (state == PSP_NET_APCTL_STATE_GOT_IP)
			return 0;
		if (state != PSP_NET_APCTL_STATE_DISCONNECTED)
			left_disconnected = 1;
		else if (left_disconnected)
		{
			platform_log("network: the access point refused the PSP or went out of range");
			return -1;
		}
		if (sceKernelGetSystemTimeLow() - start > JOIN_TIMEOUT_US)
		{
			platform_log("network: no connection after %d s", JOIN_TIMEOUT_US / 1000000);
			return -1;
		}
		sceKernelDelayThread(50 * 1000);
	}
}

/* this machine's address and mask, from the connection; 1 if read */
static int wifi_read_addresses(void)
{
	union SceNetApctlInfo ip, mask, ssid;
	struct in_addr own, netmask, broadcast;

	if (sceNetApctlGetInfo(PSP_NET_APCTL_INFO_IP, &ip) < 0 ||
		sceNetApctlGetInfo(PSP_NET_APCTL_INFO_SUBNETMASK, &mask) < 0)
	{
		platform_log("network: connected, but the address can't be read");
		return 0;
	}
	if (!inet_aton(ip.ip, &own) || !inet_aton(mask.subNetMask, &netmask))
	{
		platform_log("network: an address that doesn't read: %s / %s", ip.ip, mask.subNetMask);
		return 0;
	}
	wifi.address = own.s_addr;
	wifi.broadcast = own.s_addr | ~netmask.s_addr;
	if (sceNetApctlGetInfo(PSP_NET_APCTL_INFO_SSID, &ssid) < 0)
		ssid.ssid[0] = 0;
	ssid.ssid[sizeof(ssid.ssid) - 1] = 0;
	broadcast.s_addr = wifi.broadcast;
	platform_log("network: on \"%s\" as %s, mask %s (broadcast %s)", ssid.ssid, ip.ip, mask.subNetMask,
		inet_ntoa(broadcast));
	/* (WLAN power save sleeps the radio between the access point's beacons,
	which then holds what comes for the PSP: seconds of delay under a
	stream of datagrams) */
	{
		union SceNetApctlInfo save;
		int setting = -1;

		if (sceNetApctlGetInfo(PSP_NET_APCTL_INFO_POWER_SAVE, &save) < 0)
			save.powerSave = 0xFF;
		sceUtilityGetSystemParamInt(PSP_SYSTEMPARAM_ID_INT_WLAN_POWERSAVE, &setting);
		platform_log("network: WLAN power save %s (System Settings: %s)",
			save.powerSave == 0xFF ? "unknown" : save.powerSave ? "on" : "off",
			setting < 0 ? "unknown" : setting ? "on" : "off");
	}
	return 1;
}

/* the priority sceNetInit gives the stack's threads (NET_THREAD_PRIORITY;
psp_net_adhoc.c too) */
int psp_net_stack_priority(void)
{
	int priority = psp_keep_number("netprio", NET_THREAD_PRIORITY);

	return priority < 0x10 ? 0x10 : priority > 0x6f ? 0x6f : priority;
}

/* keep word netpaceN (on by default, netpace0 off): a thread above the game
thread drops it to the bottom (0x70) for 1 ms every N ms while the PSP is
on a network, so every thread under it runs. The Wi-Fi driver's threads
(SceWlanMac, SceWlanHal) sit at 0x27, under the game thread's 0x20, and
the firmware shares no time down the priorities: through a catch-up tick
of 100-500 ms the driver can't empty the radio's receive buffer, and
packets the radio already acknowledged are lost. That stalled the host's
stream behind one missing segment and dropped the PSP from every public
game within ~30 s. It costs the game thread at most 1 ms in N, less when
the threads under it have nothing to do */
#define NET_PACE_DEFAULT_MS 5
static SceUID pace_game_thread = -1;
static int pace_game_priority;
static int pace_every_ms;

static int pace_thread(SceSize length, void *argument)
{
	(void)length;
	(void)argument;
	for (;;)
	{
		sceKernelDelayThread((SceUInt)pace_every_ms * 1000);
		if (!wifi.connected)
			continue;
		sceKernelChangeThreadPriority(pace_game_thread, 0x70);
		sceKernelDelayThread(1000);
		sceKernelChangeThreadPriority(pace_game_thread, pace_game_priority);
	}
	return 0;
}

/* every thread the firmware lists to this program, with its priority, a few
to a line: where the Wi-Fi driver's would sit against the game thread */
static void log_all_threads(const char *when)
{
	SceUID ids[128];
	char line[400];
	int count = 0, index, used = 0, unreadable = 0;

	if (sceKernelGetThreadmanIdList(SCE_KERNEL_TMID_Thread, ids, 128, &count) < 0)
		count = 0;
	if (count > 128)
		count = 128;
	for (index = 0; index < count; index++)
	{
		SceKernelThreadInfo info;

		memset(&info, 0, sizeof(info));
		info.size = sizeof(info);
		if (sceKernelReferThreadStatus(ids[index], &info) < 0)
		{
			unreadable++;
			continue;
		}
		if (used > (int)sizeof(line) - 64)
		{
			platform_log("network: threads %s: %s", when, line);
			used = 0;
		}
		used += snprintf(line + used, sizeof(line) - (size_t)used, "%s%s 0x%x", used ? ", " : "",
			info.name, info.currentPriority);
	}
	platform_log("network: threads %s: %s; %d listed, %d not readable from here", when,
		used ? line : "(none)", count, unreadable);
}

/* after each game tick (psp_platform.c's tick accounting): the thread list
once the PSP is on a network, netpaceN's thread, and netyieldN (the game
thread waits 1 ms once every N ms of its ticks; in a heavy game that is
once a tick, too seldom for the driver, which is why netpace runs on a
clock of its own) */
void psp_net_tick(void)
{
	static int every_ms = -1, listed;
	static unsigned int last;
	unsigned int now;

	if (!wifi.connected)
		return;
	if (!listed)
	{
		listed = 1;
		log_all_threads("on the network");
	}
	if (every_ms < 0)
	{
		every_ms = psp_keep_number("netyield", 0);
		pace_every_ms = psp_keep_number("netpace", NET_PACE_DEFAULT_MS);
		if (pace_every_ms > 0)
		{
			SceKernelThreadInfo info;
			SceUID pacer;

			memset(&info, 0, sizeof(info));
			info.size = sizeof(info);
			pace_game_thread = sceKernelGetThreadId();
			sceKernelReferThreadStatus(pace_game_thread, &info);
			pace_game_priority = info.currentPriority;
			pacer = sceKernelCreateThread("halo net pacer", pace_thread, 0x18, 2048, 0, NULL);
			if (pacer >= 0)
				sceKernelStartThread(pacer, 0, NULL);
			platform_log("network: netpace: the game thread (0x%x) at 0x70 for 1 ms every %d ms%s",
				pace_game_priority, pace_every_ms, pacer >= 0 ? "" : "; its thread wasn't made");
		}
	}
	if (every_ms <= 0)
		return;
	now = sceKernelGetSystemTimeLow();
	if (now - last < (unsigned int)every_ms * 1000)
		return;
	sceKernelDelayThread(1000);
	last = sceKernelGetSystemTimeLow();
}

/* the stack's threads as the firmware has them, once it has started: to
check their names and priorities on the PSP (the emulator makes its own) */
void psp_net_log_threads(void)
{
	SceUID ids[64];
	char line[256];
	int count = 0, index, used = 0;

	if (sceKernelGetThreadmanIdList(SCE_KERNEL_TMID_Thread, ids, 64, &count) < 0)
		count = 0;
	line[0] = 0;
	for (index = 0; index < count && index < 64; index++)
	{
		SceKernelThreadInfo info;

		memset(&info, 0, sizeof(info));
		info.size = sizeof(info);
		if (sceKernelReferThreadStatus(ids[index], &info) < 0 ||
			strncmp(info.name, "SceNet", 6) != 0 || used >= (int)sizeof(line) - 48)
			continue;
		used += snprintf(line + used, sizeof(line) - (size_t)used, "%s%s 0x%x",
			used ? ", " : "", info.name, info.currentPriority);
	}
	platform_log("network: the stack's threads, asked for at 0x%x: %s", psp_net_stack_priority(),
		used ? line : "none seen");
}

/* the network modules loaded and the stack started, once (psp_memory.c calls
it at start-up, when the game state and the tag window are placed) */
void psp_net_start(void)
{
	unsigned int start;
	int result, free_kb, largest_kb, pool_kb;

	if (wifi.start_tried)
		return;
	wifi.start_tried = 1;
	/* mp5: ad hoc, PSP to PSP (psp_net_adhoc.c), its own modules and the
	same pool */
#ifdef PSP_MP_HOST
	if (psp_keep("adhoc"))
	{
		adhoc_mode = 1;
		pool_kb = psp_keep_number("netpool", NET_POOL_KB);
		psp_adhoc_start(pool_kb < 64 ? 64 : pool_kb > 1024 ? 1024 : pool_kb);
		return;
	}
#endif
	/* (what the modules and the stack take, measured: the RAM the
	multiplayer build has to leave free outside its heap) */
	free_kb = (int)sceKernelTotalFreeMemSize() / 1024;
	largest_kb = (int)sceKernelMaxFreeMemSize() / 1024;
	start = sceKernelGetSystemTimeLow();
	result = sceUtilityLoadNetModule(PSP_NET_MODULE_COMMON);
	wifi.common_loaded = result >= 0;
	if (result >= 0)
	{
		result = sceUtilityLoadNetModule(PSP_NET_MODULE_INET);
		wifi.inet_loaded = result >= 0;
	}
	if (result >= 0)
	{
		/* the stack's buffers (NET_POOL_KB; pspsdk's samples: 128 KB) */
		pool_kb = psp_keep_number("netpool", NET_POOL_KB);
		pool_kb = pool_kb < 64 ? 64 : pool_kb > 1024 ? 1024 : pool_kb;
		result = sceNetInit(pool_kb * 1024, psp_net_stack_priority(), 4 * 1024,
			psp_net_stack_priority(), 4 * 1024);
		wifi.net_started = result >= 0;
	}
	if (result >= 0)
	{
		result = sceNetInetInit();
		wifi.inet_started = result >= 0;
	}
	if (result >= 0)
	{
		/* its thread: the access point's link (keep word apctlprioN, decimal;
		the firmware's samples' 0x42 otherwise) */
		result = sceNetApctlInit(0x1600, psp_keep_number("apctlprio", 0x42));
		wifi.apctl_started = result >= 0;
	}
	if (result < 0)
	{
		platform_log("network: the network modules didn't start (0x%08x); user memory free %d KB (largest block %d KB): no network play this run",
			(unsigned int)result, free_kb, largest_kb);
		wifi_stop();
		return;
	}
	platform_log("network: the modules and the stack started in %u ms (a %d KB pool); user memory free %d KB (largest block %d KB), was %d KB (%d KB)",
		(sceKernelGetSystemTimeLow() - start) / 1000, pool_kb, (int)sceKernelTotalFreeMemSize() / 1024,
		(int)sceKernelMaxFreeMemSize() / 1024, free_kb, largest_kb);
	psp_net_log_threads();
}

/* mp3: the heartbeat's network line (psp_platform.c), once the PSP has
joined: the stack's pool (what's free now, the least it has been), and the
access point if the PSP has left it. What tells a full pool from a lost
access point when a game goes quiet */
void psp_net_heartbeat(void)
{
	static int last_state = PSP_NET_APCTL_STATE_GOT_IP;
	SceNetMallocStat stat;
	int state = PSP_NET_APCTL_STATE_GOT_IP;

	if (adhoc_mode)
	{
		psp_adhoc_heartbeat();
		return;
	}
	if (!wifi.connected || sceNetGetMallocStat(&stat) < 0)
		return;
	if (sceNetApctlGetState(&state) < 0)
		state = -1;
	platform_log("network: the stack's pool %d of %d KB free (%d KB at most in use)%s%s",
		stat.free / 1024, stat.pool / 1024, stat.maximum / 1024,
		state != last_state ? "; the access point: " : "", state != last_state ? apctl_state_name(state) : "");
	last_state = state;
}

/* joined, joining now if need be: 0, or negative (the log says why) */
static int wifi_join(void)
{
	unsigned int start;
	int profile, result;

	if (wifi.connected)
		return 0;
	/* (start-up's, if nothing placed the tag window first) */
	psp_net_start();
	if (!wifi.apctl_started)
	{
		if (!wifi.start_failure_logged)
			platform_log("network: no network stack (it didn't start at start-up)");
		wifi.start_failure_logged = 1;
		return -1;
	}
	if (wifi.failed_at && sceKernelGetSystemTimeLow() - wifi.failed_at < JOIN_RETRY_US)
		return -1;
	if (sceWlanGetSwitchState() != 1)
	{
		platform_log("network: the WLAN switch is off");
		wifi.failed_at = sceKernelGetSystemTimeLow() | 1;
		wifi.join_failed = 1;
		return -1;
	}
	profile = psp_keep_number("netprofile", 1);
	if (sceUtilityCheckNetParam(profile) != 0)
	{
		platform_log("network: the PSP has no network connection number %d (Settings, Network Settings)", profile);
		wifi.failed_at = sceKernelGetSystemTimeLow() | 1;
		wifi.join_failed = 1;
		return -1;
	}
	platform_log("network: joining network connection %d", profile);
	start = sceKernelGetSystemTimeLow();
	result = sceNetApctlConnect(profile);
	if (result >= 0)
		result = wifi_wait_for_address();
	if (result < 0 || !wifi_read_addresses())
	{
		wifi_leave();
		wifi.failed_at = sceKernelGetSystemTimeLow() | 1;
		wifi.join_failed = 1;
		return result < 0 ? result : -1;
	}
	wifi.connected = 1;
	wifi.failed_at = 0;
	platform_log("network: up in %u ms", (sceKernelGetSystemTimeLow() - start) / 1000);
	/* internet play opens its socket at start-up, and is off for the run if
	that first join failed; a later join starts it (xnet.c's fixup). Not on
	the first join: internet play's own socket call is making it */
	if (wifi.join_failed)
	{
		void psp_internet_start(void);

		psp_internet_start();
	}
	return 0;
}

/* ---------- addresses: Winsock's sockaddr and the PSP's

Winsock: family (2 bytes, little-endian), port, address, zero.
The PSP (BSD): length (1 byte), family (1 byte), port, address, zero. */

/* the game's address, at most a sockaddr_in, as the PSP's; 0 if it isn't one */
static int to_psp_address(const void *address, int length, struct sockaddr_in *psp)
{
	const unsigned char *bytes = address;

	if (!address || length < 2 || length > (int)sizeof(*psp))
		return 0;
	memset(psp, 0, sizeof(*psp));
	memcpy(psp, address, (size_t)length);
	psp->sin_len = (unsigned char)sizeof(*psp);
	psp->sin_family = (sa_family_t)(bytes[0] | bytes[1] << 8);
	return 1;
}

/* the PSP's address as the game's, in place */
static void to_game_address(void *address, int length)
{
	unsigned char *bytes = address;

	if (address && length >= 2)
	{
		unsigned char family = bytes[1];

		bytes[0] = family;
		bytes[1] = 0;
	}
}

/* ---------- the sockets */

int posix_socket_last_error(void)
{
	return last_error;
}

int posix_socket(int family, int type, int protocol)
{
	if (adhoc_mode)
		return adhoc_result(psp_adhoc_socket(family, type, protocol));
	if (wifi_join() < 0)
	{
		last_error = WSAENETDOWN;
		return -1;
	}
	return succeed(socket(family, type, protocol));
}

int posix_socket_close(int socket)
{
	if (adhoc_mode)
		return adhoc_result(psp_adhoc_close(socket));
	return succeed(close(socket));
}

int posix_socket_bind(int socket, const void *address, int address_length)
{
	struct sockaddr_in psp;

	if (adhoc_mode)
		return adhoc_result(psp_adhoc_bind(socket, address, address_length));
	if (!to_psp_address(address, address_length, &psp))
	{
		last_error = WSAEFAULT;
		return -1;
	}
	return succeed(bind(socket, (struct sockaddr *)&psp, sizeof(psp)));
}

int posix_socket_connect(int socket, const void *address, int address_length)
{
	struct sockaddr_in psp;
	int result;

	if (adhoc_mode)
		return adhoc_result(psp_adhoc_connect(socket, address, address_length));
	if (!to_psp_address(address, address_length, &psp))
	{
		last_error = WSAEFAULT;
		return -1;
	}
	/* a non-blocking connect under way: Winsock's WSAEWOULDBLOCK, which the
	game waits on, as posix_net.c answers (POSIX's EINPROGRESS, as
	WSAEINPROGRESS, is a failure to it: the first emulator join,
	a netplay run) */
	result = connect(socket, (struct sockaddr *)&psp, sizeof(psp));
	if (result < 0 && errno == EINPROGRESS)
	{
		last_error = WSAEWOULDBLOCK;
		return -1;
	}
	if (result < 0 && errno != EAGAIN)
	{
		/* (the first few: internet play's brokers retry for as long as the
		game runs) */
		static int logged;

		if (logged < 8)
		{
			char text[56];

			logged++;
			platform_log("network: a connection to %s failed at once (errno %d)",
				log_address_ipv4((unsigned long)psp.sin_addr.s_addr, psp.sin_port, text, sizeof(text)), errno);
		}
	}
	return succeed(result);
}

int posix_socket_listen(int socket, int backlog)
{
	if (adhoc_mode)
		return adhoc_result(psp_adhoc_listen(socket, backlog));
	return succeed(listen(socket, backlog));
}

int posix_socket_accept(int socket, void *address, int *address_length)
{
	struct sockaddr_in psp;
	socklen_t length = sizeof(psp);
	int result;

	if (adhoc_mode)
		return adhoc_result(psp_adhoc_accept(socket, address, address_length));
	result = accept(socket, (struct sockaddr *)&psp, &length);

	if (result >= 0 && address && address_length)
	{
		int copied = *address_length < (int)length ? *address_length : (int)length;

		memcpy(address, &psp, (size_t)copied);
		to_game_address(address, copied);
		*address_length = copied;
	}
	return succeed(result);
}

int posix_socket_send(int socket, const void *buffer, int length, int flags)
{
	if (adhoc_mode)
		return adhoc_result(psp_adhoc_send(socket, buffer, length));
	return succeed(send(socket, buffer, (size_t)length, flags));
}

int posix_socket_sendto(int socket, const void *buffer, int length, int flags,
	const void *address, int address_length)
{
	struct sockaddr_in psp;
	int result;

	if (adhoc_mode)
		return adhoc_result(psp_adhoc_sendto(socket, buffer, length, address, address_length));
	if (!address)
		return succeed(sendto(socket, buffer, (size_t)length, flags, NULL, 0));
	if (!to_psp_address(address, address_length, &psp))
	{
		last_error = WSAEFAULT;
		return -1;
	}
	result = sendto(socket, buffer, (size_t)length, flags, (struct sockaddr *)&psp, sizeof(psp));
	/* (a connected socket given an address: sent as posix_net.c sends it) */
	if (result < 0 && errno == EISCONN)
		result = send(socket, buffer, (size_t)length, flags);
	return succeed(result);
}

int posix_socket_recv(int socket, void *buffer, int length, int flags)
{
	if (adhoc_mode)
		return adhoc_result(psp_adhoc_recv(socket, buffer, length));
	return succeed(recv(socket, buffer, (size_t)length, flags));
}

int posix_socket_recvfrom(int socket, void *buffer, int length, int flags,
	void *address, int *address_length)
{
	struct sockaddr_in psp;
	socklen_t psp_length = sizeof(psp);
	int result;

	if (adhoc_mode)
		return adhoc_result(psp_adhoc_recvfrom(socket, buffer, length, address, address_length));
	result = recvfrom(socket, buffer, (size_t)length, flags, (struct sockaddr *)&psp, &psp_length);

	if (result >= 0 && address && address_length)
	{
		int copied = *address_length < (int)psp_length ? *address_length : (int)psp_length;

		memcpy(address, &psp, (size_t)copied);
		to_game_address(address, copied);
		*address_length = copied;
	}
	return succeed(result);
}

int posix_socket_shutdown(int socket, int how)
{
	if (adhoc_mode)
		return adhoc_result(psp_adhoc_shutdown(socket));
	return succeed(shutdown(socket, how));
}

/* (not ioctl(FIONBIO), which libcglue leaves undone: O_NONBLOCK, which it
passes to the socket) */
int posix_socket_set_nonblocking(int socket, int nonblocking)
{
	int flags;

	if (adhoc_mode)
		return adhoc_result(psp_adhoc_set_nonblocking(socket, nonblocking));
	flags = fcntl(socket, F_GETFL, 0);
	if (flags < 0)
		return fail();
	flags = nonblocking ? flags | O_NONBLOCK : flags & ~O_NONBLOCK;
	return succeed(fcntl(socket, F_SETFL, flags) < 0 ? -1 : 0);
}

/* (the game asks only FIONBIO of ioctlsocket; FIONREAD, which libcglue has
no ioctl for, is the bytes a peek finds, up to one datagram's) */
int posix_socket_bytes_available(int socket, posix_ulong *count)
{
	static char peek[1500];
	int result;

	if (adhoc_mode)
		return adhoc_result(psp_adhoc_bytes_available(socket, count));
	result = recv(socket, peek, sizeof(peek), MSG_PEEK | MSG_DONTWAIT);

	if (result < 0 && errno == EAGAIN)
		result = 0;
	if (count)
		*count = result > 0 ? (posix_ulong)result : 0;
	return succeed(result < 0 ? -1 : 0);
}

int posix_socket_set_nodelay(int socket)
{
	int set = 1;

	if (adhoc_mode)
		return adhoc_result(0);
	return succeed(setsockopt(socket, IPPROTO_TCP, TCP_NODELAY, &set, sizeof(set)));
}

/* whether the system knows a socket option: SOL_SOCKET's that posix_net.c
translates (their numbers are Winsock's and the PSP's alike), and every
IPPROTO_IP or IPPROTO_TCP one (shared numbers) */
static int option_known(int level, int name)
{
	if (level != SOL_SOCKET)
		return 1;
	switch (name)
	{
	case SO_REUSEADDR: case SO_KEEPALIVE: case SO_BROADCAST: case SO_LINGER:
	case SO_SNDBUF: case SO_RCVBUF: case SO_ERROR: case SO_TYPE:
		return 1;
	default:
		return 0;
	}
}

int posix_socket_setsockopt(int socket, int level, int name, const void *value, int length)
{
	if (adhoc_mode)
		return adhoc_result(psp_adhoc_setsockopt(socket, level, name, value, length));
	/* an Xbox-only option (SO_ENCRYPT and the like) has nothing to do here,
	as posix_net.c answers */
	if (!option_known(level, name))
	{
		last_error = 0;
		return 0;
	}
	/* mp3: a socket's buffers no larger than SOCKET_BUFFER_KB (or
	netbufN): every socket's come from the stack's one pool */
	if (level == SOL_SOCKET && (name == SO_SNDBUF || name == SO_RCVBUF) && value && length == (int)sizeof(int))
	{
		static int logged;
		int asked, cap = psp_keep_number("netbuf", SOCKET_BUFFER_KB) * 1024;

		memcpy(&asked, value, sizeof(asked));
		if (cap > 0 && asked > cap)
		{
			if (!logged)
				platform_log("network: a socket's buffers %d KB, not the %d KB asked (keep word netbufN; the stack's pool is shared)",
					cap / 1024, asked / 1024);
			logged = 1;
			return succeed(setsockopt(socket, level, name, &cap, sizeof(cap)));
		}
	}
	return succeed(setsockopt(socket, level, name, value, (socklen_t)length));
}

int posix_socket_getsockopt(int socket, int level, int name, void *value, int *length)
{
	socklen_t psp_length = length ? (socklen_t)*length : 0;
	int result;

	if (adhoc_mode)
		return adhoc_result(psp_adhoc_getsockopt(socket, level, name, value, length));
	if (!option_known(level, name))
	{
		last_error = WSAENOPROTOOPT;
		return -1;
	}
	result = getsockopt(socket, level, name, value, &psp_length);
	if (result >= 0 && length)
		*length = (int)psp_length;
	return succeed(result);
}

int posix_socket_getsockname(int socket, void *address, int *address_length)
{
	struct sockaddr_in psp;
	socklen_t length = sizeof(psp);
	int result;

	if (adhoc_mode)
		return adhoc_result(psp_adhoc_getsockname(socket, address, address_length));
	result = getsockname(socket, (struct sockaddr *)&psp, &length);

	if (result >= 0 && address && address_length)
	{
		int copied = *address_length < (int)length ? *address_length : (int)length;

		memcpy(address, &psp, (size_t)copied);
		to_game_address(address, copied);
		*address_length = copied;
	}
	return succeed(result);
}

int posix_socket_getpeername(int socket, void *address, int *address_length)
{
	struct sockaddr_in psp;
	socklen_t length = sizeof(psp);
	int result;

	if (adhoc_mode)
		return adhoc_result(psp_adhoc_getpeername(socket, address, address_length));
	result = getpeername(socket, (struct sockaddr *)&psp, &length);

	if (result >= 0 && address && address_length)
	{
		int copied = *address_length < (int)length ? *address_length : (int)length;

		memcpy(address, &psp, (size_t)copied);
		to_game_address(address, copied);
		*address_length = copied;
	}
	return succeed(result);
}

/* each list kept to its ready descriptors, in order; how many */
static int keep_ready(int *list, int *count, fd_set *ready)
{
	int index, kept = 0;

	if (!list || !count)
		return 0;
	for (index = 0; index < *count; index++)
	{
		if (FD_ISSET(list[index], ready))
			list[kept++] = list[index];
	}
	*count = kept;
	return kept;
}

static void add_all(int const *list, int const *count, fd_set *set, int *highest)
{
	int index;

	FD_ZERO(set);
	if (!list || !count)
		return;
	for (index = 0; index < *count; index++)
	{
		FD_SET(list[index], set);
		if (list[index] > *highest)
			*highest = list[index];
	}
}

/* select through the firmware's own poll (keep word inetpoll), in place of
libcglue's select. In a public game on a PSP-3000
the desktop's capture saw 1,442 of the host's packets sent to the tunnel's
port, while the tunnel read about 190, and the game's own packets went out
in bursts after seconds of nothing: the sockets held them, and select didn't
say they were ready [U]. libcglue's select asks the firmware one socket at a
time, with no wait, between sleeps it times with clock(). sceNetInetPoll
asks for them all at once and waits in the firmware. Its descriptors are the
firmware's: libcglue's table holds each (the descriptor first, its kind at
byte 16, 3 for a socket, as libcglue's own accept reads it); any descriptor
not a socket leaves the call to libcglue's select. -1: not one */
extern void *__descriptormap[];

static int firmware_socket(int descriptor)
{
	const unsigned char *entry;

	if (descriptor < 0 || descriptor >= 1024)
		return -1;
	entry = __descriptormap[descriptor];
	if (!entry || entry[16] != 3)
		return -1;
	return *(const int *)entry;
}

/* select through the firmware's select (keep word inetselect), one socket
at a time as libcglue's does, without its fault. libcglue clears only
newlib's 64 bits of the set it hands the firmware while setting the
socket's bit anywhere up to 256 (the word from bits 5 to 7 of the
firmware's number) [D, libcglue's select.o]: for a firmware number of 64 or
more, the bit lands past the cleared words, in libcglue's own result sets,
and the firmware is asked about whatever was there. That would explain the
tunnel missing its ready sockets once a join had made more of them, where
the game alone, with few sockets, plays a local server well [U]. Here each
socket gets a set of 1,024 bits cleared whole (the firmware's size isn't
documented; this covers both readings), asked with no wait, in rounds 0.5 ms
apart until something is ready or the timeout ends, as libcglue's rounds.
(All the sockets in one call, with the firmware waiting, broke play against
a local server in the emulator.) -2: not
done (the caller uses libcglue's select) */
enum { FIRMWARE_SET_WORDS = 32 };

static int firmware_ready(int number, int kind)
{
	unsigned int set[FIRMWARE_SET_WORDS];
	SceNetInetTimeval no_wait = { 0, 0 };

	memset(set, 0, sizeof(set));
	set[number >> 5] = 1u << (number & 31);
	return sceNetInetSelect(number + 1, kind == 0 ? (fd_set *)set : NULL, kind == 1 ? (fd_set *)set : NULL,
		kind == 2 ? (fd_set *)set : NULL, &no_wait) > 0;
}

static int inet_select(int count, fd_set *read_set, fd_set *write_set, fd_set *error_set,
	long seconds, long microseconds, int infinite)
{
	fd_set *game_sets[3] = { read_set, write_set, error_set };
	fd_set asked[3];
	int firmware[64];
	int index, kind, ready;
	unsigned int start = sceKernelGetSystemTimeLow();
	unsigned long long wait_us = (unsigned long long)seconds * 1000000u + (unsigned long long)microseconds;

	for (kind = 0; kind < 3; kind++)
	{
		FD_ZERO(&asked[kind]);
		if (game_sets[kind])
			asked[kind] = *game_sets[kind];
	}
	for (index = 0; index < count && index < FD_SETSIZE; index++)
	{
		firmware[index] = -1;
		for (kind = 0; kind < 3; kind++)
		{
			if (game_sets[kind] && FD_ISSET(index, &asked[kind]))
				break;
		}
		if (kind == 3)
			continue;
		firmware[index] = firmware_socket(index);
		if (firmware[index] < 0 || firmware[index] >= FIRMWARE_SET_WORDS * 32)
			return -2;
	}
	for (;;)
	{
		ready = 0;
		for (kind = 0; kind < 3; kind++)
		{
			if (!game_sets[kind])
				continue;
			FD_ZERO(game_sets[kind]);
			for (index = 0; index < count && index < FD_SETSIZE; index++)
			{
				if (firmware[index] >= 0 && FD_ISSET(index, &asked[kind]) && firmware_ready(firmware[index], kind))
				{
					FD_SET(index, game_sets[kind]);
					ready++;
				}
			}
		}
		if (ready || (!infinite && (unsigned long long)(sceKernelGetSystemTimeLow() - start) >= wait_us))
			return ready;
		sceKernelDelayThread(500);
	}
}

/* the sets' ready descriptors, as select gives them; -2 when a descriptor
isn't a socket (the caller uses libcglue's select) */
static int inet_poll(int count, fd_set *read_set, fd_set *write_set, fd_set *error_set, long timeout_ms)
{
	SceNetInetPollfd polls[64];
	int descriptors[64];
	int used = 0, index, result, ready = 0;

	for (index = 0; index < count && index < FD_SETSIZE; index++)
	{
		short events = 0;

		if (read_set && FD_ISSET(index, read_set))
			events |= SCE_NET_INET_POLLIN;
		if (write_set && FD_ISSET(index, write_set))
			events |= SCE_NET_INET_POLLOUT;
		if (error_set && FD_ISSET(index, error_set))
			events |= SCE_NET_INET_POLLPRI;
		if (!events)
			continue;
		if (used == (int)(sizeof(polls) / sizeof(polls[0])) || (polls[used].fd = firmware_socket(index)) < 0)
			return -2;
		polls[used].events = events;
		polls[used].revents = 0;
		descriptors[used++] = index;
	}
	{
		unsigned int start = sceKernelGetSystemTimeLow();

		result = sceNetInetPoll(polls, (size_t)used, (int)timeout_ms);
		/* (the emulator's poll comes back at once with nothing ready, whatever
		the timeout (a netplay run); if a PSP's ever does, a
		raised thread would spin: the rest of the wait in steps of 1 ms) */
		while (result == 0 && timeout_ms > 0 &&
			(long)((sceKernelGetSystemTimeLow() - start) / 1000) < timeout_ms)
		{
			sceKernelDelayThread(1000);
			result = sceNetInetPoll(polls, (size_t)used, 0);
		}
	}
	if (result < 0)
	{
		errno = sceNetInetGetErrno();
		return -1;
	}
	if (read_set)
		FD_ZERO(read_set);
	if (write_set)
		FD_ZERO(write_set);
	if (error_set)
		FD_ZERO(error_set);
	for (index = 0; index < used; index++)
	{
		short got = polls[index].revents;
		short asked = polls[index].events;

		/* (an error or a hang-up is readable and writeable, as select has it:
		the call that follows says which) */
		if (read_set && (asked & SCE_NET_INET_POLLIN) &&
			(got & (SCE_NET_INET_POLLIN | SCE_NET_INET_POLLERR | SCE_NET_INET_POLLHUP)))
		{
			FD_SET(descriptors[index], read_set);
			ready++;
		}
		if (write_set && (asked & SCE_NET_INET_POLLOUT) &&
			(got & (SCE_NET_INET_POLLOUT | SCE_NET_INET_POLLERR | SCE_NET_INET_POLLHUP)))
		{
			FD_SET(descriptors[index], write_set);
			ready++;
		}
		if (error_set && (asked & SCE_NET_INET_POLLPRI) && (got & (SCE_NET_INET_POLLPRI | SCE_NET_INET_POLLERR)))
		{
			FD_SET(descriptors[index], error_set);
			ready++;
		}
	}
	return ready;
}

/* how long select takes against what it was asked to wait: the tunnel's
thread passes through its loop between selects (psp_net_select_report) */
static struct
{
	unsigned long calls, late;
	unsigned long long us;
	unsigned int longest_us;
	/* the highest descriptor asked of select; FD_SETSIZE is 64 */
	int highest;
	/* inetpoll's or inetselect's calls left to libcglue's select: a
	descriptor not a socket */
	unsigned long not_sockets;
	/* the highest firmware number of a socket asked about */
	int highest_firmware;
} select_times;

void psp_net_select_report(char *text, int size)
{
	snprintf(text, (size_t)size, "selects %lu, %llu ms in all, the longest %u ms, %lu over their timeout by 20 ms; the highest descriptor asked %d (firmware number %d); %lu left to libcglue (inetpoll, inetselect)",
		select_times.calls, select_times.us / 1000, select_times.longest_us / 1000, select_times.late, select_times.highest,
		select_times.highest_firmware, select_times.not_sockets);
}

int posix_socket_select(int *read, int *read_count, int *write, int *write_count,
	int *error, int *error_count, posix_long timeout_seconds, posix_long timeout_microseconds, int infinite)
{
	fd_set read_set, write_set, error_set;
	struct timeval timeout;
	int highest = -1;
	int result, index;

	if (adhoc_mode)
	{
		int connect_error = 0;

		result = psp_adhoc_select(read, read_count, write, write_count, error, error_count,
			(long)timeout_seconds, (long)timeout_microseconds, infinite, &connect_error);
		if (result < 0)
			return adhoc_result(result);
		/* (as below: a ready socket clears the last error; a failed
		connection leaves its own) */
		if (connect_error)
			last_error = connect_error;
		else if (result > 0)
			last_error = 0;
		return result;
	}
	add_all(read, read_count, &read_set, &highest);
	add_all(write, write_count, &write_set, &highest);
	add_all(error, error_count, &error_set, &highest);
	timeout.tv_sec = timeout_seconds;
	timeout.tv_usec = timeout_microseconds;
	if (highest > select_times.highest)
		select_times.highest = highest;
	{
		unsigned int start = sceKernelGetSystemTimeLow(), took;

		static int use_poll = -1, use_select = -1;

		if (use_poll < 0 && psp_keep_loaded())
		{
			use_poll = psp_keep("inetpoll");
			use_select = psp_keep("inetselect");
		}
		/* (the firmware's numbers for what's asked: past 63, libcglue's
		select asks the firmware about the wrong sockets) */
		for (index = 0; index <= highest && index < FD_SETSIZE; index++)
		{
			if ((read && FD_ISSET(index, &read_set)) || (write && FD_ISSET(index, &write_set)) ||
				(error && FD_ISSET(index, &error_set)))
			{
				int number = firmware_socket(index);

				if (number > select_times.highest_firmware)
					select_times.highest_firmware = number;
			}
		}
		result = -2;
		if (use_select > 0)
		{
			result = inet_select(highest + 1, read ? &read_set : NULL, write ? &write_set : NULL,
				error ? &error_set : NULL, (long)timeout_seconds, (long)timeout_microseconds, infinite);
			if (result == -2)
				select_times.not_sockets++;
		}
		if (result == -2 && use_poll > 0)
		{
			long timeout_ms = infinite ? -1 : (long)timeout_seconds * 1000 + ((long)timeout_microseconds + 999) / 1000;

			result = inet_poll(highest + 1, read ? &read_set : NULL, write ? &write_set : NULL,
				error ? &error_set : NULL, timeout_ms);
			if (result == -2)
				select_times.not_sockets++;
		}
		if (result == -2)
			result = select(highest + 1, read ? &read_set : NULL, write ? &write_set : NULL, error ? &error_set : NULL,
				infinite ? NULL : &timeout);
		took = sceKernelGetSystemTimeLow() - start;
		if (!infinite)
		{
			unsigned int asked = (unsigned int)timeout_seconds * 1000000u + (unsigned int)timeout_microseconds;

			select_times.calls++;
			select_times.us += took;
			if (took > select_times.longest_us)
				select_times.longest_us = took;
			if (took > asked + 20000)
				select_times.late++;
		}
	}
	if (result < 0)
		return fail();
	/* a socket whose connect failed: BSD reports it writeable, the failure
	in SO_ERROR (which reading clears); Winsock reports it in the error set
	and not writeable, and the game takes writeable as connected
	(connect_endpoint). So, as posix_net.c does: out of the write list, into
	the error list (where the game asked), its failure the last error */
	for (index = 0; write && write_count && index < *write_count; index++)
	{
		int pending = 0;
		socklen_t length = sizeof(pending);

		if (FD_ISSET(write[index], &write_set) &&
			getsockopt(write[index], SOL_SOCKET, SO_ERROR, &pending, &length) == 0 && pending)
		{
			FD_CLR(write[index], &write_set);
			FD_SET(write[index], &error_set);
			errno = pending;
			fail();
		}
	}
	result = keep_ready(read, read_count, &read_set);
	result += keep_ready(write, write_count, &write_set);
	result += keep_ready(error, error_count, &error_set);
	/* like Winsock, a select with nothing ready leaves the last error as it
	was: after a connect under way, still WSAEWOULDBLOCK, which the game
	reads as not connected yet (posix_net.c) */
	if (result > 0)
		last_error = 0;
	return result;
}

/* this machine's address on its network (network byte order), joining the
access point if need be; 0 when there's none */
posix_ulong posix_local_ipv4_address(void)
{
	if (adhoc_mode)
		return (posix_ulong)psp_adhoc_local_address();
	return wifi_join() < 0 ? 0 : (posix_ulong)wifi.address;
}

/* a host's address (network byte order), or 0: a dotted quad as it is, a
name through the access point's DNS (mpb1: internet play's brokers and STUN
servers are names). The firmware's resolver is started at the first name,
once the PSP has joined; each lookup has a resolver of its own, so the
p2p layer's threads can look names up at once. A lookup blocks for up to
RESOLVE_TIMEOUT_S x RESOLVE_RETRIES: call it off the game thread, as the p2p
layer does */
enum
{
	RESOLVE_TIMEOUT_S = 3,
	RESOLVE_RETRIES = 2,
};

static void resolver_start(void)
{
	resolver_result = sceNetResolverInit();
	if (resolver_result < 0)
		platform_log("network: the resolver didn't start (0x%08x): no names are looked up", (unsigned int)resolver_result);
}

posix_ulong posix_resolve_ipv4(const char *host)
{
	struct in_addr address;
	unsigned char work[1024];
	int resolver, result;

	if (!host || !*host)
		return 0;
	if (inet_aton(host, &address))
		return (posix_ulong)address.s_addr;
	if (adhoc_mode || wifi_join() < 0)
		return 0;
	pthread_once(&resolver_once, resolver_start);
	if (resolver_result < 0)
		return 0;
	if (sceNetResolverCreate(&resolver, work, sizeof(work)) < 0)
		return 0;
	result = sceNetResolverStartNtoA(resolver, host, &address, RESOLVE_TIMEOUT_S, RESOLVE_RETRIES);
	sceNetResolverDelete(resolver);
	if (result < 0)
	{
		platform_log("network: %s wasn't found (0x%08x)", host, (unsigned int)result);
		return 0;
	}
	{
		/* (as log_address writes it: an address on the internet as a tag) */
		char text[56];

		platform_log("network: %s is %s", host, log_address_ipv4((unsigned long)address.s_addr, 0, text, sizeof(text)));
	}
	return (posix_ulong)address.s_addr;
}
