/* psp_chupa_platform.c - ChupathingyCE's desktop platform features, answered for the PSP (build 110)

The platform layer files the PSP build compiles (psp_build.py's
PLATFORM_FILES) call into ChupathingyCE's newer ones, which the PSP leaves
out: they draw with desktop OpenGL, read a touchscreen, or talk to the
internet. Each answer is the feature switched off, as the real function
answers with nothing set up, so the caller takes its plain path. Built as
sdl_platform.c is (its flags carry HALO_VERSION), with the real headers, so
a signature that differs doesn't compile. psp_chupa.c answers the game's
calls. */

#include "platform.h"
#include "sdl_platform.h"
#include "build_identity.h"
#include "hud_hires.h"
#include "menu_files.h"
#include "text_hires.h"
#include "touch_input.h"
#include "xgpu.h"
#include "delta.h"
#include "port_config.h"

#include <pspkernel.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef HALO_VERSION
#define HALO_VERSION "dev"
#endif

/* ---------- the build's identity (build_identity.c, updater.c) */

/* ChupathingyCE's version, as its own builds name themselves, and the PSP */
const char *build_identity(void)
{
	return "ChupathingyCE " HALO_VERSION " (PSP)";
}

/* the real one also logs the netcode's tables, which the PSP has none of */
void build_identity_log(void)
{
	static int logged;

	if (logged)
		return;
	logged = 1;
	platform_log("build: %s", build_identity());
}

const char *updater_version(void)
{
	return HALO_VERSION;
}

/* ---------- the netcode's version tables (delta.c): no internet play */

void delta_legacy_start(void) {}

/* The numbers in use: the OpenCE network version a host announces, and the
range of hosts' versions a client joins (psp_limits.h declares these, so
the signatures are checked). Only the multiplayer build calls them.

A PC takes ChupathingyCE's signed legacy table from the internet, or from
another machine over Delta Peer, and follows its row for the build's wire
(delta.c; docs/delta.md upstream). The PSP has neither, so with no table
it keeps the built-in numbers, and a server that has taken a newer table
is one it can't join (build_mp3.md: the table of 2026-10-10 announces 25,
the PSP joined only 11 to 24). mp4: like a PC's local override
(network.legacy_table), a table file is read once, unsigned: the setting's
file, relative to the data root, or legacy.json there (ms0:/halomp). Only
its row for this build's wire is read, and only if its numbers make a
range; the published table (ChupathingyCE's delta-table branch,
legacy.json) can be copied there as it is */
static struct
{
	int read, announce, minimum, maximum;
} psp_legacy;

/* the number after "key": in text, within its first bytes; 0 if absent */
static int psp_legacy_number(const char *text, const char *key, int *value)
{
	const char *at = strstr(text, key);
	char *end;
	long number;

	if (!at)
		return 0;
	at += strlen(key);
	while (*at == ' ' || *at == '\t' || *at == '\r' || *at == '\n' || *at == ':')
		at++;
	number = strtol(at, &end, 10);
	if (end == at || number < 1 || number > 65535)
		return 0;
	*value = (int)number;
	return 1;
}

static void psp_legacy_read(void)
{
	const char *name = config_string("network.legacy_table");
	const char *root = getenv("HALO_DATA_ROOT");
	char path[256], text[4096];
	const char *row;
	FILE *file;
	size_t size = 0;
	int announce, minimum, maximum;

	psp_legacy.read = 1;
	psp_legacy.announce = HALO_PORT_NETWORK_VERSION;
	psp_legacy.minimum = HALO_PORT_NETWORK_VERSION_MINIMUM;
	psp_legacy.maximum = HALO_PORT_NETWORK_VERSION_MAXIMUM;
	if (!name || !name[0])
		name = "legacy.json";
	if (strchr(name, ':') || name[0] == '/')
		snprintf(path, sizeof(path), "%s", name);
	else
		snprintf(path, sizeof(path), "%s/%s", root && root[0] ? root : ".", name);
	file = fopen(path, "rb");
	if (file)
	{
		size = fread(text, 1, sizeof(text) - 1, file);
		fclose(file);
	}
	text[size] = 0;
	row = size ? strstr(text, "\"" DELTA_WIRE "\"") : NULL;
	if (!row)
	{
		platform_log("network: %s: %s; the built-in numbers: announcing %d, joining %d to %d", path,
			size ? "no row for wire " DELTA_WIRE : "no legacy table", psp_legacy.announce, psp_legacy.minimum,
			psp_legacy.maximum);
		return;
	}
	if (!strstr(text, "\"delta_legacy\"") || !psp_legacy_number(row, "\"announce\"", &announce) ||
		!psp_legacy_number(row, "\"minimum\"", &minimum) || !psp_legacy_number(row, "\"maximum\"", &maximum) ||
		!(minimum <= announce && announce <= maximum))
	{
		platform_log("network: %s: its row for wire " DELTA_WIRE " doesn't read as a range; the built-in numbers", path);
		return;
	}
	psp_legacy.announce = announce;
	psp_legacy.minimum = minimum;
	psp_legacy.maximum = maximum;
	platform_log("network: the legacy table %s: announcing %d, joining %d to %d (wire " DELTA_WIRE ", unsigned)",
		path, announce, minimum, maximum);
}

