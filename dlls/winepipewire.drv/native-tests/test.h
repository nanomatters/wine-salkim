/* Native test harness for the PipeWire Unix library.
 *
 * Copyright 2026 Erhan Bilgili
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include <assert.h>

#ifndef PIPEWIRE_TEST_SOURCE
#define PIPEWIRE_TEST_SOURCE "../pipewire.c"
#endif
#include PIPEWIRE_TEST_SOURCE

/* No Wine runtime or audio server is needed for these deterministic tests.
 * Unused driver entry points are removed by the linker's section GC. */
unsigned char __wine_dbg_get_channel_flags(struct __wine_debug_channel *channel)
{
    return 0;
}

int __wine_dbg_header(enum __wine_debug_class cls, struct __wine_debug_channel *channel,
                      const char *function)
{
    return -1;
}

int __wine_dbg_output(const char *str)
{
    return 0;
}
