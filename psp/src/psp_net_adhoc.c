/* psp_net_adhoc.c - the multiplayer build's ad hoc network: PSP to PSP, no
access point (keep word "adhoc"; psp_net.h)

Halo's network is IP: system link searches and game datagrams over UDP, and
each machine's connection to the host over TCP (network_connection.c's
reliable stream). Ad hoc isn't IP. PSPs in one ad hoc group reach each other
by MAC address and port, through two kinds of socket the firmware offers:
- PDP, datagrams, which may be lost (UDP's part). A datagram to the MAC
  ff:ff:ff:ff:ff:ff reaches every PSP in the group: system link's searches
  work as broadcasts, as on an Xbox, with no address lists;
- PTP, a stream that arrives whole and in order (TCP's part): listen,
  accept, open and connect, send and receive.

So this file answers xnet.c's socket calls with those, and the game never
knows. What it has to make up:
- An IPv4 address for each PSP: 169.254.x.y (link-local), x.y the last two
  bytes of its MAC. A datagram's sender is known by its MAC, which gives its
  address; an address the game names is turned back into a MAC through the
  PSPs heard so far, then the group's member list. Two PSPs whose MACs end
  in the same two bytes would share an address: logged when seen.
- Socket numbers of its own, from PSP_ADHOC_SOCKET_BASE: a PDP socket is
  made when the game binds (or first sends), a PTP one when it listens or
  connects, which is when the firmware needs the port.
- select(): what the firmware reports of each socket (sceNetAdhocGetPdpStat,
  sceNetAdhocGetPtpStat: bytes waiting, a connection's state), polled. A
  listening socket is ready when a connection could be accepted: the accept
  is made then, and handed over at the game's accept.
- Blocking calls: the firmware's own blocking takes a timeout whose zero
  isn't documented, so a blocking socket's call polls instead.
- The loopback. A host plays in its own game through a client of its own,
  which reaches the host's server at 127.0.0.1 (the server knows that
  machine by the address), and ad hoc has no way to a PSP's own sockets.
  So datagrams and connections to 127.0.0.1, or to this PSP's own address,
  stay in memory here (LOOP_*), and come from 127.0.0.1 (found by the
  emulator's first PSP-hosted game, 2026-10-12, over IP).

The group: every PSP running this joins the ad hoc group ADHOC_GROUP under
the product code ADHOC_PRODUCT (the firmware's way of keeping one game's
groups apart from another's). The first PSP to join makes it. The channel is
the PSP's own setting (Settings, Network Settings, Ad Hoc Mode).

One thread at a time (the game's calls come from its main thread), as
psp_net.c. */

#include <pspkernel.h>
#include <pspnet.h>
#include <pspnet_adhoc.h>
#include <pspnet_adhocctl.h>
#include <psputility.h>
#include <pspwlan.h>

#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>

#include "psp_net.h"

enum
{
	/* the most sockets the game has open at once: a host's system link
	socket, its game socket, its listener and one connection per machine
	(16, PSP_HOST_MAXIMUM_NETWORK_MACHINES), with room */
	ADHOC_SOCKETS = 40,
	/* a PDP socket's receive buffer, and a PTP connection's buffers (each
	from sceNetInit's pool, which every socket shares): a datagram is at
	most 1,200 bytes (DATAGRAM_MAXIMUM_SIZE), a stream message 4 KB
	(HALO_PORT_MAXIMUM_NETWORK_MESSAGE_SIZE). Keep word netbufN sets the
	PTP size (KB) */
	PDP_BUFFER_BYTES = 16 * 1024,
	PTP_BUFFER_KB = 8,
	/* a PTP connection's retransmission: every 200 ms, 50 times (10 s
	without an answer, then the connection is lost) */
	PTP_RETRY_US = 200 * 1000,
	PTP_RETRY_COUNT = 50,
	/* a connection still not made after this fails (the host's machine
	slots answer at once when they answer at all) */
	CONNECT_TIMEOUT_US = 15 * 1000 * 1000,
	/* joining the group: past this, no PSP answers on the channel and the
	firmware hasn't made the group either */
	JOIN_TIMEOUT_US = 20 * 1000 * 1000,
	JOIN_RETRY_US = 10 * 1000 * 1000,
	/* ports the firmware's sockets get when the game binds to port 0 */
	FIRST_EPHEMERAL_PORT = 49152,
	/* the PSPs heard from: a group holds at most 16 */
	MAXIMUM_PEERS = 32,
	/* the loopback: datagrams waiting (the host's server and its own client
	trade a few a tick), and the in-memory streams (a connection is two, one
	each way; one connection at a time, two while one closes) */
	LOOP_DATAGRAMS = 24,
	LOOP_DATAGRAM_BYTES = 1280,
	LOOP_STREAMS = 4,
	LOOP_STREAM_BYTES = 16 * 1024,

	/* the firmware's answers (pspsdk names none; PPSSPP's ErrorCodes.h) */
	ADHOC_INVALID_SOCKET_ID = (int)0x80410701,
	ADHOC_INVALID_ADDR = (int)0x80410702,
	ADHOC_INVALID_PORT = (int)0x80410703,
	ADHOC_INVALID_DATALEN = (int)0x80410705,
	ADHOC_NOT_ENOUGH_SPACE = (int)0x80400706,
	ADHOC_SOCKET_DELETED = (int)0x80410707,
	ADHOC_WOULD_BLOCK = (int)0x80410709,
	ADHOC_PORT_IN_USE = (int)0x8041070a,
	ADHOC_NOT_CONNECTED = (int)0x8041070b,
	ADHOC_DISCONNECTED = (int)0x8041070c,
	ADHOC_SOCKET_ID_NOT_AVAIL = (int)0x8041070f,
	ADHOC_PORT_NOT_AVAIL = (int)0x80410710,
	ADHOC_NOT_INITIALIZED = (int)0x80410712,
	ADHOC_BUSY = (int)0x80410714,
	ADHOC_TIMEOUT = (int)0x80410715,
	ADHOC_CONNECTION_REFUSED = (int)0x80410718,
	ADHOCCTL_WLAN_SWITCH_OFF = (int)0x80410b03,

	/* sceNetAdhocctlGetState's states */
	ADHOCCTL_DISCONNECTED = 0,
	ADHOCCTL_CONNECTED = 1,
	/* a PTP socket's (ptpStatStruct's last word) */
	PTP_CLOSED = 0,
	PTP_LISTEN = 1,
	PTP_ESTABLISHED = 4,
};

/* every PSP running Sierra-III's multiplayer build uses these two */
#define ADHOC_PRODUCT "SIER00003"
#define ADHOC_GROUP "SIERRA3"

static const unsigned char broadcast_mac[6] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};

