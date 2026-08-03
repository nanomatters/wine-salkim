/*
 * Wayland core handling
 *
 * Copyright (c) 2020 Alexandros Frantzis for Collabora Ltd
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

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

WINE_DEFAULT_DEBUG_CHANNEL(waylanddrv);

struct wayland process_wayland =
{
    .seat.mutex = PTHREAD_MUTEX_INITIALIZER,
    .keyboard.mutex = PTHREAD_MUTEX_INITIALIZER,
    .pointer.mutex = PTHREAD_MUTEX_INITIALIZER,
    .touch.touch_points = { &process_wayland.touch.touch_points,
                            &process_wayland.touch.touch_points },
    .text_input.mutex = PTHREAD_MUTEX_INITIALIZER,
    .dmabuf_formats = {&process_wayland.dmabuf_formats, &process_wayland.dmabuf_formats},
    .dmabuf_default_feedback.pending_formats =
        {&process_wayland.dmabuf_default_feedback.pending_formats,
         &process_wayland.dmabuf_default_feedback.pending_formats},
    .dmabuf_mutex = PTHREAD_MUTEX_INITIALIZER,
    .data_device.mutex = PTHREAD_MUTEX_INITIALIZER,
    .output_list = {&process_wayland.output_list, &process_wayland.output_list},
    .output_mutex = PTHREAD_MUTEX_INITIALIZER,
    .activation_mutex = PTHREAD_MUTEX_INITIALIZER,
};

struct wayland_cached_image_description
{
    enum wayland_image_description_color_space color_space;
    enum wayland_image_description_status status;
    struct wp_image_description_v1 *description;
};

static pthread_mutex_t image_description_mutex = PTHREAD_MUTEX_INITIALIZER;
/* Ready image descriptions are immutable and may be reused on any surface. */
static struct wayland_cached_image_description cached_image_descriptions[] =
{
    [WAYLAND_IMAGE_DESCRIPTION_SCRGB] =
        {.color_space = WAYLAND_IMAGE_DESCRIPTION_SCRGB},
    [WAYLAND_IMAGE_DESCRIPTION_BT2100] =
        {.color_space = WAYLAND_IMAGE_DESCRIPTION_BT2100},
};

/**********************************************************************
 *          xdg_wm_base handling
 */

static void xdg_wm_base_handle_ping(void *data, struct xdg_wm_base *shell,
                                    uint32_t serial)
{
    xdg_wm_base_pong(shell, serial);
}

static const struct xdg_wm_base_listener xdg_wm_base_listener =
{
    xdg_wm_base_handle_ping
};

/**********************************************************************
 *          wl_seat handling
 */

static void wl_seat_handle_capabilities(void *data, struct wl_seat *seat,
                                        enum wl_seat_capability caps)
{
    if ((caps & WL_SEAT_CAPABILITY_POINTER) && !process_wayland.pointer.wl_pointer)
        wayland_pointer_init(wl_seat_get_pointer(seat));
    else if (!(caps & WL_SEAT_CAPABILITY_POINTER) && process_wayland.pointer.wl_pointer)
        wayland_pointer_deinit();

    if ((caps & WL_SEAT_CAPABILITY_KEYBOARD) && !process_wayland.keyboard.wl_keyboard)
        wayland_keyboard_init(wl_seat_get_keyboard(seat));
    else if (!(caps & WL_SEAT_CAPABILITY_KEYBOARD) && process_wayland.keyboard.wl_keyboard)
        wayland_keyboard_deinit();

    if ((caps & WL_SEAT_CAPABILITY_TOUCH) && !process_wayland.touch.wl_touch)
        wayland_touch_init(wl_seat_get_touch(seat));
    else if (!(caps & WL_SEAT_CAPABILITY_TOUCH) && process_wayland.touch.wl_touch)
        wayland_touch_deinit();
}

static void wl_seat_handle_name(void *data, struct wl_seat *seat, const char *name)
{
}

static const struct wl_seat_listener seat_listener =
{
    wl_seat_handle_capabilities,
    wl_seat_handle_name
};

static void wayland_color_manager_handle_supported_intent(void *data,
    struct wp_color_manager_v1 *wp_color_manager_v1, uint32_t intent)
{
}

static void wayland_color_manager_handle_supported_feature(void *data,
    struct wp_color_manager_v1 *wp_color_manager_v1, uint32_t feature)
{
    pthread_mutex_lock(&process_wayland.output_mutex);

    TRACE("feature %u\n", feature);

