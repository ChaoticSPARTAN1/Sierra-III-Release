/* psp_net.h - what the multiplayer build's two network layers share

psp_net.c answers xnet.c's posix_socket_* calls over Wi-Fi through an access
point (the firmware's IP stack). psp_net_adhoc.c answers the same calls over
ad hoc Wi-Fi, PSP to PSP with no access point (the firmware's PDP and PTP),
when keep.txt says "adhoc". psp_net.c chooses once, at start-up, and hands
each call to the ad hoc layer in that mode. */
#ifndef PSP_NET_H
#define PSP_NET_H

/* ---------- Winsock's error codes (as posix_net.c has them): what the game
reads after a call that failed */
enum
{
	WSAEINTR = 10004,
	WSAEBADF = 10009,
	WSAEACCES = 10013,
	WSAEFAULT = 10014,
	WSAEINVAL = 10022,
	WSAEMFILE = 10024,
	WSAEWOULDBLOCK = 10035,
	WSAEINPROGRESS = 10036,
	WSAEALREADY = 10037,
	WSAENOTSOCK = 10038,
	WSAEDESTADDRREQ = 10039,
	WSAEMSGSIZE = 10040,
	WSAEPROTOTYPE = 10041,
	WSAENOPROTOOPT = 10042,
	WSAEPROTONOSUPPORT = 10043,
	WSAEOPNOTSUPP = 10045,
	WSAEAFNOSUPPORT = 10047,
	WSAEADDRINUSE = 10048,
	WSAEADDRNOTAVAIL = 10049,
	WSAENETDOWN = 10050,
	WSAENETUNREACH = 10051,
	WSAENETRESET = 10052,
	WSAECONNABORTED = 10053,
	WSAECONNRESET = 10054,
	WSAENOBUFS = 10055,
	WSAEISCONN = 10056,
	WSAENOTCONN = 10057,
	WSAESHUTDOWN = 10058,
	WSAETIMEDOUT = 10060,
	WSAECONNREFUSED = 10061,
	WSAEHOSTDOWN = 10064,
	WSAEHOSTUNREACH = 10065,
};

void platform_log(const char *format, ...);
/* psp_platform.c's keep words: one word; a prefix and a number */
int psp_keep(const char *name);
int psp_keep_number(const char *prefix, int fallback);
/* whether keep.txt has been read yet */
int psp_keep_loaded(void);
/* the priority of the stack's own threads (keep word netprioN), and a log
line naming them as the firmware has them */
int psp_net_stack_priority(void);
void psp_net_log_threads(void);
/* after each game tick: keep word netyieldN's wait */
void psp_net_tick(void);

/* ---------- the ad hoc layer (psp_net_adhoc.c)

Each call below returns what the posix_socket_* call it stands for returns
on success, or minus a Winsock error code on failure (psp_net.c turns that
into -1 and the last error). Socket numbers start at PSP_ADHOC_SOCKET_BASE,
so neither layer's can be taken for the other's. */
enum { PSP_ADHOC_SOCKET_BASE = 0x4000 };

/* the modules (COMMON, then ADHOC) and the stack, at start-up; 0 or negative
(the log says why). pool_kb: sceNetInit's pool */
int psp_adhoc_start(int pool_kb);
/* the heartbeat's network line */
void psp_adhoc_heartbeat(void);
/* this machine's address (network byte order), joining the group if need
be; 0 when there's none */
unsigned long psp_adhoc_local_address(void);

int psp_adhoc_socket(int family, int type, int protocol);
int psp_adhoc_close(int socket);
int psp_adhoc_bind(int socket, const void *address, int address_length);
int psp_adhoc_connect(int socket, const void *address, int address_length);
int psp_adhoc_listen(int socket, int backlog);
int psp_adhoc_accept(int socket, void *address, int *address_length);
int psp_adhoc_send(int socket, const void *buffer, int length);
int psp_adhoc_sendto(int socket, const void *buffer, int length, const void *address, int address_length);
int psp_adhoc_recv(int socket, void *buffer, int length);
int psp_adhoc_recvfrom(int socket, void *buffer, int length, void *address, int *address_length);
int psp_adhoc_shutdown(int socket);
int psp_adhoc_set_nonblocking(int socket, int nonblocking);
int psp_adhoc_bytes_available(int socket, unsigned long *count);
int psp_adhoc_setsockopt(int socket, int level, int name, const void *value, int length);
int psp_adhoc_getsockopt(int socket, int level, int name, void *value, int *length);
int psp_adhoc_getsockname(int socket, void *address, int *address_length);
int psp_adhoc_getpeername(int socket, void *address, int *address_length);
/* the lists kept to their ready sockets, as posix_socket_select; how many
are ready, or minus an error. A connection that failed moves from the write
list to the error list, its failure in *connect_error */
int psp_adhoc_select(int *read, int *read_count, int *write, int *write_count, int *error, int *error_count,
	long timeout_seconds, long timeout_microseconds, int infinite, int *connect_error);

#endif
