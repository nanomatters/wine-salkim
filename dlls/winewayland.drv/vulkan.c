/* WAYLANDDRV Vulkan implementation
 *
 * Copyright 2017 Roderick Colenbrander
 * Copyright 2021 Alexandros Frantzis
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

#include <stdlib.h>
#include <string.h>

#include "ntstatus.h"
#define WIN32_NO_STATUS
#include "waylanddrv.h"
#include "wine/debug.h"

#include "wine/vulkan.h"
#include "wine/hwnd_dmabuf.h"

#ifdef __linux__
#include <sys/sysmacros.h>
#endif
#include "wine/vulkan_driver.h"

WINE_DEFAULT_DEBUG_CHANNEL(vulkan);

static const struct vulkan_driver_funcs wayland_vulkan_driver_funcs;

static void stash_client_surface(HWND hwnd, struct wayland_client_surface *surface)
{
    struct wayland_client_surface *previous;
    struct wayland_win_data *data;

    if (!(data = wayland_win_data_get(hwnd))) return;
    if ((previous = data->stashed_client) != surface)
    {
        client_surface_add_ref(&surface->client);
        data->stashed_client = surface;
    }
    wayland_win_data_release(data);

    if (previous != surface && previous) client_surface_release(&previous->client);
}

static struct wayland_client_surface *take_stashed_client_surface(HWND hwnd)
{
    struct wayland_client_surface *surface;
    struct wayland_win_data *data;

    if (!(data = wayland_win_data_get(hwnd))) return NULL;
    if ((surface = data->stashed_client) && !ReadAcquire(&surface->client.busy_ref))
    {
        /* Transfer the stash reference to the new VkSurface. */
        data->stashed_client = NULL;
        /* detach the client surface to ensure it is reparented */
        wayland_client_surface_attach(surface, NULL);
        if (data->client_surface == surface) data->client_surface = NULL;
    }
    else surface = NULL;

    wayland_win_data_release(data);
    return surface;
}

static VkResult wayland_vulkan_create_host_surface(const struct vulkan_instance *instance,
                                                   struct wl_surface *wl_surface,
                                                   VkSurfaceKHR *host_surface)
{
    VkWaylandSurfaceCreateInfoKHR create_info_host =
    {
        .sType = VK_STRUCTURE_TYPE_WAYLAND_SURFACE_CREATE_INFO_KHR,
        .display = process_wayland.wl_display,
        .surface = wl_surface,
    };

    return instance->p_vkCreateWaylandSurfaceKHR(instance->host.instance, &create_info_host,
                                                 NULL /* allocator */, host_surface);
}

/* Demote a no-longer-eligible direct-toplevel client back to the child
 * subsurface model, creating a new host surface for it when instance is
 * provided. */
static VkResult wayland_vulkan_surface_demote(HWND hwnd, const struct vulkan_instance *instance,
                                              struct client_surface *client, VkSurfaceKHR old_host_surface,
                                              VkSurfaceKHR *host_surface, BOOL *updated)
{
    struct wl_surface *new_wl_surface;
    const char *reason = NULL;
    BOOL needed = FALSE;
    VkResult res;

    if (!(new_wl_surface = wayland_client_surface_prepare_demotion(client, hwnd, &reason, &needed)))
    {
        if (needed)
        {
            WARN("Failed to demote hwnd=%p from direct toplevel: %s\n", hwnd, reason);
            client_surface_cancel_presentation_retirement(client);
            return VK_ERROR_OUT_OF_HOST_MEMORY;
        }
        TRACE("keeping hwnd=%p direct toplevel: %s\n", hwnd, reason);
        client_surface_cancel_presentation_retirement(client);
        return VK_SUCCESS;
    }
    TRACE("demoting hwnd=%p from direct toplevel: %s\n", hwnd, reason);

    if (client_surface_prepare_presentation_retirement(client))
    {
        wl_surface_destroy(new_wl_surface);
        return VK_NOT_READY;
    }

    if (instance)
    {
        res = wayland_vulkan_create_host_surface(instance, new_wl_surface, host_surface);
        if (res != VK_SUCCESS)
        {
            WARN("Failed to create demoted vulkan wayland surface, res=%d\n", res);
            client_surface_complete_presentation_retirement(client);
            wl_surface_destroy(new_wl_surface);
            return res;
        }
    }

    if (!wayland_client_surface_finish_demotion(client, hwnd, new_wl_surface, old_host_surface))
    {
        if (instance)
        {
            instance->p_vkDestroySurfaceKHR(instance->host.instance, *host_surface, NULL /* allocator */);
            *host_surface = VK_NULL_HANDLE;
        }
        wl_surface_destroy(new_wl_surface);
        client_surface_complete_presentation_retirement(client);
        return VK_ERROR_OUT_OF_DATE_KHR;
    }
    client_surface_complete_presentation_retirement(client);
    *updated = TRUE;
    return VK_SUCCESS;
}

