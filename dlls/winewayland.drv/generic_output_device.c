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

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

WINE_DEFAULT_DEBUG_CHANNEL(waylanddrv);

#define MAX_EDID_SIZE (256 * 128)

BOOL wayland_output_edid_is_valid(const unsigned char *edid, UINT edid_len)
{
    static const unsigned char header[] = {0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00};
    UINT i, j;

    if (edid_len < 128 || edid_len % 128 || edid_len > MAX_EDID_SIZE) return FALSE;
    if (memcmp(edid, header, sizeof(header))) return FALSE;

    for (i = 0; i < edid_len; i += 128)
    {
        unsigned char checksum = 0;

        for (j = 0; j < 128; j++) checksum += edid[i + j];
        if (checksum) return FALSE;
    }

    return TRUE;
}

BOOL wayland_output_edid_supports_hdr(const unsigned char *edid, UINT edid_len)
{
    UINT i;

    if (!wayland_output_edid_is_valid(edid, edid_len)) return FALSE;

    for (i = 128; i < edid_len; i += 128)
    {
        const unsigned char *ext = edid + i;
        unsigned int end, offset;

        if (ext[0] != 0x02) continue; /* CTA-861 extension */

        end = ext[2];
        if (!end || end > 127) end = 127;

        for (offset = 4; offset < end; )
        {
            unsigned int tag = ext[offset] >> 5;
            unsigned int len = ext[offset] & 0x1f;
            const unsigned char *data = ext + offset + 1;

            offset += len + 1;
            if (offset > end) break;

            if (tag == 0x7 && len >= 2 && data[0] == 0x06 && (data[1] & 0x04))
                return TRUE;
        }
    }

    return FALSE;
}

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

static UINT read_edid_file(const char *output_name, const char *path, const char *source,
                           BOOL warn, unsigned char **edid)
{
    unsigned char *data;
    size_t len;
    FILE *file;

    if (!(file = fopen(path, "rb")))
    {
        if (warn)
            WARN("Failed to open %s EDID %s: %s.\n", source, debugstr_a(path), strerror(errno));
        else
            TRACE("Failed to open %s EDID %s: %s.\n", source, debugstr_a(path), strerror(errno));
        return 0;
    }

    if (!(data = malloc(MAX_EDID_SIZE + 1)))
    {
        fclose(file);
        return 0;
    }

    len = fread(data, 1, MAX_EDID_SIZE + 1, file);
    if (ferror(file))
    {
        if (warn)
            WARN("Failed to read %s EDID %s: %s.\n", source, debugstr_a(path), strerror(errno));
        else
            TRACE("Failed to read %s EDID %s: %s.\n", source, debugstr_a(path), strerror(errno));
        fclose(file);
        free(data);
        return 0;
    }

    fclose(file);

    if (len > MAX_EDID_SIZE || !wayland_output_edid_is_valid(data, len))
    {
        if (warn)
            WARN("Ignoring invalid %s EDID %s.\n", source, debugstr_a(path));
        else
            TRACE("Ignoring invalid %s EDID %s.\n", source, debugstr_a(path));
        free(data);
        return 0;
    }

    *edid = data;
    TRACE("Using %s EDID for output %s from %s, %u bytes.\n",
          source, debugstr_a(output_name), debugstr_a(path), (UINT)len);
    return (UINT)len;
}

UINT wayland_generic_output_get_edid_override(const char *output_name, unsigned char **edid)
{
    const char *env = getenv("WINE_WAYLAND_EDID");
    size_t output_name_len;
    const char *entry;

    *edid = NULL;
    if (!env || !*env || !output_name) return 0;

    output_name_len = strlen(output_name);

    /* Format: output:path[,output:path...]. Paths may not contain commas. */
    for (entry = env; *entry; )
    {
        const char *entry_end = strchr(entry, ',');
        const char *colon;

        if (!entry_end) entry_end = entry + strlen(entry);
        colon = memchr(entry, ':', entry_end - entry);

        if (colon && colon != entry &&
            (size_t)(colon - entry) == output_name_len &&
            !strncmp(entry, output_name, output_name_len))
        {
            size_t path_len = entry_end - colon - 1;
            char *path;
            UINT ret;

            if (!path_len)
            {
                WARN("Ignoring empty EDID override path for output %s.\n",
                     debugstr_a(output_name));
                return 0;
            }

            if (!(path = malloc(path_len + 1))) return 0;
            memcpy(path, colon + 1, path_len);
            path[path_len] = 0;

            ret = read_edid_file(output_name, path, "override", TRUE, edid);
            free(path);
            return ret;
        }

        entry = *entry_end ? entry_end + 1 : entry_end;
    }

    return 0;
}