struct adhoc_socket
{
	int used;
	/* SOCK_DGRAM (PDP) or SOCK_STREAM (PTP) */
	int type;
	/* the firmware's socket; -1 until one is made */
	int id;
	int nonblocking;
	/* the local port: what bind asked, or an ephemeral one; 0 until then */
	unsigned short port;
	/* PTP: listening; connecting (since connect_started); connected */
	int listening, connecting, connected;
	unsigned int connect_started;
	/* the peer (PTP, or a PDP socket the game connected) */
	unsigned char peer_mac[6];
	unsigned short peer_port;
	int has_peer;
	/* a listener: a connection accepted by select, for the game's accept
	(its firmware socket, -1 for none) */
	int pending_id;
	unsigned char pending_mac[6];
	unsigned short pending_port;
	int backlog;
	/* the loopback: a connection's streams in (read) and out (written),
	LOOP_STREAMS indices, -1 for a firmware connection; a listener's
	connection from this PSP waiting for accept (a socket index, -1) */
	int loop_in, loop_out;
	int pending_loop;
};

/* one direction of a loopback connection: a ring of bytes */
struct loop_stream
{
	int used;
	int head, count;
	/* each end closed: the stream goes when both have */
	int writer_closed, reader_closed;
	unsigned char data[LOOP_STREAM_BYTES];
};

/* a datagram sent to this PSP itself, for the socket on its port */
struct loop_datagram
{
	/* the socket it waits for (an index), -1 when free; the order sent */
	int socket;
	unsigned int sequence;
	unsigned short from_port;
	int length;
	unsigned char data[LOOP_DATAGRAM_BYTES];
};

static struct
{
	int common_loaded, adhoc_loaded, net_started, adhoc_started, adhocctl_started;
	int start_failure_logged;
	int connected;
	unsigned int failed_at;
	unsigned char mac[6];
	unsigned long address;
	unsigned short next_port;
	struct adhoc_socket sockets[ADHOC_SOCKETS];
	/* the PSPs heard from: MAC and the address made of it */
	struct
	{
		unsigned char mac[6];
		unsigned long address;
	} peers[MAXIMUM_PEERS];
	int peer_count;
	int last_state;
	/* the loopback's buffers (~94 KB), allocated only in ad hoc mode
	(psp_adhoc_start): the access point's mode doesn't pay for them. Kept
	for the run (the modules stay loaded too) */
	struct loop_stream *streams;
	struct loop_datagram *datagrams;
	unsigned int datagram_sequence;
} adhoc;

#define LOOPBACK_ADDRESS htonl(0x7F000001ul)

/* ---------- addresses */

/* the made-up address of a MAC: 169.254.x.y, network byte order */
static unsigned long mac_address(const unsigned char *mac)
{
	return htonl(0xA9FE0000ul | (unsigned long)mac[4] << 8 | mac[5]);
}

static int is_broadcast(unsigned long address)
{
	return address == htonl(INADDR_BROADCAST) || address == htonl(0xA9FEFFFFul);
}

static int is_adhoc_address(unsigned long address)
{
	return (ntohl(address) & 0xFFFF0000ul) == 0xA9FE0000ul;
}

/* 127.0.0.0/8, or this PSP's own address: the loopback */
static int is_loopback(unsigned long address)
{
	return (ntohl(address) >> 24) == 127 || (adhoc.address && address == adhoc.address);
}