static VkResult wayland_vulkan_surface_update(HWND hwnd, const struct vulkan_instance *instance,
                                              struct client_surface *client, VkSurfaceKHR old_host_surface,
                                              VkSurfaceKHR *host_surface, BOOL *updated)
{
    struct wayland_client_surface *impl = impl_from_client_surface(client);
    struct wl_surface *toplevel_wl_surface;
    const char *reason = NULL;
    VkResult res;

    *updated = FALSE;
    if (impl->hwnd_dmabuf_producer)
    {
        client_surface_cancel_presentation_retirement(client);
        return VK_SUCCESS;
    }

    /* An already promoted client is demoted when it stops being an eligible
     * borrower of the current toplevel. */
    if (ReadAcquire(&impl->direct_toplevel))
        return wayland_vulkan_surface_demote(hwnd, instance, client, old_host_surface, host_surface, updated);

    if (!(toplevel_wl_surface = wayland_client_surface_prepare_direct_promotion(client, hwnd, &reason)))
    {
        TRACE("direct toplevel promotion unavailable for hwnd=%p: %s\n", hwnd, reason);
        client_surface_cancel_presentation_retirement(client);
        return VK_SUCCESS;
    }

    if (client_surface_prepare_presentation_retirement(client)) return VK_NOT_READY;

    res = wayland_vulkan_create_host_surface(instance, toplevel_wl_surface, host_surface);
    if (res != VK_SUCCESS)
    {
        WARN("Failed to create promoted vulkan wayland surface, res=%d\n", res);
        client_surface_complete_presentation_retirement(client);
        return res;
    }

    if (!wayland_client_surface_finish_direct_promotion(client, hwnd, toplevel_wl_surface,
                                                        old_host_surface, *host_surface, &reason))
    {
        TRACE("direct toplevel promotion aborted for hwnd=%p: %s\n", hwnd, reason);
        instance->p_vkDestroySurfaceKHR(instance->host.instance, *host_surface, NULL /* allocator */);
        *host_surface = VK_NULL_HANDLE;
        client_surface_complete_presentation_retirement(client);
        return VK_ERROR_OUT_OF_DATE_KHR;
    }

    client_surface_complete_presentation_retirement(client);
    TRACE("hwnd=%p promoted to direct toplevel Wayland surface=%p\n", hwnd, toplevel_wl_surface);
    *updated = TRUE;
    return VK_SUCCESS;
}

static VkResult wayland_vulkan_surface_create(HWND hwnd, BOOL raw, const struct vulkan_instance *instance,
                                              VkSurfaceKHR *handle, struct client_surface **client)
{
    VkResult res;
    VkWaylandSurfaceCreateInfoKHR create_info_host;
    struct wayland_client_surface *surface;

    TRACE("%p %p %p %p\n", hwnd, instance, handle, client);
    (void)raw;

    surface = take_stashed_client_surface(hwnd);
    if (!surface && !(surface = wayland_client_surface_create(hwnd)))
    {
        ERR("Failed to create vulkan client surface\n");
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }

    create_info_host.sType = VK_STRUCTURE_TYPE_WAYLAND_SURFACE_CREATE_INFO_KHR;
    create_info_host.pNext = NULL;
    create_info_host.flags = 0; /* reserved */
    create_info_host.display = process_wayland.wl_display;
    create_info_host.surface = surface->wl_surface;

    res = instance->p_vkCreateWaylandSurfaceKHR(instance->host.instance, &create_info_host, NULL /* allocator */, handle);
    if (res != VK_SUCCESS)
    {
        ERR("Failed to create vulkan wayland surface, res=%d\n", res);
        client_surface_release(&surface->client);
        return res;
    }

    set_client_surface(hwnd, surface);
    wayland_client_surface_reactivate_direct_toplevel(&surface->client, hwnd, *handle);
    stash_client_surface(hwnd, surface);
    *client = &surface->client;

    TRACE("Created surface=0x%s, client=%p\n", wine_dbgstr_longlong(*handle), *client);
    return VK_SUCCESS;
}

