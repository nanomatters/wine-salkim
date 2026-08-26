/*
 * WAYLAND display device functions
 *
 * Copyright 2020 Alexandros Frantzis for Collabora Ltd
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

#include "ntstatus.h"
#define WIN32_NO_STATUS
#include "waylanddrv.h"

#include "wine/debug.h"

#include "ntuser.h"

#include <stdlib.h>

WINE_DEFAULT_DEBUG_CHANNEL(waylanddrv);

static int output_info_cmp_primary_x_y(const void *va, const void *vb)
{
    const struct output_info *a = va;
    const struct output_info *b = vb;

    if (a->is_primary && !b->is_primary) return -1;
    if (!a->is_primary && b->is_primary) return 1;
    if (a->x < b->x) return -1;
    if (a->x > b->x) return 1;
    if (a->y < b->y) return -1;
    if (a->y > b->y) return 1;
    return strcmp(a->output->name, b->output->name);
}

static inline BOOL output_info_overlap(struct output_info *a, struct output_info *b)
{
    return b->x < a->x + a->output->current_mode->width &&
           b->x + b->output->current_mode->width > a->x &&
           b->y < a->y + a->output->current_mode->height &&
           b->y + b->output->current_mode->height > a->y;
}

/* Map a point to one of the four quadrants of our 2d coordinate space:
 * 0: bottom right (x >= 0, y >= 0)
 * 1: top right (x >= 0, y < 0)
 * 2: bottom left (x < 0, y >= 0)
 * 3: top left (x < 0, y < 0) */
static inline int point_to_quadrant(int x, int y)
{
    return (x < 0) * 2 + (y < 0);
}

/* Decide which of two outputs to keep stationary in order
 * to resolve an overlap. */
static struct output_info *output_info_get_overlap_anchor(struct output_info *a,
                                                          struct output_info *b)
{
    /* Preferences for the direction of growth in each quadrant, with a
     * lower value signifying a higher preference. */
    static const int quadrant_prefs[4][4] =
    {
        {0, 1, 2, 3}, /* quadrant 0 */
        {3, 0, 2, 1}, /* quadrant 1 */
        {2, 3, 0, 1}, /* quadrant 2 */
        {3, 2, 1, 0}, /* quadrant 3 */
    };
    int qa = point_to_quadrant(a->output->logical_x, a->output->logical_y);
    int qb = point_to_quadrant(b->output->logical_x, b->output->logical_y);
    /* Direction of growth if a is the anchor. */
    int qab = point_to_quadrant(b->output->logical_x - a->output->logical_x,
                                b->output->logical_y - a->output->logical_y);
    /* Direction of growth if b is the anchor. */
    int qba = point_to_quadrant(a->output->logical_x - b->output->logical_x,
                                a->output->logical_y - b->output->logical_y);

    /* If the two output origins are in different quadrants, use the output
     * in the lower valued quadrant as the anchor (so effectively outputs
     * grow/move away from quadrant 0). */
    if (qa != qb) return (qa < qb) ? a : b;

    /* If the outputs are in the same quadrant, use the preference for the
     * direction of growth in that quadrant to select the anchor. Again the
     * intended effect is to grow/move outputs away from the origin. */
    return (quadrant_prefs[qa][qab] < quadrant_prefs[qa][qba]) ? a : b;
}

static BOOL output_info_array_resolve_overlaps(struct wl_array *output_info_array)
{
    struct output_info *a, *b;
    BOOL found_overlap = FALSE;

    wl_array_for_each(a, output_info_array)
    {
        wl_array_for_each(b, output_info_array)
        {
            struct output_info *anchor, *move;
            BOOL x_use_end, y_use_end;
            double rel_x, rel_y;

            /* Break if we reach the same output in the inner loop, so that we
             * don't process output pairs twice (since order doesn't matter for
             * our algorithm.) */
            if (a == b) break;

            if (!output_info_overlap(a, b)) continue;
            found_overlap = TRUE;

            /* Decide which output to move to resolve the overlap. */
            anchor = output_info_get_overlap_anchor(a, b);
            move = anchor == a ? b : a;

            /* Move the selected output on the X axis to resolve the overlap,
             * while maintaining the same relative positioning of the outputs as
             * the one they have in logical space. Use either the start or end
             * of the moved output as the point to maintain the relative
             * position of, depending on whether the anchor is before or after
             * the moved output on the axis. */
            x_use_end = move->output->logical_x < anchor->output->logical_x;
            rel_x = (move->output->logical_x - anchor->output->logical_x +
                     (x_use_end ? move->output->logical_w : 0)) /
                    (double)anchor->output->logical_w;
            move->x = anchor->x + anchor->output->current_mode->width * rel_x -
                      (x_use_end ? move->output->current_mode->width : 0);

            /* Similarly for the Y axis. */
            y_use_end = move->output->logical_y < anchor->output->logical_y;
            rel_y = (move->output->logical_y - anchor->output->logical_y +
                     (y_use_end ? move->output->logical_h : 0)) /
                    (double)anchor->output->logical_h;
            move->y = anchor->y + anchor->output->current_mode->height * rel_y -
                      (y_use_end ? move->output->current_mode->height : 0);
        }
    }

    return found_overlap;
}