    if (feature == WP_COLOR_MANAGER_V1_FEATURE_PARAMETRIC)
        process_wayland.supports_parametric = TRUE;
    else if (feature == WP_COLOR_MANAGER_V1_FEATURE_SET_PRIMARIES)
        process_wayland.supports_set_primaries = TRUE;
    else if (feature == WP_COLOR_MANAGER_V1_FEATURE_SET_LUMINANCES)
        process_wayland.supports_set_luminances = TRUE;
    else if (feature == WP_COLOR_MANAGER_V1_FEATURE_WINDOWS_SCRGB)
        process_wayland.supports_win_scrgb = TRUE;
    else if (feature == WP_COLOR_MANAGER_V1_FEATURE_WINDOWS_BT2100)
        process_wayland.supports_windows_bt2100 = TRUE;
    else if (feature == WP_COLOR_MANAGER_V1_FEATURE_EXTENDED_TARGET_VOLUME)
        process_wayland.supports_extended_volume = TRUE;

    pthread_mutex_unlock(&process_wayland.output_mutex);
}

static void wayland_color_manager_handle_supported_named_tf(void *data,
    struct wp_color_manager_v1 *wp_color_manager_v1, uint32_t tf)
{
    pthread_mutex_lock(&process_wayland.output_mutex);

    TRACE("named tf %u\n", tf);

    if (tf == WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_ST2084_PQ)
        process_wayland.supports_pq = TRUE;

    pthread_mutex_unlock(&process_wayland.output_mutex);
}

static void wayland_color_manager_handle_supported_primaries(void *data,
    struct wp_color_manager_v1 *wp_color_manager_v1, uint32_t primaries)
{
    pthread_mutex_lock(&process_wayland.output_mutex);

    TRACE("primaries %u\n", primaries);

    if (primaries == WP_COLOR_MANAGER_V1_PRIMARIES_BT2020)
        process_wayland.supports_bt2020_primaries = TRUE;

    pthread_mutex_unlock(&process_wayland.output_mutex);
}

static const char *wayland_image_description_color_space_name(
        enum wayland_image_description_color_space color_space)
{
    switch (color_space)
    {
    case WAYLAND_IMAGE_DESCRIPTION_SCRGB:
        return "Windows scRGB";
    case WAYLAND_IMAGE_DESCRIPTION_BT2100:
        return "Windows BT.2100";
    default:
        return "unknown";
    }
}

static void cached_image_description_failed(void *data,
        struct wp_image_description_v1 *description, uint32_t cause,
        const char *message)
{
    struct wayland_cached_image_description *cached = data;

    pthread_mutex_lock(&image_description_mutex);
    if (cached->description == description)
    {
        cached->description = NULL;
        cached->status = WAYLAND_IMAGE_DESCRIPTION_FAILED;
    }
    pthread_mutex_unlock(&image_description_mutex);

    ERR("Failed to create %s image description, cause=%u message=%s\n",
        wayland_image_description_color_space_name(cached->color_space),
        cause, debugstr_a(message));
    wp_image_description_v1_destroy(description);
}

static void cached_image_description_ready2(void *data,
        struct wp_image_description_v1 *description,
        uint32_t identity_hi, uint32_t identity_lo)
{
    struct wayland_cached_image_description *cached = data;

    pthread_mutex_lock(&image_description_mutex);
    if (cached->description == description)
        cached->status = WAYLAND_IMAGE_DESCRIPTION_READY;
    pthread_mutex_unlock(&image_description_mutex);

    TRACE("%s image description ready, id=%#x%x\n",
          wayland_image_description_color_space_name(cached->color_space),
          identity_hi, identity_lo);
}

static void cached_image_description_ready(void *data,
        struct wp_image_description_v1 *description, uint32_t identity)
{
    cached_image_description_ready2(data, description, 0, identity);
}

static const struct wp_image_description_v1_listener cached_image_description_listener =
{
    cached_image_description_failed,
    cached_image_description_ready,
    cached_image_description_ready2,
};

static BOOL wayland_color_manager_cache_image_description(
        enum wayland_image_description_color_space color_space)
{
    struct wayland_cached_image_description *cached =
        &cached_image_descriptions[color_space];
    struct wp_image_description_v1 *description = NULL;
    BOOL supported = FALSE;

    pthread_mutex_lock(&image_description_mutex);
    if (cached->status != WAYLAND_IMAGE_DESCRIPTION_UNINITIALIZED)
    {
        pthread_mutex_unlock(&image_description_mutex);
        return FALSE;
    }