static void wayland_vulkan_surface_release(struct client_surface *client, VkSurfaceKHR host_surface)
{
    wayland_client_surface_release_vulkan_surface(client, host_surface);
}

static struct wayland_fullscreen_request *find_fullscreen_request(
        struct wayland_client_surface *client, UINT64 owner)
{
    struct wayland_fullscreen_request *request;

    LIST_FOR_EACH_ENTRY(request, &client->fullscreen_requests,
                        struct wayland_fullscreen_request, entry)
        if (request->owner == owner) return request;
    return NULL;
}

static BOOL resolve_fullscreen_rect(
        const struct vulkan_surface_fullscreen_info *info, RECT *rect)
{
    struct wayland_output *output;

    if (!info || IsRectEmpty(&info->rect) ||
        (info->target != VULKAN_SURFACE_FULLSCREEN_TARGET_FIXED &&
         info->target != VULKAN_SURFACE_FULLSCREEN_TARGET_WINDOW))
        return FALSE;

    if (!(output = wayland_output_for_rect(&info->rect, rect, NULL))) return FALSE;
    wayland_output_release(output);
    return TRUE;
}

static VkBool32 wayland_vulkan_surface_fullscreen_supported(
        struct client_surface *client_surface,
        const struct vulkan_surface_fullscreen_info *info)
{
    struct wayland_client_surface *client;
    struct wayland_win_data *data;
    RECT output_rect;
    BOOL valid;

    if (!client_surface || !resolve_fullscreen_rect(info, &output_rect))
        return VK_FALSE;

    client = impl_from_client_surface(client_surface);
    wayland_win_data_lock();
    data = wayland_win_data_get_nolock(client_surface->hwnd);
    valid = data && data->client_surface == client;
    wayland_win_data_unlock();
    return valid;
}

