/* compile-only trial: pull newlib's headers in before the Halo prefix
redefines __inline (newlib's own headers use "static __inline") */
#include <stddef.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <ctype.h>
#include <time.h>
#include <float.h>
#include <assert.h>
#include <errno.h>
#include <setjmp.h>
#include <signal.h>
#include <locale.h>
/* <time.h> and <signal.h> bring in <limits.h>, whose macros collide with
cseries.h's enum of the same names */
#undef LONG_MAX
#undef LONG_MIN
#undef CHAR_MAX
#undef CHAR_MIN
/* the Linux port's <wchar.h> replaces the C library's and defines a 16-bit
wint_t unless one exists; keep newlib's */
#define __wint_t_defined 1