int delta_legacy_announce(void) { if (!psp_legacy.read) psp_legacy_read(); return psp_legacy.announce; }
int delta_legacy_minimum(void) { if (!psp_legacy.read) psp_legacy_read(); return psp_legacy.minimum; }
int delta_legacy_maximum(void) { if (!psp_legacy.read) psp_legacy_read(); return psp_legacy.maximum; }

/* ---------- addresses in the log (log_address.c)

The same rule as the real one: an address that is no one's on the internet
(loopback, the private ranges, link-local, carrier NAT) is written whole,
any other as a tag, "addr#3f2a9c", the same for the same address all run
long. The real tag is a salted SHA-256, whose code is internet play's
(p2p_crypto.c, left out); here it is FNV-1a, salted from the clock at first
use: a run's lines still match up and a tag doesn't show its address, but
it is weaker than the real one's (enough for logs that come to us, not for
posting). debug.log_addresses writes every address whole, as the real one
does. Only the multiplayer build's netcode calls it */

static int psp_address_is_local(const unsigned char *bytes, int length)
{
	if (length == 4)
	{
		return bytes[0] == 0 || bytes[0] == 10 || bytes[0] == 127 ||
			(bytes[0] == 172 && (bytes[1] & 0xF0) == 16) || (bytes[0] == 192 && bytes[1] == 168) ||
			(bytes[0] == 169 && bytes[1] == 254) || (bytes[0] == 100 && (bytes[1] & 0xC0) == 64);
	}
	else
	{
		static const unsigned char zero[15] = { 0 };

		/* ::1 and ::, fc00::/7 and fe80::/10 */
		if (!memcmp(bytes, zero, 15) && bytes[15] <= 1)
			return 1;
		return (bytes[0] & 0xFE) == 0xFC || (bytes[0] == 0xFE && (bytes[1] & 0xC0) == 0x80);
	}
}

int log_address_local(const unsigned char *bytes, int length)
{
	return (length == 4 || length == 16) && psp_address_is_local(bytes, length);
}

const char *log_address(const unsigned char *bytes, int length, int port, char *text, int size)
{
	static unsigned int salt;
	char address[48];

	if (!text || size <= 0)
		return "";
	if (!bytes || (length != 4 && length != 16))
	{
		snprintf(text, (size_t)size, "unknown");
		return text;
	}
	if (psp_address_is_local(bytes, length) || config_boolean("debug.log_addresses"))
	{
		if (length == 4)
		{
			snprintf(address, sizeof(address), "%u.%u.%u.%u", bytes[0], bytes[1], bytes[2], bytes[3]);
		}
		else
		{
			int group, used;

			for (group = 0, used = 0; group < 8; group++)
			{
				used += snprintf(address + used, sizeof(address) - (size_t)used, "%s%x", group ? ":" : "",
					(unsigned int)(bytes[2 * group] << 8 | bytes[2 * group + 1]));
			}
		}
	}
	else
	{
		unsigned int hash = 2166136261u;
		int index;

		if (!salt)
			salt = sceKernelGetSystemTimeLow() | 1;
		for (index = 0; index < 4; index++)
			hash = (hash ^ ((salt >> (8 * index)) & 0xFF)) * 16777619u;
		hash = (hash ^ (unsigned int)length) * 16777619u;
		for (index = 0; index < length; index++)
			hash = (hash ^ bytes[index]) * 16777619u;
		snprintf(address, sizeof(address), "addr#%06x", hash & 0xFFFFFF);
		/* (a tag has no colons: no brackets) */
		length = 4;
	}
	if (port < 0)
		snprintf(text, (size_t)size, "%s", address);
	else if (length == 16)
		snprintf(text, (size_t)size, "[%s]:%d", address, port);
	else
		snprintf(text, (size_t)size, "%s:%d", address, port);
	return text;
}