    switch (color_space)
    {
    case WAYLAND_IMAGE_DESCRIPTION_SCRGB:
        supported = process_wayland.supports_win_scrgb;
        if (supported)
            description = wp_color_manager_v1_create_windows_scrgb(
                    process_wayland.wp_color_manager_v1);
        break;
    case WAYLAND_IMAGE_DESCRIPTION_BT2100:
        supported = wayland_color_manager_can_present_bt2100();
        if (supported)
            description = wayland_color_manager_create_windows_bt2100();
        break;
    default:
        break;
    }

    if (description)
    {
        cached->description = description;
        cached->status = WAYLAND_IMAGE_DESCRIPTION_PENDING;
        wp_image_description_v1_add_listener(
                description, &cached_image_description_listener, cached);
    }
    else
        cached->status = supported ? WAYLAND_IMAGE_DESCRIPTION_FAILED :
                                     WAYLAND_IMAGE_DESCRIPTION_UNSUPPORTED;
    pthread_mutex_unlock(&image_description_mutex);
    return description != NULL;
}

enum wayland_image_description_status wayland_color_manager_get_image_description(
        enum wayland_image_description_color_space color_space,
        struct wp_image_description_v1 **description)
{
    enum wayland_image_description_status status;

    if (description) *description = NULL;
    if (color_space <= WAYLAND_IMAGE_DESCRIPTION_DEFAULT ||
        color_space > WAYLAND_IMAGE_DESCRIPTION_BT2100)
        return WAYLAND_IMAGE_DESCRIPTION_UNSUPPORTED;

    pthread_mutex_lock(&image_description_mutex);
    status = cached_image_descriptions[color_space].status;
    if (description && status == WAYLAND_IMAGE_DESCRIPTION_READY)
        *description = cached_image_descriptions[color_space].description;
    pthread_mutex_unlock(&image_description_mutex);
    return status;
}

static void wayland_color_manager_handle_done(void *data,
                        struct wp_color_manager_v1 *wp_color_manager_v1)
{
    BOOL created = FALSE;

    /* Capability events precede done on this event queue. */
    created |= wayland_color_manager_cache_image_description(
            WAYLAND_IMAGE_DESCRIPTION_SCRGB);
    created |= wayland_color_manager_cache_image_description(
            WAYLAND_IMAGE_DESCRIPTION_BT2100);
    if (created) wl_display_flush(process_wayland.wl_display);
}

static const struct wp_color_manager_v1_listener wp_color_manager_listener = {
    wayland_color_manager_handle_supported_intent,
    wayland_color_manager_handle_supported_feature,
    wayland_color_manager_handle_supported_named_tf,
    wayland_color_manager_handle_supported_primaries,
    wayland_color_manager_handle_done
};

static int wayland_disable_ssd(void)
{
    static int disabled = -1;
    const char *env;

    if (disabled == -1)
        disabled = (env = getenv("WAYLANDDRV_SSD")) && !strcmp(env, "0");

    return disabled;
}

static void wayland_dmabuf_clear_format_list(struct wl_list *list)
{
    struct wayland_dmabuf_format *entry, *next;

    wl_list_for_each_safe(entry, next, list, link)
    {
        wl_list_remove(&entry->link);
        free(entry);
    }
    wl_list_init(list);
}

static BOOL wayland_dmabuf_add_format_to_list(struct wl_list *list, uint32_t format,
                                              uint64_t modifier, uint32_t tranche_index,
                                              uint32_t tranche_flags)
{
    struct wayland_dmabuf_format *entry;

    wl_list_for_each(entry, list, link)
    {
        if (entry->format == format && entry->modifier == modifier)
            return TRUE;
    }

    if (!(entry = calloc(1, sizeof(*entry)))) return FALSE;
    entry->format = format;
    entry->modifier = modifier;
    entry->tranche_index = tranche_index;
    entry->tranche_flags = tranche_flags;
    wl_list_insert(list->prev, &entry->link);
    return TRUE;
}

static void wayland_dmabuf_add_format(uint32_t format, uint64_t modifier)
{
    pthread_mutex_lock(&process_wayland.dmabuf_mutex);
    wayland_dmabuf_add_format_to_list(&process_wayland.dmabuf_formats, format, modifier, 0, 0);
    pthread_mutex_unlock(&process_wayland.dmabuf_mutex);
}

static void zwp_linux_dmabuf_v1_handle_format(void *data, struct zwp_linux_dmabuf_v1 *dmabuf,
                                              uint32_t format)
{
    wayland_dmabuf_add_format(format, DRM_FORMAT_MOD_INVALID);
}