static VkResult wayland_vulkan_surface_fullscreen(
        struct client_surface *client_surface, UINT64 owner,
        enum vulkan_surface_fullscreen_action action,
        const struct vulkan_surface_fullscreen_info *info)
{
    struct wayland_client_surface *client = impl_from_client_surface(client_surface);
    struct wayland_fullscreen_request *request, *new_request = NULL, *latest;
    struct wayland_win_data *data;
    RECT active_before, active_after, target_rect;
    BOOL had_active, has_active, refresh;
    VkResult res = VK_SUCCESS;

    if (action == VULKAN_SURFACE_FULLSCREEN_PREPARE)
    {
        if (!resolve_fullscreen_rect(info, &target_rect))
            return VK_ERROR_INITIALIZATION_FAILED;
        if (!(new_request = calloc(1, sizeof(*new_request)))) return VK_ERROR_OUT_OF_HOST_MEMORY;
        new_request->owner = owner;
        new_request->rect = target_rect;
        new_request->target = info->target;
    }

    wayland_win_data_lock();
    data = wayland_win_data_get_nolock(client_surface->hwnd);
    had_active = wayland_client_surface_get_fullscreen_rect(client, TRUE,
                                                            &active_before);

    switch (action)
    {
    case VULKAN_SURFACE_FULLSCREEN_PREPARE:
        if (!data || data->client_surface != client)
        {
            res = VK_ERROR_SURFACE_LOST_KHR;
            break;
        }
        if (find_fullscreen_request(client, owner))
        {
            res = VK_ERROR_INITIALIZATION_FAILED;
            break;
        }
        list_add_tail(&client->fullscreen_requests, &new_request->entry);
        new_request = NULL;
        break;

    case VULKAN_SURFACE_FULLSCREEN_ACQUIRE:
        if (!data || data->client_surface != client ||
            !(request = find_fullscreen_request(client, owner)))
        {
            res = VK_ERROR_FULL_SCREEN_EXCLUSIVE_MODE_LOST_EXT;
            break;
        }
        latest = LIST_ENTRY(list_tail(&client->fullscreen_requests),
                            struct wayland_fullscreen_request, entry);
        if (latest != request)
        {
            res = VK_ERROR_FULL_SCREEN_EXCLUSIVE_MODE_LOST_EXT;
            break;
        }
        client->fullscreen_active_owner = owner;
        break;

    case VULKAN_SURFACE_FULLSCREEN_RELEASE:
        if (client->fullscreen_active_owner != owner)
        {
            res = VK_ERROR_FULL_SCREEN_EXCLUSIVE_MODE_LOST_EXT;
            break;
        }
        client->fullscreen_active_owner = 0;
        break;

    case VULKAN_SURFACE_FULLSCREEN_CLEAR:
        if ((request = find_fullscreen_request(client, owner)))
        {
            list_remove(&request->entry);
            free(request);
        }
        if (client->fullscreen_active_owner == owner)
            client->fullscreen_active_owner = 0;
        break;
    }

    has_active = wayland_client_surface_get_fullscreen_rect(client, TRUE,
                                                            &active_after);
    refresh = had_active != has_active ||
              (had_active && !EqualRect(&active_before, &active_after));
    if (refresh && data && data->client_surface == client)
        wayland_win_data_refresh_fullscreen(data);
    wayland_win_data_unlock();

    free(new_request);
    return res;
}

static VkBool32 wayland_get_physical_device_presentation_support(struct vulkan_physical_device *physical_device,
                                                                 uint32_t index)
{
    struct vulkan_instance *instance = physical_device->instance;

    TRACE("%p %u\n", physical_device, index);

    return instance->p_vkGetPhysicalDeviceWaylandPresentationSupportKHR(physical_device->host.physical_device, index,
                                                                        process_wayland.wl_display);
}

static void wayland_map_instance_extensions(struct vulkan_instance_extensions *extensions)
{
    if (extensions->has_VK_KHR_win32_surface) extensions->has_VK_KHR_wayland_surface = 1;
    if (extensions->has_VK_KHR_wayland_surface) extensions->has_VK_KHR_win32_surface = 1;
}

static void wayland_map_device_extensions(struct vulkan_device_extensions *extensions)
{
    if (extensions->has_VK_KHR_external_memory_win32) extensions->has_VK_KHR_external_memory_fd = 1;
    if (extensions->has_VK_KHR_external_memory_fd) extensions->has_VK_KHR_external_memory_win32 = 1;
    if (extensions->has_VK_KHR_external_semaphore_win32) extensions->has_VK_KHR_external_semaphore_fd = 1;
    if (extensions->has_VK_KHR_external_semaphore_fd) extensions->has_VK_KHR_external_semaphore_win32 = 1;
    if (extensions->has_VK_KHR_external_fence_win32) extensions->has_VK_KHR_external_fence_fd = 1;
    if (extensions->has_VK_KHR_external_fence_fd) extensions->has_VK_KHR_external_fence_win32 = 1;
}

static enum wayland_image_description_color_space wayland_vulkan_image_color_space(
        VkColorSpaceKHR colorspace)
{
    if (colorspace == VK_COLOR_SPACE_EXTENDED_SRGB_LINEAR_EXT &&
        process_wayland.supports_win_scrgb)
        return WAYLAND_IMAGE_DESCRIPTION_SCRGB;
    if (colorspace == VK_COLOR_SPACE_HDR10_ST2084_EXT &&
        wayland_color_manager_can_present_bt2100())
        return WAYLAND_IMAGE_DESCRIPTION_BT2100;
    return WAYLAND_IMAGE_DESCRIPTION_DEFAULT;
}