/* the same for a sockaddr_in's address and port, both in network byte order
(0: no port), as log_address.c's (mpb1: internet play logs its peers) */
const char *log_address_ipv4(unsigned long network_address, unsigned short network_port, char *text, int size)
{
	unsigned int value = (unsigned int)network_address;
	unsigned char bytes[4];
	unsigned char port[2];

	memcpy(bytes, &value, 4);
	memcpy(port, &network_port, 2);
	return log_address(bytes, 4, network_port ? (port[0] << 8 | port[1]) : -1, text, size);
}

/* ---------- the touchscreen (touch_input.c): the PSP has buttons */

void touch_input_event(unsigned int type, const SDL_TouchFingerEvent *finger) { (void)type; (void)finger; }
void touch_input_cancel(void) {}
void touch_input_menu_set_active(int active) { (void)active; }
/* (the caller reads the pointer only on a platform with a touchscreen) */
void touch_input_menu_read(struct platform_ui_pointer *pointer) { (void)pointer; }
void touch_input_gamepad(XINPUT_GAMEPAD *pad) { (void)pad; }
void touch_input_controls(XINPUT_GAMEPAD *pad, int menus) { (void)pad; (void)menus; }
/* no finger moved: the look is left as it is */
void touch_input_look(float scale, float *yaw, float *pitch) { (void)scale; (void)yaw; (void)pitch; }
void touch_input_rumble(unsigned int left, unsigned int right) { (void)left; (void)right; }

/* ---------- desktop OpenGL's extras (xgpu_post.c, xgpu_shader_cache.c)

The PSP runs d3d8_gl.c as the null renderer (the GE draws, psp_ge.c): no
antialiasing pass, and no shader cache on the Memory Stick */

BOOL xgpu_post_prepare(BOOL smaa) { (void)smaa; return FALSE; }
BOOL xgpu_post_anti_alias(BOOL smaa, GLuint framebuffer, unsigned long width, unsigned long height,
	const GLint corners[4])
{
	(void)smaa; (void)framebuffer; (void)width; (void)height; (void)corners;
	return FALSE;
}
/* the real one's answer when it can't keep a shader: compiled as it is */
GLuint xgpu_shader_cache_compile(GLenum type, const char *source, const char *what)
{
	return xgpu_compile_shader(type, source, what);
}
void xgpu_shader_cache_linked(GLuint vertex_shader, GLuint fragment_shader) { (void)vertex_shader; (void)fragment_shader; }
/* nothing cached to warm */
void xgpu_shader_cache_warm(void (*linked)(GLuint vertex_shader, GLuint fragment_shader, GLuint program))
{
	(void)linked;
}

/* ---------- high-resolution HUD and text (hud_hires.c, text_hires.c)

A desktop's larger screen gets sharper pictures and fonts; the PSP's is
smaller than the Xbox's, and the GE draws the game's own: no override, no
font (-1, the real ones' "none") */

long hud_hires_override_find(unsigned long address, unsigned long width, unsigned long height,
	unsigned long level0_size)
{
	(void)address; (void)width; (void)height; (void)level0_size;
	return -1;
}
unsigned int hud_hires_override_texture(long asset, unsigned long *levels) { (void)asset; (void)levels; return 0; }
int hud_hires_override_coverage(long asset) { (void)asset; return 0; }
int hud_hires_override_point_threshold(long asset) { (void)asset; return 0; }

long text_hires_font(char const *tag_name, float cap_height, float oversample)
{
	(void)tag_name; (void)cap_height; (void)oversample;
	return -1;
}
int text_hires_covers(long font, unsigned long code) { (void)font; (void)code; return 0; }
int text_hires_glyph(long font, unsigned long code, struct text_hires_glyph *glyph)
{
	(void)font; (void)code; (void)glyph;
	return 0;
}
void text_hires_register_atlas(const unsigned long *texture, unsigned long width, unsigned long height)
{
	(void)texture; (void)width; (void)height;
}
unsigned int text_hires_atlas_texture(unsigned long data) { (void)data; return 0; }

/* ---------- the PC menus' pictures (menu_files.c): no PC menus (psp_chupa.c) */

unsigned int menu_art_texture(unsigned long data, unsigned long *levels) { (void)data; (void)levels; return 0; }