static void output_info_array_set_origin(struct wl_array *output_info_array,
                                         const struct output_info *origin)
{
    int x_offset = origin->x, y_offset = origin->y;
    struct output_info *info;

    wl_array_for_each(info, output_info_array)
    {
        info->x -= x_offset;
        info->y -= y_offset;
    }
}

static void output_info_array_select_primary(struct wl_array *output_info_array)
{
    const char *env = getenv("WAYLANDDRV_PRIMARY_MONITOR");
    struct output_info *info, *primary = NULL;
    int count = 0;

    /* Wayland has no primary-output state. Use an explicit override or the
     * output at the compositor's logical origin. */
    if (env && *env)
    {
        wl_array_for_each(info, output_info_array)
        {
            if (!strcmp(info->output->name, env))
            {
                primary = info;
                count++;
            }
        }

        if (count == 1)
        {
            primary->is_primary = TRUE;
            return;
        }
        if (count > 1)
            ERR("More than one output with name %s\n", debugstr_a(env));
        else
            ERR("Could not find output %s\n", debugstr_a(env));
    }

    wl_array_for_each(info, output_info_array)
    {
        if (info->output->logical_x || info->output->logical_y) continue;
        info->is_primary = TRUE;
        return;
    }
}

static void output_info_array_arrange_physical_coords(struct wl_array *output_info_array)
{
    struct output_info *info;
    size_t num_outputs = output_info_array->size / sizeof(struct output_info);
    int steps = 0;

    /* Set the initial physical pixel coordinates. */
    wl_array_for_each(info, output_info_array)
    {
        info->x = info->output->logical_x;
        info->y = info->output->logical_y;
        info->is_primary = FALSE;
    }

    /* Try to iteratively resolve overlaps, but be defensive and set an upper
     * iteration bound to ensure we avoid infinite loops. */
    while (output_info_array_resolve_overlaps(output_info_array) &&
           ++steps < num_outputs)
        continue;

    output_info_array_select_primary(output_info_array);

    /* Enumerate the selected primary first, then follow the layout order. */
    qsort(output_info_array->data, num_outputs, sizeof(struct output_info),
          output_info_cmp_primary_x_y);

    /* Keep every output consumer in the primary-relative Windows space. */
    if (num_outputs)
        output_info_array_set_origin(output_info_array, output_info_array->data);
}

static void wayland_add_device_gpu(const struct gdi_device_manager *device_manager,
                                   void *param)
{
    struct pci_id pci_id = {0};

    TRACE("\n");

    device_manager->add_gpu(NULL, &pci_id, NULL, param);
}

static void wayland_add_device_source(const struct gdi_device_manager *device_manager,
                                       void *param, UINT state_flags, struct output_info *output_info)
{
    UINT dpi = NtUserGetSystemDpiForProcess( NULL );
    TRACE("name=%s state_flags=0x%x\n",
          output_info->output->name, state_flags);
    device_manager->add_source(output_info->output->name, state_flags, dpi, param);
}

static void wayland_add_device_monitor(const struct gdi_device_manager *device_manager,
                                       void *param, struct output_info *output_info,
                                       struct output_info *primary)
{
    const struct wayland_output_state *output = output_info->output;
    BOOL desktop_hdr_enabled, panel_hdr_supported;
    const char *env;
    struct gdi_monitor monitor = {0};
    UINT64 sdr_white_level;

    SetRect(&monitor.rc_monitor, output_info->x, output_info->y,
            output_info->x + output_info->output->current_mode->width,
            output_info->y + output_info->output->current_mode->height);
    OffsetRect(&monitor.rc_monitor, -primary->x, -primary->y);

