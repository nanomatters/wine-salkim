/* Copyright 2026 Erhan Bilgili
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#ifndef __WINE_STEAM_INPUT_H
#define __WINE_STEAM_INPUT_H

#include <limits.h>
#include <stdio.h>
#include <string.h>

/* Keep the complete HID instance opaque to Steam's native-hook adapters. */
static inline BOOL wine_steam_input_get_instance( const WCHAR *path, UINT length, BOOL xinput,
                                                 const WCHAR **instance, UINT *instance_len, UINT *slot )
{
    static const WCHAR prefix[] =
    {
        '\\','\\','?','\\','H','I','D','#','V','I','D','_','2','8','D','E','&','P','I','D','_','1','1','F','F','&',
    };
    const WCHAR *p, *end, *start;
    UINT i, value = 0;

    if (!path || length < ARRAY_SIZE(prefix) + 5) return FALSE;
    end = path + length;
    for (i = 0; i < ARRAY_SIZE(prefix); ++i)
        if ((path[i] >= 'a' && path[i] <= 'z' ? path[i] - 'a' + 'A' : path[i]) != prefix[i])
            return FALSE;
    p = path + ARRAY_SIZE(prefix);
    if ((p[0] & ~0x20) != (xinput ? 'X' : 'I') ||
        (p[1] & ~0x20) != (xinput ? 'I' : 'G') || p[2] != '_') return FALSE;
    start = p += 3;
    while (p != end && *p >= '0' && *p <= '9')
    {
        if (value > (UINT_MAX - (*p - '0')) / 10) return FALSE;
        value = value * 10 + *p++ - '0';
    }
    if (p == start || p == end || *p++ != '#') return FALSE;
    start = p;
    while (p != end && *p != '#')
    {
        if (!*p || *p == '\\' || *p == '/') return FALSE;
        ++p;
    }
    if (p == start || p == end) return FALSE;
    *instance = start;
    *instance_len = p - start;
    *slot = value;
    return TRUE;
}

static inline UINT wine_steam_input_format_token( WCHAR *buffer, UINT capacity, BOOL wgi,
                                                 UINT16 vid, UINT16 pid, const WCHAR *instance,
                                                 UINT instance_len, UINT slot, UINT process_id )
{
    char prefix[80], suffix[32];
    int prefix_len, suffix_len;
    UINT i;

    prefix_len = snprintf( prefix, sizeof(prefix), wgi ? "{wgi/nrid/:steam-%04X&%04X&" :
                           "\\\\.\\pipe\\HID#VID_045E&PID_028E&IG_00#%04X&%04X&", vid, pid );
    suffix_len = snprintf( suffix, sizeof(suffix), wgi ? "#%u#%u}" : "#%u#%u", slot, process_id );
    if (prefix_len < 0 || prefix_len >= (int)sizeof(prefix) || suffix_len < 0 || suffix_len >= (int)sizeof(suffix) ||
        instance_len >= capacity || (UINT)(prefix_len + suffix_len) >= capacity - instance_len) return 0;
    for (i = 0; i < (UINT)prefix_len; ++i) buffer[i] = prefix[i];
    memcpy( buffer + prefix_len, instance, instance_len * sizeof(WCHAR) );
    for (i = 0; i <= (UINT)suffix_len; ++i) buffer[prefix_len + instance_len + i] = suffix[i];
    return prefix_len + instance_len + suffix_len;
}

#endif /* __WINE_STEAM_INPUT_H */