static VkColorSpaceKHR wayland_vulkan_map_colorspace(VkColorSpaceKHR colorspace,
                                                     struct client_surface *client)
{
    enum wayland_image_description_color_space color_space =
        wayland_vulkan_image_color_space(colorspace);
    enum wayland_image_description_status status;

    if (!client) return colorspace;

    if (!wayland_client_surface_set_image_description(client, color_space))
    {
        status = wayland_color_manager_get_image_description(color_space, NULL);
        wayland_client_surface_set_image_description(
                client, WAYLAND_IMAGE_DESCRIPTION_DEFAULT);
        if (status == WAYLAND_IMAGE_DESCRIPTION_UNINITIALIZED ||
            status == WAYLAND_IMAGE_DESCRIPTION_PENDING)
            TRACE("Image description is not ready; using host colorspace %u.\n", colorspace);
        else
            ERR("Failed to configure image description for client surface.\n");
        return colorspace;
    }

    if (color_space == WAYLAND_IMAGE_DESCRIPTION_DEFAULT) return colorspace;
    TRACE("mapping colorspace %u => %u\n", colorspace, VK_COLOR_SPACE_PASS_THROUGH_EXT);
    return VK_COLOR_SPACE_PASS_THROUGH_EXT;
}

static void wayland_vulkan_surface_set_color_description(
        VkColorSpaceKHR colorspace, BOOL use_image_description,
        struct client_surface *client)
{
    enum wayland_image_description_color_space color_space;
    struct wayland_client_surface *surface;

    if (!client) return;
    surface = impl_from_client_surface(client);
    if (!use_image_description &&
        !ReadAcquire(&surface->has_image_description))
        return;

    color_space = use_image_description ?
                  wayland_vulkan_image_color_space(colorspace) :
                  WAYLAND_IMAGE_DESCRIPTION_DEFAULT;

    if (!wayland_client_surface_set_image_description(client, color_space))
        WARN("Failed to update image description for client surface.\n");
}

static void wayland_vulkan_surface_set_alpha(VkCompositeAlphaFlagBitsKHR alpha_bits,
                                             struct client_surface *client)
{
    /* Wayland does not support inherited alpha. */
    wayland_client_surface_set_alpha(client, !(alpha_bits & VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR));
}

