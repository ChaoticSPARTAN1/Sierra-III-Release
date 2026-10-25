/* PSP bring-up: after the Linux prefix and the MSVC-semantics header.

The Linux prefix makes every __inline function "static" (one private copy
per unit); the build mirror has already turned the source's own
"static __inline" into "static __inline__", so the rest become weak inline
functions here: one copy survives the link, as with MSVC's COMDAT. */
#undef __inline
#undef _inline
#undef __forceinline
#define __inline __inline__ __attribute__((weak))
#define _inline __inline__ __attribute__((weak))
#define __forceinline __inline__ __attribute__((weak, always_inline))

/* MSVC storage classes and calling conventions */
#define __declspec(x) __declspec_##x
#define __declspec_align(n) __attribute__((aligned(n)))
#define __declspec_noreturn __attribute__((noreturn))
#define __declspec_dllexport
#define __declspec_dllimport
#define __declspec_selectany __attribute__((weak))
#define __cdecl
#define __stdcall
#define __fastcall
#define _cdecl
#define _stdcall
#define __int64 long long
#define __int32 int
#define __int16 short
#define __int8 char

/* newlib's <ctype.h> defines these as macros; the game declares some of
them as functions */
#undef isalpha
#undef isdigit
#undef isspace
#undef isupper
#undef islower
#undef isalnum
#undef isxdigit
#undef ispunct
#undef isprint
#undef iscntrl
#undef isgraph
#undef tolower
#undef toupper

/* build 114: a keep word's answer remembered at its call site, for the
asks made per object or per draw (shadows, decals, the HUD, every bitmap a
draw binds): psp_keep remembers answers itself, but each ask still hashes
the word and compares it, about a thousand asks a frame on b30 (b112's
cache-model run: 61,000 instructions a frame of drawing in psp_keep).
keep.txt is read once at start-up (psp_platform.c, keep_load); before
that, the answer is 0 and isn't remembered */
int psp_keep(const char *name);
int psp_keep_loaded(void);
#define PSP_KEEP_ONCE(word) __extension__ ({ \
	static signed char psp_keep_once_ = -1; \
	if (psp_keep_once_ < 0 && psp_keep_loaded()) \
		psp_keep_once_ = (signed char)psp_keep(word); \
	psp_keep_once_ > 0; })

/* the release's extra keep defaults (psp_platform.c's keep_defaults): kept
here so the table's line count, and every function's line after it, is the
same in both builds and one profile fits them */
#if defined(PSP_METRICS) && PSP_METRICS
#define PSP_RELEASE_DEFAULTS
#else
#define PSP_RELEASE_DEFAULTS { "notimers", "timerson" }, { "nooverlay", "overlay" },
#endif