static void zwp_linux_dmabuf_v1_handle_modifier(void *data, struct zwp_linux_dmabuf_v1 *dmabuf,
                                                uint32_t format, uint32_t modifier_hi,
                                                uint32_t modifier_lo)
{
    wayland_dmabuf_add_format(format, ((uint64_t)modifier_hi << 32) | modifier_lo);
}

static const struct zwp_linux_dmabuf_v1_listener zwp_linux_dmabuf_v1_listener =
{
    zwp_linux_dmabuf_v1_handle_format,
    zwp_linux_dmabuf_v1_handle_modifier
};

static void zwp_linux_dmabuf_feedback_v1_handle_done(void *data,
                                                     struct zwp_linux_dmabuf_feedback_v1 *feedback)
{
    struct wayland_dmabuf_feedback *state = data;
    struct wayland_dmabuf_format *entry, *next;
    unsigned int count = 0;

    if (!state->valid)
    {
        wayland_dmabuf_clear_format_list(&state->pending_formats);
        state->tranche_index = 0;
        state->tranche_flags = 0;
        return;
    }

    pthread_mutex_lock(&process_wayland.dmabuf_mutex);
    wayland_dmabuf_clear_format_list(&process_wayland.dmabuf_formats);
    wl_list_for_each_safe(entry, next, &state->pending_formats, link)
    {
        wl_list_remove(&entry->link);
        wl_list_insert(process_wayland.dmabuf_formats.prev, &entry->link);
        count++;
    }
    pthread_mutex_unlock(&process_wayland.dmabuf_mutex);
    wl_list_init(&state->pending_formats);
    state->tranche_index = 0;
    state->tranche_flags = 0;

    TRACE("default dmabuf feedback advertised %u format/modifier pairs.\n", count);
}

static void zwp_linux_dmabuf_feedback_v1_handle_format_table(void *data,
        struct zwp_linux_dmabuf_feedback_v1 *feedback, int fd, uint32_t size)
{
    struct wayland_dmabuf_feedback *state = data;
    struct wayland_dmabuf_feedback_format *table;
    void *map;

    free(state->format_table);
    state->format_table = NULL;
    state->format_table_count = 0;
    state->valid = FALSE;

    if (!size || size % sizeof(*state->format_table))
    {
        WARN("Ignoring invalid dmabuf feedback format table size %u.\n", size);
        close(fd);
        return;
    }

    if ((map = mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0)) == MAP_FAILED)
    {
        WARN("Failed to map dmabuf feedback format table.\n");
        close(fd);
        return;
    }
    close(fd);

    if ((table = malloc(size)))
    {
        memcpy(table, map, size);
        state->format_table = table;
        state->format_table_count = size / sizeof(*state->format_table);
        state->valid = TRUE;
    }
    munmap(map, size);
}

static void zwp_linux_dmabuf_feedback_v1_handle_main_device(void *data,
        struct zwp_linux_dmabuf_feedback_v1 *feedback, struct wl_array *device)
{
}

static void zwp_linux_dmabuf_feedback_v1_handle_tranche_done(void *data,
        struct zwp_linux_dmabuf_feedback_v1 *feedback)
{
    struct wayland_dmabuf_feedback *state = data;
    struct wayland_dmabuf_format *entry;

    wl_list_for_each(entry, &state->pending_formats, link)
    {
        if (entry->tranche_index == state->tranche_index)
            entry->tranche_flags = state->tranche_flags;
    }

    state->tranche_index++;
    state->tranche_flags = 0;
}

static void zwp_linux_dmabuf_feedback_v1_handle_tranche_target_device(void *data,
        struct zwp_linux_dmabuf_feedback_v1 *feedback, struct wl_array *device)
{
}

static void zwp_linux_dmabuf_feedback_v1_handle_tranche_formats(void *data,
        struct zwp_linux_dmabuf_feedback_v1 *feedback, struct wl_array *indices)
{
    struct wayland_dmabuf_feedback *state = data;
    uint16_t *index;

    if (!state->valid)
        return;

    if (indices->size % sizeof(*index))
    {
        WARN("Ignoring invalid dmabuf feedback tranche index list size %zu.\n", indices->size);
        return;
    }

    wl_array_for_each(index, indices)
    {
        if (*index >= state->format_table_count)
        {
            WARN("Ignoring out-of-range dmabuf feedback format index %u.\n", *index);
            continue;
        }
        wayland_dmabuf_add_format_to_list(&state->pending_formats,
                state->format_table[*index].format, state->format_table[*index].modifier,
                state->tranche_index, 0);
    }
}