static UINT wayland_vulkan_get_hwnd_dmabuf_caps(HWND hwnd, void *caps_ptr, void *format_modifiers_ptr,
                                                UINT max_format_modifiers, UINT *format_modifier_count)
{
    hwnd_dmabuf_host_caps_t *caps = caps_ptr;
    hwnd_dmabuf_format_modifier_t *format_modifiers = format_modifiers_ptr;
    struct wayland_dmabuf_format *entry;
    UINT count = 0, copied = 0;

    if (format_modifier_count) *format_modifier_count = 0;
    if (!caps || !format_modifier_count || (max_format_modifiers && !format_modifiers))
        return HWND_DMABUF_INVALID_ARGS;
    if (!process_wayland.zwp_linux_dmabuf_v1)
    {
        TRACE("hwnd %p has no linux-dmabuf transport\n", hwnd);
        return HWND_DMABUF_NOT_FOUND;
    }

    /* Most local top-levels should present directly through Vulkan WSI. If the
     * top-level's client content cannot be represented as a rectangular Wayland
     * surface, route it through the managed producer path where Wine can apply
     * the Win32 visible region. */
    {
        HWND toplevel = NtUserGetAncestor(hwnd, GA_ROOT);
        struct wayland_win_data *toplevel_data;
        BOOL toplevel_presentable_locally = FALSE;
        BOOL toplevel_unmaskable = FALSE;

        if (toplevel && (toplevel_data = wayland_win_data_get(toplevel)))
        {
            struct wayland_surface *surface = toplevel_data->wayland_surface;

            toplevel_presentable_locally = surface != NULL;
            toplevel_unmaskable = surface && wayland_surface_client_is_unmaskable(surface);
            wayland_win_data_release(toplevel_data);
        }

        if (toplevel_presentable_locally && !toplevel_unmaskable)
        {
            TRACE("hwnd %p toplevel %p has a local wayland surface; direct present, no dmabuf bridge\n",
                  hwnd, toplevel);
            return HWND_DMABUF_NOT_FOUND;
        }
    }

    pthread_mutex_lock(&process_wayland.dmabuf_mutex);
    wl_list_for_each(entry, &process_wayland.dmabuf_formats, link)
        count++;

    memset(caps, 0, sizeof(*caps));
    wl_list_for_each(entry, &process_wayland.dmabuf_formats, link)
    {
        if (copied >= max_format_modifiers)
            break;
        format_modifiers[copied].fourcc = entry->format;
        format_modifiers[copied].tranche_index = entry->tranche_index;
        format_modifiers[copied].tranche_flags = entry->tranche_flags;
        format_modifiers[copied].modifier = entry->modifier;
        copied++;
    }
    pthread_mutex_unlock(&process_wayland.dmabuf_mutex);

    caps->format_modifier_count = copied;
    caps->flags |= HWND_DMABUF_HOST_CAP_CONSUMER_STATE;
    if (process_wayland.wl_shm)
        caps->flags |= HWND_DMABUF_HOST_CAP_SHM;
    if (process_wayland.dmabuf_default_feedback.has_main_device)
    {
        unsigned int main_major = major(process_wayland.dmabuf_default_feedback.main_device);
        unsigned int main_minor = minor(process_wayland.dmabuf_default_feedback.main_device);

        caps->flags |= HWND_DMABUF_HOST_CAP_MAIN_DEVICE;
        caps->flags |= (main_major << HWND_DMABUF_HOST_CAP_MAIN_MAJOR_SHIFT) &
                       HWND_DMABUF_HOST_CAP_MAIN_MAJOR_MASK;
        caps->flags |= (main_minor << HWND_DMABUF_HOST_CAP_MAIN_MINOR_SHIFT) &
                       HWND_DMABUF_HOST_CAP_MAIN_MINOR_MASK;
    }
    if (wayland_syncobj_available() || process_wayland.zwp_linux_explicit_synchronization_v1)
        caps->flags |= HWND_DMABUF_HOST_CAP_EXPLICIT_SYNC;
    *format_modifier_count = count;
    return HWND_DMABUF_OK;
}

static const struct vulkan_driver_funcs wayland_vulkan_driver_funcs =
{
    .p_vulkan_surface_create = wayland_vulkan_surface_create,
    .p_vulkan_surface_update = wayland_vulkan_surface_update,
    .p_vulkan_surface_release = wayland_vulkan_surface_release,
    .p_vulkan_map_colorspace = wayland_vulkan_map_colorspace,
    .p_vulkan_surface_set_color_description =
        wayland_vulkan_surface_set_color_description,
    .p_vulkan_surface_set_alpha = wayland_vulkan_surface_set_alpha,
    .p_vulkan_surface_fullscreen_supported =
        wayland_vulkan_surface_fullscreen_supported,
    .p_vulkan_surface_fullscreen = wayland_vulkan_surface_fullscreen,
    .p_vulkan_get_hwnd_dmabuf_caps = wayland_vulkan_get_hwnd_dmabuf_caps,
    .p_get_physical_device_presentation_support = wayland_get_physical_device_presentation_support,
    .p_map_instance_extensions = wayland_map_instance_extensions,
    .p_map_device_extensions = wayland_map_device_extensions,
};

/**********************************************************************
 *           WAYLAND_VulkanInit
 */
UINT WAYLAND_VulkanInit(UINT version, void *vulkan_handle, const struct vulkan_driver_funcs **driver_funcs)
{
    if (version != WINE_VULKAN_DRIVER_VERSION)
    {
        ERR("version mismatch, win32u wants %u but driver has %u\n", version, WINE_VULKAN_DRIVER_VERSION);
        return STATUS_INVALID_PARAMETER;
    }

    *driver_funcs = &wayland_vulkan_driver_funcs;
    return STATUS_SUCCESS;
}