    monitor.edid_len = wayland_generic_output_get_edid_override(output_info->output->name,
                                                                &monitor.edid);
    if (!monitor.edid_len)
        monitor.edid_len = wayland_generic_output_get_edid_sysfs(output_info->output->name,
                                                                 &monitor.edid);

    if (monitor.edid_len)
        panel_hdr_supported = wayland_output_edid_supports_hdr(monitor.edid, monitor.edid_len);
    else
        /* Without a real EDID, use compositor HDR support as a best-effort panel hint. */
        panel_hdr_supported = wayland_color_manager_may_support_hdr();

    monitor.hdr_supported = panel_hdr_supported && wayland_color_manager_can_present_bt2100();
    desktop_hdr_enabled = output->supports_hdr;
    monitor.hdr_enabled = monitor.hdr_supported && desktop_hdr_enabled;
    if ((env = getenv("DXVK_HDR")) && *env == '1')
    {
        monitor.hdr_supported = TRUE;
        monitor.hdr_enabled = TRUE;
    }
    else if ((env = getenv("DXVK_NO_HDR")) && *env == '1')
    {
        monitor.hdr_enabled = FALSE;
    }

    monitor.sdr_white_level = WINE_SDR_WHITE_LEVEL_DEFAULT;
    if (monitor.hdr_enabled && output->ref_lum)
    {
        sdr_white_level = ((UINT64)output->ref_lum * 1000 + 40) / 80;
        monitor.sdr_white_level = min(sdr_white_level, (UINT64)(UINT)-1);
    }

    if (!monitor.edid_len)
        monitor.edid_len = wayland_generic_output_get_edid(output_info->output,
                                                           monitor.hdr_supported, &monitor.edid);
    /* We don't have a direct way to get the work area in Wayland. */
    monitor.rc_work = monitor.rc_monitor;

    TRACE("name=%s rc_monitor=rc_work=%s hdr_supported=%u hdr_enabled=%u sdr_white_level=%u\n",
          output_info->output->name, wine_dbgstr_rect(&monitor.rc_monitor),
          monitor.hdr_supported, monitor.hdr_enabled, monitor.sdr_white_level);

    device_manager->add_monitor(&monitor, param);
    free(monitor.edid);
}

static void populate_devmode(struct wayland_output_mode *output_mode, DEVMODEW *mode)
{
    mode->dmFields = DM_DISPLAYORIENTATION | DM_BITSPERPEL | DM_PELSWIDTH | DM_PELSHEIGHT |
                     DM_DISPLAYFLAGS | DM_DISPLAYFREQUENCY;
    mode->dmDisplayOrientation = DMDO_DEFAULT;
    mode->dmDisplayFlags = 0;
    mode->dmBitsPerPel = 32;
    mode->dmPelsWidth = output_mode->width;
    mode->dmPelsHeight = output_mode->height;
    /* Round the refresh rate to calculate the win32 display frequency. */
    mode->dmDisplayFrequency = (output_mode->refresh + 500) / 1000;
}

static void wayland_add_device_modes(const struct gdi_device_manager *device_manager,
                                     void *param, struct output_info *output_info,
                                     struct output_info *primary)
{
    DEVMODEW *modes, current = {.dmSize = sizeof(current)};
    struct wayland_output_mode *output_mode;
    int modes_count = 0;

    if (!(modes = malloc(output_info->output->modes_count * sizeof(*modes))))
        return;

    populate_devmode(output_info->output->current_mode, &current);
    current.dmFields |= DM_POSITION;
    current.dmPosition.x = output_info->x - primary->x;
    current.dmPosition.y = output_info->y - primary->y;

    RB_FOR_EACH_ENTRY(output_mode, &output_info->output->modes,
                      struct wayland_output_mode, entry)
    {
        DEVMODEW mode = {.dmSize = sizeof(mode)};
        populate_devmode(output_mode, &mode);
        modes[modes_count++] = mode;
    }

    device_manager->add_modes(&current, modes_count, modes, param);
    free(modes);
}

void output_info_array_update(void)
{
    struct output_info *output_info;
    struct wayland_output *output;
    struct wl_array *output_info_array = &process_wayland.output_info_array;

    /* reset the output info array */
    wl_array_release(&process_wayland.output_info_array);
    wl_array_init(&process_wayland.output_info_array);

    wl_list_for_each(output, &process_wayland.output_list, link)
    {
        if (!output->current.current_mode) continue;
        output_info = wl_array_add(output_info_array, sizeof(*output_info));
        if (output_info) output_info->output = &output->current;
        else ERR("Failed to allocate space for output_info\n");
    }

    output_info_array_arrange_physical_coords(output_info_array);
}

