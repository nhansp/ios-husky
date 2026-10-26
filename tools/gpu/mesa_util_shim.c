/* SPDX-License-Identifier: MIT
 * mesa_util_shim.c - one symbol Mesa's ralloc.c needs but that lives in util/u_printf.c.
 * Intended repo path: tests/common/mesa_util_shim.c
 *
 * ralloc.c's vasprintf helpers call u_printf_length(); the tests never use them, but the
 * object must link.  Providing it here avoids pulling util/u_printf.c (which drags in the
 * C11 threads shim and a pile of platform macros) into the host and device test builds.
 */
#include <stdarg.h>
#include <stdio.h>

int u_printf_length(const char *fmt, va_list ap)
{
    return vsnprintf(NULL, 0, fmt, ap);
}