static void zwp_linux_dmabuf_feedback_v1_handle_tranche_flags(void *data,
        struct zwp_linux_dmabuf_feedback_v1 *feedback, uint32_t flags)
{
    struct wayland_dmabuf_feedback *state = data;

    state->tranche_flags = flags;
}

static const struct zwp_linux_dmabuf_feedback_v1_listener zwp_linux_dmabuf_feedback_v1_listener =
{
    zwp_linux_dmabuf_feedback_v1_handle_done,
    zwp_linux_dmabuf_feedback_v1_handle_format_table,
    zwp_linux_dmabuf_feedback_v1_handle_main_device,
    zwp_linux_dmabuf_feedback_v1_handle_tranche_done,
    zwp_linux_dmabuf_feedback_v1_handle_tranche_target_device,
    zwp_linux_dmabuf_feedback_v1_handle_tranche_formats,
    zwp_linux_dmabuf_feedback_v1_handle_tranche_flags,
};

BOOL wayland_dmabuf_format_supported(uint32_t format, uint64_t modifier)
{
    struct wayland_dmabuf_format *entry;
    BOOL supported = FALSE;

    pthread_mutex_lock(&process_wayland.dmabuf_mutex);
    wl_list_for_each(entry, &process_wayland.dmabuf_formats, link)
    {
        if (entry->format == format && entry->modifier == modifier)
        {
            supported = TRUE;
            break;
        }
    }
    pthread_mutex_unlock(&process_wayland.dmabuf_mutex);

    return supported;
}

/**********************************************************************
 *          Registry handling
 */