static BOOL output_name_matches_connector_prefix(const char *output_name, const char *prefix)
{
    size_t output_len = strlen(output_name);
    size_t len = strlen(prefix);
    const char *p;

    if (output_len <= len) return FALSE;
    if (strncmp(output_name, prefix, len)) return FALSE;
    p = output_name + len;
    if (*p++ != '-') return FALSE;
    if (!isdigit((unsigned char)*p)) return FALSE;

    while (*p)
    {
        if (!isdigit((unsigned char)*p)) return FALSE;
        p++;
    }

    return TRUE;
}

static BOOL output_name_is_drm_connector(const char *output_name)
{
    static const char *const prefixes[] =
    {
        "DP",
        "HDMI-A",
        "HDMI-B",
        "eDP",
        "DVI-A",
        "DVI-D",
        "DVI-I",
        "DSI",
        "LVDS",
        "VGA",
        "DisplayPort",
    };
    unsigned int i;

    if (!output_name) return FALSE;

    for (i = 0; i < ARRAY_SIZE(prefixes); i++)
    {
        if (output_name_matches_connector_prefix(output_name, prefixes[i]))
            return TRUE;
    }

    return FALSE;
}

static BOOL drm_sysfs_entry_matches_output(const char *entry, const char *output_name)
{
    const char *p = entry;

    if (strncmp(p, "card", 4)) return FALSE;
    p += 4;

    if (!isdigit((unsigned char)*p)) return FALSE;
    while (isdigit((unsigned char)*p)) p++;

    return *p++ == '-' && !strcmp(p, output_name);
}

UINT wayland_generic_output_get_edid_sysfs(const char *output_name, unsigned char **edid)
{
    char path[PATH_MAX], matched_path[PATH_MAX] = {0};
    unsigned int matches = 0;
    struct dirent *entry;
    DIR *dir;

    *edid = NULL;
    if (!output_name_is_drm_connector(output_name)) return 0;

    if (!(dir = opendir("/sys/class/drm")))
    {
        TRACE("Failed to open DRM sysfs directory: %s.\n", strerror(errno));
        return 0;
    }

    while ((entry = readdir(dir)))
    {
        int ret;

        if (!drm_sysfs_entry_matches_output(entry->d_name, output_name)) continue;

        matches++;
        if (matches > 1) break;

        ret = snprintf(path, sizeof(path), "/sys/class/drm/%s/edid", entry->d_name);
        if (ret < 0 || (size_t)ret >= sizeof(path))
        {
            matches = 0;
            break;
        }
        strcpy(matched_path, path);
    }

    closedir(dir);

    if (!matches) return 0;
    if (matches > 1)
    {
        TRACE("Ignoring ambiguous DRM sysfs EDID match for output %s.\n", debugstr_a(output_name));
        return 0;
    }

    return read_edid_file(output_name, matched_path, "DRM sysfs", FALSE, edid);
}

UINT wayland_generic_output_get_edid(const struct wayland_output_state *output,
                                     BOOL hdr_supported, unsigned char **edid)
{
    static const unsigned char manufacturer[3] = {23, 12, 4}; /* WLD */
    const struct wayland_primaries *primaries = &output->primaries;
    struct wayland_output_mode *mode = output->current_mode;
    unsigned int edid_size = 128, extensions = 0;
    const char *model = output->model;
    unsigned int i, mwidth, mheight;
    unsigned int pixel_clock;
    unsigned char *data, *p, c;
    char temp_model[13] = {0};
    uint32_t serial;

    if (hdr_supported)
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

    if (hdr_supported)
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

        p = data + 128;
        c = 0;

        for (i = 0; i < 127; ++i)
            c += p[i];
        p[127] = 256 - c;
    }

    return edid_size;
}
