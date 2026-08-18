/*
 * Generic Wayland output-device fallback handling
 *
 * Copyright 2026 Erhan Bilgili
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#if 0
#pragma makedep unix
#endif

#include "config.h"

#include "waylanddrv.h"

#include "wine/debug.h"

#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

WINE_DEFAULT_DEBUG_CHANNEL(waylanddrv);

/* borrowed from gamescope with permission */
static uint8_t encode_max_luminance(float nits)
{
    if (nits == 0.0f)
        return 0;

    return ceilf((logf(nits / 50.0f) / logf(2.0f)) * 32.0f);
}

static uint32_t hash_string(uint32_t hash, const char *str)
{
    if (!str) return hash;

    while (*str)
    {
        hash ^= (unsigned char)*str++;
        hash *= 16777619;
    }

    return hash;
}

static unsigned int mode_pixel_clock_10khz(const struct wayland_output_mode *mode)
{
    uint64_t clock;

    clock = ((uint64_t)mode->width * mode->height * mode->refresh + 9999999) / 10000000;
    if (!clock) return 1;
    if (clock > 0xffff) return 0xffff;
    return clock;
}

static unsigned int pixels_to_mm(int32_t pixels)
{
    return round(pixels * 25.4 / 150.0);
}

UINT wayland_generic_output_get_edid(const struct wayland_output_state *output,
                                     unsigned char **edid)
{
    static const unsigned char manufacturer[3] = {23, 12, 4}; /* WLD */
    const struct wayland_primaries *primaries = &output->primaries;
    struct wayland_output_mode *mode = output->current_mode;
    const char *model = output->model;
    unsigned int edid_size = 128, extensions = 0;
    unsigned int i, mwidth, mheight;
    unsigned int pixel_clock;
    unsigned char *data, *p, c;
    char temp_model[13] = {0};
    uint32_t serial;

    if (output->supports_hdr)
    {
        edid_size += 128;
        extensions++;
    }

    if (!(*edid = calloc(edid_size, sizeof(**edid))))
        return 0;

    data = *edid;

    mwidth = output->physical_w;
    mheight = output->physical_h;

    if (mwidth == 0 || mheight == 0)
    {
        mwidth = pixels_to_mm(mode->width);
        mheight = pixels_to_mm(mode->height);
    }

    *(uint64_t*)data = 0x00ffffffffffff00;

    data[8] = ((manufacturer[0] & 0x1f) << 2) | ((manufacturer[1] & 0x18) >> 3);
    data[9] = ((manufacturer[1] & 0x7) << 5) | (manufacturer[2] & 0x1f);
    data[10] = 0x01;
    data[11] = 0x00;

    serial = hash_string(2166136261u, output->name);
    serial = hash_string(serial, output->model);
    data[12] = serial;
    data[13] = serial >> 8;
    data[14] = serial >> 16;
    data[15] = serial >> 24;

    data[16] = 0xFF;
    data[17] = 31; /* 2021 */
    data[18] = 1;
    data[19] = 4;
    data[20] = 0xa0; /* FIXME */
    data[21] = round(mwidth / 10.0); /* cm */
    data[22] = round(mheight / 10.0); /* cm */
    data[24] = 0x6;
    data[25] = ((primaries->r_x & 0x3) << 6) | ((primaries->r_y & 0x3) << 4) |
               ((primaries->g_x & 0x3) << 2) | (primaries->g_y & 0x3);
    data[26] = ((primaries->b_x & 0x3) << 6) | ((primaries->b_y & 0x3) << 4) |
               ((primaries->w_x & 0x3) << 2) | (primaries->w_y & 0x3);
    data[27] = (primaries->r_x & 0x3fc) >> 2;
    data[28] = (primaries->r_y & 0x3fc) >> 2;
    data[29] = (primaries->g_x & 0x3fc) >> 2;
    data[30] = (primaries->g_y & 0x3fc) >> 2;
    data[31] = (primaries->b_x & 0x3fc) >> 2;
    data[32] = (primaries->b_y & 0x3fc) >> 2;
    data[33] = (primaries->w_x & 0x3fc) >> 2;
    data[34] = (primaries->w_y & 0x3fc) >> 2;

    for (i = 0; i < 16; ++i) data[38 + i] = 1;

    p = data + 54;

    pixel_clock = mode_pixel_clock_10khz(mode);
    *(uint16_t*)p = pixel_clock;

    /* assume blanking time is 0 */
    p[2] = mode->width;
    p[4] = (((mode->width >> 8) & 0xf) << 4);
    p[5] = mode->height;
    p[7] = (((mode->height >> 8) & 0xf) << 4);
    p[12] = mwidth;
    p[13] = mheight;
    p[14] = (((mwidth >> 8) & 0xf) << 4) | ((mheight >> 8) & 0xf);

    p += 18;
    p[3] = 0xfc;

    if (model) lstrcpynA(temp_model, model, sizeof(temp_model));
    else strcpy(temp_model, "Default");

    for (i = 0; i < sizeof(temp_model); i++)
    {
        if (!temp_model[i])
        {
            temp_model[i++] = '\n';
            break;
        }
    }
    for (; i < sizeof(temp_model); i++)
    {
        if (!temp_model[i]) temp_model[i] = ' ';
    }

    TRACE("edid model %s\n", debugstr_an(temp_model, sizeof(temp_model)));
    memcpy((char *)p + 5, temp_model, sizeof(temp_model));

    p += 18;
    p[3] = 0x10;
    p += 18;
    p[3] = 0x10;

    c = 0;
    data[126] = extensions;
    for (i = 0; i < 127; ++i)
        c += data[i];
    data[127] = 256 - c;

    p = data;

    if (output->supports_hdr)
    {
        p += 128;

        p[0] = 2;
        p[1] = 3;
        p[2] = 0xb;

        p += 4;

        p[0] = (0x7 << 5) | 0x6; /* HDR static metadata size */
        p[1] = 6;

        /* HDR static metadata block */

        p[2] = 0x7; /* ST2084 | SDR | HDR */
        p[3] = 1;
        p[4] = encode_max_luminance(output->max_cll);
        p[5] = encode_max_luminance(output->max_fall);
        p[6] = 0; /* assume undefined, games often don't implement this properly */

        /* reset p to beginning of the CTA block */
        p = data + 128;
        c = 0;

        for (i = 0; i < 127; ++i)
            c += p[i];
        p[127] = 256 - c;
    }

    return edid_size;
}