static void registry_handle_global(void *data, struct wl_registry *registry,
                                   uint32_t id, const char *interface,
                                   uint32_t version)
{
    TRACE("interface=%s version=%u id=%u\n", interface, version, id);

    if (strcmp(interface, "wl_output") == 0)
    {
        if (!wayland_output_create(id, version))
            ERR("Failed to create wayland_output for global id=%u\n", id);
    }
    else if (strcmp(interface, "zxdg_output_manager_v1") == 0)
    {
        struct wayland_output *output;

        process_wayland.zxdg_output_manager_v1 =
            wl_registry_bind(registry, id, &zxdg_output_manager_v1_interface,
                             version < 3 ? version : 3);

        /* Add zxdg_output_v1 to existing outputs. */
        wl_list_for_each(output, &process_wayland.output_list, link)
            wayland_output_use_xdg_extension(output);
    }
    else if (strcmp(interface, "wl_compositor") == 0)
    {
        process_wayland.wl_compositor =
            wl_registry_bind(registry, id, &wl_compositor_interface, 4);
    }
    else if (strcmp(interface, "xdg_wm_base") == 0)
    {
        /* version 3 is required for xdg_popup::reposition */
        if (version < 3) return;
        process_wayland.xdg_wm_base = wl_registry_bind(registry, id, &xdg_wm_base_interface, 3);
        xdg_wm_base_add_listener(process_wayland.xdg_wm_base, &xdg_wm_base_listener, NULL);
    }
    else if (strcmp(interface, "wl_shm") == 0)
    {
        process_wayland.wl_shm = wl_registry_bind(registry, id, &wl_shm_interface, 1);
    }
    else if (strcmp(interface, "wl_seat") == 0)
    {
        struct wayland_seat *seat = &process_wayland.seat;
        if (seat->wl_seat)
        {
            WARN("Only a single seat is currently supported, ignoring additional seats.\n");
            return;
        }
        if (version < 3)
        {
            ERR("wl_seat version 3 not supported, Aborting!\n");
            return;
        }
        pthread_mutex_lock(&seat->mutex);
        seat->wl_seat = wl_registry_bind(registry, id, &wl_seat_interface,
                                         version < 5 ? version : 5);
        seat->global_id = id;
        wl_seat_add_listener(seat->wl_seat, &seat_listener, NULL);
        pthread_mutex_unlock(&seat->mutex);
        if (process_wayland.zwp_text_input_manager_v3) wayland_text_input_init();
        /* Recreate the data device for the new seat. */
        if (process_wayland.data_device.zwlr_data_control_device_v1 ||
            process_wayland.data_device.ext_data_control_device_v1 ||
            process_wayland.data_device.wl_data_device)
        {
            wayland_data_device_init();
        }
    }
    else if (strcmp(interface, "wp_viewporter") == 0)
    {
        process_wayland.wp_viewporter =
            wl_registry_bind(registry, id, &wp_viewporter_interface, 1);
    }
    else if (strcmp(interface, "wl_subcompositor") == 0)
    {
        process_wayland.wl_subcompositor =
            wl_registry_bind(registry, id, &wl_subcompositor_interface, 1);
    }
    else if (strcmp(interface, "zwp_pointer_constraints_v1") == 0)
    {
        process_wayland.zwp_pointer_constraints_v1 =
            wl_registry_bind(registry, id, &zwp_pointer_constraints_v1_interface, 1);
    }
    else if (strcmp(interface, "zwp_relative_pointer_manager_v1") == 0)
    {
        process_wayland.zwp_relative_pointer_manager_v1 =
            wl_registry_bind(registry, id, &zwp_relative_pointer_manager_v1_interface, 1);
    }
    else if (strcmp(interface, "zwp_text_input_manager_v3") == 0)
    {
        process_wayland.zwp_text_input_manager_v3 =
            wl_registry_bind(registry, id, &zwp_text_input_manager_v3_interface, 1);
        if (process_wayland.seat.wl_seat) wayland_text_input_init();
    }
    else if (strcmp(interface, "zwlr_data_control_manager_v1") == 0)
    {
        process_wayland.zwlr_data_control_manager_v1 =
            wl_registry_bind(registry, id, &zwlr_data_control_manager_v1_interface, 1);
    }
    else if (strcmp(interface, "ext_data_control_manager_v1") == 0)
    {
        process_wayland.ext_data_control_manager_v1 =
            wl_registry_bind(registry, id, &ext_data_control_manager_v1_interface, 1);
    }
    else if (strcmp(interface, "wl_data_device_manager") == 0)
    {
        process_wayland.wl_data_device_manager =
            wl_registry_bind(registry, id, &wl_data_device_manager_interface, 2);
    }
    else if (strcmp(interface, "xdg_toplevel_icon_manager_v1") == 0)
    {
        process_wayland.xdg_toplevel_icon_manager_v1 =
            wl_registry_bind(registry, id, &xdg_toplevel_icon_manager_v1_interface, 1);
    }
    else if (strcmp(interface, "wp_cursor_shape_manager_v1") == 0)
    {
        process_wayland.wp_cursor_shape_manager_v1 =
            wl_registry_bind(registry, id, &wp_cursor_shape_manager_v1_interface,
                             version < 2 ? version : 2);
    }
    else if (strcmp(interface, "wp_pointer_warp_v1") == 0)
    {
        process_wayland.wp_pointer_warp_v1 =
            wl_registry_bind(registry, id, &wp_pointer_warp_v1_interface, 1);
    }
    else if (strcmp(interface, "wp_alpha_modifier_v1") == 0)
    {
        process_wayland.wp_alpha_modifier_v1 =
            wl_registry_bind(registry, id, &wp_alpha_modifier_v1_interface, 1);
    }
    else if (strcmp(interface, "wp_fractional_scale_manager_v1") == 0)
    {
        process_wayland.wp_fractional_scale_manager_v1 =
            wl_registry_bind(registry, id, &wp_fractional_scale_manager_v1_interface, 1);
    }
    else if (strcmp(interface, "xdg_toplevel_tag_manager_v1") == 0)
    {
        process_wayland.xdg_toplevel_tag_manager_v1 =
            wl_registry_bind(registry, id, &xdg_toplevel_tag_manager_v1_interface, 1);
    }
    else if (strcmp(interface, "wp_content_type_manager_v1") == 0)
    {
        process_wayland.wp_content_type_manager_v1 =
            wl_registry_bind(registry, id, &wp_content_type_manager_v1_interface, 1);
    }
    else if (strcmp(interface, "zwp_linux_explicit_synchronization_v1") == 0)
    {
        process_wayland.zwp_linux_explicit_synchronization_v1 =
            wl_registry_bind(registry, id, &zwp_linux_explicit_synchronization_v1_interface, 1);
    }
    else if (strcmp(interface, "zxdg_decoration_manager_v1") == 0)
    {
        if (wayland_disable_ssd()) return;
        if (version < 2) return;
        process_wayland.zxdg_decoration_manager_v1 =
            wl_registry_bind(registry, id, &zxdg_decoration_manager_v1_interface, 2);
    }
    else if (strcmp(interface, "wp_color_manager_v1") == 0)
    {
        struct wayland_output *output;

        process_wayland.wp_color_manager_v1 =
            wl_registry_bind(registry, id, &wp_color_manager_v1_interface,
                             version < 3 ? version : 3);
        wp_color_manager_v1_add_listener(process_wayland.wp_color_manager_v1,
                                         &wp_color_manager_listener, NULL);
        /* Add image descriptions to existing outputs. */
        wl_list_for_each(output, &process_wayland.output_list, link)
            wayland_output_use_image_description(output);
    }
    else if (strcmp(interface, "xdg_activation_v1") == 0)
    {
        process_wayland.xdg_activation_v1 =
            wl_registry_bind(registry, id, &xdg_activation_v1_interface, 1);
    }
    else if (strcmp(interface, "zwp_keyboard_shortcuts_inhibit_manager_v1") == 0)
    {
        process_wayland.zwp_keyboard_shortcuts_inhibit_manager_v1 =
            wl_registry_bind(registry, id, &zwp_keyboard_shortcuts_inhibit_manager_v1_interface, 1);
    }
    else if (strcmp(interface, "zwlr_layer_shell_v1") == 0)
    {
        process_wayland.zwlr_layer_shell_v1 =
            wl_registry_bind(registry, id, &zwlr_layer_shell_v1_interface,
                             version < 4 ? version : 4);
    }
    else if (strcmp(interface, "zwp_linux_dmabuf_v1") == 0)
    {
        process_wayland.zwp_linux_dmabuf_v1 =
            wl_registry_bind(registry, id, &zwp_linux_dmabuf_v1_interface,
                             version < 5 ? version : 5);
        zwp_linux_dmabuf_v1_add_listener(process_wayland.zwp_linux_dmabuf_v1,
                                         &zwp_linux_dmabuf_v1_listener, NULL);
        if (version >= 4)
        {
            struct wayland_dmabuf_feedback *feedback = &process_wayland.dmabuf_default_feedback;

            feedback->zwp_linux_dmabuf_feedback_v1 =
                zwp_linux_dmabuf_v1_get_default_feedback(process_wayland.zwp_linux_dmabuf_v1);
            zwp_linux_dmabuf_feedback_v1_add_listener(feedback->zwp_linux_dmabuf_feedback_v1,
                                                      &zwp_linux_dmabuf_feedback_v1_listener,
                                                      feedback);
        }
    }
}

