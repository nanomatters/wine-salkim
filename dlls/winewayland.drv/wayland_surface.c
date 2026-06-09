/*
 * Wayland surfaces
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

#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/socket.h>

#include "waylanddrv.h"
#include "dxgi1_2.h"
#include "wine/debug.h"
#include "wine/hwnd_dmabuf.h"
#include "wine/server.h"

WINE_DEFAULT_DEBUG_CHANNEL(waylanddrv);

struct wayland_hwnd_dmabuf_surface;

struct wayland_hwnd_dmabuf_buffer
{
    struct wl_list link;
    struct wayland_hwnd_dmabuf_surface *surface;
    struct wl_buffer *wl_buffer;
    UINT64 producer_unique_id;
    UINT64 release_token;
    UINT64 modifier;              /* cached-slot layout identity */
    unsigned int image_id;        /* producer ring slot; cache key (stable-slot path) */
    unsigned int ring_generation; /* swapchain-rebuild counter; invalidates a cached slot */
    unsigned int fourcc;
    unsigned int stride;
    unsigned int offset;
    unsigned int alpha_mode;
    int width;
    int height;
    LONG ref;          /* owner ref + one ref per outstanding compositor commit */
    LONG released;     /* uncached path: set by wl_buffer.release, reaped next pass */
    LONG cache_valid;  /* stable-slot cache entry is still retained by this surface */
    BOOL stable_slot;  /* cached and reused per slot; release token sent on release */
    unsigned int release_flags;
    int channel_fd;    /* dup of surface->channel_fd for sending release tokens */
};

struct wayland_hwnd_dmabuf_surface
{
    struct wl_list link;
    HWND hwnd;
    struct wl_surface *wl_surface;
    struct wl_subsurface *wl_subsurface;
    struct wp_viewport *wp_viewport;
    struct wayland_hwnd_dmabuf_buffer *current;
    struct wl_list buffers;
    UINT frame_seq;
    BOOL seen;
    unsigned long long last_seen_ms; /* tick when last present in the child list */
    int channel_fd;                 /* consumer end of the producer socket, or -1 */
};

/* A child may briefly drop out of the descendant list between frames; tearing its surface
 * (and dmabuf cache) down on a single miss churns the cache and, with fd-once, strands slots
 * the producer thinks are still cached. Keep an unseen surface for a grace window first. */
#define WAYLAND_DMABUF_SURFACE_GRACE_MS 1000