static int scale_output_coordinate(int offset, int physical_size, int logical_size)
{
    return ((LONGLONG)offset * physical_size + logical_size / 2) / logical_size;
}

static BOOL map_logical_notify_icon_point(const POINT *point, POINT *mapped)
{
    struct output_info *output_info;

    wl_array_for_each(output_info, &process_wayland.output_info_array)
    {
        const struct wayland_output_state *output = output_info->output;

        if (!output->current_mode || output->logical_w <= 0 || output->logical_h <= 0 ||
            point->x < output->logical_x || point->x >= output->logical_x + output->logical_w ||
            point->y < output->logical_y || point->y >= output->logical_y + output->logical_h)
            continue;

        mapped->x = output_info->x + scale_output_coordinate(point->x - output->logical_x,
                                                              output->current_mode->width,
                                                              output->logical_w);
        mapped->y = output_info->y + scale_output_coordinate(point->y - output->logical_y,
                                                              output->current_mode->height,
                                                              output->logical_h);
        return TRUE;
    }
    return FALSE;
}

static BOOL map_device_local_notify_icon_point(const POINT *point, POINT *mapped)
{
    struct output_info *output_info, *best = NULL;
    LONGLONG best_distance = 0;

    wl_array_for_each(output_info, &process_wayland.output_info_array)
    {
        const struct wayland_output_state *output = output_info->output;
        LONGLONG local_x, local_y, distance;

        if (!output->current_mode || output->logical_w <= 0 || output->logical_h <= 0)
            continue;

        local_x = (LONGLONG)point->x - output->logical_x;
        local_y = (LONGLONG)point->y - output->logical_y;
        if (local_x < 0 || local_x >= output->current_mode->width ||
            local_y < 0 || local_y >= output->current_mode->height)
            continue;

        distance = 0;
        if (local_x >= output->logical_w) distance += local_x - output->logical_w + 1;
        if (local_y >= output->logical_h) distance += local_y - output->logical_h + 1;
        if (best && distance >= best_distance) continue;

        best = output_info;
        best_distance = distance;
    }

    if (!best) return FALSE;
    mapped->x = best->x + point->x - best->output->logical_x;
    mapped->y = best->y + point->y - best->output->logical_y;
    return TRUE;
}

void WAYLAND_MapNotifyIconPoint(POINT *point)
{
    static BOOL device_local_coordinates;
    const char *mapping = NULL;
    POINT mapped = *point;

    pthread_mutex_lock(&process_wayland.output_mutex);
    if (device_local_coordinates)
    {
        if (map_device_local_notify_icon_point(point, &mapped))
            mapping = "device-local";
        else if (map_logical_notify_icon_point(point, &mapped))
            mapping = "logical-fallback";
    }
    else if (map_logical_notify_icon_point(point, &mapped))
    {
        mapping = "logical";
    }
    else if (map_device_local_notify_icon_point(point, &mapped))
    {
        /* The host coordinate convention remains stable across output changes. */
        device_local_coordinates = TRUE;
        mapping = "device-local";
    }
    pthread_mutex_unlock(&process_wayland.output_mutex);

    TRACE("%s %d,%d => physical %d,%d\n", mapping ? mapping : "unmapped",
          point->x, point->y, mapped.x, mapped.y);
    *point = mapped;
}

/***********************************************************************
 *      UpdateDisplayDevices (WAYLAND.@)
 */
UINT WAYLAND_UpdateDisplayDevices(const struct gdi_device_manager *device_manager, void *param)
{
    DWORD state_flags = DISPLAY_DEVICE_ATTACHED_TO_DESKTOP | DISPLAY_DEVICE_PRIMARY_DEVICE;
    struct output_info *primary = NULL, *output_info;

    TRACE("\n");

    pthread_mutex_lock(&process_wayland.output_mutex);

    output_info_array_update();

    /* Populate GDI devices. */
    wayland_add_device_gpu(device_manager, param);

    wl_array_for_each(output_info, &process_wayland.output_info_array)
    {
        if (!primary) primary = output_info;
        wayland_add_device_source(device_manager, param, state_flags, output_info);
        wayland_add_device_monitor(device_manager, param, output_info, primary);
        wayland_add_device_modes(device_manager, param, output_info, primary);
        state_flags &= ~DISPLAY_DEVICE_PRIMARY_DEVICE;
    }

    pthread_mutex_unlock(&process_wayland.output_mutex);

    return STATUS_SUCCESS;
}