static void registry_handle_global_remove(void *data, struct wl_registry *registry,
                                          uint32_t id)
{
    struct wayland_output *output, *tmp;
    struct wayland_seat *seat;

    TRACE("id=%u\n", id);

    wl_list_for_each_safe(output, tmp, &process_wayland.output_list, link)
    {
        if (output->global_id == id)
        {
            TRACE("removing output->name=%s\n", output->current.name);
            wayland_output_remove(output);
            return;
        }
    }

    seat = &process_wayland.seat;
    if (seat->wl_seat && seat->global_id == id)
    {
        TRACE("removing seat\n");
        if (process_wayland.pointer.wl_pointer) wayland_pointer_deinit();
        if (process_wayland.text_input.zwp_text_input_v3) wayland_text_input_deinit();
        pthread_mutex_lock(&seat->mutex);
        wl_seat_release(seat->wl_seat);
        seat->wl_seat = NULL;
        seat->global_id = 0;
        pthread_mutex_unlock(&seat->mutex);
    }
}

static const struct wl_registry_listener registry_listener = {
    registry_handle_global,
    registry_handle_global_remove
};

/**********************************************************************
 *          wayland_process_init
 *
 *  Initialise the per process wayland objects.
 *
 */
BOOL wayland_process_init(void)
{
    struct wl_display *wl_display_wrapper;

    process_wayland.wl_display = wl_display_connect(NULL);
    if (!process_wayland.wl_display)
        return FALSE;

    TRACE("wl_display=%p\n", process_wayland.wl_display);

#if (WAYLAND_VERSION_MAJOR == 1 && WAYLAND_VERSION_MINOR >= 23)
    if (!(process_wayland.wl_event_queue =
          wl_display_create_queue_with_name(process_wayland.wl_display, process_name ? process_name : "winewayland")))
#else
    if (!(process_wayland.wl_event_queue = wl_display_create_queue(process_wayland.wl_display)))
#endif
    {
        ERR("Failed to create event queue\n");
        return FALSE;
    }

    if (!(wl_display_wrapper = wl_proxy_create_wrapper(process_wayland.wl_display)))
    {
        ERR("Failed to create proxy wrapper for wl_display\n");
        return FALSE;
    }
    wl_proxy_set_queue((struct wl_proxy *) wl_display_wrapper,
                       process_wayland.wl_event_queue);

    process_wayland.wl_registry = wl_display_get_registry(wl_display_wrapper);
    wl_proxy_wrapper_destroy(wl_display_wrapper);
    if (!process_wayland.wl_registry)
    {
        ERR("Failed to get to wayland registry\n");
        return FALSE;
    }

    /* initialize win data mutex */
    wayland_window_init();

    /* Populate registry */
    wl_registry_add_listener(process_wayland.wl_registry, &registry_listener, NULL);

    /* We need two roundtrips. One to get and bind globals, one to handle all
     * initial events produced from registering the globals. */
    wl_display_roundtrip_queue(process_wayland.wl_display, process_wayland.wl_event_queue);
    wl_display_roundtrip_queue(process_wayland.wl_display, process_wayland.wl_event_queue);

    /* Resolve zxdg output state and the cached color descriptions. */
    wl_display_roundtrip_queue(process_wayland.wl_display, process_wayland.wl_event_queue);

    /* Check for required protocol globals. */
    if (!process_wayland.wl_compositor)
    {
        ERR("Wayland compositor doesn't support wl_compositor\n");
        return FALSE;
    }
    if (!process_wayland.xdg_wm_base)
    {
        ERR("Wayland compositor doesn't support xdg_wm_base\n");
        return FALSE;
    }
    if (!process_wayland.wl_shm)
    {
        ERR("Wayland compositor doesn't support wl_shm\n");
        return FALSE;
    }
    if (!process_wayland.wl_subcompositor)
    {
        ERR("Wayland compositor doesn't support wl_subcompositor\n");
        return FALSE;
    }
    if (!process_wayland.wp_viewporter)
    {
        ERR("Wayland compositor doesn't support wp_viewporter\n");
        return FALSE;
    }

    /* Check for optional globals. */
    if (!process_wayland.zwp_pointer_constraints_v1)
        ERR("Wayland compositor doesn't support optional zwp_pointer_constraints_v1 (pointer locking/confining won't work)\n");

    if (!process_wayland.zwp_relative_pointer_manager_v1)
        ERR("Wayland compositor doesn't support optional zwp_relative_pointer_manager_v1 (relative motion won't work)\n");

    if (!process_wayland.zwp_text_input_manager_v3)
        ERR("Wayland compositor doesn't support optional zwp_text_input_manager_v3 (host input methods won't work)\n");

    if (!process_wayland.zwlr_data_control_manager_v1)
    {
        if (!process_wayland.wl_data_device_manager)
            ERR("Wayland compositor doesn't support optional wl_data_device_manager (clipboard won't work)\n");
        else if (!process_wayland.ext_data_control_manager_v1)
            ERR("Wayland compositor doesn't support optional zwlr_data_control_manager_v1 or ext_data_control_manager_v1 (clipboard functionality will be limited)\n");
    }

    if (!process_wayland.xdg_toplevel_icon_manager_v1)
        ERR("Wayland compositor doesn't support xdg_toplevel_icon_manager_v1 (window icons will not be supported)\n");

    if (!process_wayland.wp_fractional_scale_manager_v1)
        ERR("Wayland compositor doesn't support wp_fractional_scale_manager_v1 (fractional scaling will be broken)\n");

    if (!process_wayland.xdg_toplevel_tag_manager_v1)
        WARN("Wayland compositor doesn't support optional xdg_toplevel_tag_manager_v1!\n");

    if (!process_wayland.wp_content_type_manager_v1)
        WARN("Wayland compositor doesn't support optional wp_content_type_manager_v1!\n");

    if (!process_wayland.zxdg_decoration_manager_v1)
        WARN("Wayland compositor doesn't support optional zxdg_decoration_manager_v1!\n");

    if (!process_wayland.xdg_activation_v1)
        ERR("Wayland compositor doesn't support xdg_activation_v1! (Window Activation will not be supported)\n");

    if (!wayland_color_manager_can_present_bt2100())
        ERR("Wayland compositor cannot present Windows BT.2100 (HDR may look broken)!\n");

    if (!process_wayland.supports_win_scrgb)
        ERR("Wayland compositor doesn't expose windows_scrgb image description (HDR may look broken)!\n");

    if (!process_wayland.zwp_linux_dmabuf_v1)
        WARN("Wayland compositor doesn't support optional zwp_linux_dmabuf_v1 (HWND dmabuf forwarding won't work)\n");

    if (!process_wayland.zwp_linux_explicit_synchronization_v1)
        TRACE("Wayland compositor doesn't support optional zwp_linux_explicit_synchronization_v1; "
              "HWND dmabuf forwarding will wait for GPU completion\n");

    if (!process_wayland.zwlr_layer_shell_v1)
        WARN("Wayland compositor doesn't support optional zwlr_layer_shell_v1 (some tray menus may be misplaced)\n");

    process_wayland.initialized = TRUE;

    return TRUE;
}