static unsigned long long wayland_dmabuf_now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (unsigned long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static unsigned int wayland_hwnd_dmabuf_buffer_cache_flags(struct wayland_hwnd_dmabuf_buffer *buffer)
{
    if (buffer->stable_slot && InterlockedCompareExchange(&buffer->cache_valid, FALSE, FALSE))
        return HWND_DMABUF_RELEASE_CACHED;
    return 0;
}

static UINT64 wayland_hwnd_dmabuf_buffer_exchange_release_token(struct wayland_hwnd_dmabuf_buffer *buffer,
                                                                UINT64 release_token)
{
    LONGLONG volatile *token = (LONGLONG volatile *)&buffer->release_token;
    LONGLONG old;

    do old = *token;
    while (InterlockedCompareExchange64(token, release_token, old) != old);
    return old;
}

static void wayland_hwnd_dmabuf_buffer_send_release(struct wayland_hwnd_dmabuf_buffer *buffer,
                                                    unsigned int flags)
{
    UINT64 release_token;

    if (buffer->channel_fd < 0) return;
    release_token = wayland_hwnd_dmabuf_buffer_exchange_release_token(buffer, 0);
    if (release_token)
    {
        hwnd_dmabuf_release_t rel = { buffer->producer_unique_id, release_token,
                                      flags | wayland_hwnd_dmabuf_buffer_cache_flags(buffer),
                                      buffer->image_id, buffer->ring_generation, 0 };
        send(buffer->channel_fd, &rel, sizeof(rel), MSG_DONTWAIT | MSG_NOSIGNAL);
    }
}

/* Drop a buffer reference; on the last unref destroy the wl_buffer and free.
 * This is lock-free and may run on either the present thread or the event
 * thread: the last unref happens either inside the wl_buffer.release handler
 * (event thread) or on the present thread after release already fired, so the
 * wl_buffer_destroy is never concurrent with a release dispatch. */
static void wayland_hwnd_dmabuf_buffer_unref(struct wayland_hwnd_dmabuf_buffer *buffer)
{
    if (InterlockedDecrement(&buffer->ref) > 0) return;
    wayland_hwnd_dmabuf_buffer_send_release(buffer, buffer->release_flags ?
                                            buffer->release_flags : HWND_DMABUF_RELEASE_ORPHANED);
    if (buffer->channel_fd >= 0) close(buffer->channel_fd);
    if (buffer->wl_buffer) wl_buffer_destroy(buffer->wl_buffer);
    free(buffer);
}

/* Send a release token to the producer over its channel so it can recycle the image. */
static void wayland_hwnd_dmabuf_send_release(struct wayland_hwnd_dmabuf_surface *surface,
                                             UINT64 producer_unique_id, UINT64 release_token,
                                             unsigned int flags, unsigned int image_id,
                                             unsigned int ring_generation)
{
    hwnd_dmabuf_release_t rel = { producer_unique_id, release_token, flags, image_id, ring_generation, 0 };

    if (release_token && surface->channel_fd >= 0)
        send(surface->channel_fd, &rel, sizeof(rel), MSG_DONTWAIT | MSG_NOSIGNAL);
}

/* Present-thread teardown: detach from the surface, and drop the owner ref.
 * Must be called with win_data_mutex held.
 * If the compositor still holds the buffer it survives as an orphan until its
 * release handler drops the last ref. */
static void wayland_hwnd_dmabuf_buffer_reap(struct wayland_hwnd_dmabuf_buffer *buffer)
{
    struct wayland_hwnd_dmabuf_surface *surface = buffer->surface;

    if (surface && surface->current == buffer) surface->current = NULL;
    InterlockedExchange(&buffer->cache_valid, FALSE);
    wl_list_remove(&buffer->link);
    buffer->surface = NULL;
    wayland_hwnd_dmabuf_buffer_unref(buffer);
}

/* wl_buffer.release runs on the event thread: lock-free only (no surface/list
 * mutation). Cached (stable-slot) buffers stay cached for the slot's next frame,
 * so send the release token now to recycle the slot; the busy gate keeps the
 * producer from overwriting it until then. Uncached buffers are flagged for the
 * present thread to reap next pass (their token is sent on destroy). Either way
 * drop the per-commit ref. */
static void wayland_hwnd_dmabuf_buffer_handle_release(void *data, struct wl_buffer *wl_buffer)
{
    struct wayland_hwnd_dmabuf_buffer *buffer = data;

    if (buffer->stable_slot)
        wayland_hwnd_dmabuf_buffer_send_release(buffer, HWND_DMABUF_RELEASE_PRESENTED);
    else
    {
        buffer->release_flags = HWND_DMABUF_RELEASE_PRESENTED;
        InterlockedExchange(&buffer->released, TRUE);
    }
    wayland_hwnd_dmabuf_buffer_unref(buffer);
}

static const struct wl_buffer_listener wayland_hwnd_dmabuf_buffer_listener =
{
    wayland_hwnd_dmabuf_buffer_handle_release
};

static void wayland_hwnd_dmabuf_surface_destroy(struct wayland_hwnd_dmabuf_surface *surface)
{
    struct wayland_hwnd_dmabuf_buffer *buffer, *buffer_next;

    wl_list_for_each_safe(buffer, buffer_next, &surface->buffers, link)
        wayland_hwnd_dmabuf_buffer_reap(buffer);

    wl_list_remove(&surface->link);
    if (surface->wl_subsurface) wl_subsurface_destroy(surface->wl_subsurface);
    if (surface->wp_viewport) wp_viewport_destroy(surface->wp_viewport);
    if (surface->wl_surface) wl_surface_destroy(surface->wl_surface);
    if (surface->channel_fd >= 0) close(surface->channel_fd);
    free(surface);
}

static void xdg_surface_handle_configure(void *private, struct xdg_surface *xdg_surface,
                                         uint32_t serial)
{
    struct wayland_surface *surface;
    BOOL should_post = FALSE, initial_configure = FALSE;
    struct wayland_win_data *data;
    HWND hwnd = private;

    TRACE("serial=%u\n", serial);

    if (!(data = wayland_win_data_get(hwnd))) return;

    /* Handle this event only if wayland_surface is still associated with
     * the target xdg_surface. */
    if ((surface = data->wayland_surface) && surface->xdg_surface == xdg_surface)
    {
        if (wayland_surface_is_toplevel(surface))
        {
            /* If we have a previously requested config, we have already sent a
             * WM_WAYLAND_CONFIGURE which hasn't been handled yet. In that case,
             * avoid sending another message to reduce message queue traffic. */
            should_post = surface->requested.serial == 0;
            initial_configure = surface->current.serial == 0;
            surface->pending.serial = serial;
            if (!surface->pending.caps && surface->current.caps)
                surface->pending.caps = surface->current.caps;
            if (!surface->pending.decor && surface->current.decor)
                surface->pending.decor = surface->current.decor;
            else if (surface->pending.decor)
            {
                should_post |= (surface->pending.decor != surface->current.decor);
                initial_configure |= (surface->pending.decor != surface->current.decor);
            }
            surface->requested = surface->pending;
            memset(&surface->pending, 0, sizeof(surface->pending));
        }
        else if (wayland_surface_is_popup(surface))
        {
            /* Popups take their geometry from the positioner, not configure
             * negotiation, so ack immediately and expose so the surface maps. */
            xdg_surface_ack_configure(surface->xdg_surface, serial);
            initial_configure = surface->current.serial == 0;
            surface->current.serial = serial;
        }
    }

    wayland_win_data_release(data);

    if (should_post) NtUserPostMessage(hwnd, WM_WAYLAND_CONFIGURE, 0, 0);

    /* Flush the window surface in case there is content that we weren't
     * able to flush before due to the lack of the initial configure. */
    if (initial_configure)
    {
        NtUserExposeWindowSurface(hwnd, 0, NULL, 0);
    }
}

static const struct xdg_surface_listener xdg_surface_listener =
{
    xdg_surface_handle_configure
};

static void xdg_toplevel_handle_configure(void *private,
                                          struct xdg_toplevel *xdg_toplevel,
                                          int32_t width, int32_t height,
                                          struct wl_array *states)
{
    struct wayland_surface *surface;
    HWND hwnd = private;
    uint32_t *state;
    enum wayland_surface_config_state config_state = 0;
    struct wayland_win_data *data;

    wl_array_for_each(state, states)
    {
        switch(*state)
        {
        case XDG_TOPLEVEL_STATE_MAXIMIZED:
            config_state |= WAYLAND_SURFACE_CONFIG_STATE_MAXIMIZED;
            break;
        case XDG_TOPLEVEL_STATE_RESIZING:
            config_state |= WAYLAND_SURFACE_CONFIG_STATE_RESIZING;
            break;
        case XDG_TOPLEVEL_STATE_TILED_LEFT:
        case XDG_TOPLEVEL_STATE_TILED_RIGHT:
        case XDG_TOPLEVEL_STATE_TILED_TOP:
        case XDG_TOPLEVEL_STATE_TILED_BOTTOM:
            config_state |= WAYLAND_SURFACE_CONFIG_STATE_TILED;
            break;
        case XDG_TOPLEVEL_STATE_FULLSCREEN:
            config_state |= WAYLAND_SURFACE_CONFIG_STATE_FULLSCREEN;
            break;
        default:
            break;
        }
    }

    TRACE("hwnd=%p %dx%d,%#x\n", hwnd, width, height, config_state);

    if (!(data = wayland_win_data_get(hwnd))) return;

    if ((surface = data->wayland_surface) && wayland_surface_is_toplevel(surface))
    {
        surface->pending.width = width;
        surface->pending.height = height;
        surface->pending.state = config_state;
    }

    wayland_win_data_release(data);
}

static void xdg_toplevel_handle_close(void *data, struct xdg_toplevel *xdg_toplevel)
{
    NtUserPostMessage((HWND)data, WM_SYSCOMMAND, SC_CLOSE, 0);
}
static void xdg_popup_handle_configure(void *data, struct xdg_popup *xdg_popup,
                                       int32_t x, int32_t y, int32_t width, int32_t height)
{
    /* Compositor-chosen geometry (relative to parent). Trace only: the popup's
     * content is composited at its surface regardless of this. */
    TRACE("hwnd=%p pos=%d,%d size=%dx%d\n", (HWND)data, x, y, width, height);
}

static void xdg_popup_handle_popup_done(void *data, struct xdg_popup *xdg_popup)
{
    HWND hwnd = data;
    TRACE("hwnd=%p dismissed by compositor\n", hwnd);
    /* Let Win32's own menu loop tear the window down (it then hides the window,
     * which clears the role and destroys the xdg_popup in clear_role). Do not
     * destroy the xdg_popup here, to keep a single, correctly-ordered teardown. */
    NtUserPostMessage(hwnd, WM_CANCELMODE, 0, 0);
}

static const struct xdg_popup_listener xdg_popup_listener =
{
    xdg_popup_handle_configure,
    xdg_popup_handle_popup_done,
};


static void xdg_toplevel_handle_configure_bounds(void *private, struct xdg_toplevel *xdg_toplevel, int width, int height)
{
    HWND hwnd = private;

    /* FIXME: we don't respect these yet */
    TRACE("hwnd %p, (%d, %d)\n", hwnd, width, height);
}

static void xdg_toplevel_handle_wm_caps(void *private, struct xdg_toplevel *xdg_toplevel, struct wl_array *caps)
{
    int *state;
    HWND hwnd = private;
    struct wayland_surface *surface;
    struct wayland_win_data *data;
    enum wayland_surface_wm_caps cap = WAYLAND_SURFACE_WM_CAPS_CHANGED;

    wl_array_for_each(state, caps)
    {
        switch (*state)
        {
            case XDG_TOPLEVEL_WM_CAPABILITIES_FULLSCREEN:
                cap |= WAYLAND_SURFACE_WM_CAPS_FULLSCREEN;
                break;
            case XDG_TOPLEVEL_WM_CAPABILITIES_MINIMIZE:
                cap |= WAYLAND_SURFACE_WM_CAPS_MINIMIZE;
                break;
            case XDG_TOPLEVEL_WM_CAPABILITIES_MAXIMIZE:
                cap |= WAYLAND_SURFACE_WM_CAPS_MAXIMIZE;
                break;
            case XDG_TOPLEVEL_WM_CAPABILITIES_WINDOW_MENU:
                cap |= WAYLAND_SURFACE_WM_CAPS_SHOW_WINDOW;
                break;
            default: break;
        }
    }

    TRACE("hwnd %p caps %x\n", hwnd, cap);

    if (!(cap & WAYLAND_SURFACE_WM_CAPS_FULLSCREEN))
        WARN("Compositor does not support fullscreen!\n");
    if (!(cap & WAYLAND_SURFACE_WM_CAPS_MAXIMIZE))
        WARN("Compositor does not support maximize!\n");
    if (!(cap & WAYLAND_SURFACE_WM_CAPS_MINIMIZE))
        WARN("Compositor does not support minimize, cannot implement window focus loss!\n");

    if (!(data = wayland_win_data_get(hwnd))) return;

    if ((surface = data->wayland_surface) && wayland_surface_is_toplevel(surface))
    {
        surface->pending.caps = cap;
    }

    wayland_win_data_release(data);
}

static const struct xdg_toplevel_listener xdg_toplevel_listener =
{
    xdg_toplevel_handle_configure,
    xdg_toplevel_handle_close,
    xdg_toplevel_handle_configure_bounds,
    xdg_toplevel_handle_wm_caps
};

void wp_fractional_scale_handle_scale(void* user_data,
                                      struct wp_fractional_scale_v1 *fractional_scale_v1,
                                      uint32_t scale_fixed)
{
    struct wayland_win_data *data;
    struct wayland_surface *surface;
    HWND hwnd = user_data;
    BOOL updated = FALSE;

    if ((data = wayland_win_data_get(hwnd)))
    {
        if ((surface = data->wayland_surface))
        {
            double scale = scale_fixed / 120.0;

            updated = (scale != surface->window.fractional_scale);
            surface->window.scale = surface->window.fractional_scale = scale;

            if (updated)
            {
                /* detach the client surface as its rect has changed */
                if (data->client_surface)
                {
                    wayland_client_surface_attach(data->client_surface, NULL);
                    data->client_surface = NULL;
                }

                /* the subsurface rect has changed */
                if (surface->role == WAYLAND_SURFACE_ROLE_SUBSURFACE)
                {
                    surface->processing.serial = 1;
                    surface->processing.processed = TRUE;
                }
            }

            TRACE("Got scale %lf\n", scale);
        }

        wayland_win_data_release(data);
    }

    if (updated) NtUserExposeWindowSurface(hwnd, 0, NULL, 0);
}

static const struct wp_fractional_scale_v1_listener wp_fractional_scale_listener =
{
    wp_fractional_scale_handle_scale
};

static void zxdg_toplevel_decoration_v1_configure(void *user_data,
                                                  struct zxdg_toplevel_decoration_v1 *decoration,
                                                  uint32_t mode)
{
    struct wayland_win_data *data;
    struct wayland_surface *surface;
    HWND hwnd = user_data;

    if ((data = wayland_win_data_get(hwnd)))
    {
        if ((surface = data->wayland_surface) && wayland_surface_is_toplevel(surface))
        {
            TRACE("got mode %u for surface %p\n", mode, surface);
            surface->pending.decor = mode;
        }
        wayland_win_data_release(data);
    }
}

static const struct zxdg_toplevel_decoration_v1_listener zxdg_toplevel_decoration_listener =
{
    zxdg_toplevel_decoration_v1_configure
};

/**********************************************************************
 *          wayland_surface_create
 *
 * Creates a role-less wayland surface.
 */
struct wayland_surface *wayland_surface_create(HWND hwnd)
{
    struct wayland_surface *surface;

    surface = calloc(1, sizeof(*surface));
    if (!surface)
    {
        ERR("Failed to allocate space for Wayland surface\n");
        goto err;
    }

    TRACE("surface=%p\n", surface);

    surface->hwnd = hwnd;
    wl_list_init(&surface->hwnd_dmabuf_surfaces);
    surface->wl_surface = wl_compositor_create_surface(process_wayland.wl_compositor);
    if (!surface->wl_surface)
    {
        ERR("Failed to create wl_surface Wayland surface\n");
        goto err;
    }
    wl_surface_set_user_data(surface->wl_surface, hwnd);

    surface->wp_viewport =
        wp_viewporter_get_viewport(process_wayland.wp_viewporter,
                                   surface->wl_surface);
    if (!surface->wp_viewport)
    {
        ERR("Failed to create wp_viewport Wayland surface\n");
        goto err;
    }

    surface->window.scale = 1.0;

    return surface;

err:
    if (surface) wayland_surface_destroy(surface);
    return NULL;
}

/**********************************************************************
 *          wayland_surface_destroy
 *
 * Destroys a wayland surface.
 */
void wayland_surface_destroy(struct wayland_surface *surface)
{
    struct wayland_hwnd_dmabuf_surface *dmabuf_surface, *dmabuf_surface_next;

    wl_list_for_each_safe(dmabuf_surface, dmabuf_surface_next,
                          &surface->hwnd_dmabuf_surfaces, link)
        wayland_hwnd_dmabuf_surface_destroy(dmabuf_surface);

    if (surface->dmabuf_frame_cb) wl_callback_destroy(surface->dmabuf_frame_cb);

    pthread_mutex_lock(&process_wayland.pointer.mutex);
    if (process_wayland.pointer.focused_hwnd == surface->hwnd)
    {
        process_wayland.pointer.focused_hwnd = NULL;
        process_wayland.pointer.enter_serial = 0;
    }
    if (process_wayland.pointer.constraint_hwnd == surface->hwnd)
        wayland_pointer_clear_constraint();
    pthread_mutex_unlock(&process_wayland.pointer.mutex);

    pthread_mutex_lock(&process_wayland.keyboard.mutex);
    if (process_wayland.keyboard.focused_hwnd == surface->hwnd)
        process_wayland.keyboard.focused_hwnd = NULL;
    pthread_mutex_unlock(&process_wayland.keyboard.mutex);

    pthread_mutex_lock(&process_wayland.text_input.mutex);
    if (process_wayland.text_input.focused_hwnd == surface->hwnd)
        process_wayland.text_input.focused_hwnd = NULL;
    pthread_mutex_unlock(&process_wayland.text_input.mutex);

    wayland_surface_clear_role(surface);

    if (surface->wp_viewport)
    {
        wp_viewport_destroy(surface->wp_viewport);
        surface->wp_viewport = NULL;
    }

    if (surface->wl_surface)
    {
        wl_surface_destroy(surface->wl_surface);
        surface->wl_surface = NULL;
    }

    if (surface->big_icon_buffer)
    {
        wayland_shm_buffer_unref(surface->big_icon_buffer);
        surface->big_icon_buffer = NULL;
    }

    if (surface->small_icon_buffer)
    {
        wayland_shm_buffer_unref(surface->small_icon_buffer);
        surface->small_icon_buffer = NULL;
    }

    wl_display_flush(process_wayland.wl_display);

    free(surface);
}

static void wayland_surface_init_fractional_scale(struct wayland_surface *surface,
                                                  double initial_scale)
{
    if (process_wayland.wp_fractional_scale_manager_v1)
    {
        surface->window.fractional_scale = initial_scale;
        surface->wp_fractional_scale_v1 =
            wp_fractional_scale_manager_v1_get_fractional_scale(
                process_wayland.wp_fractional_scale_manager_v1,
                surface->wl_surface);
        if (!surface->wp_fractional_scale_v1)
        {
            ERR("Failed to create wp_fractional_scale_v1\n");
            return;
        }
        wp_fractional_scale_v1_add_listener(
            surface->wp_fractional_scale_v1,
            &wp_fractional_scale_listener,
            surface->hwnd);
    }
}

static void wayland_surface_init_decoration(struct wayland_surface *surface)
{
    TRACE("surface %p\n", surface);

    if (process_wayland.zxdg_decoration_manager_v1)
    {
        surface->current.decor = 0;
        surface->zxdg_toplevel_decoration_v1 =
        zxdg_decoration_manager_v1_get_toplevel_decoration(
            process_wayland.zxdg_decoration_manager_v1,
            surface->xdg_toplevel);
        if (!surface->zxdg_toplevel_decoration_v1)
        {
            ERR("Failed to create toplevel zxdg_toplevel_decoration_v1\n");
            return;
        }
        zxdg_toplevel_decoration_v1_set_mode(
            surface->zxdg_toplevel_decoration_v1,
            ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
        zxdg_toplevel_decoration_v1_add_listener(
            surface->zxdg_toplevel_decoration_v1,
            &zxdg_toplevel_decoration_listener,
            surface->hwnd);
    }
}

void wayland_surface_sync_alpha(struct wayland_surface *surface)
{
    if (!process_wayland.wp_alpha_modifier_v1) return;

    if (surface->alpha_multiplier != UINT32_MAX)
    {
        if (!surface->wp_alpha_modifier_surface_v1)
        {
            surface->wp_alpha_modifier_surface_v1 =
                wp_alpha_modifier_v1_get_surface(process_wayland.wp_alpha_modifier_v1,
                                                 surface->wl_surface);
        }
        if (!surface->wp_alpha_modifier_surface_v1)
        {
            ERR("Failed to create alpha modifier surface\n");
            return;
        }
        wp_alpha_modifier_surface_v1_set_multiplier(surface->wp_alpha_modifier_surface_v1,
                                                    surface->alpha_multiplier);
    }
    else if (surface->wp_alpha_modifier_surface_v1)
    {
        wp_alpha_modifier_surface_v1_destroy(surface->wp_alpha_modifier_surface_v1);
        surface->wp_alpha_modifier_surface_v1 = NULL;
    }
}

/**********************************************************************
 *          wayland_surface_make_toplevel
 *
 * Gives the toplevel role to a plain wayland surface.
 */
void wayland_surface_make_toplevel(struct wayland_surface *surface, BOOL server_decor)
{
    static char steam_proton[] = "steam_proton";
    const char *app_id = getenv("SteamAppId");
    char proton_app_class[128];
    WCHAR text[1024];

    TRACE("surface=%p\n", surface);

    assert(!surface->role || surface->role == WAYLAND_SURFACE_ROLE_TOPLEVEL);
    if (surface->xdg_surface && surface->xdg_toplevel)
    {
        if (process_wayland.zxdg_decoration_manager_v1)
        {
            if (!server_decor && surface->zxdg_toplevel_decoration_v1)
            {
                zxdg_toplevel_decoration_v1_destroy(surface->zxdg_toplevel_decoration_v1);
                surface->zxdg_toplevel_decoration_v1 = NULL;
                surface->pending.decor = surface->current.decor = 0;
            }
            else if (server_decor && !surface->zxdg_toplevel_decoration_v1)
                wayland_surface_init_decoration(surface);

            wl_surface_commit(surface->wl_surface);
            wl_display_flush(process_wayland.wl_display);
        }
        return;
    }

    wayland_surface_clear_role(surface);
    surface->role = WAYLAND_SURFACE_ROLE_TOPLEVEL;

    surface->xdg_surface =
        xdg_wm_base_get_xdg_surface(process_wayland.xdg_wm_base, surface->wl_surface);
    if (!surface->xdg_surface) goto err;
    xdg_surface_add_listener(surface->xdg_surface, &xdg_surface_listener, surface->hwnd);

    surface->xdg_toplevel = xdg_surface_get_toplevel(surface->xdg_surface);
    if (!surface->xdg_toplevel) goto err;
    xdg_toplevel_add_listener(surface->xdg_toplevel, &xdg_toplevel_listener, surface->hwnd);

    if(!app_id || !*app_id) {
        app_id = getenv("WINE_WMCLASS");
    }

    if (app_id && *app_id) {
        snprintf(proton_app_class, sizeof(proton_app_class), "steam_app_%s", app_id);
        xdg_toplevel_set_app_id(surface->xdg_toplevel, proton_app_class);
    } else {
        xdg_toplevel_set_app_id(surface->xdg_toplevel, steam_proton);
    }

    if (process_wayland.xdg_toplevel_tag_manager_v1)
    {
        xdg_toplevel_tag_manager_v1_set_toplevel_tag(
            process_wayland.xdg_toplevel_tag_manager_v1, surface->xdg_toplevel,
            "proton-game"
        );
        xdg_toplevel_tag_manager_v1_set_toplevel_description(
            process_wayland.xdg_toplevel_tag_manager_v1, surface->xdg_toplevel,
            "This is a game running through proton"
        );
    }

    if (!NtUserInternalGetWindowText(surface->hwnd, text, ARRAY_SIZE(text)))
        text[0] = 0;
    wayland_surface_set_title(surface, text);

    wayland_surface_assign_icon(surface);

    wayland_surface_init_fractional_scale(surface, 1.0);

    wayland_surface_sync_alpha(surface);

    if (server_decor) wayland_surface_init_decoration(surface);

    wl_surface_commit(surface->wl_surface);
    wl_display_flush(process_wayland.wl_display);

    return;

err:
    wayland_surface_clear_role(surface);
    ERR("Failed to assign toplevel role to wayland surface\n");
}

/**********************************************************************
 *          wayland_surface_make_subsurface
 *
 * Gives the subsurface role to a plain wayland surface.
 */
void wayland_surface_make_subsurface(struct wayland_surface *surface,
                                     struct wayland_surface *parent)
{
    assert(!surface->role || surface->role == WAYLAND_SURFACE_ROLE_SUBSURFACE);
    if (surface->wl_subsurface && surface->toplevel_hwnd == parent->hwnd) return;

    wayland_surface_clear_role(surface);
    surface->role = WAYLAND_SURFACE_ROLE_SUBSURFACE;

    TRACE("surface=%p parent=%p\n", surface, parent);

    surface->wl_subsurface =
        wl_subcompositor_get_subsurface(process_wayland.wl_subcompositor,
                                        surface->wl_surface,
                                        parent->wl_surface);
    if (!surface->wl_subsurface)
    {
        ERR("Failed to create client wl_subsurface\n");
        goto err;
    }

    wayland_surface_init_fractional_scale(surface, parent->window.fractional_scale);

    wayland_surface_sync_alpha(surface);

    surface->role = WAYLAND_SURFACE_ROLE_SUBSURFACE;
    surface->toplevel_hwnd = parent->hwnd;

    /* Present contents independently of the parent surface. */
    wl_subsurface_set_desync(surface->wl_subsurface);

    wl_display_flush(process_wayland.wl_display);

    return;

err:
    wayland_surface_clear_role(surface);
    ERR("Failed to assign subsurface role to wayland surface\n");
}

/**********************************************************************
 *          wayland_surface_make_popup
 *
 * Gives the xdg_popup role to a plain wayland surface, positioned by the
 * compositor relative to the parent (owner) surface. Used for owned,
 * caption-less popups (menus, dropdowns, tooltips): a Wayland toplevel has no
 * client-set position, so these must be popups to land where the app intends.
 */
void wayland_surface_make_popup(struct wayland_surface *surface,
                                struct wayland_surface *parent)
{
    struct xdg_positioner *positioner;
    int x, y, w, h;

    TRACE("surface=%p parent=%p\n", surface, parent);

    assert(!surface->role || surface->role == WAYLAND_SURFACE_ROLE_POPUP);
    if (surface->xdg_surface && surface->xdg_popup) return;

    wayland_surface_clear_role(surface);
    surface->role = WAYLAND_SURFACE_ROLE_POPUP;

    surface->xdg_surface =
        xdg_wm_base_get_xdg_surface(process_wayland.xdg_wm_base, surface->wl_surface);
    if (!surface->xdg_surface) goto err;
    xdg_surface_add_listener(surface->xdg_surface, &xdg_surface_listener, surface->hwnd);

    if (!(positioner = xdg_wm_base_create_positioner(process_wayland.xdg_wm_base)))
        goto err;

    /* Anchor the popup's top-left at the popup's offset from the parent window
     * origin (in parent-surface coords), growing down and to the right. */
    wayland_surface_coords_from_window(parent,
                                       surface->window.rect.left - parent->window.rect.left,
                                       surface->window.rect.top - parent->window.rect.top,
                                       &x, &y);
    wayland_surface_coords_from_window(surface,
                                       surface->window.rect.right - surface->window.rect.left,
                                       surface->window.rect.bottom - surface->window.rect.top,
                                       &w, &h);
    if (w < 1) w = 1;
    if (h < 1) h = 1;

    xdg_positioner_set_size(positioner, w, h);
    xdg_positioner_set_anchor_rect(positioner, x, y, 1, 1);
    xdg_positioner_set_anchor(positioner, XDG_POSITIONER_ANCHOR_TOP_LEFT);
    xdg_positioner_set_gravity(positioner, XDG_POSITIONER_GRAVITY_BOTTOM_RIGHT);
    xdg_positioner_set_constraint_adjustment(positioner,
        XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_FLIP_X |
        XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_FLIP_Y |
        XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_SLIDE_X |
        XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_SLIDE_Y);

    surface->xdg_popup = xdg_surface_get_popup(surface->xdg_surface,
                                               parent->xdg_surface, positioner);
    xdg_positioner_destroy(positioner);
    if (!surface->xdg_popup) goto err;
    xdg_popup_add_listener(surface->xdg_popup, &xdg_popup_listener, surface->hwnd);

    /* Grab so the compositor routes input to the menu and dismisses it
     * (xdg_popup.popup_done) when the user clicks outside it -- including onto
     * another window -- the way a menu is expected to behave. The grab uses the
     * most recent input serial (menus open in response to a click); if the
     * compositor does not accept the serial it simply ignores the grab and the
     * popup still positions correctly. */
    {
        uint32_t serial = process_wayland.input_serial;
        pthread_mutex_lock(&process_wayland.seat.mutex);
        if (process_wayland.seat.wl_seat && serial)
            xdg_popup_grab(surface->xdg_popup, process_wayland.seat.wl_seat, serial);
        pthread_mutex_unlock(&process_wayland.seat.mutex);
    }

    wl_surface_commit(surface->wl_surface);
    wl_display_flush(process_wayland.wl_display);

    return;

err:
    wayland_surface_clear_role(surface);
    ERR("Failed to assign popup role to wayland surface\n");
}

/**********************************************************************
 *          wayland_surface_clear_role
 *
 * Clears the role related Wayland objects of a Wayland surface, making it a
 * plain surface again. We can later assign the same role (but not a
 * different one!) to the surface.
 */
void wayland_surface_clear_role(struct wayland_surface *surface)
{
    TRACE("surface=%p\n", surface);

    /* some objects are shared between several roles */

    if (surface->wp_fractional_scale_v1)
    {
        wp_fractional_scale_v1_destroy(surface->wp_fractional_scale_v1);
        surface->wp_fractional_scale_v1 = NULL;
    }

    if (surface->wp_alpha_modifier_surface_v1)
    {
        wp_alpha_modifier_surface_v1_destroy(surface->wp_alpha_modifier_surface_v1);
        surface->wp_alpha_modifier_surface_v1 = NULL;
    }

    switch (surface->role)
    {
    case WAYLAND_SURFACE_ROLE_NONE:
        break;

    case WAYLAND_SURFACE_ROLE_TOPLEVEL:
        if (surface->xdg_toplevel_icon)
        {
            xdg_toplevel_icon_manager_v1_set_icon(
                process_wayland.xdg_toplevel_icon_manager_v1,
                surface->xdg_toplevel, NULL);
            xdg_toplevel_icon_v1_destroy(surface->xdg_toplevel_icon);
            surface->xdg_toplevel_icon = NULL;
        }

        if (surface->zwp_keyboard_shortcuts_inhibitor_v1)
        {
            zwp_keyboard_shortcuts_inhibitor_v1_destroy(
                surface->zwp_keyboard_shortcuts_inhibitor_v1);
            surface->zwp_keyboard_shortcuts_inhibitor_v1 = NULL;
        }

        if (surface->zxdg_toplevel_decoration_v1)
        {
            zxdg_toplevel_decoration_v1_destroy(surface->zxdg_toplevel_decoration_v1);
            surface->zxdg_toplevel_decoration_v1 = NULL;
        }

        if (surface->xdg_toplevel)
        {
            xdg_toplevel_destroy(surface->xdg_toplevel);
            surface->xdg_toplevel = NULL;
        }

        if (surface->xdg_surface)
        {
            xdg_surface_destroy(surface->xdg_surface);
            surface->xdg_surface = NULL;
        }

        surface->requested_output = NULL;
        break;

    case WAYLAND_SURFACE_ROLE_SUBSURFACE:
        if (surface->wl_subsurface)
        {
            wl_subsurface_destroy(surface->wl_subsurface);
            surface->wl_subsurface = NULL;
        }

        surface->toplevel_hwnd = 0;
        break;

    case WAYLAND_SURFACE_ROLE_POPUP:
        /* The xdg_popup must be destroyed before its xdg_surface. */
        if (surface->xdg_popup)
        {
            xdg_popup_destroy(surface->xdg_popup);
            surface->xdg_popup = NULL;
        }

        if (surface->xdg_surface)
        {
            xdg_surface_destroy(surface->xdg_surface);
            surface->xdg_surface = NULL;
        }
        break;
    }

    memset(&surface->pending, 0, sizeof(surface->pending));
    memset(&surface->requested, 0, sizeof(surface->requested));
    memset(&surface->processing, 0, sizeof(surface->processing));
    memset(&surface->current, 0, sizeof(surface->current));
    surface->toplevel_hwnd = 0;

    /* Ensure no buffer is attached, otherwise future role assignments may fail. */
    wl_surface_attach(surface->wl_surface, NULL, 0, 0);
    wl_surface_commit(surface->wl_surface);

    surface->content_width = 0;
    surface->content_height = 0;
    SetRect(&surface->geometry, 0, 0, 0, 0);

    wl_display_flush(process_wayland.wl_display);
}

/**********************************************************************
 *          wayland_surface_attach_shm
 *
 * Attaches a SHM buffer to a wayland surface.
 *
 * The buffer is marked as unavailable until committed and subsequently
 * released by the compositor.
 */
void wayland_surface_attach_shm(struct wayland_surface *surface,
                                struct wayland_shm_buffer *shm_buffer,
                                HRGN surface_damage_region)
{
    RGNDATA *surface_damage;
    int win_width, win_height;

    TRACE("surface=%p shm_buffer=%p (%dx%d)\n",
          surface, shm_buffer, shm_buffer->width, shm_buffer->height);

    shm_buffer->busy = TRUE;
    wayland_shm_buffer_ref(shm_buffer);

    wl_surface_attach(surface->wl_surface, shm_buffer->wl_buffer, 0, 0);

    /* Add surface damage, i.e., which parts of the surface have changed since
     * the last surface commit. Note that this is different from the buffer
     * damage region. */
    surface_damage = get_region_data(surface_damage_region);
    if (surface_damage)
    {
        RECT *rgn_rect = (RECT *)surface_damage->Buffer;
        RECT *rgn_rect_end = rgn_rect + surface_damage->rdh.nCount;

        for (;rgn_rect < rgn_rect_end; rgn_rect++)
        {
            wl_surface_damage_buffer(surface->wl_surface,
                                     rgn_rect->left, rgn_rect->top,
                                     rgn_rect->right - rgn_rect->left,
                                     rgn_rect->bottom - rgn_rect->top);
        }
        free(surface_damage);
    }

    win_width = surface->window.rect.right - surface->window.rect.left;
    win_height = surface->window.rect.bottom - surface->window.rect.top;

    /* It is an error to specify a wp_viewporter source rectangle that
     * is partially or completely outside of the wl_buffe.
     * 0 is also an invalid width / height value so use 1x1 instead.
     */
    win_width = max(1, min(win_width, shm_buffer->width));
    win_height = max(1, min(win_height, shm_buffer->height));

    wp_viewport_set_source(surface->wp_viewport, 0, 0,
                           wl_fixed_from_int(win_width),
                           wl_fixed_from_int(win_height));

    surface->content_width = win_width;
    surface->content_height = win_height;
}

/**********************************************************************
 *          wayland_surface_config_is_compatible
 *
 * Checks whether a wayland_surface_config object is compatible with the
 * the provided arguments.
 */
BOOL wayland_surface_config_is_compatible(struct wayland_surface_config *conf,
                                          int width, int height,
                                          enum wayland_surface_config_state state)
{
    static enum wayland_surface_config_state mask =
        WAYLAND_SURFACE_CONFIG_STATE_MAXIMIZED;

    /* The fullscreen state requires a size smaller or equal to the configured
     * size. If we have a larger size, we can use surface geometry during
     * surface reconfiguration to provide the smaller size, so we are always
     * compatible with a fullscreen state.
     * NOTE: Fullscreen combined with maximized is the same as fullscreen. */
    if (conf->state & WAYLAND_SURFACE_CONFIG_STATE_FULLSCREEN)
        return TRUE;

    /* We require the same state. */
    if ((state & mask) != (conf->state & mask)) return FALSE;

    /* The maximized state requires the configured size. During surface
     * reconfiguration we can use surface geometry to provide smaller areas
     * from larger sizes, so only smaller sizes are incompatible. */
    if ((conf->state & WAYLAND_SURFACE_CONFIG_STATE_MAXIMIZED) &&
        (width < conf->width || height < conf->height))
    {
        return FALSE;
    }

    return TRUE;
}

/**********************************************************************
 *          wayland_surface_get_rect_in_monitor
 *
 * Gets the largest rectangle within a surface's window (in window coordinates)
 * that is visible in a monitor.
 */
static void wayland_surface_get_rect_in_monitor(struct wayland_surface *surface,
                                                RECT *rect)
{
    HMONITOR hmonitor;
    MONITORINFO mi;

    mi.cbSize = sizeof(mi);
    if (!(hmonitor = NtUserMonitorFromRect(&surface->window.rect, 0)) ||
        !NtUserGetMonitorInfo(hmonitor, (MONITORINFO *)&mi))
    {
        SetRectEmpty(rect);
        return;
    }

    intersect_rect(rect, &mi.rcMonitor, &surface->window.rect);
    OffsetRect(rect, -surface->window.rect.left, -surface->window.rect.top);
}

/**********************************************************************
 *          wayland_surface_reconfigure_geometry
 *
 * Sets the xdg_surface geometry
 */
static void wayland_surface_reconfigure_geometry(struct wayland_surface *surface,
                                                 int width, int height)
{
    RECT rect;

    /* If the window size is bigger than the current state accepts, use the
     * largest visible (from Windows' perspective) subregion of the window. */
    if ((surface->current.state & (WAYLAND_SURFACE_CONFIG_STATE_MAXIMIZED |
                                   WAYLAND_SURFACE_CONFIG_STATE_FULLSCREEN)) &&
        (width > surface->current.width || height > surface->current.height))
    {
        wayland_surface_get_rect_in_monitor(surface, &rect);

        wayland_surface_coords_from_window(surface, rect.left, rect.top,
                                           (int *)&rect.left, (int *)&rect.top);
        wayland_surface_coords_from_window(surface, rect.right, rect.bottom,
                                           (int *)&rect.right, (int *)&rect.bottom);

        /* If the window rect in the monitor is smaller than required,
         * fall back to an appropriately sized rect at the top-left. */
        if ((surface->current.state & WAYLAND_SURFACE_CONFIG_STATE_MAXIMIZED) &&
            !(surface->current.state & WAYLAND_SURFACE_CONFIG_STATE_FULLSCREEN) &&
            (rect.right - rect.left < surface->current.width ||
             rect.bottom - rect.top < surface->current.height))
        {
            SetRect(&rect, 0, 0, surface->current.width, surface->current.height);
        }
        else
        {
            rect.right = min(rect.right, rect.left + surface->current.width);
            rect.bottom = min(rect.bottom, rect.top + surface->current.height);
        }
        TRACE("Window is too large for Wayland state, using subregion\n");
    }
    else
    {
        SetRect(&rect, 0, 0, width, height);
    }

    TRACE("hwnd=%p geometry=%s\n", surface->hwnd, wine_dbgstr_rect(&rect));

    if (!IsRectEmpty(&rect) && !EqualRect(&surface->geometry, &rect))
    {
        int width = rect.right - rect.left, height = rect.bottom - rect.top;
        xdg_surface_set_window_geometry(surface->xdg_surface,
                                        rect.left, rect.top,
                                        width, height);
        surface->geometry = rect;
        if (surface->window.resizeable)
        {
            xdg_toplevel_set_min_size(surface->xdg_toplevel, 0, 0);
            xdg_toplevel_set_max_size(surface->xdg_toplevel, 0, 0);
        }
        else
        {
            xdg_toplevel_set_min_size(surface->xdg_toplevel, width, height);
            xdg_toplevel_set_max_size(surface->xdg_toplevel, width, height);
        }
    }
}

/**********************************************************************
 *          wayland_surface_reconfigure_size
 *
 * Sets the surface size with viewporter
 */
static void wayland_surface_reconfigure_size(struct wayland_surface *surface,
                                             int width, int height)
{
    TRACE("hwnd=%p size=%dx%d\n", surface->hwnd, width, height);

    if (width > 0 && height > 0)
        wp_viewport_set_destination(surface->wp_viewport, width, height);
    else
        wp_viewport_set_destination(surface->wp_viewport, -1, -1);
}

/**********************************************************************
 *          wayland_surface_reconfigure_client
 *
 * Reconfigures the subsurface covering the client area.
 */
static void wayland_surface_reconfigure_client(struct wayland_surface *surface,
                                               struct wayland_client_surface *client,
                                               const RECT *client_rect)
{
    struct wayland_window_config *window = &surface->window;
    int client_x, client_y, x, y;
    int client_width, client_height, width, height;

    /* The offset of the client area origin relatively to the window origin. */
    client_x = client_rect->left + window->client_rect.left - window->rect.left;
    client_y = client_rect->top + window->client_rect.top - window->rect.top;

    client_width = client_rect->right - client_rect->left;
    client_height = client_rect->bottom - client_rect->top;

    wayland_surface_coords_from_window(surface, client_x, client_y, &x, &y);
    wayland_surface_coords_from_window(surface, client_width, client_height,
                                       &width, &height);

    TRACE("hwnd=%p subsurface=%d,%d+%dx%d\n", surface->hwnd, x, y, width, height);

    if (client->wl_subsurface)
    {
        wl_subsurface_set_position(client->wl_subsurface, x, y);
        wl_subsurface_place_above(client->wl_subsurface, surface->wl_surface);
    }

    if (width > 0 && height > 0)
        wp_viewport_set_destination(client->wp_viewport, width, height);
    else /* We can't have a 0x0 destination, use 1x1 instead. */
        wp_viewport_set_destination(client->wp_viewport, 1, 1);
}

static struct wayland_hwnd_dmabuf_surface *wayland_hwnd_dmabuf_surface_get(struct wayland_surface *parent,
                                                                           HWND hwnd)
{
    struct wayland_hwnd_dmabuf_surface *surface;

    wl_list_for_each(surface, &parent->hwnd_dmabuf_surfaces, link)
        if (surface->hwnd == hwnd) return surface;
    return NULL;
}

static struct wayland_hwnd_dmabuf_surface *wayland_hwnd_dmabuf_surface_create(struct wayland_surface *parent,
                                                                              HWND hwnd)
{
    struct wayland_hwnd_dmabuf_surface *surface;
    struct wl_region *empty_region;

    if (!(surface = calloc(1, sizeof(*surface)))) return NULL;
    surface->hwnd = hwnd;
    surface->channel_fd = -1;
    wl_list_init(&surface->buffers);

    if (!(surface->wl_surface = wl_compositor_create_surface(process_wayland.wl_compositor))) goto err;
    wl_surface_set_user_data(surface->wl_surface, hwnd);

    empty_region = wl_compositor_create_region(process_wayland.wl_compositor);
    if (!empty_region) goto err;
    wl_surface_set_input_region(surface->wl_surface, empty_region);
    wl_region_destroy(empty_region);

    if (!(surface->wp_viewport = wp_viewporter_get_viewport(process_wayland.wp_viewporter,
                                                            surface->wl_surface))) goto err;

    if (!(surface->wl_subsurface = wl_subcompositor_get_subsurface(process_wayland.wl_subcompositor,
                                                                   surface->wl_surface,
                                                                   parent->wl_surface))) goto err;
    wl_subsurface_set_desync(surface->wl_subsurface);

    wl_list_insert(parent->hwnd_dmabuf_surfaces.prev, &surface->link);
    return surface;

err:
    if (surface->wl_subsurface) wl_subsurface_destroy(surface->wl_subsurface);
    if (surface->wp_viewport) wp_viewport_destroy(surface->wp_viewport);
    if (surface->wl_surface) wl_surface_destroy(surface->wl_surface);
    free(surface);
    return NULL;
}

static void wayland_hwnd_dmabuf_surface_set_opaque(struct wayland_hwnd_dmabuf_surface *surface,
                                                   int width, int height)
{
    struct wl_region *region = NULL;

    if (surface->current && surface->current->alpha_mode == DXGI_ALPHA_MODE_IGNORE &&
        (region = wl_compositor_create_region(process_wayland.wl_compositor)))
    {
        wl_region_add(region, 0, 0, width, height);
        wl_surface_set_opaque_region(surface->wl_surface, region);
        wl_region_destroy(region);
    }
    else wl_surface_set_opaque_region(surface->wl_surface, NULL);
}

static BOOL wayland_hwnd_dmabuf_surface_configure(struct wayland_surface *parent,
                                                  struct wayland_hwnd_dmabuf_surface *surface,
                                                  const hwnd_dmabuf_frame_info_t *info,
                                                  struct wl_surface *above)
{
    RECT rect = wine_server_get_rect(info->client), clipped, client;
    double source_x, source_y, source_width, source_height;
    int rect_width, rect_height;
    int x, y, width, height;

    rect_width = rect.right - rect.left;
    rect_height = rect.bottom - rect.top;
    if (rect_width <= 0 || rect_height <= 0) return FALSE;

    client.left = client.top = 0;
    client.right = parent->window.client_rect.right - parent->window.client_rect.left;
    client.bottom = parent->window.client_rect.bottom - parent->window.client_rect.top;

    clipped.left = max(rect.left, client.left);
    clipped.top = max(rect.top, client.top);
    clipped.right = min(rect.right, client.right);
    clipped.bottom = min(rect.bottom, client.bottom);
    if (IsRectEmpty(&clipped)) return FALSE;

    source_x = (double)(clipped.left - rect.left) * surface->current->width / rect_width;
    source_y = (double)(clipped.top - rect.top) * surface->current->height / rect_height;
    source_width = (double)(clipped.right - clipped.left) * surface->current->width / rect_width;
    source_height = (double)(clipped.bottom - clipped.top) * surface->current->height / rect_height;

    rect = clipped;
    OffsetRect(&rect, parent->window.client_rect.left - parent->window.rect.left,
               parent->window.client_rect.top - parent->window.rect.top);

    wayland_surface_coords_from_window(parent, rect.left, rect.top, &x, &y);
    wayland_surface_coords_from_window(parent, rect.right - rect.left,
                                       rect.bottom - rect.top, &width, &height);
    width = max(1, width);
    height = max(1, height);

    wl_subsurface_set_position(surface->wl_subsurface, x, y);
    wl_subsurface_place_above(surface->wl_subsurface, above);
    wp_viewport_set_source(surface->wp_viewport,
                           wl_fixed_from_double(source_x), wl_fixed_from_double(source_y),
                           wl_fixed_from_double(source_width), wl_fixed_from_double(source_height));
    wp_viewport_set_destination(surface->wp_viewport, width, height);
    wayland_hwnd_dmabuf_surface_set_opaque(surface, width, height);
    return TRUE;
}

/* Claim this child's consumer channel end once; the producer mints it lazily, so retry. */
static void wayland_hwnd_dmabuf_surface_claim_channel(struct wayland_hwnd_dmabuf_surface *surface)
{
    HANDLE handle = 0;
    int fd = -1;

    if (surface->channel_fd >= 0) return;
    if (wine_hwnd_dmabuf_claim_channel(surface->hwnd, &handle) != HWND_DMABUF_OK || !handle)
        return;
    if (wine_server_handle_to_fd(handle, FILE_READ_DATA | FILE_WRITE_DATA, &fd, NULL))
        fd = -1;
    NtClose(handle);
    surface->channel_fd = fd;
    if (fd >= 0) TRACE("hwnd=%p claimed dmabuf socket channel fd %d\n", surface->hwnd, fd);
}

/* Drain the channel to the newest frame; returns its fd and fills desc, or -1 if none. */
/* Receive one frame. Returns 1 with *out_fd set (a dmabuf fd >= 0, or -1 for an fd-less
 * slot-reference frame) when a frame was read, 0 when the channel is empty, -1 on EOF.
 * The caller drains in a loop so it can import every fd-bearing slot, not just the newest. */
static int wayland_hwnd_dmabuf_channel_recv_one(int channel_fd, hwnd_dmabuf_frame_desc_t *desc,
                                                int *out_fd)
{
    char control[CMSG_SPACE(sizeof(int))];
    struct msghdr msg = {0};
    struct cmsghdr *cmsg;
    struct iovec iov;
    int fd = -1;
    ssize_t n;

    iov.iov_base = desc;
    iov.iov_len = sizeof(*desc);
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = control;
    msg.msg_controllen = sizeof(control);

    n = recvmsg(channel_fd, &msg, MSG_DONTWAIT | MSG_CMSG_CLOEXEC);
    if (n == 0) return -1;                       /* peer closed the channel */
    if (n != (ssize_t)sizeof(*desc)) return 0;   /* EAGAIN or short read: nothing usable now */

    for (cmsg = CMSG_FIRSTHDR(&msg); cmsg; cmsg = CMSG_NXTHDR(&msg, cmsg))
        if (cmsg->cmsg_level == SOL_SOCKET && cmsg->cmsg_type == SCM_RIGHTS)
            memcpy(&fd, CMSG_DATA(cmsg), sizeof(fd));
    *out_fd = fd;
    return 1;
}

/* Build a wl_buffer wrapping the dmabuf fd plus its tracking buffer, cached on the
 * surface. Does not close fd (caller owns it). Returns NULL on failure. */
static struct wayland_hwnd_dmabuf_buffer *wayland_hwnd_dmabuf_create_buffer(
        struct wayland_hwnd_dmabuf_surface *surface, const hwnd_dmabuf_frame_desc_t *desc,
        int fd, BOOL stable_slot)
{
    struct wayland_hwnd_dmabuf_buffer *buffer;
    struct zwp_linux_buffer_params_v1 *params;

    if (!(params = zwp_linux_dmabuf_v1_create_params(process_wayland.zwp_linux_dmabuf_v1)))
        return NULL;
    zwp_linux_buffer_params_v1_add(params, fd, 0, desc->offset, desc->stride,
                                   desc->modifier >> 32, desc->modifier & 0xffffffff);
    if (!(buffer = calloc(1, sizeof(*buffer))))
    {
        zwp_linux_buffer_params_v1_destroy(params);
        return NULL;
    }
    buffer->channel_fd = -1;
    buffer->wl_buffer = zwp_linux_buffer_params_v1_create_immed(params, desc->width, desc->height,
                                                                desc->fourcc, 0);
    zwp_linux_buffer_params_v1_destroy(params);
    if (!buffer->wl_buffer)
    {
        free(buffer);
        return NULL;
    }
    buffer->ref = 1;  /* owner ref; a compositor ref is added per attach in the update loop */
    buffer->surface = surface;
    buffer->producer_unique_id = desc->producer_unique_id;
    buffer->image_id = desc->image_id;
    buffer->ring_generation = desc->ring_generation;
    buffer->fourcc = desc->fourcc;
    buffer->stride = desc->stride;
    buffer->offset = desc->offset;
    buffer->modifier = desc->modifier;
    buffer->width = desc->width;
    buffer->height = desc->height;
    buffer->stable_slot = stable_slot;
    buffer->cache_valid = stable_slot ? TRUE : FALSE;
    buffer->release_flags = HWND_DMABUF_RELEASE_ORPHANED;
    buffer->channel_fd = surface->channel_fd >= 0 ? dup(surface->channel_fd) : -1;
    wl_list_insert(surface->buffers.prev, &buffer->link);
    wl_buffer_add_listener(buffer->wl_buffer, &wayland_hwnd_dmabuf_buffer_listener, buffer);
    return buffer;
}

/* Make buffer the surface's current frame and publish its (new) contents. */
static void wayland_hwnd_dmabuf_attach_frame(struct wayland_hwnd_dmabuf_surface *surface,
        struct wayland_hwnd_dmabuf_buffer *buffer, const hwnd_dmabuf_frame_desc_t *desc)
{
    if (wayland_hwnd_dmabuf_buffer_exchange_release_token(buffer, desc->release_token))
        WARN("hwnd=%p slot=%u overwrote an unreleased token\n", surface->hwnd, desc->image_id);
    buffer->alpha_mode = desc->alpha_mode;
    buffer->release_flags = HWND_DMABUF_RELEASE_PRESENTED;
    surface->current = buffer;
    surface->frame_seq = desc->frame_seq;
    wl_surface_attach(surface->wl_surface, buffer->wl_buffer, 0, 0);
    wl_surface_damage_buffer(surface->wl_surface, 0, 0, desc->width, desc->height);
    wp_viewport_set_source(surface->wp_viewport, 0, 0,
                           wl_fixed_from_int(desc->width), wl_fixed_from_int(desc->height));
}

static void wayland_hwnd_dmabuf_drop_current_frame(struct wayland_hwnd_dmabuf_surface *surface,
                                                   unsigned int flags)
{
    struct wayland_hwnd_dmabuf_buffer *buffer = surface->current;

    if (!buffer) return;
    wayland_hwnd_dmabuf_buffer_send_release(buffer, flags);
    surface->current = NULL;
    if (!buffer->stable_slot) wayland_hwnd_dmabuf_buffer_reap(buffer);
}

/* Stable-slot path (HWND_DMABUF_FLAG_STABLE_SLOT): ensure this slot is imported and cached
 * (no attach). The producer sends a slot's dmabuf fd only once, then fd-less references, so:
 *  - cache hit (exact layout match): reuse the cached wl_buffer.
 *  - fd-bearing miss: import and cache it.
 *  - fd-less miss: the slot is not cached -> return NULL; the failed release clears the
 *    producer's cache state so it resends the fd.
 * Stale slots (new producer/ring generation, or a changed layout) are reaped first. */
static struct wayland_hwnd_dmabuf_buffer *wayland_hwnd_dmabuf_cache_slot(
        struct wayland_hwnd_dmabuf_surface *surface, const hwnd_dmabuf_frame_desc_t *desc,
        int fd, BOOL *created_buffer)
{
    struct wayland_hwnd_dmabuf_buffer *buffer = NULL, *it, *it_next;

    wl_list_for_each_safe(it, it_next, &surface->buffers, link)
        if (it->producer_unique_id != desc->producer_unique_id ||
            it->ring_generation != desc->ring_generation)
        {
            TRACE("hwnd=%p cache-drop stale producer/gen slot=%u gen %u->%u\n",
                  surface->hwnd, it->image_id, it->ring_generation, desc->ring_generation);
            wayland_hwnd_dmabuf_buffer_reap(it);
        }

    wl_list_for_each_safe(it, it_next, &surface->buffers, link)
    {
        if (it->image_id != desc->image_id) continue;
        if (it->width == (int)desc->width && it->height == (int)desc->height &&
            it->fourcc == desc->fourcc && it->modifier == desc->modifier &&
            it->stride == desc->stride && it->offset == desc->offset)
            buffer = it;
        else
        {
            TRACE("hwnd=%p cache-drop layout-mismatch slot=%u\n", surface->hwnd, it->image_id);
            wayland_hwnd_dmabuf_buffer_reap(it);
        }
        break;
    }

    if (buffer)
    {
        TRACE("hwnd=%p cache-hit slot=%u gen=%u frame_seq=%u\n",
              surface->hwnd, desc->image_id, desc->ring_generation, desc->frame_seq);
        return buffer;
    }
    if (fd < 0)
    {
        TRACE("hwnd=%p cache-miss-no-fd slot=%u (awaiting fd resend)\n",
              surface->hwnd, desc->image_id);
        return NULL;
    }
    if (!(buffer = wayland_hwnd_dmabuf_create_buffer(surface, desc, fd, TRUE))) return NULL;
    TRACE("hwnd=%p cache-miss import slot=%u gen=%u frame_seq=%u\n",
          surface->hwnd, desc->image_id, desc->ring_generation, desc->frame_seq);
    *created_buffer = TRUE;
    return buffer;
}

/* A queued frame superseded by a newer one. For a stable slot, still import/cache it so a
 * later reference resolves; then release the present so the producer can recycle the slot.
 * Uncached frames are simply dropped (each is a fresh dmabuf, nothing to keep). */
static void wayland_hwnd_dmabuf_retire_frame(struct wayland_hwnd_dmabuf_surface *surface,
        const hwnd_dmabuf_frame_desc_t *desc, int fd, BOOL *created_buffer)
{
    struct wayland_hwnd_dmabuf_buffer *buffer = NULL;
    unsigned int flags = HWND_DMABUF_RELEASE_DROPPED;

    if (desc->flags & HWND_DMABUF_FLAG_STABLE_SLOT)
    {
        buffer = wayland_hwnd_dmabuf_cache_slot(surface, desc, fd, created_buffer);
        if (buffer) flags |= HWND_DMABUF_RELEASE_CACHED;
        else flags = HWND_DMABUF_RELEASE_FAILED;
    }
    if (fd >= 0) close(fd);
    wayland_hwnd_dmabuf_send_release(surface, desc->producer_unique_id, desc->release_token,
                                     flags, desc->image_id, desc->ring_generation);
}

/* Drain the channel, importing every fd-bearing slot so none is lost, then show the newest
 * frame. Stable-slot producers send a slot's fd once and fd-less references after, so the fd
 * is dup'd + passed via SCM_RIGHTS once per slot, not once per frame. created_buffer reports
 * a freshly imported wl_buffer; attached_frame reports a frame was published. */
static struct wayland_hwnd_dmabuf_buffer *wayland_hwnd_dmabuf_surface_import_buffer(
        struct wayland_hwnd_dmabuf_surface *surface, const hwnd_dmabuf_frame_info_t *info,
        BOOL *created_buffer, BOOL *attached_frame)
{
    struct wayland_hwnd_dmabuf_buffer *buffer = NULL;
    hwnd_dmabuf_frame_desc_t pdesc, desc;
    BOOL have_pending = FALSE;
    int pfd = -1, fd, r;

    *created_buffer = FALSE;
    *attached_frame = FALSE;
    memset(&pdesc, 0, sizeof(pdesc));

    wayland_hwnd_dmabuf_surface_claim_channel(surface);
    if (surface->channel_fd < 0) return surface->current;

    while ((r = wayland_hwnd_dmabuf_channel_recv_one(surface->channel_fd, &desc, &fd)) > 0)
    {
        if (desc.version != HWND_DMABUF_DESC_VERSION_V1 || !desc.width || !desc.height ||
            !desc.stride || !desc.fourcc || !desc.release_token ||
            !wayland_dmabuf_format_supported(desc.fourcc, desc.modifier))
        {
            WARN("hwnd=%p import rejected info_seq=%u desc_seq=%u fourcc=%#x\n",
                 surface->hwnd, info->frame_seq, desc.frame_seq, desc.fourcc);
            if (fd >= 0) close(fd);
            if (desc.release_token)
                wayland_hwnd_dmabuf_send_release(surface, desc.producer_unique_id, desc.release_token,
                                                 HWND_DMABUF_RELEASE_FAILED,
                                                 desc.image_id, desc.ring_generation);
            continue;
        }
        if (have_pending)
            wayland_hwnd_dmabuf_retire_frame(surface, &pdesc, pfd, created_buffer);
        pdesc = desc;
        pfd = fd;
        have_pending = TRUE;
    }

    if (!have_pending) return surface->current;

    /* Show the newest drained frame. */
    if (pdesc.flags & HWND_DMABUF_FLAG_STABLE_SLOT)
        buffer = wayland_hwnd_dmabuf_cache_slot(surface, &pdesc, pfd, created_buffer);
    else if ((buffer = wayland_hwnd_dmabuf_create_buffer(surface, &pdesc, pfd, FALSE)))
        *created_buffer = TRUE;

    if (pfd >= 0) close(pfd);

    if (!buffer)
    {
        wayland_hwnd_dmabuf_send_release(surface, pdesc.producer_unique_id, pdesc.release_token,
                                         HWND_DMABUF_RELEASE_FAILED,
                                         pdesc.image_id, pdesc.ring_generation);
        return surface->current;
    }
    wayland_hwnd_dmabuf_attach_frame(surface, buffer, &pdesc);
    *attached_frame = TRUE;
    return buffer;
}

/* Frame callback (event thread): just post a message; the present thread owns the throttle. */
static void wayland_dmabuf_frame_done(void *data, struct wl_callback *cb, uint32_t time)
{
    NtUserPostMessage((HWND)data, WM_WAYLAND_DMABUF_VSYNC, 0, 0);
}

static const struct wl_callback_listener wayland_dmabuf_frame_listener =
{
    wayland_dmabuf_frame_done
};

static void wayland_surface_update_hwnd_dmabufs(struct wayland_surface *surface)
{
    hwnd_dmabuf_frame_info_t stack_frames[16], *frames = stack_frames;
    unsigned int total = 0, count = 0, i;
    struct wayland_hwnd_dmabuf_buffer *buffer, *buffer_next;
    struct wayland_hwnd_dmabuf_surface *dmabuf_surface, *next;
    enum hwnd_dmabuf_status status;
    struct wayland_win_data *data;
    struct wl_surface *above;
    unsigned long long now = wayland_dmabuf_now_ms();
    BOOL any_new = FALSE;

    if (!process_wayland.zwp_linux_dmabuf_v1) return;
    /* dmabuf children are composited for primary surfaces: toplevels and popups
     * (menus). The popup runs this with its own hwnd, importing its own
     * cross-process dmabuf content; the frozen import/release logic is unchanged. */
    if (!wayland_surface_is_toplevel(surface) && !wayland_surface_is_popup(surface)) return;

    /* Throttle to vsync: a pending frame callback means we already committed this frame.
     * Its WM_WAYLAND_DMABUF_VSYNC clears it and runs us again, pacing the producers. */
    if (surface->dmabuf_frame_cb) return;

    wl_list_for_each(dmabuf_surface, &surface->hwnd_dmabuf_surfaces, link)
        dmabuf_surface->seen = FALSE;

    status = wine_hwnd_dmabuf_list(surface->hwnd, frames, ARRAY_SIZE(stack_frames), &total, &count);
    if (status != HWND_DMABUF_OK)
    {
        TRACE("hwnd=%p wine_hwnd_dmabuf_list status=%u\n", surface->hwnd, status);
        return;
    }
    if (total > count)
    {
        if (!(frames = calloc(total, sizeof(*frames)))) return;
        status = wine_hwnd_dmabuf_list(surface->hwnd, frames, total, &total, &count);
        if (status != HWND_DMABUF_OK) count = 0;
    }

    TRACE("hwnd=%p listed total=%u count=%u\n", surface->hwnd, total, count);

    /* Anchor the dmabuf chain above the client subsurface, not the main surface.
     * Both used to place_above() main, so whichever committed last won the top,
     * a per-frame z-order race (the flicker). A fixed sibling anchor makes
     * [main, client, dmabuf...] resolve the same regardless. (win_data_mutex held.) */
    above = surface->wl_surface;
    if ((data = wayland_win_data_get_nolock(surface->hwnd)) &&
        data->client_surface && data->client_surface->wl_subsurface &&
        data->client_surface->toplevel == surface->hwnd)
        above = data->client_surface->wl_surface;
    /* Server child lists are returned in z-order from top to bottom, while
     * Wayland subsurface stacking needs to be applied from bottom to top when
     * chaining place_above() calls. */
    for (i = count; i-- > 0; )
    {
        HWND hwnd = (HWND)(UINT_PTR)frames[i].hwnd;
        BOOL created_buffer, attached_frame;

        TRACE("hwnd=%p child=%p frame_seq=%u opened=%u\n",
              surface->hwnd, hwnd, frames[i].frame_seq, frames[i].opened);

        if (!(dmabuf_surface = wayland_hwnd_dmabuf_surface_get(surface, hwnd)) &&
            !(dmabuf_surface = wayland_hwnd_dmabuf_surface_create(surface, hwnd)))
            continue;

        dmabuf_surface->seen = TRUE;
        dmabuf_surface->last_seen_ms = now;
        /* Reap uncached buffers the compositor has released; cached stable-slot
         * buffers set no released flag and stay cached for reuse. */
        wl_list_for_each_safe(buffer, buffer_next, &dmabuf_surface->buffers, link)
            if (buffer->released)
                wayland_hwnd_dmabuf_buffer_reap(buffer);

        wayland_hwnd_dmabuf_surface_import_buffer(dmabuf_surface, &frames[i],
                                                  &created_buffer, &attached_frame);
        if (attached_frame) any_new = TRUE;
        if (!dmabuf_surface->current) continue;
        if (!wayland_hwnd_dmabuf_surface_configure(surface, dmabuf_surface, &frames[i], above))
        {
            TRACE("hwnd=%p child=%p configure failed frame_seq=%u\n",
                  surface->hwnd, hwnd, frames[i].frame_seq);
            wayland_hwnd_dmabuf_drop_current_frame(dmabuf_surface, HWND_DMABUF_RELEASE_DROPPED);
            if (dmabuf_surface->wl_surface)
            {
                wl_surface_attach(dmabuf_surface->wl_surface, NULL, 0, 0);
                wl_surface_commit(dmabuf_surface->wl_surface);
            }
            continue;
        }

        wl_surface_commit(dmabuf_surface->wl_surface);
        /* One compositor ref per attached frame; the wl_buffer.release handler drops
         * it. Cached slots re-attach the same wl_buffer each new frame, so the ref is
         * per-attach; the busy gate keeps at most one attach of a slot outstanding. */
        if (attached_frame)
            InterlockedIncrement(&dmabuf_surface->current->ref);
        above = dmabuf_surface->wl_surface;
    }

    /* A child absent from the descendant list: hide its now-stale overlay at once so the
     * content showing through (e.g. the client surface) is not occluded -- but keep the
     * dmabuf cache for a grace window so a brief gap does not force a re-import when the
     * child returns. Reap the cache only once the grace expires. */
    wl_list_for_each_safe(dmabuf_surface, next, &surface->hwnd_dmabuf_surfaces, link)
    {
        if (dmabuf_surface->seen) continue;
        if (now - dmabuf_surface->last_seen_ms > WAYLAND_DMABUF_SURFACE_GRACE_MS)
        {
            wayland_hwnd_dmabuf_surface_destroy(dmabuf_surface);
            any_new = TRUE;
        }
        else if (dmabuf_surface->current)
        {
            wl_surface_attach(dmabuf_surface->wl_surface, NULL, 0, 0);
            wl_surface_commit(dmabuf_surface->wl_surface);
            dmabuf_surface->current = NULL;
            any_new = TRUE;
        }
    }

    /* Commit and arm the next vsync only when something changed, so an idle window costs nothing. */
    if (any_new)
    {
        surface->dmabuf_frame_cb = wl_surface_frame(surface->wl_surface);
        wl_callback_add_listener(surface->dmabuf_frame_cb, &wayland_dmabuf_frame_listener,
                                 surface->hwnd);
        wl_surface_commit(surface->wl_surface);
    }
    if (frames != stack_frames) free(frames);
}

/**********************************************************************
 *          wayland_surface_reconfigure_xdg
 *
 * Reconfigures the xdg surface as needed to match the latest requested
 * state.
 */
static BOOL wayland_surface_reconfigure_xdg(struct wayland_surface *surface,
                                            int width, int height)
{
    struct wayland_window_config *window = &surface->window;

    /* Acknowledge any compatible processed config. */
    if (surface->processing.serial && surface->processing.processed &&
        wayland_surface_config_is_compatible(&surface->processing,
                                             width, height,
                                             window->state))
    {
        surface->current = surface->processing;
        memset(&surface->processing, 0, sizeof(surface->processing));
        xdg_surface_ack_configure(surface->xdg_surface, surface->current.serial);
    }
    /* If this is the initial configure, and we have a compatible requested
     * config, use that, in order to draw windows that don't go through the
     * message loop (e.g., some splash screens). */
    else if (!surface->current.serial && surface->requested.serial &&
             wayland_surface_config_is_compatible(&surface->requested,
                                                  width, height,
                                                  window->state))
    {
        surface->current = surface->requested;
        memset(&surface->requested, 0, sizeof(surface->requested));
        xdg_surface_ack_configure(surface->xdg_surface, surface->current.serial);
    }
    else if (!surface->current.serial ||
             !wayland_surface_config_is_compatible(&surface->current,
                                                   width, height,
                                                   window->state))
    {
        return FALSE;
    }

    wayland_surface_reconfigure_geometry(surface, width, height);

    return TRUE;
}

/**********************************************************************
 *          wayland_surface_reconfigure_subsurface
 *
 * Reconfigures the subsurface as needed to match the latest requested
 * state.
 */
static void wayland_surface_reconfigure_subsurface(struct wayland_surface *surface)
{
    struct wayland_win_data *toplevel_data;
    struct wayland_surface *toplevel_surface;
    int local_x, local_y, x, y;

    if (surface->processing.serial && surface->processing.processed &&
        (toplevel_data = wayland_win_data_get_nolock(surface->toplevel_hwnd)) &&
        (toplevel_surface = toplevel_data->wayland_surface))
    {
        local_x = surface->window.rect.left - toplevel_surface->window.rect.left;
        local_y = surface->window.rect.top - toplevel_surface->window.rect.top;

        wayland_surface_coords_from_window(surface, local_x, local_y, &x, &y);

        TRACE("hwnd=%p pos=%d,%d\n", surface->hwnd, x, y);

        wl_subsurface_set_position(surface->wl_subsurface, x, y);
        if (toplevel_data->client_surface && toplevel_data->client_surface->wl_subsurface)
            wl_subsurface_place_above(surface->wl_subsurface, toplevel_data->client_surface->wl_surface);
        else
            wl_subsurface_place_above(surface->wl_subsurface, toplevel_surface->wl_surface);
        wl_surface_commit(toplevel_surface->wl_surface);

        memset(&surface->processing, 0, sizeof(surface->processing));
    }
}

/**********************************************************************
 *          wayland_surface_reconfigure
 *
 * Reconfigures the wayland surface as needed to match the latest requested
 * state.
 */
BOOL wayland_surface_reconfigure(struct wayland_surface *surface)
{
    struct wayland_window_config *window = &surface->window;
    int win_width, win_height, width, height;

    win_width = surface->window.rect.right - surface->window.rect.left;
    win_height = surface->window.rect.bottom - surface->window.rect.top;

    wayland_surface_coords_from_window(surface, win_width, win_height,
                                       &width, &height);

    TRACE("hwnd=%p window=%dx%d,%#x processing=%dx%d,%#x current=%dx%d,%#x\n",
          surface->hwnd, win_width, win_height, window->state,
          surface->processing.width, surface->processing.height,
          surface->processing.state, surface->current.width,
          surface->current.height, surface->current.state);

    switch (surface->role)
    {
    case WAYLAND_SURFACE_ROLE_NONE:
        break;
    case WAYLAND_SURFACE_ROLE_TOPLEVEL:
        if (!surface->xdg_surface) break; /* surface role has been cleared */
        if (!wayland_surface_reconfigure_xdg(surface, width, height)) return FALSE;
        break;
    case WAYLAND_SURFACE_ROLE_SUBSURFACE:
        if (!surface->wl_subsurface) break; /* surface role has been cleared */
        wayland_surface_reconfigure_subsurface(surface);
        break;
    case WAYLAND_SURFACE_ROLE_POPUP:
        if (!surface->xdg_popup) break; /* surface role has been cleared */
        /* Defer attaching a buffer until the popup's xdg_surface has been
         * configured and ack'd; attaching before that is a protocol error. */
        if (!surface->current.serial) return FALSE;
        break;
    }

    wayland_surface_reconfigure_size(surface, width, height);

    return TRUE;
}

/**********************************************************************
 *          wayland_shm_buffer_ref
 *
 * Increases the reference count of a SHM buffer.
 */
void wayland_shm_buffer_ref(struct wayland_shm_buffer *shm_buffer)
{
    InterlockedIncrement(&shm_buffer->ref);
}

/**********************************************************************
 *          wayland_shm_buffer_unref
 *
 * Decreases the reference count of a SHM buffer (and may destroy it).
 */
void wayland_shm_buffer_unref(struct wayland_shm_buffer *shm_buffer)
{
    if (InterlockedDecrement(&shm_buffer->ref) > 0) return;

    TRACE("destroying %p map=%p\n", shm_buffer, shm_buffer->map_data);

    if (shm_buffer->wl_buffer)
        wl_buffer_destroy(shm_buffer->wl_buffer);
    if (shm_buffer->map_data)
        NtUnmapViewOfSection(GetCurrentProcess(), shm_buffer->map_data);
    if (shm_buffer->damage_region)
        NtGdiDeleteObjectApp(shm_buffer->damage_region);

    free(shm_buffer);
}

/**********************************************************************
 *          wayland_shm_buffer_create
 *
 * Creates a SHM buffer with the specified width, height and format.
 */
struct wayland_shm_buffer *wayland_shm_buffer_create(int width, int height,
                                                     enum wl_shm_format format)
{
    struct wayland_shm_buffer *shm_buffer = NULL;
    HANDLE handle = 0;
    int fd = -1;
    SIZE_T view_size = 0;
    LARGE_INTEGER section_size;
    NTSTATUS status;
    struct wl_shm_pool *pool;
    int stride, size;

    stride = width * WINEWAYLAND_BYTES_PER_PIXEL;
    size = stride * height;
    if (size == 0)
    {
        ERR("Invalid shm_buffer size %dx%d\n", width, height);
        goto err;
    }

    shm_buffer = calloc(1, sizeof(*shm_buffer));
    if (!shm_buffer)
    {
        ERR("Failed to allocate space for SHM buffer\n");
        goto err;
    }

    TRACE("%p %dx%d format=%d size=%d\n", shm_buffer, width, height, format, size);

    shm_buffer->ref = 1;
    shm_buffer->width = width;
    shm_buffer->height = height;
    shm_buffer->format = format;
    shm_buffer->map_size = size;

    shm_buffer->damage_region = NtGdiCreateRectRgn(0, 0, width, height);
    if (!shm_buffer->damage_region)
    {
        ERR("Failed to create buffer damage region\n");
        goto err;
    }

    section_size.QuadPart = size;
    status = NtCreateSection(&handle,
                             GENERIC_READ | SECTION_MAP_READ | SECTION_MAP_WRITE,
                             NULL, &section_size, PAGE_READWRITE, SEC_COMMIT, 0);
    if (status)
    {
        ERR("Failed to create SHM section status=0x%x\n", status);
        goto err;
    }

    status = NtMapViewOfSection(handle, GetCurrentProcess(),
                                (PVOID)&shm_buffer->map_data, 0, 0, NULL,
                                &view_size, ViewUnmap, 0, PAGE_READWRITE);
    if (status)
    {
        shm_buffer->map_data = NULL;
        ERR("Failed to create map SHM handle status=0x%x\n", status);
        goto err;
    }

    status = wine_server_handle_to_fd(handle, FILE_READ_DATA, &fd, NULL);
    if (status)
    {
        ERR("Failed to get fd from SHM handle status=0x%x\n", status);
        goto err;
    }

    pool = wl_shm_create_pool(process_wayland.wl_shm, fd, size);
    if (!pool)
    {
        ERR("Failed to create SHM pool fd=%d size=%d\n", fd, size);
        goto err;
    }
    shm_buffer->wl_buffer = wl_shm_pool_create_buffer(pool, 0, width, height,
                                                      stride, format);
    wl_shm_pool_destroy(pool);
    if (!shm_buffer->wl_buffer)
    {
        ERR("Failed to create SHM buffer %dx%d\n", width, height);
        goto err;
    }

    close(fd);
    NtClose(handle);

    TRACE("=> map=%p\n", shm_buffer->map_data);

    return shm_buffer;

err:
    if (fd >= 0) close(fd);
    if (handle) NtClose(handle);
    if (shm_buffer) wayland_shm_buffer_unref(shm_buffer);
    return NULL;
}

/***********************************************************************
 *           copy_rectangle_into_center_of_square
 *
 * Copies non-square rectangle src to the center of square dest.
 */
static void copy_rectangle_into_center_of_square(const unsigned int *src,
                                                 int src_w, int src_h,
                                                 unsigned int *dest)
{
    int dest_length;

    if (src_w > src_h)
    {
        dest += src_w * (src_w - src_h) / 2;
        dest_length = src_w;
    }
    else
    {
        dest += (src_h - src_w) / 2;
        dest_length = src_h;
    }

    for (int h = 0; h < src_h; h++, dest += dest_length, src += src_w)
        memcpy(dest, src, src_w * 4);
}

/***********************************************************************
 *           wayland_shm_buffer_from_color_bitmaps
 *
 * Create a wayland_shm_buffer for a color bitmap.
 *
 * Adapted from wineandroid.drv code.
 */
struct wayland_shm_buffer *wayland_shm_buffer_from_color_bitmaps(HDC hdc, HBITMAP color,
                                                                 HBITMAP mask,
                                                                 BOOL allow_padding)
{
    struct wayland_shm_buffer *shm_buffer = NULL;
    char buffer[FIELD_OFFSET(BITMAPINFO, bmiColors[256])];
    BITMAPINFO *info = (BITMAPINFO *)buffer;
    BITMAP bm;
    unsigned int *ptr, *bits = NULL;
    unsigned char *mask_bits = NULL;
    int i, j, square_length;
    BOOL has_alpha = FALSE, use_padding = FALSE;

    if (!NtGdiExtGetObjectW(color, sizeof(bm), &bm)) goto failed;

    info->bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info->bmiHeader.biWidth = bm.bmWidth;
    info->bmiHeader.biHeight = -bm.bmHeight;
    info->bmiHeader.biPlanes = 1;
    info->bmiHeader.biBitCount = 32;
    info->bmiHeader.biCompression = BI_RGB;
    info->bmiHeader.biSizeImage = bm.bmWidth * bm.bmHeight * 4;
    info->bmiHeader.biXPelsPerMeter = 0;
    info->bmiHeader.biYPelsPerMeter = 0;
    info->bmiHeader.biClrUsed = 0;
    info->bmiHeader.biClrImportant = 0;

    use_padding = allow_padding && bm.bmWidth != bm.bmHeight;

    if (use_padding)
    {
        square_length = max(bm.bmWidth, bm.bmHeight);
        shm_buffer = wayland_shm_buffer_create(square_length, square_length,
                                               WL_SHM_FORMAT_ARGB8888);
        if (!shm_buffer) goto failed;
        if (!(bits = malloc(info->bmiHeader.biSizeImage))) goto failed;
    }
    else
    {
        shm_buffer = wayland_shm_buffer_create(bm.bmWidth, bm.bmHeight,
                                               WL_SHM_FORMAT_ARGB8888);
        if (!shm_buffer) goto failed;
        bits = shm_buffer->map_data;
    }

    if (!NtGdiGetDIBitsInternal(hdc, color, 0, bm.bmHeight, bits, info,
                                DIB_RGB_COLORS, 0, 0))
        goto failed;

    for (i = 0; i < bm.bmWidth * bm.bmHeight; i++)
        if ((has_alpha = (bits[i] & 0xff000000) != 0)) break;

    if (!has_alpha)
    {
        unsigned int width_bytes = (bm.bmWidth + 31) / 32 * 4;
        /* generate alpha channel from the mask */
        info->bmiHeader.biBitCount = 1;
        info->bmiHeader.biSizeImage = width_bytes * bm.bmHeight;
        if (!(mask_bits = malloc(info->bmiHeader.biSizeImage))) goto failed;
        if (!NtGdiGetDIBitsInternal(hdc, mask, 0, bm.bmHeight, mask_bits,
                                    info, DIB_RGB_COLORS, 0, 0))
            goto failed;
        ptr = bits;
        for (i = 0; i < bm.bmHeight; i++)
        {
            for (j = 0; j < bm.bmWidth; j++, ptr++)
            {
                if (!((mask_bits[i * width_bytes + j / 8] << (j % 8)) & 0x80))
                    *ptr |= 0xff000000;
            }
        }
        free(mask_bits);
    }

    if (use_padding)
    {
        copy_rectangle_into_center_of_square(bits, bm.bmWidth,
                                             bm.bmHeight, shm_buffer->map_data);
        free(bits);
        bits = shm_buffer->map_data;
    }

    /* Wayland requires pre-multiplied alpha values */
    for (ptr = bits, i = 0; i < shm_buffer->width * shm_buffer->height; ptr++, i++)
    {
        unsigned char alpha = *ptr >> 24;
        if (alpha == 0)
        {
            *ptr = 0;
        }
        else if (alpha != 255)
        {
            *ptr = (alpha << 24) |
                   (((BYTE)(*ptr >> 16) * alpha / 255) << 16) |
                   (((BYTE)(*ptr >> 8) * alpha / 255) << 8) |
                   (((BYTE)*ptr * alpha / 255));
        }
    }

    return shm_buffer;

failed:
    if (shm_buffer) wayland_shm_buffer_unref(shm_buffer);
    if (use_padding) free(bits);
    free(mask_bits);
    return NULL;
}

/**********************************************************************
 *          wayland_surface_coords_from_window
 *
 * Converts the window (logical) coordinates to wayland surface-local coordinates.
 */
void wayland_surface_coords_from_window(struct wayland_surface *surface,
                                        int window_x, int window_y,
                                        int *surface_x, int *surface_y)
{
    *surface_x = round(window_x / surface->window.scale);
    *surface_y = round(window_y / surface->window.scale);
}

/**********************************************************************
 *          wayland_surface_coords_to_window
 *
 * Converts the surface-local coordinates to window (logical) coordinates.
 */
void wayland_surface_coords_to_window(struct wayland_surface *surface,
                                      double surface_x, double surface_y,
                                      int *window_x, int *window_y)
{
    *window_x = round(surface_x * surface->window.scale);
    *window_y = round(surface_y * surface->window.scale);
}

static struct wayland_client_surface *impl_from_client_surface(struct client_surface *client)
{
    return CONTAINING_RECORD(client, struct wayland_client_surface, client);
}

static void wayland_client_surface_destroy(struct client_surface *client)
{
    struct wayland_client_surface *surface = impl_from_client_surface(client);

    TRACE("%s\n", debugstr_client_surface(client));

    if (surface->wp_color_management_surface_v1)
        wp_color_management_surface_v1_destroy(surface->wp_color_management_surface_v1);
    if (surface->wp_content_type_v1)
        wp_content_type_v1_destroy(surface->wp_content_type_v1);
    if (surface->wp_viewport)
        wp_viewport_destroy(surface->wp_viewport);
    if (surface->wl_subsurface)
        wl_subsurface_destroy(surface->wl_subsurface);
    if (surface->wl_surface)
        wl_surface_destroy(surface->wl_surface);
}

static void wayland_client_surface_detach(struct client_surface *client)
{
    struct wayland_client_surface *surface = impl_from_client_surface(client);
    struct wayland_win_data *data;

    if ((data = wayland_win_data_get(client->hwnd)))
    {
        if (data->client_surface == surface) data->client_surface = NULL;
        wayland_client_surface_attach(surface, NULL);
        wayland_win_data_release(data);
    }
}

static void wayland_client_surface_update(struct client_surface *client)
{
    struct wayland_client_surface *surface = impl_from_client_surface(client);
    HWND hwnd = client->hwnd, toplevel = NtUserGetAncestor(hwnd, GA_ROOT);
    struct wayland_win_data *data;

    if (!(data = wayland_win_data_get(hwnd))) return;

    if (data->client_surface != surface)
    {
        if (toplevel && NtUserIsWindowVisible(hwnd))
            wayland_client_surface_attach(surface, toplevel);
        else
            wayland_client_surface_attach(surface, NULL);
    }

    wayland_win_data_release(data);
}

static BOOL wayland_client_surface_is_hwnd_dmabuf_producer(struct wayland_client_surface *surface)
{
    struct wayland_win_data *data;
    HANDLE handle = 0;
    HWND hwnd = surface->client.hwnd;

    if (surface->hwnd_dmabuf_producer) return TRUE;

    if ((data = wayland_win_data_get(hwnd)))
    {
        wayland_win_data_release(data);
        return FALSE;
    }

    if (wine_hwnd_dmabuf_claim_channel(hwnd, &handle) != HWND_DMABUF_OK || !handle)
        return FALSE;

    NtClose(handle);
    surface->hwnd_dmabuf_producer = TRUE;
    TRACE("hwnd=%p is an HWND dmabuf producer without local wayland data; skipping client surface presents\n",
          hwnd);
    return TRUE;
}

static void wayland_client_surface_present(struct client_surface *client, HDC hdc)
{
    struct wayland_client_surface *surface = impl_from_client_surface(client);
    HWND hwnd = client->hwnd, toplevel = NtUserGetAncestor(hwnd, GA_ROOT);

    TRACE("%s hdc=%p toplevel=%p\n", debugstr_client_surface(client), hdc, toplevel);

    if (wayland_client_surface_is_hwnd_dmabuf_producer(surface)) return;

    ensure_window_surface_contents(toplevel);
    set_client_surface(hwnd, surface);
}

static const struct client_surface_funcs wayland_client_surface_funcs =
{
    .destroy = wayland_client_surface_destroy,
    .detach = wayland_client_surface_detach,
    .update = wayland_client_surface_update,
    .present = wayland_client_surface_present,
};

struct wayland_client_surface *wayland_client_surface_create(HWND hwnd)
{
    struct wayland_client_surface *client;
    struct wl_region *empty_region;

    if (!(client = client_surface_create(sizeof(*client), &wayland_client_surface_funcs, hwnd))) return NULL;

    client->wl_surface =
        wl_compositor_create_surface(process_wayland.wl_compositor);
    if (!client->wl_surface)
    {
        ERR("Failed to create client wl_surface\n");
        goto err;
    }
    wl_surface_set_user_data(client->wl_surface, hwnd);

    /* Let parent handle all pointer events. */
    empty_region = wl_compositor_create_region(process_wayland.wl_compositor);
    if (!empty_region)
    {
        ERR("Failed to create wl_region\n");
        goto err;
    }
    wl_surface_set_input_region(client->wl_surface, empty_region);
    wl_region_destroy(empty_region);

    client->wp_viewport =
        wp_viewporter_get_viewport(process_wayland.wp_viewporter,
                                    client->wl_surface);
    if (!client->wp_viewport)
    {
        ERR("Failed to create client wp_viewport\n");
        goto err;
    }

    if (process_wayland.wp_content_type_manager_v1)
    {
        client->wp_content_type_v1 =
            wp_content_type_manager_v1_get_surface_content_type(
                process_wayland.wp_content_type_manager_v1,
                client->wl_surface
            );
        if (client->wp_content_type_v1)
        {
            wp_content_type_v1_set_content_type(client->wp_content_type_v1,
                                                WP_CONTENT_TYPE_V1_TYPE_GAME);
            TRACE("set game content on client surface!\n");
        }
    }

    return client;

err:
    client_surface_release(&client->client);
    return NULL;
}

void wayland_client_surface_attach(struct wayland_client_surface *client, HWND toplevel)
{
    struct wayland_win_data *toplevel_data;
    struct wayland_surface *surface;
    HWND hwnd = client->client.hwnd;
    RECT client_rect;

    if (!toplevel)
    {
        if (client->wl_subsurface)
        {
            wl_subsurface_destroy(client->wl_subsurface);
            client->wl_subsurface = NULL;
        }

        client->toplevel = 0;
        return;
    }

    if (!(toplevel_data = wayland_win_data_get_nolock(toplevel)) || !(surface = toplevel_data->wayland_surface))
    {
        wayland_client_surface_attach(client, NULL);
        return;
    }

    if (client->toplevel != toplevel)
    {
        wayland_client_surface_attach(client, NULL);

        client->wl_subsurface =
            wl_subcompositor_get_subsurface(process_wayland.wl_subcompositor,
                                            client->wl_surface,
                                            surface->wl_surface);
        if (!client->wl_subsurface)
        {
            ERR("Failed to create client wl_subsurface\n");
            return;
        }
        /* Present contents independently of the parent surface. */
        wl_subsurface_set_desync(client->wl_subsurface);

        client->toplevel = toplevel;
    }

    NtUserGetClientRect(hwnd, &client_rect, NtUserGetWinMonitorDpi(hwnd, MDT_RAW_DPI));
    NtUserMapWindowPoints(hwnd, toplevel, (POINT *)&client_rect, 2, NtUserGetWinMonitorDpi(hwnd, MDT_RAW_DPI));

    wayland_surface_reconfigure_client(surface, client, &client_rect);
    /* Commit to apply subsurface positioning. */
    wl_surface_commit(surface->wl_surface);
}

static void wayland_image_description_v1_failed(void *user_data,
                    struct wp_image_description_v1 *wp_image_description_v1,
                    uint32_t cause, const char *msg)
{
    struct wayland_client_surface *surface = user_data;
    ERR("cause=%u msg=%s\n", cause, debugstr_a(msg));

    wp_image_description_v1_destroy(wp_image_description_v1);
    client_surface_release(&surface->client);
}

static void wayland_image_description_v1_ready2(void *user_data,
                    struct wp_image_description_v1 *wp_image_description_v1,
                    uint32_t identity_hi, uint32_t identity_lo)
{
    struct wayland_client_surface *surface = user_data;
    TRACE("id=%#x%x\n", identity_hi, identity_lo);

    if (!surface->wp_color_management_surface_v1)
    {
        surface->wp_color_management_surface_v1 =
            wp_color_manager_v1_get_surface(process_wayland.wp_color_manager_v1,
                                            surface->wl_surface);
        if (!surface->wp_color_management_surface_v1)
        {
            ERR("Failed to create color management surface for client surface!\n");
            return;
        }
    }
    wp_color_management_surface_v1_set_image_description(
        surface->wp_color_management_surface_v1,
        wp_image_description_v1,
        WP_COLOR_MANAGER_V1_RENDER_INTENT_PERCEPTUAL);
    wp_image_description_v1_destroy(wp_image_description_v1);
    client_surface_release(&surface->client);
}

static void wayland_image_description_v1_ready(void *user_data,
                    struct wp_image_description_v1 *wp_image_description_v1,
                    uint32_t identity)
{
    wayland_image_description_v1_ready2(user_data, wp_image_description_v1, 0, identity);
}

static const struct wp_image_description_v1_listener image_description_listener = {
    wayland_image_description_v1_failed,
    wayland_image_description_v1_ready,
    wayland_image_description_v1_ready2
};

void wayland_client_surface_attach_image_description(struct client_surface *client,
                                                     struct wp_image_description_v1 *image_desc)
{
    struct wayland_client_surface *surface = impl_from_client_surface(client);
    if (!image_desc)
    {
        if (surface->wp_color_management_surface_v1)
        {
            wp_color_management_surface_v1_destroy(surface->wp_color_management_surface_v1);
            surface->wp_color_management_surface_v1 = NULL;
        }
        return;
    }

    client_surface_add_ref(&surface->client);
    wp_image_description_v1_add_listener(image_desc, &image_description_listener, surface);
    wl_display_flush(process_wayland.wl_display);
}

void wayland_client_surface_set_alpha(struct client_surface *client, BOOL alpha)
{
    struct wayland_client_surface *surface = impl_from_client_surface(client);
    surface->has_alpha = alpha;
}

static void dummy_buffer_release(void *data, struct wl_buffer *buffer)
{
    struct wayland_shm_buffer *shm_buffer = data;
    TRACE("shm_buffer=%p\n", shm_buffer);
    wayland_shm_buffer_unref(shm_buffer);
}

static const struct wl_buffer_listener dummy_buffer_listener =
{
    dummy_buffer_release
};

/**********************************************************************
 *          wayland_surface_ensure_contents
 *
 * Ensure that the wayland surface has up-to-date contents, by committing
 * a dummy buffer if necessary.
 */
void wayland_surface_ensure_contents(struct wayland_surface *surface,
                                     struct wayland_client_surface *client)
{
    struct wayland_shm_buffer *dummy_shm_buffer;
    HRGN damage = NULL;
    int width, height;
    BOOL needs_contents;

    width = surface->window.rect.right - surface->window.rect.left;
    height = surface->window.rect.bottom - surface->window.rect.top;
    needs_contents = surface->window.visible && client &&
                     (surface->content_width != width ||
                      surface->content_height != height);

    if (!needs_contents)
    {
        wayland_surface_update_hwnd_dmabufs(surface);
        return;
    }

    TRACE("surface=%p hwnd=%p needs_contents=%d\n",
          surface, surface->hwnd, needs_contents);

    if (wayland_surface_reconfigure(surface))
    {
        enum wl_shm_format format;

        /* the toplevel can be transparent only if the client is,
         * and we assume there is no alpha to begin with. */
        format = client->has_alpha ? WL_SHM_FORMAT_ARGB8888 : WL_SHM_FORMAT_XRGB8888;

        dummy_shm_buffer = wayland_shm_buffer_create(width, height, format);
        if (!dummy_shm_buffer)
        {
            ERR("Failed to create dummy buffer\n");
            return;
        }
        wl_buffer_add_listener(dummy_shm_buffer->wl_buffer, &dummy_buffer_listener,
                               dummy_shm_buffer);

        if (!(damage = NtGdiCreateRectRgn(0, 0, width, height)))
            WARN("Failed to create damage region for dummy buffer\n");

        wayland_surface_attach_shm(surface, dummy_shm_buffer, damage);
        wl_surface_commit(surface->wl_surface);
    }

    if (damage) NtGdiDeleteObjectApp(damage);
    wayland_surface_update_hwnd_dmabufs(surface);
}

/**********************************************************************
 *          wayland_surface_set_title
 */
void wayland_surface_set_title(struct wayland_surface *surface, LPCWSTR text)
{
    DWORD text_len;
    DWORD utf8_count;
    char *utf8 = NULL;

    assert(wayland_surface_is_toplevel(surface));

    TRACE("surface=%p hwnd=%p text='%s'\n",
          surface, surface->hwnd, wine_dbgstr_w(text));

    text_len = (lstrlenW(text) + 1) * sizeof(WCHAR);

    if (!RtlUnicodeToUTF8N(NULL, 0, &utf8_count, text, text_len) &&
        (utf8 = malloc(utf8_count)))
    {
        RtlUnicodeToUTF8N(utf8, utf8_count, &utf8_count, text, text_len);
        xdg_toplevel_set_title(surface->xdg_toplevel, utf8);
    }

    free(utf8);
}

/**********************************************************************
 *          wayland_surface_set_icon_buffer
 */
void wayland_surface_set_icon_buffer(struct wayland_surface *surface, UINT type, const ICONINFO *ii)
{
    struct wayland_shm_buffer *icon_buf;
    HDC hDC;

    if (!process_wayland.xdg_toplevel_icon_manager_v1) return;

    assert(ii);

    TRACE("surface=%p type=%x ii=%p\n", surface, type, ii);

    hDC = NtGdiCreateCompatibleDC(0);
    icon_buf = wayland_shm_buffer_from_color_bitmaps(hDC, ii->hbmColor, ii->hbmMask, TRUE);
    NtGdiDeleteObjectApp(hDC);

    if (surface->big_icon_buffer && type == ICON_BIG)
    {
        wayland_shm_buffer_unref(surface->big_icon_buffer);
        surface->big_icon_buffer = NULL;
    }
    else if (surface->small_icon_buffer && type != ICON_BIG)
    {
        wayland_shm_buffer_unref(surface->small_icon_buffer);
        surface->small_icon_buffer = NULL;
    }

    if (icon_buf)
    {
        if (type == ICON_BIG) surface->big_icon_buffer = icon_buf;
        else surface->small_icon_buffer = icon_buf;
    }
}

/**********************************************************************
 *          wayland_surface_assign_icon
 */
void wayland_surface_assign_icon(struct wayland_surface *surface)
{
    if (!process_wayland.xdg_toplevel_icon_manager_v1) return;

    assert(wayland_surface_is_toplevel(surface));

    TRACE("surface=%p\n", surface);

    if (surface->xdg_toplevel_icon)
    {
        xdg_toplevel_icon_manager_v1_set_icon(process_wayland.xdg_toplevel_icon_manager_v1,
                                              surface->xdg_toplevel, NULL);
        xdg_toplevel_icon_v1_destroy(surface->xdg_toplevel_icon);
        surface->xdg_toplevel_icon = NULL;
    }

    if (surface->big_icon_buffer)
    {
        surface->xdg_toplevel_icon =
            xdg_toplevel_icon_manager_v1_create_icon(process_wayland.xdg_toplevel_icon_manager_v1);

        /* FIXME: what to do with scale ? */
        xdg_toplevel_icon_v1_add_buffer(surface->xdg_toplevel_icon,
                                        surface->big_icon_buffer->wl_buffer, 1);
        if (surface->small_icon_buffer)
        {
            xdg_toplevel_icon_v1_add_buffer(surface->xdg_toplevel_icon,
                                            surface->small_icon_buffer->wl_buffer, 1);
        }

        xdg_toplevel_icon_v1_set_name(surface->xdg_toplevel_icon, "");

        xdg_toplevel_icon_manager_v1_set_icon(process_wayland.xdg_toplevel_icon_manager_v1,
                                              surface->xdg_toplevel, surface->xdg_toplevel_icon);
    }
}

static void xdg_activation_token_handle_done(void *user_data,
                                             struct xdg_activation_token_v1 *xdg_activation_token_v1,
                                             const char *token)
{
    HWND hwnd = user_data;
    struct wayland_win_data *data;
    struct wayland_surface *surface;


    if ((data = wayland_win_data_get(hwnd)))
    {
        if ((surface = data->wayland_surface))
            xdg_activation_v1_activate(process_wayland.xdg_activation_v1, token, surface->wl_surface);
        wayland_win_data_release(data);
    }

    xdg_activation_token_v1_destroy(xdg_activation_token_v1);
}

const static struct xdg_activation_token_v1_listener xdg_activation_listener = {
    xdg_activation_token_handle_done
};

void wayland_surface_activate(struct wayland_surface *surface)
{
    struct xdg_activation_token_v1 *token;
    assert(surface);

    if (process_wayland.xdg_activation_v1)
    {
        token = xdg_activation_v1_get_activation_token(process_wayland.xdg_activation_v1);

        if (!token)
        {
            ERR("Failed to create activation token!\n");
            return;
        }

        xdg_activation_token_v1_add_listener(token, &xdg_activation_listener, surface->hwnd);
        xdg_activation_token_v1_set_surface(token, surface->wl_surface);
        xdg_activation_token_v1_commit(token);
    }
}

static BOOL use_inhibit(void)
{
    static int enabled = -1;

    if (enabled == -1)
    {
        const char *env = getenv("WAYLANDDRV_SHORTCUT_INHIBIT");
        enabled = env && atoi(env);
    }

    return enabled;
}

void wayland_surface_shortcut_control(struct wayland_surface *surface, BOOL inhibit)
{
    BOOL should_inhibit = inhibit && use_inhibit();

    if (!process_wayland.zwp_keyboard_shortcuts_inhibit_manager_v1) return;

    if (should_inhibit)
    {
        if (!surface->zwp_keyboard_shortcuts_inhibitor_v1)
        {
            surface->zwp_keyboard_shortcuts_inhibitor_v1 =
                zwp_keyboard_shortcuts_inhibit_manager_v1_inhibit_shortcuts(
                    process_wayland.zwp_keyboard_shortcuts_inhibit_manager_v1,
                    surface->wl_surface, process_wayland.seat.wl_seat);
            /* dont create a listener since we dont care
             * if the shortcuts are actually inhibited or not */
        }
    }
    else if (surface->zwp_keyboard_shortcuts_inhibitor_v1)
    {
        zwp_keyboard_shortcuts_inhibitor_v1_destroy(
            surface->zwp_keyboard_shortcuts_inhibitor_v1);
        surface->zwp_keyboard_shortcuts_inhibitor_v1 = NULL;
    }
}