/* a PSP heard from (or listed by the group): remembered */
static void note_peer(const unsigned char *mac)
{
	unsigned long address = mac_address(mac);
	int index;

	if (!memcmp(mac, adhoc.mac, 6))
		return;
	for (index = 0; index < adhoc.peer_count; index++)
	{
		if (!memcmp(adhoc.peers[index].mac, mac, 6))
			return;
		if (adhoc.peers[index].address == address)
		{
			platform_log("network: ad hoc: %02x:%02x:%02x:%02x:%02x:%02x and %02x:%02x:%02x:%02x:%02x:%02x share an address (their MACs' last two bytes); the second isn't reachable",
				mac[0], mac[1], mac[2], mac[3], mac[4], mac[5], adhoc.peers[index].mac[0], adhoc.peers[index].mac[1],
				adhoc.peers[index].mac[2], adhoc.peers[index].mac[3], adhoc.peers[index].mac[4], adhoc.peers[index].mac[5]);
			return;
		}
	}
	if (adhoc.peer_count == MAXIMUM_PEERS)
		return;
	memcpy(adhoc.peers[adhoc.peer_count].mac, mac, 6);
	adhoc.peers[adhoc.peer_count].address = address;
	adhoc.peer_count++;
	platform_log("network: ad hoc: a PSP at %u.%u.%u.%u (%02x:%02x:%02x:%02x:%02x:%02x)",
		(unsigned)(ntohl(address) >> 24), (unsigned)(ntohl(address) >> 16 & 255), (unsigned)(ntohl(address) >> 8 & 255),
		(unsigned)(ntohl(address) & 255), mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

/* the group's member list, into the peers */
static void read_group(void)
{
	static SceNetAdhocctlPeerInfo list[16];
	int length = (int)sizeof(list);
	SceNetAdhocctlPeerInfo *peer;

	memset(list, 0, sizeof(list));
	if (sceNetAdhocctlGetPeerList(&length, list) < 0 || length <= 0)
		return;
	/* (the firmware links the entries it filled) */
	for (peer = list; peer && (char *)peer < (char *)list + sizeof(list); peer = peer->next)
		note_peer(peer->mac);
}

/* the MAC for an address the game names: 1 if known. A broadcast is
everyone's; an address outside 169.254/16 (network.broadcast left set for
the access point's network) is taken as one too, logged once */
static int address_mac(unsigned long address, unsigned char *mac)
{
	static int outside_logged;
	int pass, index;

	if (is_broadcast(address) || !is_adhoc_address(address))
	{
		if (!is_broadcast(address) && !outside_logged)
		{
			outside_logged = 1;
			platform_log("network: ad hoc: %u.%u.%u.%u isn't an ad hoc address (network.broadcast?): sent to every PSP in the group",
				(unsigned)(ntohl(address) >> 24), (unsigned)(ntohl(address) >> 16 & 255),
				(unsigned)(ntohl(address) >> 8 & 255), (unsigned)(ntohl(address) & 255));
		}
		memcpy(mac, broadcast_mac, 6);
		return 1;
	}
	for (pass = 0; pass < 2; pass++)
	{
		for (index = 0; index < adhoc.peer_count; index++)
		{
			if (adhoc.peers[index].address == address)
			{
				memcpy(mac, adhoc.peers[index].mac, 6);
				return 1;
			}
		}
		if (pass == 0)
			read_group();
	}
	return 0;
}

/* Winsock's sockaddr_in (a two-byte family, then the port and the address
in network order): read and written byte by byte, as the game's may sit at
any address */
static int read_address(const void *address, int length, unsigned long *ip, unsigned short *port)
{
	const unsigned char *bytes = address;

	if (!address || length < 8)
		return 0;
	*port = (unsigned short)(bytes[2] << 8 | bytes[3]);
	memcpy(ip, bytes + 4, 4);
	return 1;
}

static void write_address(void *address, int *length, unsigned long ip, unsigned short port)
{
	unsigned char bytes[16] = {0};
	int copied;

	if (!address || !length)
		return;
	bytes[0] = AF_INET;
	bytes[2] = (unsigned char)(port >> 8);
	bytes[3] = (unsigned char)port;
	memcpy(bytes + 4, &ip, 4);
	copied = *length < (int)sizeof(bytes) ? *length : (int)sizeof(bytes);
	memcpy(address, bytes, (size_t)copied);
	*length = copied;
}

/* ---------- the firmware's answers as Winsock's (negative) */

static int adhoc_error(int code)
{
	static int unknown_logged;

	switch (code)
	{
	case ADHOC_WOULD_BLOCK: case ADHOC_BUSY: return -WSAEWOULDBLOCK;
	case ADHOC_PORT_IN_USE: case ADHOC_PORT_NOT_AVAIL: return -WSAEADDRINUSE;
	case ADHOC_NOT_CONNECTED: return -WSAENOTCONN;
	case ADHOC_DISCONNECTED: return -WSAECONNRESET;
	case ADHOC_CONNECTION_REFUSED: return -WSAECONNREFUSED;
	case ADHOC_TIMEOUT: return -WSAETIMEDOUT;
	case ADHOC_INVALID_SOCKET_ID: case ADHOC_SOCKET_DELETED: return -WSAENOTSOCK;
	case ADHOC_NOT_ENOUGH_SPACE: case ADHOC_INVALID_DATALEN: return -WSAEMSGSIZE;
	case ADHOC_INVALID_ADDR: case ADHOC_INVALID_PORT: return -WSAEADDRNOTAVAIL;
	case ADHOC_SOCKET_ID_NOT_AVAIL: return -WSAEMFILE;
	case ADHOC_NOT_INITIALIZED: return -WSAENETDOWN;
	default:
		if (!unknown_logged)
		{
			unknown_logged = 1;
			platform_log("network: ad hoc: the firmware answered 0x%08x, which this doesn't know (taken as WSAEINVAL)",
				(unsigned int)code);
		}
		return -WSAEINVAL;
	}
}

/* ---------- the group */

static void adhoc_leave(void)
{
	int state = ADHOCCTL_DISCONNECTED;

	if (adhoc.adhocctl_started && sceNetAdhocctlGetState(&state) >= 0 && state != ADHOCCTL_DISCONNECTED)
	{
		unsigned int start = sceKernelGetSystemTimeLow();

		sceNetAdhocctlDisconnect();
		while (sceNetAdhocctlGetState(&state) >= 0 && state != ADHOCCTL_DISCONNECTED &&
			sceKernelGetSystemTimeLow() - start < 1000 * 1000)
		{
			sceKernelDelayThread(10 * 1000);
		}
	}
	if (adhoc.connected)
		platform_log("network: ad hoc: left the group");
	adhoc.connected = 0;
}

static void adhoc_stop(void)
{
	adhoc_leave();
	if (adhoc.adhocctl_started)
		sceNetAdhocctlTerm();
	if (adhoc.adhoc_started)
		sceNetAdhocTerm();
	if (adhoc.net_started)
		sceNetTerm();
	if (adhoc.adhoc_loaded)
		sceUtilityUnloadNetModule(PSP_NET_MODULE_ADHOC);
	if (adhoc.common_loaded)
		sceUtilityUnloadNetModule(PSP_NET_MODULE_COMMON);
	adhoc.adhocctl_started = adhoc.adhoc_started = adhoc.net_started = adhoc.adhoc_loaded = adhoc.common_loaded = 0;
}

int psp_adhoc_start(int pool_kb)
{
	struct productStruct product;
	unsigned int start = sceKernelGetSystemTimeLow();
	int free_kb = (int)sceKernelTotalFreeMemSize() / 1024;
	int largest_kb = (int)sceKernelMaxFreeMemSize() / 1024;
	int result = sceUtilityLoadNetModule(PSP_NET_MODULE_COMMON);

	adhoc.common_loaded = result >= 0;
	if (result >= 0)
	{
		result = sceUtilityLoadNetModule(PSP_NET_MODULE_ADHOC);
		adhoc.adhoc_loaded = result >= 0;
	}
	if (result >= 0)
	{
		result = sceNetInit(pool_kb * 1024, psp_net_stack_priority(), 4 * 1024,
			psp_net_stack_priority(), 4 * 1024);
		adhoc.net_started = result >= 0;
	}
	if (result >= 0)
	{
		result = sceNetAdhocInit();
		adhoc.adhoc_started = result >= 0;
	}
	if (result >= 0)
	{
		memset(&product, 0, sizeof(product));
		memcpy(product.product, ADHOC_PRODUCT, sizeof(product.product));
		result = sceNetAdhocctlInit(0x2000, 0x30, &product);
		adhoc.adhocctl_started = result >= 0;
	}
	if (result < 0)
	{
		platform_log("network: ad hoc: the modules didn't start (0x%08x); user memory free %d KB (largest block %d KB): no network play this run",
			(unsigned int)result, free_kb, largest_kb);
		adhoc_stop();
		return result;
	}
	/* the loopback's buffers: from the user RAM outside the heap when there's
	room (the modules' neighbours there; a PSP host's heap peaked at 4,148
	of 4,152 KB in the emulator's first ad hoc game),
	else from the heap */
	{
		unsigned long streams = LOOP_STREAMS * sizeof(*adhoc.streams);
		unsigned long bytes = streams + LOOP_DATAGRAMS * sizeof(*adhoc.datagrams);
		unsigned char *memory = NULL;
		SceUID block = -1;

		if ((unsigned long)sceKernelMaxFreeMemSize() > bytes + 64 &&
			(unsigned long)sceKernelTotalFreeMemSize() > bytes + 64 + 160 * 1024)
		{
			block = sceKernelAllocPartitionMemory(2, "halo ad hoc loopback", PSP_SMEM_High, bytes + 64, NULL);
		}
		if (block >= 0)
		{
			memory = (unsigned char *)(((unsigned long)sceKernelGetBlockHeadAddr(block) + 63) & ~63ul);
			memset(memory, 0, bytes);
		}
		else
			memory = calloc(1, bytes);
		if (!memory)
		{
			platform_log("network: ad hoc: no memory for the loopback (%lu KB): no network play this run", bytes / 1024);
			adhoc_stop();
			return -1;
		}
		adhoc.streams = (void *)memory;   /* (64-aligned; each struct a multiple of 4 bytes) */
		adhoc.datagrams = (void *)(memory + streams);
		platform_log("network: ad hoc: the loopback's %lu KB %s", bytes / 1024,
			block >= 0 ? "outside the heap" : "from the heap");
	}
	{
		int index;

		for (index = 0; index < LOOP_DATAGRAMS; index++)
			adhoc.datagrams[index].socket = -1;
	}
	adhoc.last_state = -1;
	sceWlanGetEtherAddr(adhoc.mac);
	adhoc.address = mac_address(adhoc.mac);
	adhoc.next_port = FIRST_EPHEMERAL_PORT;
	platform_log("network: ad hoc: the modules and the stack started in %u ms (a %d KB pool); user memory free %d KB (largest block %d KB), was %d KB (%d KB)",
		(sceKernelGetSystemTimeLow() - start) / 1000, pool_kb, (int)sceKernelTotalFreeMemSize() / 1024,
		(int)sceKernelMaxFreeMemSize() / 1024, free_kb, largest_kb);
	return 0;
}

/* in the group, joining it now if need be: 0, or negative */
static int adhoc_join(void)
{
	unsigned int start;
	int result, state, last_state = -1;

	if (adhoc.connected)
		return 0;
	if (!adhoc.adhocctl_started)
	{
		if (!adhoc.start_failure_logged)
			platform_log("network: ad hoc: no network stack (it didn't start at start-up)");
		adhoc.start_failure_logged = 1;
		return -1;
	}
	if (adhoc.failed_at && sceKernelGetSystemTimeLow() - adhoc.failed_at < JOIN_RETRY_US)
		return -1;
	if (sceWlanGetSwitchState() != 1)
	{
		platform_log("network: the WLAN switch is off");
		adhoc.failed_at = sceKernelGetSystemTimeLow() | 1;
		return -1;
	}
	platform_log("network: ad hoc: joining group %s (%s), as %02x:%02x:%02x:%02x:%02x:%02x", ADHOC_GROUP, ADHOC_PRODUCT,
		adhoc.mac[0], adhoc.mac[1], adhoc.mac[2], adhoc.mac[3], adhoc.mac[4], adhoc.mac[5]);
	start = sceKernelGetSystemTimeLow();
	result = sceNetAdhocctlConnect(ADHOC_GROUP);
	while (result >= 0)
	{
		state = ADHOCCTL_DISCONNECTED;
		result = sceNetAdhocctlGetState(&state);
		if (result < 0)
			break;
		if (state != last_state)
		{
			platform_log("network: ad hoc: state %d", state);
			last_state = state;
		}
		if (state == ADHOCCTL_CONNECTED)
			break;
		if (sceKernelGetSystemTimeLow() - start > JOIN_TIMEOUT_US)
		{
			platform_log("network: ad hoc: not in the group after %d s", JOIN_TIMEOUT_US / 1000000);
			result = -1;
			break;
		}
		sceKernelDelayThread(50 * 1000);
	}
	if (result < 0)
	{
		if (result != -1)
			platform_log("network: ad hoc: joining failed (0x%08x)%s", (unsigned int)result,
				result == ADHOCCTL_WLAN_SWITCH_OFF ? ": the WLAN switch is off" : "");
		adhoc_leave();
		adhoc.failed_at = sceKernelGetSystemTimeLow() | 1;
		return -1;
	}
	adhoc.connected = 1;
	adhoc.failed_at = 0;
	adhoc.last_state = ADHOCCTL_CONNECTED;
	platform_log("network: ad hoc: in group %s as %u.%u.%u.%u, up in %u ms", ADHOC_GROUP,
		(unsigned)(ntohl(adhoc.address) >> 24), (unsigned)(ntohl(adhoc.address) >> 16 & 255),
		(unsigned)(ntohl(adhoc.address) >> 8 & 255), (unsigned)(ntohl(adhoc.address) & 255),
		(sceKernelGetSystemTimeLow() - start) / 1000);
	read_group();
	return 0;
}

unsigned long psp_adhoc_local_address(void)
{
	return adhoc_join() < 0 ? 0 : adhoc.address;
}

void psp_adhoc_heartbeat(void)
{
	SceNetMallocStat stat;
	int state = ADHOCCTL_CONNECTED;

	if (!adhoc.connected || sceNetGetMallocStat(&stat) < 0)
		return;
	if (sceNetAdhocctlGetState(&state) < 0)
		state = -1;
	platform_log("network: the stack's pool %d of %d KB free (%d KB at most in use); ad hoc, %d PSPs heard%s",
		stat.free / 1024, stat.pool / 1024, stat.maximum / 1024, adhoc.peer_count,
		state != adhoc.last_state ? (state == ADHOCCTL_CONNECTED ? "; back in the group" : "; out of the group") : "");
	adhoc.last_state = state;
}

/* ---------- the sockets */

static struct adhoc_socket *find(int socket)
{
	int index = socket - PSP_ADHOC_SOCKET_BASE;

	if (index < 0 || index >= ADHOC_SOCKETS || !adhoc.sockets[index].used)
		return NULL;
	return &adhoc.sockets[index];
}

static int number(const struct adhoc_socket *s)
{
	return PSP_ADHOC_SOCKET_BASE + (int)(s - adhoc.sockets);
}

static struct adhoc_socket *allocate(int type)
{
	int index;

	for (index = 0; index < ADHOC_SOCKETS; index++)
	{
		struct adhoc_socket *s = &adhoc.sockets[index];

		if (!s->used)
		{
			memset(s, 0, sizeof(*s));
			s->used = 1;
			s->type = type;
			s->id = -1;
			s->pending_id = -1;
			s->loop_in = s->loop_out = s->pending_loop = -1;
			return s;
		}
	}
	return NULL;
}

static unsigned short ephemeral_port(void)
{
	unsigned short port = adhoc.next_port;

	adhoc.next_port = adhoc.next_port == 65535 ? FIRST_EPHEMERAL_PORT : adhoc.next_port + 1;
	return port;
}

/* a PTP connection's buffers: PTP_BUFFER_KB, or keep word netbufN */
static unsigned int ptp_buffer_bytes(void)
{
	static unsigned int bytes;

	if (!bytes)
	{
		int kb = psp_keep_number("netbuf", PTP_BUFFER_KB);

		bytes = (unsigned int)(kb < 2 ? 2 : kb > 64 ? 64 : kb) * 1024;
	}
	return bytes;
}

/* ---------- the loopback (this PSP to itself) */

static int index_of(const struct adhoc_socket *s)
{
	return (int)(s - adhoc.sockets);
}

/* the bound datagram socket on a port, or NULL */
static struct adhoc_socket *datagram_socket_on(unsigned short port)
{
	int index;

	for (index = 0; index < ADHOC_SOCKETS; index++)
	{
		struct adhoc_socket *s = &adhoc.sockets[index];

		if (s->used && s->type == SOCK_DGRAM && s->port == port)
			return s;
	}
	return NULL;
}

/* a datagram to this PSP: queued for the socket on the port (none there,
or the queue full: dropped, as UDP may) */
static int loop_send_datagram(struct adhoc_socket *from, const void *buffer, int length, unsigned short port)
{
	struct adhoc_socket *to = datagram_socket_on(port);
	int index;

	if (length > LOOP_DATAGRAM_BYTES)
		return -WSAEMSGSIZE;
	if (!from->port)
		from->port = ephemeral_port();
	if (!to)
		return length;
	for (index = 0; index < LOOP_DATAGRAMS; index++)
	{
		struct loop_datagram *d = &adhoc.datagrams[index];

		if (d->socket < 0)
		{
			d->socket = index_of(to);
			d->sequence = adhoc.datagram_sequence++;
			d->from_port = from->port;
			d->length = length;
			memcpy(d->data, buffer, (size_t)length);
			return length;
		}
	}
	return length;
}

/* the oldest datagram waiting for a socket, or NULL */
static struct loop_datagram *loop_datagram_for(const struct adhoc_socket *s)
{
	struct loop_datagram *oldest = NULL;
	int index;

	for (index = 0; index < LOOP_DATAGRAMS; index++)
	{
		struct loop_datagram *d = &adhoc.datagrams[index];

		if (d->socket == index_of(s) && (!oldest || (int)(d->sequence - oldest->sequence) < 0))
			oldest = d;
	}
	return oldest;
}

static int new_stream(void)
{
	int index;

	for (index = 0; index < LOOP_STREAMS; index++)
	{
		struct loop_stream *stream = &adhoc.streams[index];

		if (!stream->used)
		{
			stream->used = 1;
			stream->head = stream->count = 0;
			stream->writer_closed = stream->reader_closed = 0;
			return index;
		}
	}
	return -1;
}

static void release_stream(int index, int writer)
{
	struct loop_stream *stream;

	if (index < 0)
		return;
	stream = &adhoc.streams[index];
	if (writer)
		stream->writer_closed = 1;
	else
		stream->reader_closed = 1;
	if (stream->writer_closed && stream->reader_closed)
		stream->used = 0;
}

/* a connection to this PSP's own listener on the port: both ends made now,
the listener's handed over at its accept */
static int loop_connect(struct adhoc_socket *s, unsigned short port)
{
	struct adhoc_socket *listener = NULL, *server;
	int index, up, down;

	for (index = 0; index < ADHOC_SOCKETS; index++)
	{
		struct adhoc_socket *l = &adhoc.sockets[index];

		if (l->used && l->type == SOCK_STREAM && l->listening && l->port == port)
			listener = l;
	}
	if (!listener || listener->pending_loop >= 0)
		return -WSAECONNREFUSED;
	up = new_stream();
	down = new_stream();
	server = allocate(SOCK_STREAM);
	if (up < 0 || down < 0 || !server)
	{
		if (up >= 0)
			adhoc.streams[up].used = 0;
		if (down >= 0)
			adhoc.streams[down].used = 0;
		if (server)
			server->used = 0;
		return -WSAENOBUFS;
	}
	if (!s->port)
		s->port = ephemeral_port();
	s->loop_out = server->loop_in = up;
	s->loop_in = server->loop_out = down;
	server->port = listener->port;
	server->connected = server->has_peer = 1;
	server->peer_port = s->port;
	s->has_peer = 1;
	s->peer_port = port;
	listener->pending_loop = index_of(server);
	/* (made at once. A non-blocking socket's is reported as under way, the
	answer the game waits on for every connection: writeable at the next
	select) */
	if (!s->nonblocking)
	{
		s->connected = 1;
		return 0;
	}
	s->connecting = 1;
	s->connect_started = sceKernelGetSystemTimeLow();
	return -WSAEWOULDBLOCK;
}

static int loop_write(struct adhoc_socket *s, const void *buffer, int length)
{
	struct loop_stream *stream = &adhoc.streams[s->loop_out];
	int written = 0;

	for (;;)
	{
		if (stream->reader_closed)
			return -WSAECONNRESET;
		while (written < length && stream->count < LOOP_STREAM_BYTES)
		{
			stream->data[(stream->head + stream->count) % LOOP_STREAM_BYTES] = ((const unsigned char *)buffer)[written];
			stream->count++;
			written++;
		}
		if (written || !length)
			return written;
		if (s->nonblocking)
			return -WSAEWOULDBLOCK;
		sceKernelDelayThread(1000);
	}
}

static int loop_read(struct adhoc_socket *s, void *buffer, int length)
{
	struct loop_stream *stream = &adhoc.streams[s->loop_in];
	int read = 0;

	for (;;)
	{
		while (read < length && stream->count > 0)
		{
			((unsigned char *)buffer)[read++] = stream->data[stream->head];
			stream->head = (stream->head + 1) % LOOP_STREAM_BYTES;
			stream->count--;
		}
		if (read)
			return read;
		/* (the other end closed and all read: the stream's end) */
		if (stream->writer_closed)
			return 0;
		if (s->nonblocking)
			return -WSAEWOULDBLOCK;
		sceKernelDelayThread(1000);
	}
}

/* a PDP socket's firmware socket, made on its port (an ephemeral one if
none was bound) */
static int pdp_ready(struct adhoc_socket *s)
{
	int id, tries;

	if (s->id >= 0)
		return 0;
	for (tries = 0; tries < 16; tries++)
	{
		if (!s->port)
			s->port = ephemeral_port();
		id = sceNetAdhocPdpCreate(adhoc.mac, s->port, PDP_BUFFER_BYTES, 0);
		if (id >= 0)
		{
			s->id = id;
			return 0;
		}
		/* (an ephemeral port in use: the next) */
		if (id != ADHOC_PORT_IN_USE || s->port < FIRST_EPHEMERAL_PORT)
			return adhoc_error(id);
		s->port = 0;
	}
	return -WSAEADDRINUSE;
}

int psp_adhoc_socket(int family, int type, int protocol)
{
	struct adhoc_socket *s;

	(void)protocol;
	if (adhoc_join() < 0)
		return -WSAENETDOWN;
	if (family != AF_INET || (type != SOCK_DGRAM && type != SOCK_STREAM))
		return -WSAEAFNOSUPPORT;
	s = allocate(type);
	if (!s)
		return -WSAEMFILE;
	return number(s);
}

int psp_adhoc_close(int socket)
{
	struct adhoc_socket *s = find(socket);

	if (!s)
		return -WSAENOTSOCK;
	if (s->id >= 0)
	{
		if (s->type == SOCK_DGRAM)
			sceNetAdhocPdpDelete(s->id, 0);
		else
			sceNetAdhocPtpClose(s->id, 0);
	}
	if (s->pending_id >= 0)
		sceNetAdhocPtpClose(s->pending_id, 0);
	/* the loopback: its streams' ends, a waiting connection, its datagrams */
	release_stream(s->loop_out, 1);
	release_stream(s->loop_in, 0);
	if (s->pending_loop >= 0)
		psp_adhoc_close(PSP_ADHOC_SOCKET_BASE + s->pending_loop);
	{
		int index;

		for (index = 0; index < LOOP_DATAGRAMS; index++)
			if (adhoc.datagrams[index].socket == index_of(s))
				adhoc.datagrams[index].socket = -1;
	}
	s->used = 0;
	return 0;
}

int psp_adhoc_bind(int socket, const void *address, int address_length)
{
	struct adhoc_socket *s = find(socket);
	unsigned long ip;
	unsigned short port;

	if (!s)
		return -WSAENOTSOCK;
	if (!read_address(address, address_length, &ip, &port))
		return -WSAEFAULT;
	if (s->port)
		return -WSAEINVAL;
	s->port = port;
	/* a datagram socket's firmware socket now: it receives from here on */
	return s->type == SOCK_DGRAM ? pdp_ready(s) : 0;
}

int psp_adhoc_connect(int socket, const void *address, int address_length)
{
	struct adhoc_socket *s = find(socket);
	unsigned long ip;
	unsigned short port;
	int result;

	if (!s)
		return -WSAENOTSOCK;
	if (!read_address(address, address_length, &ip, &port))
		return -WSAEFAULT;
	if (is_loopback(ip))
	{
		if (s->type == SOCK_DGRAM)
		{
			memset(s->peer_mac, 0, 6);
			s->peer_port = port;
			s->has_peer = 2;
			return 0;
		}
		if (s->connected || s->connecting)
			return s->connected ? -WSAEISCONN : -WSAEALREADY;
		return loop_connect(s, port);
	}
	if (!address_mac(ip, s->peer_mac) || !memcmp(s->peer_mac, broadcast_mac, 6))
		return -WSAEHOSTUNREACH;
	s->peer_port = port;
	s->has_peer = 1;
	if (s->type == SOCK_DGRAM)
		return pdp_ready(s);
	if (s->connected)
		return -WSAEISCONN;
	if (s->connecting)
		return -WSAEALREADY;
	if (!s->port)
		s->port = ephemeral_port();
	s->id = sceNetAdhocPtpOpen(adhoc.mac, s->port, s->peer_mac, port, ptp_buffer_bytes(), PTP_RETRY_US,
		PTP_RETRY_COUNT, 0);
	if (s->id < 0)
	{
		result = s->id;
		s->id = -1;
		return adhoc_error(result);
	}
	s->connect_started = sceKernelGetSystemTimeLow();
	result = sceNetAdhocPtpConnect(s->id, s->nonblocking ? 0 : CONNECT_TIMEOUT_US, s->nonblocking);
	if (result >= 0)
	{
		s->connected = 1;
		return 0;
	}
	if (s->nonblocking && (result == ADHOC_WOULD_BLOCK || result == ADHOC_BUSY))
	{
		/* under way: select reports it writeable once made (Winsock's
		answer, which the game waits on) */
		s->connecting = 1;
		return -WSAEWOULDBLOCK;
	}
	return adhoc_error(result);
}

int psp_adhoc_listen(int socket, int backlog)
{
	struct adhoc_socket *s = find(socket);
	int id;

	if (!s)
		return -WSAENOTSOCK;
	if (s->type != SOCK_STREAM)
		return -WSAEOPNOTSUPP;
	if (s->listening)
		return 0;
	if (!s->port)
		s->port = ephemeral_port();
	s->backlog = backlog < 1 ? 1 : backlog > 16 ? 16 : backlog;
	id = sceNetAdhocPtpListen(adhoc.mac, s->port, ptp_buffer_bytes(), PTP_RETRY_US, PTP_RETRY_COUNT, s->backlog, 0);
	if (id < 0)
		return adhoc_error(id);
	s->id = id;
	s->listening = 1;
	return 0;
}

/* a listener's next connection, accepted without waiting: 1 if there's one
(in pending_id), 0 if not, or minus an error */
static int accept_now(struct adhoc_socket *s)
{
	int id;

	if (s->pending_id >= 0)
		return 1;
	id = sceNetAdhocPtpAccept(s->id, s->pending_mac, &s->pending_port, 0, 1);
	if (id >= 0)
	{
		s->pending_id = id;
		note_peer(s->pending_mac);
		return 1;
	}
	return id == ADHOC_WOULD_BLOCK || id == ADHOC_BUSY || id == ADHOC_TIMEOUT ? 0 : adhoc_error(id);
}

int psp_adhoc_accept(int socket, void *address, int *address_length)
{
	struct adhoc_socket *s = find(socket), *c;
	int result;

	if (!s)
		return -WSAENOTSOCK;
	if (!s->listening)
		return -WSAEINVAL;
	/* this PSP's own client first (the loopback) */
	if (s->pending_loop >= 0)
	{
		c = &adhoc.sockets[s->pending_loop];
		s->pending_loop = -1;
		write_address(address, address_length, LOOPBACK_ADDRESS, c->peer_port);
		return number(c);
	}
	while ((result = accept_now(s)) == 0)
	{
		if (s->nonblocking)
			return -WSAEWOULDBLOCK;
		sceKernelDelayThread(1000);
	}
	if (result < 0)
		return result;
	c = allocate(SOCK_STREAM);
	if (!c)
		return -WSAEMFILE;
	c->id = s->pending_id;
	c->port = s->port;
	c->connected = 1;
	c->has_peer = 1;
	memcpy(c->peer_mac, s->pending_mac, 6);
	c->peer_port = s->pending_port;
	s->pending_id = -1;
	write_address(address, address_length, mac_address(c->peer_mac), c->peer_port);
	return number(c);
}

static int send_datagram(struct adhoc_socket *s, const void *buffer, int length, const unsigned char *mac,
	unsigned short port)
{
	int result = pdp_ready(s);

	if (result < 0)
		return result;
	result = sceNetAdhocPdpSend(s->id, (unsigned char *)mac, port, (void *)buffer, (unsigned int)length, 0, 1);
	return result < 0 ? adhoc_error(result) : length;
}

int psp_adhoc_send(int socket, const void *buffer, int length)
{
	struct adhoc_socket *s = find(socket);
	int sent, result;

	if (!s)
		return -WSAENOTSOCK;
	if (s->type == SOCK_DGRAM)
	{
		if (s->has_peer == 2)
			return loop_send_datagram(s, buffer, length, s->peer_port);
		return s->has_peer ? send_datagram(s, buffer, length, s->peer_mac, s->peer_port) : -WSAENOTCONN;
	}
	if (s->loop_out >= 0)
		return loop_write(s, buffer, length);
	if (!s->connected)
		return -WSAENOTCONN;
	for (;;)
	{
		sent = length;
		result = sceNetAdhocPtpSend(s->id, (void *)buffer, &sent, 0, 1);
		if (result >= 0)
			return sent;
		if (result != ADHOC_WOULD_BLOCK || s->nonblocking)
			return adhoc_error(result);
		sceKernelDelayThread(1000);
	}
}

int psp_adhoc_sendto(int socket, const void *buffer, int length, const void *address, int address_length)
{
	struct adhoc_socket *s = find(socket);
	unsigned char mac[6];
	unsigned long ip;
	unsigned short port;

	if (!s)
		return -WSAENOTSOCK;
	if (!address || s->type == SOCK_STREAM)
		return psp_adhoc_send(socket, buffer, length);
	if (!read_address(address, address_length, &ip, &port))
		return -WSAEFAULT;
	if (is_loopback(ip))
		return loop_send_datagram(s, buffer, length, port);
	if (!address_mac(ip, mac))
		return -WSAEHOSTUNREACH;
	return send_datagram(s, buffer, length, mac, port);
}

int psp_adhoc_recvfrom(int socket, void *buffer, int length, void *address, int *address_length)
{
	struct adhoc_socket *s = find(socket);
	unsigned char mac[6];
	unsigned short port;
	int received, result;

	if (!s)
		return -WSAENOTSOCK;
	if (s->type == SOCK_STREAM)
	{
		result = psp_adhoc_recv(socket, buffer, length);
		if (result >= 0)
			write_address(address, address_length, s->loop_in >= 0 ? LOOPBACK_ADDRESS : mac_address(s->peer_mac),
				s->peer_port);
		return result;
	}
	for (;;)
	{
		/* this PSP's own datagrams first (the loopback): from 127.0.0.1 */
		struct loop_datagram *d = loop_datagram_for(s);

		if (d)
		{
			received = d->length < length ? d->length : length;
			memcpy(buffer, d->data, (size_t)received);
			write_address(address, address_length, LOOPBACK_ADDRESS, d->from_port);
			d->socket = -1;
			return d->length > length ? -WSAEMSGSIZE : received;
		}
		if (s->id < 0)
		{
			if (s->nonblocking || !s->port)
				return s->port ? -WSAEWOULDBLOCK : -WSAEINVAL;
			sceKernelDelayThread(1000);
			continue;
		}
		received = length;
		result = sceNetAdhocPdpRecv(s->id, mac, &port, buffer, &received, 0, 1);
		if (result >= 0)
			break;
		if (result != ADHOC_WOULD_BLOCK || s->nonblocking)
			return adhoc_error(result);
		sceKernelDelayThread(1000);
	}
	note_peer(mac);
	write_address(address, address_length, mac_address(mac), port);
	return received;
}

int psp_adhoc_recv(int socket, void *buffer, int length)
{
	struct adhoc_socket *s = find(socket);
	int received, result;

	if (!s)
		return -WSAENOTSOCK;
	if (s->type == SOCK_DGRAM)
		return psp_adhoc_recvfrom(socket, buffer, length, NULL, NULL);
	if (s->loop_in >= 0)
		return loop_read(s, buffer, length);
	if (!s->connected)
		return -WSAENOTCONN;
	for (;;)
	{
		received = length;
		result = sceNetAdhocPtpRecv(s->id, buffer, &received, 0, 1);
		if (result >= 0)
			return received;
		/* the peer gone: the stream's end, as a closed TCP connection reads */
		if (result == ADHOC_DISCONNECTED || result == ADHOC_NOT_CONNECTED)
			return 0;
		if (result != ADHOC_WOULD_BLOCK || s->nonblocking)
			return adhoc_error(result);
		sceKernelDelayThread(1000);
	}
}

int psp_adhoc_shutdown(int socket)
{
	struct adhoc_socket *s = find(socket);

	if (!s)
		return -WSAENOTSOCK;
	/* (what was sent goes out first; the close itself is the game's) */
	if (s->type == SOCK_STREAM && s->connected)
		sceNetAdhocPtpFlush(s->id, 0, 1);
	return 0;
}

int psp_adhoc_set_nonblocking(int socket, int nonblocking)
{
	struct adhoc_socket *s = find(socket);

	if (!s)
		return -WSAENOTSOCK;
	s->nonblocking = nonblocking != 0;
	return 0;
}

/* ---------- what the firmware reports of its sockets */

static pdpStatStruct pdp_stats[ADHOC_SOCKETS];
static ptpStatStruct ptp_stats[ADHOC_SOCKETS];
static int pdp_stat_count, ptp_stat_count;

static void read_stats(void)
{
	int size = (int)sizeof(pdp_stats);

	pdp_stat_count = ptp_stat_count = 0;
	if (sceNetAdhocGetPdpStat(&size, pdp_stats) >= 0 && size > 0)
		pdp_stat_count = size / (int)sizeof(pdp_stats[0]);
	size = (int)sizeof(ptp_stats);
	if (sceNetAdhocGetPtpStat(&size, ptp_stats) >= 0 && size > 0)
		ptp_stat_count = size / (int)sizeof(ptp_stats[0]);
}

static const pdpStatStruct *pdp_stat(int id)
{
	int index;

	for (index = 0; index < pdp_stat_count; index++)
		if (pdp_stats[index].pdpId == id)
			return &pdp_stats[index];
	return NULL;
}

static const ptpStatStruct *ptp_stat(int id)
{
	int index;

	for (index = 0; index < ptp_stat_count; index++)
		if (ptp_stats[index].ptpId == id)
			return &ptp_stats[index];
	return NULL;
}

int psp_adhoc_bytes_available(int socket, unsigned long *count)
{
	struct adhoc_socket *s = find(socket);
	unsigned long bytes = 0;

	if (!s)
		return -WSAENOTSOCK;
	read_stats();
	if (s->type == SOCK_DGRAM && loop_datagram_for(s))
		bytes = (unsigned long)loop_datagram_for(s)->length;
	else if (s->loop_in >= 0)
		bytes = (unsigned long)adhoc.streams[s->loop_in].count;
	else if (s->id >= 0 && s->type == SOCK_DGRAM && pdp_stat(s->id))
		bytes = pdp_stat(s->id)->rcvdData;
	else if (s->id >= 0 && s->type == SOCK_STREAM && ptp_stat(s->id))
		bytes = ptp_stat(s->id)->rcvdData;
	if (count)
		*count = bytes;
	return 0;
}

/* SO_TYPE and SO_ERROR answered; the rest (buffers, broadcast, reuse,
linger) have nothing to set here, as the IP stack's Xbox-only options don't */
int psp_adhoc_setsockopt(int socket, int level, int name, const void *value, int length)
{
	(void)level; (void)name; (void)value; (void)length;
	return find(socket) ? 0 : -WSAENOTSOCK;
}

int psp_adhoc_getsockopt(int socket, int level, int name, void *value, int *length)
{
	struct adhoc_socket *s = find(socket);
	int answer;

	if (!s)
		return -WSAENOTSOCK;
	if (level != SOL_SOCKET || (name != SO_TYPE && name != SO_ERROR) || !value || !length || *length < (int)sizeof(int))
		return -WSAENOPROTOOPT;
	answer = name == SO_TYPE ? s->type : 0;
	memcpy(value, &answer, sizeof(answer));
	*length = (int)sizeof(answer);
	return 0;
}

int psp_adhoc_getsockname(int socket, void *address, int *address_length)
{
	struct adhoc_socket *s = find(socket);

	if (!s)
		return -WSAENOTSOCK;
	write_address(address, address_length, s->loop_in >= 0 ? LOOPBACK_ADDRESS : adhoc.address, s->port);
	return 0;
}

int psp_adhoc_getpeername(int socket, void *address, int *address_length)
{
	struct adhoc_socket *s = find(socket);

	if (!s)
		return -WSAENOTSOCK;
	if (!s->has_peer)
		return -WSAENOTCONN;
	write_address(address, address_length, s->loop_in >= 0 || s->has_peer == 2 ? LOOPBACK_ADDRESS : mac_address(s->peer_mac),
		s->peer_port);
	return 0;
}

/* ---------- select */

/* readable: a datagram or stream bytes waiting, a connection to accept, or
a stream whose peer is gone (its read gives the end) */
static int readable(struct adhoc_socket *s)
{
	const ptpStatStruct *stat;

	if (s->type == SOCK_DGRAM && loop_datagram_for(s))
		return 1;
	if (s->loop_in >= 0)
		return adhoc.streams[s->loop_in].count > 0 || adhoc.streams[s->loop_in].writer_closed;
	if (s->listening && s->pending_loop >= 0)
		return 1;
	if (s->id < 0)
		return 0;
	if (s->type == SOCK_DGRAM)
		return pdp_stat(s->id) && pdp_stat(s->id)->rcvdData > 0;
	if (s->listening)
		return accept_now(s) != 0;
	stat = ptp_stat(s->id);
	return !stat || stat->rcvdData > 0 || (s->connected && stat->unk1 == PTP_CLOSED);
}

/* writeable: 1; not yet: 0; a connection that failed: minus its error */
static int writeable(struct adhoc_socket *s)
{
	const ptpStatStruct *stat;

	if (s->type == SOCK_DGRAM)
		return 1;
	if (s->loop_out >= 0)
	{
		/* (a loopback connection is made at once) */
		if (s->connecting)
		{
			s->connecting = 0;
			s->connected = 1;
		}
		return adhoc.streams[s->loop_out].reader_closed || adhoc.streams[s->loop_out].count < LOOP_STREAM_BYTES;
	}
	if (s->connected)
	{
		stat = ptp_stat(s->id);
		return !stat || stat->sentData < ptp_buffer_bytes();
	}
	if (!s->connecting)
		return 0;
	stat = ptp_stat(s->id);
	if (stat && stat->unk1 == PTP_ESTABLISHED)
	{
		s->connecting = 0;
		s->connected = 1;
		note_peer(s->peer_mac);
		return 1;
	}
	/* (closed again, given a second to start: refused) */
	if ((!stat || stat->unk1 == PTP_CLOSED) && sceKernelGetSystemTimeLow() - s->connect_started > 1000 * 1000)
	{
		s->connecting = 0;
		return -WSAECONNREFUSED;
	}
	if (sceKernelGetSystemTimeLow() - s->connect_started > CONNECT_TIMEOUT_US)
	{
		s->connecting = 0;
		return -WSAETIMEDOUT;
	}
	return 0;
}

int psp_adhoc_select(int *read, int *read_count, int *write, int *write_count, int *error, int *error_count,
	long timeout_seconds, long timeout_microseconds, int infinite, int *connect_error)
{
	unsigned int start = sceKernelGetSystemTimeLow();
	unsigned int wait = (unsigned int)(timeout_seconds * 1000000 + timeout_microseconds);
	/* (the lists asked, kept: each pass writes the ready ones over them) */
	int asked_read[64], asked_write[64];
	int reads = read && read_count ? (*read_count < 64 ? *read_count : 64) : 0;
	int writes = write && write_count ? (*write_count < 64 ? *write_count : 64) : 0;

	if (reads)
		memcpy(asked_read, read, sizeof(int) * (size_t)reads);
	if (writes)
		memcpy(asked_write, write, sizeof(int) * (size_t)writes);
	for (;;)
	{
		int ready = 0, kept_read = 0, kept_write = 0, kept_error = 0, index;

		read_stats();
		for (index = 0; index < reads; index++)
		{
			struct adhoc_socket *s = find(asked_read[index]);

			if (s && readable(s))
				read[kept_read++] = asked_read[index];
		}
		for (index = 0; index < writes; index++)
		{
			struct adhoc_socket *s = find(asked_write[index]);
			int answer = s ? writeable(s) : 0;

			if (answer > 0)
				write[kept_write++] = asked_write[index];
			else if (answer < 0)
			{
				/* (a failed connection: Winsock's error set, where the game
				looks; psp_net.c's select does the same for the IP stack) */
				if (error && error_count && kept_error < 64)
					error[kept_error++] = asked_write[index];
				if (connect_error)
					*connect_error = -answer;
			}
		}
		/* (nothing else is reported in the error set) */
		ready = kept_read + kept_write + kept_error;
		if (ready || (!infinite && sceKernelGetSystemTimeLow() - start >= wait))
		{
			if (read_count)
				*read_count = kept_read;
			if (write_count)
				*write_count = kept_write;
			if (error_count)
				*error_count = kept_error;
			return ready;
		}
		sceKernelDelayThread(1000);
	}
}
