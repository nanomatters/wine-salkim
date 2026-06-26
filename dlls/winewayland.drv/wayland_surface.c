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
#include <errno.h>

#include "waylanddrv.h"
#include "dxgi1_2.h"
#include "wine/debug.h"
#include "wine/hwnd_dmabuf.h"
#include "wine/server.h"

WINE_DEFAULT_DEBUG_CHANNEL(waylanddrv);

static void wayland_surface_clear_direct_dmabuf(struct wayland_surface *surface,
                                                struct wayland_win_data *data);
static void wayland_surface_update_hwnd_dmabufs(struct wayland_surface *surface);
static BOOL wayland_surface_try_direct_dmabuf(HWND hwnd);

static const char *wayland_child_visibility_str(enum wayland_child_visibility visibility)
{
    switch (visibility)
    {
    case WAYLAND_CHILD_VISIBILITY_AS_IS:
        return "as-is";
    case WAYLAND_CHILD_VISIBILITY_CROPPED:
        return "cropped";
    case WAYLAND_CHILD_VISIBILITY_UNMASKABLE:
        return "unmaskable";
    }

    return "unknown";
}

struct wayland_child_visibility_info
{
    enum wayland_child_visibility visibility;
    RECT rect;
    unsigned int rect_count;
};

static HRGN create_child_region(HRGN shape_region)
{
    HRGN region;

    if (!shape_region) return 0;
    if (!(region = NtGdiCreateRectRgn(0, 0, 0, 0))) return 0;

    if (NtGdiCombineRgn(region, shape_region, 0, RGN_COPY) == ERROR)
    {
        NtGdiDeleteObjectApp(region);
        return 0;
    }

    return region;
}

static void wayland_surface_set_region_constraints(struct wayland_surface *surface,
                                                   HRGN shape_region, HRGN clip_region)
{
    if (surface->child_region) NtGdiDeleteObjectApp(surface->child_region);
    surface->child_region = create_child_region(shape_region);
    surface->shaped = shape_region != 0;
    surface->occlusion_clipped = clip_region != 0 &&
        (!shape_region || !NtGdiEqualRgn(clip_region, shape_region));
}

static struct wl_region *wayland_surface_create_shape_input_region(struct wayland_surface *surface,
                                                                   HRGN shape_region)
{
    struct wl_region *region;
    RGNDATA *data;
    RECT *rect, *end;

    if (!(region = wl_compositor_create_region(process_wayland.wl_compositor)))
        return NULL;

    if (!(data = get_region_data(shape_region)))
    {
        wl_region_destroy(region);
        return NULL;
    }

    rect = (RECT *)data->Buffer;
    end = rect + data->rdh.nCount;
    for (; rect < end; rect++)
    {
        int left, top, right, bottom;

        wayland_surface_coords_from_window(surface, rect->left, rect->top, &left, &top);
        wayland_surface_coords_from_window(surface, rect->right, rect->bottom, &right, &bottom);
        if (right > left && bottom > top)
            wl_region_add(region, left, top, right - left, bottom - top);
    }

    free(data);
    return region;
}

static void wayland_surface_sync_shape_input_region(struct wayland_surface *surface, HRGN shape_region)
{
    DWORD exstyle = NtUserGetWindowLongW(surface->hwnd, GWL_EXSTYLE);
    BOOL transparent = (exstyle & WS_EX_TRANSPARENT) && (exstyle & WS_EX_LAYERED);
    struct wl_region *region = NULL;

    if (transparent)
    {
        region = wl_compositor_create_region(process_wayland.wl_compositor);
        if (region) wl_surface_set_input_region(surface->wl_surface, region);
    }
    else if (shape_region)
    {
        region = wayland_surface_create_shape_input_region(surface, shape_region);
        if (region) wl_surface_set_input_region(surface->wl_surface, region);
    }
    else wl_surface_set_input_region(surface->wl_surface, NULL);

    if (region) wl_region_destroy(region);
}

static HRGN wayland_surface_get_window_shape_region(HWND hwnd)
{
    HRGN shape_region = NtGdiCreateRectRgn(0, 0, 0, 0);

    if (!shape_region) return 0;
    if (NtUserGetWindowRgnEx(hwnd, shape_region, 0) == ERROR)
    {
        NtGdiDeleteObjectApp(shape_region);
        return 0;
    }

    return shape_region;
}

void wayland_surface_sync_window_regions(struct wayland_surface *surface,
                                         struct window_surface *window_surface)
{
    HRGN shape_region, owned_shape_region = 0;

    if (window_surface && window_surface->shape_region)
        shape_region = window_surface->shape_region;
    else
        shape_region = owned_shape_region =
            wayland_surface_get_window_shape_region(surface->hwnd);

    wayland_surface_sync_shape_input_region(surface, shape_region);
    wayland_surface_set_region_constraints(surface, shape_region,
                                           window_surface ? window_surface->clip_region : 0);

    if (owned_shape_region) NtGdiDeleteObjectApp(owned_shape_region);
}

static void request_window_surface_expose(HWND hwnd, BOOL allow_inline)
{
    /* The direct dmabuf fast path may run inline from event callbacks because
     * it only commits Wayland state under win_data_mutex. It must not enter the
     * win32u window_surface flush path, which has its own lock ordering. */
    if (wayland_surface_try_direct_dmabuf(hwnd)) return;

    /* Inline exposes preserve the initial-configure path for windows that draw
     * without pumping messages. Child-overlay windows must defer to avoid the
     * event thread taking the surface lock before win_data_mutex. */
    if (allow_inline && !window_surface_needs_child_overlays(hwnd))
    {
        NtUserExposeWindowSurface(hwnd, 0, NULL, 0);
        return;
    }

    NtUserPostMessage(hwnd, WM_WAYLAND_EXPOSE, 0, 0);
}

struct wayland_hwnd_dmabuf_surface;

struct wayland_hwnd_dmabuf_buffer
{
    struct wl_list link;
    struct wayland_hwnd_dmabuf_surface *surface;
    struct wl_buffer *wl_buffer;
    UINT64 producer_unique_id;
    UINT64 release_token;
    UINT64 modifier;              /* cached-slot layout identity */
    unsigned int image_id;        /* producer ring slot. Cache key (stable-slot path) */
    unsigned int ring_generation; /* swapchain-rebuild counter. Invalidates a cached slot */
    unsigned int fourcc;
    unsigned int stride;
    unsigned int offset;
    unsigned int alpha_mode;
    int width;
    int height;
    LONG ref;          /* owner ref + one ref per outstanding compositor commit */
    LONG released;     /* uncached path: set by wl_buffer.release, reaped next pass */
    LONG cache_valid;  /* stable-slot cache entry is still retained by this surface */
    BOOL stable_slot;  /* cached and reused per slot. Release token sent on release */
    unsigned int release_flags;
    int channel_fd;    /* dup of surface->channel_fd for sending release tokens */
};

struct wayland_hwnd_dmabuf_surface
{
    struct wl_list link;
    HWND hwnd;
    struct wayland_surface *parent;
    struct wl_surface *wl_surface;
    struct wl_subsurface *wl_subsurface;
    struct wp_viewport *wp_viewport;
    struct wayland_hwnd_dmabuf_buffer *current;
    struct wl_list buffers;
    UINT frame_seq;
    BOOL current_committed;
    BOOL logged_first_attach;
    BOOL logged_first_import;
    BOOL seen;
    BOOL direct;
    BOOL linked;
    unsigned long long last_seen_ms; /* tick when last present in the producer list */
    int committed_width, committed_height;
    int channel_fd;                 /* consumer end of the producer socket, or -1 */
    struct wayland_visual_constraint_trace visual_constraint_trace;
};

/* A child may briefly drop out of the descendant list between frames. Tearing its surface
 * (and dmabuf cache) down on a single miss churns the cache and, with fd-once, strands slots
 * the producer thinks are still cached. Keep an unseen surface for a grace window first. */
#define WAYLAND_DMABUF_SURFACE_GRACE_MS 1000
#define POPUP_GRAB_SERIAL_TIMEOUT_MS 1000

unsigned long long wayland_time_ms(void)
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

/* Send a release record, retrying EINTR. */
static int wayland_hwnd_dmabuf_channel_send_release(int channel_fd, const hwnd_dmabuf_release_t *rel)
{
    ssize_t n;

    do n = send(channel_fd, rel, sizeof(*rel), MSG_DONTWAIT | MSG_NOSIGNAL);
    while (n < 0 && errno == EINTR);
    return n == sizeof(*rel) ? 0 : n < 0 ? errno : EMSGSIZE;
}

static void wayland_hwnd_dmabuf_buffer_send_release(struct wayland_hwnd_dmabuf_buffer *buffer,
                                                    unsigned int flags, BOOL keep_token)
{
    UINT64 release_token;

    if (buffer->channel_fd < 0) return;
    release_token = wayland_hwnd_dmabuf_buffer_exchange_release_token(buffer, 0);
    if (release_token)
    {
        hwnd_dmabuf_release_t rel = { buffer->producer_unique_id, release_token,
                                      flags | wayland_hwnd_dmabuf_buffer_cache_flags(buffer),
                                      buffer->image_id, buffer->ring_generation, 0 };
        if (keep_token && wayland_hwnd_dmabuf_channel_send_release(buffer->channel_fd, &rel))
            InterlockedCompareExchange64((LONGLONG volatile *)&buffer->release_token, release_token, 0);
        else if (!keep_token)
            wayland_hwnd_dmabuf_channel_send_release(buffer->channel_fd, &rel);
    }
}

/* Drop a buffer reference. The last unref destroys the wl_buffer and frees.
 * Lock-free, may run on the present thread or the event thread (inside the
 * wl_buffer.release handler). wl_buffer_destroy never races a release dispatch. */
static void wayland_hwnd_dmabuf_buffer_unref(struct wayland_hwnd_dmabuf_buffer *buffer)
{
    if (InterlockedDecrement(&buffer->ref) > 0) return;
    wayland_hwnd_dmabuf_buffer_send_release(buffer, buffer->release_flags ?
                                            buffer->release_flags : HWND_DMABUF_RELEASE_ORPHANED, FALSE);
    if (buffer->channel_fd >= 0) close(buffer->channel_fd);
    if (buffer->wl_buffer) wl_buffer_destroy(buffer->wl_buffer);
    free(buffer);
}

/* Return a release token to the producer. */
static void wayland_hwnd_dmabuf_send_release(struct wayland_hwnd_dmabuf_surface *surface,
                                             UINT64 producer_unique_id, UINT64 release_token,
                                             unsigned int flags, unsigned int image_id,
                                             unsigned int ring_generation)
{
    hwnd_dmabuf_release_t rel = { producer_unique_id, release_token, flags, image_id, ring_generation, 0 };

    /* One-shot reject/orphan releases are best effort. */
    if (release_token && surface->channel_fd >= 0)
        wayland_hwnd_dmabuf_channel_send_release(surface->channel_fd, &rel);
}

/* Present-thread teardown: detach from the surface and drop the owner ref.
 * Must be called with win_data_mutex held.
 * If the compositor still holds the buffer it survives as an orphan until its
 * release handler drops the last ref. */
static void wayland_hwnd_dmabuf_buffer_reap(struct wayland_hwnd_dmabuf_buffer *buffer)
{
    struct wayland_hwnd_dmabuf_surface *surface = buffer->surface;

    if (surface && surface->current == buffer)
    {
        surface->current = NULL;
        surface->current_committed = FALSE;
    }
    InterlockedExchange(&buffer->cache_valid, FALSE);
    wl_list_remove(&buffer->link);
    buffer->surface = NULL;
    wayland_hwnd_dmabuf_buffer_unref(buffer);
}

/* wl_buffer.release runs on the event thread: lock-free only, no surface/list
 * mutation. Cached stable-slot buffers stay cached for the slot's next frame.
 * Send the release token now to recycle the slot. The busy gate blocks the
 * producer from overwriting it until then. Uncached buffers are flagged for the
 * present thread to reap next pass (token sent on destroy). Either way drop the
 * per-commit ref. */
static void wayland_hwnd_dmabuf_buffer_handle_release(void *data, struct wl_buffer *wl_buffer)
{
    struct wayland_hwnd_dmabuf_buffer *buffer = data;

    if (buffer->stable_slot)
        wayland_hwnd_dmabuf_buffer_send_release(buffer, HWND_DMABUF_RELEASE_PRESENTED, TRUE);
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

    if (surface->parent && surface->parent->direct_dmabuf_surface == surface)
        surface->parent->direct_dmabuf_surface = NULL;
    if (surface->linked) wl_list_remove(&surface->link);
    if (surface->wl_subsurface) wl_subsurface_destroy(surface->wl_subsurface);
    if (!surface->direct && surface->wp_viewport) wp_viewport_destroy(surface->wp_viewport);
    if (!surface->direct && surface->wl_surface) wl_surface_destroy(surface->wl_surface);
    if (surface->channel_fd >= 0) close(surface->channel_fd);
    free(surface);
}

static inline BOOL popup_config_changed(struct wayland_surface_config *c1,
                                        struct wayland_surface_config *c2)
{
    return c1->height != c2->height || c1->width != c2->width ||
           c1->x != c2->x || c1->y != c2->y;
}

static BOOL wayland_surface_config_has_bounds(const struct wayland_surface_config *config)
{
    return config->bounds_set && config->bounds_width > 0 && config->bounds_height > 0;
}

static const struct wayland_surface_config *wayland_surface_latest_config(struct wayland_surface *surface)
{
    if (surface->requested.serial) return &surface->requested;
    if (surface->processing.serial) return &surface->processing;
    if (surface->current.serial) return &surface->current;
    return NULL;
}

static void wayland_surface_config_inherit_caps_and_bounds(struct wayland_surface_config *config,
                                                           struct wayland_surface *surface)
{
    const struct wayland_surface_config *prev = wayland_surface_latest_config(surface);

    if (!prev) return;

    if (!config->caps && prev->caps) config->caps = prev->caps;
    if (!config->bounds_set)
    {
        config->bounds_set = prev->bounds_set;
        config->bounds_width = prev->bounds_width;
        config->bounds_height = prev->bounds_height;
    }
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
    if (!(surface = data->wayland_surface) || surface->xdg_surface != xdg_surface)
    {
        wayland_win_data_release(data);
        return;
    }

    if (wayland_surface_is_toplevel(surface))
    {
        /* If we have a previously requested config, we have already sent a
         * WM_WAYLAND_CONFIGURE which hasn't been handled yet. In that case,
         * avoid sending another message to reduce message queue traffic. */
        should_post = surface->requested.serial == 0;
        initial_configure = surface->current.serial == 0;
        surface->pending.serial = serial;
        wayland_surface_config_inherit_caps_and_bounds(&surface->pending, surface);
        if (!surface->pending.decor && surface->current.decor)
            surface->pending.decor = surface->current.decor;
        else if (!surface->pending.decor && surface->requested.decor)
            surface->pending.decor = surface->requested.decor;
        if (surface->pending.decor &&
            surface->pending.decor != surface->current.decor)
        {
            should_post = TRUE;
            initial_configure = TRUE;
        }
        surface->requested = surface->pending;
        memset(&surface->pending, 0, sizeof(surface->pending));
    }
    else if (wayland_surface_is_popup(surface))
    {
        /* expose the surface to ensure that the new config is ack-ed
         * and the popup can move if needed */
        initial_configure = surface->current.serial == 0 ||
                            popup_config_changed(&surface->current,
                                                 &surface->pending);
        surface->pending.serial = serial;
        surface->processing = surface->pending;
        surface->processing.processed = 1;
        memset(&surface->pending, 0, sizeof(surface->pending));
    }

    wayland_win_data_release(data);

    TRACE("post=%u expose=%u\n", should_post, initial_configure);

    if (should_post) NtUserPostMessage(hwnd, WM_WAYLAND_CONFIGURE, 0, 0);

    /* Flush content that could not be presented before the initial configure. */
    if (initial_configure) request_window_surface_expose(hwnd, TRUE);
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

static void xdg_toplevel_handle_configure_bounds(void *private,
                                                 struct xdg_toplevel *xdg_toplevel,
                                                 int width, int height)
{
    struct wayland_surface *surface;
    struct wayland_win_data *data;
    HWND hwnd = private;

    TRACE("hwnd=%p bounds=%dx%d\n", hwnd, width, height);

    if (!(data = wayland_win_data_get(hwnd))) return;

    if ((surface = data->wayland_surface) && wayland_surface_is_toplevel(surface))
    {
        surface->pending.bounds_width = width;
        surface->pending.bounds_height = height;
        surface->pending.bounds_set = TRUE;
    }

    wayland_win_data_release(data);
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

static void xdg_popup_handle_configure(void *private, struct xdg_popup *xdg_popup,
                                       int32_t x, int32_t y, int32_t width, int32_t height)
{
    HWND hwnd = private;
    struct wayland_win_data *data;
    struct wayland_surface *surface;

    TRACE("hwnd=%p %dx%d\n", hwnd, width, height);

    if (!(data = wayland_win_data_get(hwnd))) return;

    if ((surface = data->wayland_surface) && wayland_surface_is_popup(surface))
    {
        surface->pending.x = x;
        surface->pending.y = y;
        surface->pending.width = width;
        surface->pending.height = height;
        surface->pending.state = 0;
    }

    wayland_win_data_release(data);
}

static void xdg_popup_handle_done(void *private, struct xdg_popup *xdg_popup)
{
    HWND hwnd = private;

    TRACE("hwnd=%p\n", hwnd);

    if (wayland_is_menu_popup(hwnd)) NtUserPostMessage(hwnd, WM_CANCELMODE, 0, 0);
    else NtUserPostMessage(hwnd, WM_SYSCOMMAND, SC_CLOSE, 0);
}

static void xdg_popup_handle_reposition(void *private, struct xdg_popup *xdg_popup, uint32_t token)
{
    /* we get a configure event in this case so we dont need to do anything */
    TRACE("hwnd=%p\n", private);
}

static const struct xdg_popup_listener xdg_popup_listener =
{
    xdg_popup_handle_configure,
    xdg_popup_handle_done,
    xdg_popup_handle_reposition,
};

static void zwlr_layer_surface_v1_handle_configure(void *private,
                                                   struct zwlr_layer_surface_v1 *layer_surface,
                                                   uint32_t serial, uint32_t width, uint32_t height)
{
    HWND hwnd = private;
    struct wayland_surface *surface;
    struct wayland_win_data *data;

    TRACE("hwnd=%p %ux%u serial=%u\n", hwnd, width, height, serial);

    if (!(data = wayland_win_data_get(hwnd))) return;

    if ((surface = data->wayland_surface) && surface->role == WAYLAND_SURFACE_ROLE_LAYER &&
        surface->zwlr_layer_surface_v1 == layer_surface)
    {
        int window_width = surface->window.rect.right - surface->window.rect.left;
        int window_height = surface->window.rect.bottom - surface->window.rect.top;
        int surface_width, surface_height;

        wayland_surface_coords_from_window(surface, window_width, window_height,
                                           &surface_width, &surface_height);
        if (!width) width = max(1, surface_width);
        if (!height) height = max(1, surface_height);

        surface->pending.width = width;
        surface->pending.height = height;
        surface->pending.serial = serial;
        surface->processing = surface->pending;
        surface->processing.processed = TRUE;
        memset(&surface->pending, 0, sizeof(surface->pending));
    }

    wayland_win_data_release(data);

    request_window_surface_expose(hwnd, FALSE);
}

static void zwlr_layer_surface_v1_handle_closed(void *private,
                                                struct zwlr_layer_surface_v1 *layer_surface)
{
    HWND hwnd = private;

    TRACE("hwnd=%p\n", hwnd);
    wayland_cancel_layer_menu(hwnd);
}

static const struct zwlr_layer_surface_v1_listener zwlr_layer_surface_v1_listener =
{
    zwlr_layer_surface_v1_handle_configure,
    zwlr_layer_surface_v1_handle_closed,
};

void wp_fractional_scale_handle_scale(void* user_data,
                                      struct wp_fractional_scale_v1 *fractional_scale_v1,
                                      uint32_t scale_fixed)
{
    struct wayland_win_data *data, *owner_data;
    struct wayland_client_surface *client;
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
                /* reattach the client surface as its rect has changed */
                if ((client = data->client_surface))
                    wayland_client_surface_attach(client, client->toplevel);

                /* the subsurface rect has changed */
                if (surface->role == WAYLAND_SURFACE_ROLE_SUBSURFACE)
                {
                    surface->processing.serial = 1;
                    surface->processing.processed = TRUE;
                }

                /* the popup x,y position has changed */
                if (wayland_surface_is_popup(surface) &&
                   (owner_data = wayland_win_data_get_nolock(surface->owner_hwnd)) &&
                    owner_data->wayland_surface)
                {
                    wayland_surface_make_popup(surface, owner_data->wayland_surface,
                                               &data->rects.window);
                }
            }

            TRACE("hwnd=%p scale %lf\n", hwnd, scale);
        }

        wayland_win_data_release(data);
    }

    if (updated) request_window_surface_expose(hwnd, FALSE);
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
/**********************************************************************
 *          child window overlays
 *
 * A GDI child of an accelerated toplevel is painted into the toplevel's
 * window-surface, which is hidden behind the client surface. Promote each
 * visible child to a wl_subsurface above the client, copied from that buffer.
 */

static void overlay_buffer_release(void *data, struct wl_buffer *wl_buffer)
{
    wayland_shm_buffer_unref(data);
}
static const struct wl_buffer_listener overlay_buffer_listener = { overlay_buffer_release };

static void wayland_child_overlay_destroy(struct wayland_child_overlay *overlay)
{
    wl_list_remove(&overlay->link);
    if (overlay->wp_viewport) wp_viewport_destroy(overlay->wp_viewport);
    if (overlay->wl_subsurface) wl_subsurface_destroy(overlay->wl_subsurface);
    if (overlay->wl_surface) wl_surface_destroy(overlay->wl_surface);
    free(overlay);
}

void wayland_surface_clear_child_overlays(struct wayland_surface *surface)
{
    struct wayland_child_overlay *overlay, *next;

    wl_list_for_each_safe(overlay, next, &surface->child_overlays, link)
        wayland_child_overlay_destroy(overlay);
    surface->child_overlays_need_dmabuf_refresh = FALSE;
}

static void wayland_surface_clear_child_surfaces(struct wayland_surface *surface)
{
    struct wayland_hwnd_dmabuf_surface *dmabuf_surface, *next;

    wayland_surface_clear_child_overlays(surface);
    wayland_surface_clear_direct_dmabuf(surface, NULL);

    wl_list_for_each_safe(dmabuf_surface, next, &surface->hwnd_dmabuf_surfaces, link)
        wayland_hwnd_dmabuf_surface_destroy(dmabuf_surface);

    surface->dmabuf_top = NULL;
}

/* Clear cached input state when the wl_surface is destroyed. Role changes are
 * not input events; compositor enter/leave handles focus while the surface lives. */
static void wayland_surface_clear_input_state(struct wayland_surface *surface)
{
    pthread_mutex_lock(&process_wayland.pointer.mutex);
    if (process_wayland.pointer.focused_hwnd == surface->hwnd)
    {
        process_wayland.pointer.focused_hwnd = NULL;
        process_wayland.pointer.enter_serial = 0;
    }
    if (process_wayland.pointer.constraint_hwnd == surface->hwnd)
        wayland_pointer_clear_constraint();
    if (process_wayland.pointer.popup_serial_hwnd == surface->hwnd)
    {
        process_wayland.pointer.popup_serial = 0;
        process_wayland.pointer.popup_serial_hwnd = NULL;
        process_wayland.pointer.popup_serial_time = 0;
    }
    pthread_mutex_unlock(&process_wayland.pointer.mutex);

    pthread_mutex_lock(&process_wayland.keyboard.mutex);
    if (process_wayland.keyboard.focused_hwnd == surface->hwnd)
        process_wayland.keyboard.focused_hwnd = NULL;
    pthread_mutex_unlock(&process_wayland.keyboard.mutex);

    pthread_mutex_lock(&process_wayland.text_input.mutex);
    if (process_wayland.text_input.focused_hwnd == surface->hwnd)
        process_wayland.text_input.focused_hwnd = NULL;
    pthread_mutex_unlock(&process_wayland.text_input.mutex);
}

/* Prune on hide/destroy: a client-rendered toplevel may never flush GDI again. */
void wayland_surface_remove_child_overlay(struct wayland_surface *surface, HWND child)
{
    struct wayland_child_overlay *overlay, *next;
    BOOL removed = FALSE;

    wl_list_for_each_safe(overlay, next, &surface->child_overlays, link)
        if (overlay->hwnd == child) { wayland_child_overlay_destroy(overlay); removed = TRUE; }

    if (wl_list_empty(&surface->child_overlays))
        surface->child_overlays_need_dmabuf_refresh = FALSE;

    /* commit the parent so the removal shows without a new client frame */
    if (removed) wl_surface_commit(surface->wl_surface);
}

static struct wayland_child_overlay *wayland_child_overlay_create(struct wayland_surface *surface,
                                                                  HWND child)
{
    struct wayland_child_overlay *overlay;

    if (!(overlay = calloc(1, sizeof(*overlay)))) return NULL;
    overlay->hwnd = child;
    overlay->wl_surface = wl_compositor_create_surface(process_wayland.wl_compositor);
    if (overlay->wl_surface)
        overlay->wl_subsurface =
            wl_subcompositor_get_subsurface(process_wayland.wl_subcompositor,
                                            overlay->wl_surface, surface->wl_surface);
    if (!overlay->wl_subsurface)
    {
        if (overlay->wl_surface) wl_surface_destroy(overlay->wl_surface);
        free(overlay);
        return NULL;
    }
    wl_subsurface_set_desync(overlay->wl_subsurface);
    overlay->wp_viewport = wp_viewporter_get_viewport(process_wayland.wp_viewporter,
                                                      overlay->wl_surface);
    /* Pixels only: empty input region so clicks fall through to the toplevel and
     * hit-test to the real child HWND. Input region is committed state, set once. */
    {
        struct wl_region *empty =
            wl_compositor_create_region(process_wayland.wl_compositor);
        if (empty)
        {
            wl_surface_set_input_region(overlay->wl_surface, empty);
            wl_region_destroy(empty);
        }
    }
    return overlay;
}

/* Stack overlays above 'above', chaining each above the previous to preserve
 * Win32 z-order. child_overlays is ordered bottom-to-top. */
static void wayland_surface_raise_child_overlays(struct wayland_surface *surface,
                                                 struct wl_surface *above)
{
    struct wayland_child_overlay *overlay;
    wl_list_for_each(overlay, &surface->child_overlays, link)
    {
        wl_subsurface_place_above(overlay->wl_subsurface, above);
        above = overlay->wl_surface;
    }
}

/* Anchor for the overlay chain: top of the dmabuf chain if alive, else the
 * client surface if attached to this toplevel, else the main surface. The
 * liveness guards matter: place_above on a detached or reparented surface is a
 * fatal protocol error. The fixed [main, client, dmabufs, overlays] order stays
 * stable no matter which chain restacks last. */
static struct wl_surface *wayland_surface_overlay_anchor(struct wayland_surface *surface)
{
    struct wayland_hwnd_dmabuf_surface *dmabuf_surface;
    struct wayland_win_data *data;
    struct wl_surface *above = surface->wl_surface;

    if ((data = wayland_win_data_get_nolock(surface->hwnd)) &&
        data->client_surface && data->client_surface->wl_subsurface &&
        data->client_surface->toplevel == surface->hwnd)
        above = data->client_surface->wl_surface;

    /* dmabuf_top may be stale. Use it only while it is still a live member */
    if (surface->dmabuf_top)
    {
        wl_list_for_each(dmabuf_surface, &surface->hwnd_dmabuf_surfaces, link)
        {
            if (dmabuf_surface->wl_surface == surface->dmabuf_top && dmabuf_surface->current)
            {
                above = surface->dmabuf_top;
                break;
            }
        }
    }

    return above;
}

/* True if child or a descendant is a dmabuf producer of toplevel. That subtree
 * is fed through the hwnd dmabuf chain. Copying this window-surface buffer over
 * it would stack an opaque overlay above the live dmabuf. The producer is
 * in-process (CEF parents its GPU widget host below a full-window content host).
 * Process identity does not distinguish it. Walks the parent chain with NtUser.
 * Runs WITHOUT win_data_mutex. */
static BOOL child_hosts_dmabuf_producer(HWND toplevel, HWND child,
                                        const HWND *producers, unsigned int count)
{
    unsigned int i;

    for (i = 0; i < count; i++)
    {
        HWND a = producers[i];
        while (a)
        {
            if (a == child) return TRUE;
            if (a == toplevel) break;
            a = NtUserGetAncestor(a, GA_PARENT);
        }
    }
    return FALSE;
}

/* Capture visible child geometry with NtUser calls. Runs WITHOUT win_data_mutex.
 * Querying win32u under the mutex can deadlock against threads that hold win32u
 * state while waiting for it (the NGS anti-cheat launcher freeze). */
struct child_overlay_snapshot *child_overlays_snapshot(HWND hwnd)
{
    UINT dpi = NtUserGetDpiForWindow(hwnd);
    struct child_overlay_snapshot *snap;
    unsigned int capacity = 8, producer_total = 0;
    RECT top_rect, client_rect;
    HWND producers[16], child;
    unsigned int producer_count = 0;

    if (!NtUserGetWindowRect(hwnd, &top_rect, dpi)) return NULL;
    /* toplevel client area in screen coords. Children are clipped to it */
    if (!NtUserGetClientRect(hwnd, &client_rect, dpi)) return NULL;
    NtUserMapWindowPoints(hwnd, 0, (POINT *)&client_rect, 2, dpi);

    if (!(snap = malloc(offsetof(struct child_overlay_snapshot, entries[capacity]))))
        return NULL;
    snap->producer_count = 0;
    snap->count = 0;

    /* dmabuf producers under this toplevel, used below to skip their host children */
    {
        hwnd_dmabuf_frame_info_t frames[16];
        unsigned int total = 0, fcount = 0, i;
        if (wine_hwnd_dmabuf_list(hwnd, frames, ARRAY_SIZE(frames), &total, &fcount) == HWND_DMABUF_OK)
        {
            producer_total = total;
            for (i = 0; i < fcount && producer_count < ARRAY_SIZE(producers); i++)
                producers[producer_count++] = (HWND)(UINT_PTR)frames[i].hwnd;
            if (total > producer_count)
                TRACE("hwnd=%p dmabuf producers %u exceed skip capacity %u\n",
                      hwnd, total, (unsigned int)ARRAY_SIZE(producers));
        }
    }
    snap->producer_count = producer_total;

    for (child = NtUserGetWindowRelative(hwnd, GW_CHILD); child;
         child = NtUserGetWindowRelative(child, GW_HWNDNEXT))
    {
        DWORD style = NtUserGetWindowLongW(child, GWL_STYLE);
        struct child_overlay_snapshot_entry *entry;
        DWORD pid = 0;
        int rx, ry, rw, rh;
        RECT cr, vis;

        if ((style & WS_VISIBLE) != WS_VISIBLE) continue;
        /* cross-process children never paint into this buffer. Neither does an
         * in-process host whose content a nested dmabuf producer renders. Both
         * arrive through the hwnd dmabuf chain. An overlay copied over them would
         * sit opaque above the live dmabuf. */
        NtUserGetWindowThread(child, &pid);
        if (pid != GetCurrentProcessId()) continue;
        if (child_hosts_dmabuf_producer(hwnd, child, producers, producer_count)) continue;
        if (!NtUserGetWindowRect(child, &cr, dpi)) continue;
        /* clip to the client area. Sibling occlusion is handled by stacking */
        vis = cr;
        if (vis.left < client_rect.left) vis.left = client_rect.left;
        if (vis.top < client_rect.top) vis.top = client_rect.top;
        if (vis.right > client_rect.right) vis.right = client_rect.right;
        if (vis.bottom > client_rect.bottom) vis.bottom = client_rect.bottom;
        rx = vis.left - top_rect.left; ry = vis.top - top_rect.top;
        rw = vis.right - vis.left; rh = vis.bottom - vis.top;
        if (rw <= 0 || rh <= 0) continue;
        if (rx < 0) { rw += rx; rx = 0; }
        if (ry < 0) { rh += ry; ry = 0; }
        if (rw <= 0 || rh <= 0) continue;

        if (snap->count == capacity)
        {
            struct child_overlay_snapshot *grown;
            capacity *= 2;
            if (!(grown = realloc(snap, offsetof(struct child_overlay_snapshot, entries[capacity]))))
                break;
            snap = grown;
        }
        entry = &snap->entries[snap->count++];
        entry->hwnd = child;
        entry->rect = cr;
        entry->rx = rx; entry->ry = ry;
        entry->rw = rw; entry->rh = rh;
    }

    return snap;
}

/* True for a child that presents its own subsurface (in-process GPU child or a
 * cross-process dmabuf child). Its content must not be covered by a pixel copy. */
static BOOL wayland_surface_child_self_presents(struct wayland_surface *surface, HWND child)
{
    struct wayland_hwnd_dmabuf_surface *dmabuf_surface;
    struct wayland_win_data *child_data;

    if ((child_data = wayland_win_data_get_nolock(child)) &&
        child_data->wayland_surface &&
        child_data->wayland_surface->role == WAYLAND_SURFACE_ROLE_SUBSURFACE)
        return TRUE;

    wl_list_for_each(dmabuf_surface, &surface->hwnd_dmabuf_surfaces, link)
        if (dmabuf_surface->hwnd == child) return TRUE;

    return FALSE;
}

/* Apply a child geometry snapshot to the overlay subsurfaces. Runs under
 * win_data_mutex and must not call NtUser. A NULL snapshot carries no fresh
 * information. Existing overlays are kept and only restacked. shm_buffer is
 * the pixel source and may be NULL to reposition without refreshing content. */
void wayland_surface_apply_child_overlays(struct wayland_surface *surface,
                                          struct wayland_shm_buffer *shm_buffer,
                                          const struct child_overlay_snapshot *snapshot)
{
    struct wayland_child_overlay *overlay, *next;
    struct wl_list old;
    unsigned int i;

    if (snapshot)
    {
        /* refreshed children are moved back below. The rest (gone/hidden) are dropped */
        wl_list_init(&old);
        wl_list_insert_list(&old, &surface->child_overlays);
        wl_list_init(&surface->child_overlays);

        for (i = 0; i < snapshot->count; i++)
        {
            const struct child_overlay_snapshot_entry *entry = &snapshot->entries[i];
            int rx = entry->rx, ry = entry->ry, rw = entry->rw, rh = entry->rh;
            struct wayland_shm_buffer *ob;
            int r, x, y, dw, dh;

            if (wayland_surface_child_self_presents(surface, entry->hwnd)) continue;

            if (shm_buffer)
            {
                if (rx + rw > shm_buffer->width) rw = shm_buffer->width - rx;
                if (ry + rh > shm_buffer->height) rh = shm_buffer->height - ry;
            }
            if (rw <= 0 || rh <= 0) continue;

            overlay = NULL;
            wl_list_for_each(next, &old, link)
                if (next->hwnd == entry->hwnd) { overlay = next; break; }
            if (overlay) wl_list_remove(&overlay->link);
            else if (!shm_buffer) continue; /* no pixels to show a new child with */
            else if (!(overlay = wayland_child_overlay_create(surface, entry->hwnd))) continue;
            wl_list_insert(&surface->child_overlays, &overlay->link);
            overlay->rect = entry->rect;

            wayland_surface_coords_from_window(surface, rx, ry, &x, &y);
            wayland_surface_coords_from_window(surface, rw, rh, &dw, &dh);
            wl_subsurface_set_position(overlay->wl_subsurface, x, y);
            if (dw > 0 && dh > 0) wp_viewport_set_destination(overlay->wp_viewport, dw, dh);

            if (!shm_buffer)
            {
                wl_surface_commit(overlay->wl_surface);
                continue;
            }

            if (!(ob = wayland_shm_buffer_create(rw, rh, shm_buffer->format))) continue;
            for (r = 0; r < rh; r++)
                memcpy((char *)ob->map_data + (size_t)r * rw * 4,
                       (char *)shm_buffer->map_data + (size_t)(ry + r) * shm_buffer->width * 4 + (size_t)rx * 4,
                       (size_t)rw * 4);

            ob->busy = TRUE;
            wl_buffer_add_listener(ob->wl_buffer, &overlay_buffer_listener, ob);
            wl_surface_attach(overlay->wl_surface, ob->wl_buffer, 0, 0);
            wl_surface_damage_buffer(overlay->wl_surface, 0, 0, rw, rh);
            wl_surface_commit(overlay->wl_surface);

            TRACE("hwnd=%p child=%p overlay %d,%d+%dx%d\n", surface->hwnd, entry->hwnd, x, y, dw, dh);
        }

        wl_list_for_each_safe(overlay, next, &old, link)
            wayland_child_overlay_destroy(overlay);

        surface->child_overlays_need_dmabuf_refresh =
            !wl_list_empty(&surface->child_overlays) && !snapshot->producer_count;
    }

    wayland_surface_raise_child_overlays(surface, wayland_surface_overlay_anchor(surface));
}

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
    wl_list_init(&surface->child_overlays);
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
    surface->window.fractional_scale = 1.0;

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
    wayland_surface_clear_input_state(surface);
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

    if (surface->child_region)
    {
        NtGdiDeleteObjectApp(surface->child_region);
        surface->child_region = 0;
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

/* helper to intialize the positioner using a given surface config */
static struct xdg_positioner *popup_create_positioner(struct wayland_surface *surface,
                                                      struct wayland_surface *owner,
                                                      struct wayland_surface_config *config)
{
    struct xdg_positioner *xdg_positioner =
        xdg_wm_base_create_positioner(process_wayland.xdg_wm_base);

    if (!xdg_positioner) return NULL;

    if (config->width <= 0) config->width = 1;
    if (config->height <= 0) config->height = 1;

    xdg_positioner_set_anchor_rect(xdg_positioner, owner->geometry.left,
                                   owner->geometry.top, 1, 1);
    xdg_positioner_set_anchor(xdg_positioner, XDG_POSITIONER_ANCHOR_TOP_LEFT);
    xdg_positioner_set_gravity(xdg_positioner, XDG_POSITIONER_GRAVITY_BOTTOM_RIGHT);
    xdg_positioner_set_offset(xdg_positioner, config->x, config->y);
    xdg_positioner_set_size(xdg_positioner, config->width, config->height);

    return xdg_positioner;
}

static BOOL hwnd_matches_popup_owner(HWND hwnd, HWND owner)
{
    return hwnd == owner || NtUserGetAncestor(hwnd, GA_ROOT) == owner;
}

static void clear_pointer_popup_serial(uint32_t serial, HWND hwnd)
{
    pthread_mutex_lock(&process_wayland.pointer.mutex);
    if (process_wayland.pointer.popup_serial == serial &&
        (!hwnd || process_wayland.pointer.popup_serial_hwnd == hwnd))
    {
        process_wayland.pointer.popup_serial = 0;
        process_wayland.pointer.popup_serial_hwnd = NULL;
        process_wayland.pointer.popup_serial_time = 0;
    }
    pthread_mutex_unlock(&process_wayland.pointer.mutex);
}

static uint32_t popup_grab_serial_for_owner(struct wayland_surface *owner)
{
    uint32_t button_serial, popup_serial, serial = 0;
    unsigned long long popup_serial_time, now;
    HWND focused, popup_hwnd;

    if (wayland_surface_is_popup(owner) && !owner->xdg_popup_grabbed) return 0;

    pthread_mutex_lock(&process_wayland.pointer.mutex);
    focused = process_wayland.pointer.focused_hwnd;
    button_serial = process_wayland.pointer.button_serial;
    popup_serial = process_wayland.pointer.popup_serial;
    popup_hwnd = process_wayland.pointer.popup_serial_hwnd;
    popup_serial_time = process_wayland.pointer.popup_serial_time;
    pthread_mutex_unlock(&process_wayland.pointer.mutex);

    if (button_serial && focused && hwnd_matches_popup_owner(focused, owner->hwnd))
    {
        clear_pointer_popup_serial(button_serial, focused);
        return button_serial;
    }

    if (!popup_serial || !popup_hwnd) return 0;

    now = wayland_time_ms();
    if (now - popup_serial_time > POPUP_GRAB_SERIAL_TIMEOUT_MS ||
        !hwnd_matches_popup_owner(popup_hwnd, owner->hwnd))
    {
        if (now - popup_serial_time > POPUP_GRAB_SERIAL_TIMEOUT_MS)
        {
            clear_pointer_popup_serial(popup_serial, NULL);
        }
        return 0;
    }

    pthread_mutex_lock(&process_wayland.pointer.mutex);
    if (process_wayland.pointer.popup_serial == popup_serial &&
        process_wayland.pointer.popup_serial_hwnd == popup_hwnd)
    {
        serial = process_wayland.pointer.popup_serial;
        process_wayland.pointer.popup_serial = 0;
        process_wayland.pointer.popup_serial_hwnd = NULL;
        process_wayland.pointer.popup_serial_time = 0;
    }
    pthread_mutex_unlock(&process_wayland.pointer.mutex);

    return serial;
}

/**********************************************************************
 *          wayland_surface_make_popup
 *
 * Gives the popup role to a plain wayland surface.
 */
void wayland_surface_make_popup(struct wayland_surface *surface,
                                struct wayland_surface *owner,
                                const RECT *rect)
{
    struct wayland_surface_config config;
    struct xdg_positioner *xdg_positioner = NULL;
    uint32_t grab_serial = 0;

    config.x = rect->left - owner->window.rect.left;
    config.y = rect->top - owner->window.rect.top;
    config.width = rect->right - rect->left;
    config.height = rect->bottom - rect->top;

    assert(owner->xdg_surface);
    assert(!surface->role || surface->role == WAYLAND_SURFACE_ROLE_POPUP);

    if (surface->xdg_popup && surface->owner_hwnd == owner->hwnd)
    {
        if (!surface->current.serial) return;
        /* reposition the popup if needed */
        wayland_surface_coords_from_window(surface, config.x, config.y,
                                            &config.x, &config.y);
        wayland_surface_coords_from_window(surface, config.width, config.height,
                                            &config.width, &config.height);

        if (popup_config_changed(&surface->current, &config))
        {
            xdg_positioner = popup_create_positioner(surface, owner, &config);
            if (!xdg_positioner)
            {
                ERR("Failed to create positioner!\n");
                return;
            }

            xdg_popup_reposition(surface->xdg_popup, xdg_positioner, 0);
            xdg_positioner_destroy(xdg_positioner);
            wl_surface_commit(surface->wl_surface);
            wl_display_flush(process_wayland.wl_display);
        }
        return;
    }

    wayland_surface_clear_role(surface);
    surface->role = WAYLAND_SURFACE_ROLE_POPUP;
    surface->owner_hwnd = NULL;
    surface->xdg_popup_grabbed = FALSE;

    surface->xdg_surface = xdg_wm_base_get_xdg_surface(process_wayland.xdg_wm_base,
                                                       surface->wl_surface);
    if (!surface->xdg_surface) goto err;
    xdg_surface_add_listener(surface->xdg_surface, &xdg_surface_listener, surface->hwnd);

    /* seed the scale with the owner's */
    surface->window.scale = owner->window.scale;

    wayland_surface_coords_from_window(surface, config.x, config.y, &config.x, &config.y);
    wayland_surface_coords_from_window(surface, config.width, config.height,
                                       &config.width, &config.height);

    xdg_positioner = popup_create_positioner(surface, owner, &config);
    if (!xdg_positioner) goto err;

    surface->xdg_popup = xdg_surface_get_popup(surface->xdg_surface, owner->xdg_surface,
                                               xdg_positioner);
    xdg_positioner_destroy(xdg_positioner);
    if (!surface->xdg_popup) goto err;
    xdg_popup_add_listener(surface->xdg_popup, &xdg_popup_listener, surface->hwnd);

    if (wayland_is_menu_popup(surface->hwnd))
        grab_serial = popup_grab_serial_for_owner(owner);
    if (grab_serial)
    {
        pthread_mutex_lock(&process_wayland.seat.mutex);
        if (process_wayland.seat.wl_seat)
        {
            TRACE("grabbing popup hwnd=%p owner=%p serial=%u\n",
                  surface->hwnd, owner->hwnd, grab_serial);
            xdg_popup_grab(surface->xdg_popup, process_wayland.seat.wl_seat, grab_serial);
            surface->xdg_popup_grabbed = TRUE;
        }
        pthread_mutex_unlock(&process_wayland.seat.mutex);
    }

    wayland_surface_init_fractional_scale(surface, owner->window.fractional_scale);

    wayland_surface_sync_alpha(surface);

    surface->owner_hwnd = owner->hwnd;
    wl_surface_commit(surface->wl_surface);
    wl_display_flush(process_wayland.wl_display);

    return;
err:
    wayland_surface_clear_role(surface);
    ERR("Failed to assign popup role to wayland surface\n");
}

static struct wl_output *layer_surface_get_output(const RECT *rect, RECT *monitor_rect)
{
    struct wayland_output *output;
    struct wl_output *wl_output = NULL;
    MONITORINFO mi;
    HMONITOR monitor;

    if (monitor_rect) SetRectEmpty(monitor_rect);
    pthread_mutex_lock(&process_wayland.output_mutex);
    if ((output = wayland_output_for_rect(rect))) wl_output = output->wl_output;
    pthread_mutex_unlock(&process_wayland.output_mutex);

    mi.cbSize = sizeof(mi);
    if (wl_output && monitor_rect && (monitor = NtUserMonitorFromRect(rect, 0)) &&
        NtUserGetMonitorInfo(monitor, (MONITORINFO *)&mi))
        *monitor_rect = mi.rcMonitor;

    return wl_output;
}

static void wayland_surface_update_layer_config(struct wayland_surface *surface,
                                                const RECT *rect)
{
    RECT monitor_rect;
    int x = rect->left, y = rect->top;
    int width = rect->right - rect->left, height = rect->bottom - rect->top;
    uint32_t keyboard_interactivity = ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_NONE;

    layer_surface_get_output(rect, &monitor_rect);
    if (!IsRectEmpty(&monitor_rect))
    {
        x -= monitor_rect.left;
        y -= monitor_rect.top;
    }

    wayland_surface_coords_from_window(surface, x, y, &x, &y);
    wayland_surface_coords_from_window(surface, width, height, &width, &height);

    width = max(1, width);
    height = max(1, height);

    zwlr_layer_surface_v1_set_size(surface->zwlr_layer_surface_v1, width, height);
    zwlr_layer_surface_v1_set_anchor(surface->zwlr_layer_surface_v1,
                                     ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP |
                                     ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT);
    zwlr_layer_surface_v1_set_margin(surface->zwlr_layer_surface_v1, y, 0, 0, x);
    zwlr_layer_surface_v1_set_exclusive_zone(surface->zwlr_layer_surface_v1, -1);
    /* The tray menu needs keyboard focus for Esc and key navigation. */
    keyboard_interactivity = ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_EXCLUSIVE;
    zwlr_layer_surface_v1_set_keyboard_interactivity(surface->zwlr_layer_surface_v1,
                                                     keyboard_interactivity);
}

/**********************************************************************
 *          wayland_surface_make_layer
 *
 * Gives the layer-shell role to a plain wayland surface.
 */
void wayland_surface_make_layer(struct wayland_surface *surface, const RECT *rect)
{
    struct wl_output *output;

    TRACE("surface=%p rect=%s\n", surface, wine_dbgstr_rect(rect));

    assert(!surface->role || surface->role == WAYLAND_SURFACE_ROLE_LAYER);
    output = layer_surface_get_output(rect, NULL);

    if (surface->zwlr_layer_surface_v1)
    {
        wayland_surface_update_layer_config(surface, rect);
        wayland_set_layer_menu_hwnd(surface->hwnd);
        wl_surface_commit(surface->wl_surface);
        wl_display_flush(process_wayland.wl_display);
        return;
    }

    if (surface->role) wayland_surface_clear_role(surface);
    surface->role = WAYLAND_SURFACE_ROLE_LAYER;
    surface->layer_output = output;

    surface->zwlr_layer_surface_v1 =
        zwlr_layer_shell_v1_get_layer_surface(process_wayland.zwlr_layer_shell_v1,
                                              surface->wl_surface, output,
                                              ZWLR_LAYER_SHELL_V1_LAYER_TOP,
                                              "wine-menu");
    if (!surface->zwlr_layer_surface_v1) goto err;
    zwlr_layer_surface_v1_add_listener(surface->zwlr_layer_surface_v1,
                                       &zwlr_layer_surface_v1_listener,
                                       surface->hwnd);

    wayland_surface_update_layer_config(surface, rect);
    wayland_set_layer_menu_hwnd(surface->hwnd);
    wayland_surface_init_fractional_scale(surface, 1.0);
    wayland_surface_sync_alpha(surface);

    wl_surface_commit(surface->wl_surface);
    wl_display_flush(process_wayland.wl_display);

    return;

err:
    wayland_surface_clear_role(surface);
    ERR("Failed to assign layer role to wayland surface\n");
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

    /* Keep input state across role churn; it follows wl_surface enter/leave. */
    wayland_surface_clear_child_surfaces(surface);

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

    case WAYLAND_SURFACE_ROLE_POPUP:

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

        surface->owner_hwnd = NULL;
        surface->xdg_popup_grabbed = FALSE;
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

    case WAYLAND_SURFACE_ROLE_LAYER:
        wayland_clear_layer_menu_hwnd(surface->hwnd);
        if (surface->zwlr_layer_surface_v1)
        {
            zwlr_layer_surface_v1_destroy(surface->zwlr_layer_surface_v1);
            surface->zwlr_layer_surface_v1 = NULL;
        }

        surface->layer_output = NULL;
        break;

    case WAYLAND_SURFACE_ROLE_SUBSURFACE:
        if (surface->wl_subsurface)
        {
            wl_subsurface_destroy(surface->wl_subsurface);
            surface->wl_subsurface = NULL;
        }

        surface->toplevel_hwnd = 0;
        break;
    }

    memset(&surface->pending, 0, sizeof(surface->pending));
    memset(&surface->requested, 0, sizeof(surface->requested));
    memset(&surface->processing, 0, sizeof(surface->processing));
    memset(&surface->current, 0, sizeof(surface->current));
    memset(&surface->toplevel_size_limits, 0, sizeof(surface->toplevel_size_limits));
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

static BOOL wayland_surface_config_is_managed(const struct wayland_surface_config *config)
{
    return config->state & (WAYLAND_SURFACE_CONFIG_STATE_MAXIMIZED |
                            WAYLAND_SURFACE_CONFIG_STATE_FULLSCREEN |
                            WAYLAND_SURFACE_CONFIG_STATE_TILED);
}

BOOL wayland_surface_get_max_track_size(struct wayland_surface *surface, SIZE *size)
{
    const struct wayland_surface_config *config = &surface->current;
    int width, height;

    if (!surface->window.resizeable || !wayland_surface_is_toplevel(surface) ||
        !surface->xdg_toplevel)
        return FALSE;
    if (!config->serial || wayland_surface_config_is_managed(config) ||
        !wayland_surface_config_has_bounds(config))
        return FALSE;

    wayland_surface_coords_to_window(surface, config->bounds_width,
                                     config->bounds_height, &width, &height);
    if (width <= 0 || height <= 0) return FALSE;

    size->cx = width;
    size->cy = height;
    return TRUE;
}

static void wayland_surface_apply_toplevel_size_limits(struct wayland_surface *surface,
                                                       int width, int height)
{
    struct wayland_toplevel_size_limits limits = {0};

    if (!wayland_surface_is_toplevel(surface)) return;

    if (surface->window.resizeable)
    {
        if (!wayland_surface_config_is_managed(&surface->current) &&
            wayland_surface_config_has_bounds(&surface->current))
        {
            limits.max_width = surface->current.bounds_width;
            limits.max_height = surface->current.bounds_height;
        }
    }
    else
    {
        limits.min_width = limits.max_width = width;
        limits.min_height = limits.max_height = height;
    }

    if (surface->toplevel_size_limits.valid &&
        surface->toplevel_size_limits.min_width == limits.min_width &&
        surface->toplevel_size_limits.min_height == limits.min_height &&
        surface->toplevel_size_limits.max_width == limits.max_width &&
        surface->toplevel_size_limits.max_height == limits.max_height)
        return;

    xdg_toplevel_set_min_size(surface->xdg_toplevel, limits.min_width,
                              limits.min_height);
    xdg_toplevel_set_max_size(surface->xdg_toplevel, limits.max_width,
                              limits.max_height);

    limits.valid = TRUE;
    surface->toplevel_size_limits = limits;
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

    if (!IsRectEmpty(&rect))
    {
        int width = rect.right - rect.left, height = rect.bottom - rect.top;

        if (!EqualRect(&surface->geometry, &rect))
        {
            xdg_surface_set_window_geometry(surface->xdg_surface,
                                            rect.left, rect.top,
                                            width, height);
            surface->geometry = rect;
        }

        wayland_surface_apply_toplevel_size_limits(surface, width, height);
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
static BOOL wayland_surface_reconfigure_client(struct wayland_surface *surface,
                                               struct wayland_client_surface *client,
                                               const RECT *client_rect)
{
    struct wayland_window_config *window = &surface->window;
    int client_x, client_y, x, y;
    int client_width, client_height, width, height;
    RECT rect;

    /* The offset of the client area origin relatively to the window origin. */
    client_x = client_rect->left + window->client_rect.left - window->rect.left;
    client_y = client_rect->top + window->client_rect.top - window->rect.top;

    client_width = client_rect->right - client_rect->left;
    client_height = client_rect->bottom - client_rect->top;

    wayland_surface_coords_from_window(surface, client_x, client_y, &x, &y);
    wayland_surface_coords_from_window(surface, client_width, client_height,
                                       &width, &height);

    SetRect(&rect, 0, 0, width, height);
    OffsetRect(&rect, x, y);

    if (!EqualRect(&client->rect, &rect))
    {
        TRACE("hwnd=%p subsurface=%d,%d+%dx%d\n", surface->hwnd, x, y, width, height);

        client->rect = rect;

        if (client->wl_subsurface)
        {
            wl_subsurface_set_position(client->wl_subsurface, x, y);
            wl_subsurface_place_above(client->wl_subsurface, surface->wl_surface);
            /* keep any GDI child overlays stacked above the client surface */
            wayland_surface_raise_child_overlays(surface, wayland_surface_overlay_anchor(surface));
        }

        if (width > 0 && height > 0)
            wp_viewport_set_destination(client->wp_viewport, width, height);
        else /* We can't have a 0x0 destination, use 1x1 instead. */
            wp_viewport_set_destination(client->wp_viewport, 1, 1);

        return TRUE;
    }

    return FALSE;
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
    surface->parent = parent;
    surface->channel_fd = -1;
    wl_list_init(&surface->buffers);
    wl_list_init(&surface->link);

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
    surface->linked = TRUE;
    return surface;

err:
    if (surface->wl_subsurface) wl_subsurface_destroy(surface->wl_subsurface);
    if (surface->wp_viewport) wp_viewport_destroy(surface->wp_viewport);
    if (surface->wl_surface) wl_surface_destroy(surface->wl_surface);
    free(surface);
    return NULL;
}

static struct wayland_hwnd_dmabuf_surface *wayland_hwnd_dmabuf_surface_create_direct(
        struct wayland_surface *parent, HWND hwnd)
{
    struct wayland_hwnd_dmabuf_surface *surface;

    if (!(surface = calloc(1, sizeof(*surface)))) return NULL;
    surface->hwnd = hwnd;
    surface->parent = parent;
    surface->wl_surface = parent->wl_surface;
    surface->wp_viewport = parent->wp_viewport;
    surface->channel_fd = -1;
    surface->direct = TRUE;
    wl_list_init(&surface->buffers);
    wl_list_init(&surface->link);

    parent->direct_dmabuf_surface = surface;
    TRACE("hwnd=%p using direct parent dmabuf\n", hwnd);
    return surface;
}

static void wayland_hwnd_dmabuf_surface_set_opaque(struct wayland_hwnd_dmabuf_surface *surface,
                                                   int width, int height)
{
    struct wl_region *region = NULL;

    if (surface->current && surface->current->alpha_mode == HWND_DMABUF_ALPHA_MODE_IGNORE &&
        (region = wl_compositor_create_region(process_wayland.wl_compositor)))
    {
        wl_region_add(region, 0, 0, width, height);
        wl_surface_set_opaque_region(surface->wl_surface, region);
        wl_region_destroy(region);
    }
    else wl_surface_set_opaque_region(surface->wl_surface, NULL);
}

struct wayland_hwnd_dmabuf_geometry
{
    double source_x, source_y;
    double source_width, source_height;
    int x, y;
    int width, height;
};

static void wayland_surface_classify_child_visibility(struct wayland_surface *surface,
        const RECT *dst, struct wayland_child_visibility_info *info)
{
    HRGN dst_region, visible_region;
    RGNDATA *data;

    info->visibility = WAYLAND_CHILD_VISIBILITY_AS_IS;
    info->rect = *dst;
    info->rect_count = 0;
    if (!surface->child_region) return;

    info->visibility = WAYLAND_CHILD_VISIBILITY_UNMASKABLE;

    if (!(dst_region = NtGdiCreateRectRgn(dst->left, dst->top, dst->right, dst->bottom)))
        return;
    if (!(visible_region = NtGdiCreateRectRgn(0, 0, 0, 0)))
    {
        NtGdiDeleteObjectApp(dst_region);
        return;
    }

    if (NtGdiCombineRgn(visible_region, dst_region, surface->child_region, RGN_AND) != ERROR)
    {
        if (NtGdiEqualRgn(visible_region, dst_region))
        {
            info->visibility = WAYLAND_CHILD_VISIBILITY_AS_IS;
        }
        else if ((data = get_region_data(visible_region)))
        {
            info->rect = data->rdh.rcBound;
            info->rect_count = data->rdh.nCount;
            if (data->rdh.nCount == 1)
            {
                info->rect = *(RECT *)data->Buffer;
                info->visibility = WAYLAND_CHILD_VISIBILITY_CROPPED;
            }
            free(data);
        }
    }

    NtGdiDeleteObjectApp(visible_region);
    NtGdiDeleteObjectApp(dst_region);
}

static void wayland_surface_trace_child_visibility(struct wayland_surface *surface, HWND child,
        const RECT *dst, const struct wayland_child_visibility_info *info,
        struct wayland_visual_constraint_trace *cache)
{
    if (info->visibility == WAYLAND_CHILD_VISIBILITY_AS_IS)
    {
        cache->valid = FALSE;
        return;
    }

    if (cache->valid && cache->visibility == info->visibility &&
        EqualRect(&cache->dst, dst) && EqualRect(&cache->rect, &info->rect) &&
        cache->rect_count == info->rect_count)
        return;

    cache->valid = TRUE;
    cache->visibility = info->visibility;
    cache->dst = *dst;
    cache->rect = info->rect;
    cache->rect_count = info->rect_count;

    if (info->visibility == WAYLAND_CHILD_VISIBILITY_UNMASKABLE)
    {
        TRACE("hwnd=%p child=%p visual constraint %s dst=%s bounds=%s rects=%u\n",
              surface->hwnd, child, wayland_child_visibility_str(info->visibility),
              wine_dbgstr_rect(dst), wine_dbgstr_rect(&info->rect), info->rect_count);
        return;
    }

    TRACE("hwnd=%p child=%p visual constraint %s dst=%s visible=%s\n",
          surface->hwnd, child, wayland_child_visibility_str(info->visibility),
          wine_dbgstr_rect(dst), wine_dbgstr_rect(&info->rect));
}

static BOOL wayland_hwnd_dmabuf_surface_compute_geometry(struct wayland_surface *parent,
        struct wayland_hwnd_dmabuf_surface *surface, const hwnd_dmabuf_frame_info_t *info,
        struct wayland_hwnd_dmabuf_geometry *geometry)
{
    RECT rect = wine_server_get_rect(info->client), clipped, client;
    RECT dst;
    struct wayland_child_visibility_info visibility;
    int rect_width, rect_height;

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

    dst = clipped;
    OffsetRect(&dst, parent->window.client_rect.left - parent->window.rect.left,
               parent->window.client_rect.top - parent->window.rect.top);

    wayland_surface_classify_child_visibility(parent, &dst, &visibility);
    wayland_surface_trace_child_visibility(parent, surface->hwnd, &dst, &visibility,
                                           &surface->visual_constraint_trace);

    if (visibility.visibility == WAYLAND_CHILD_VISIBILITY_CROPPED)
        dst = visibility.rect;

    clipped = dst;
    OffsetRect(&clipped, parent->window.rect.left - parent->window.client_rect.left,
               parent->window.rect.top - parent->window.client_rect.top);

    geometry->source_x = (double)(clipped.left - rect.left) * surface->current->width / rect_width;
    geometry->source_y = (double)(clipped.top - rect.top) * surface->current->height / rect_height;
    geometry->source_width = (double)(clipped.right - clipped.left) * surface->current->width / rect_width;
    geometry->source_height = (double)(clipped.bottom - clipped.top) * surface->current->height / rect_height;

    wayland_surface_coords_from_window(parent, dst.left, dst.top,
                                       &geometry->x, &geometry->y);
    wayland_surface_coords_from_window(parent, dst.right - dst.left,
                                       dst.bottom - dst.top,
                                       &geometry->width, &geometry->height);
    geometry->width = max(1, geometry->width);
    geometry->height = max(1, geometry->height);

    return TRUE;
}

static void wayland_hwnd_dmabuf_surface_apply_geometry(struct wayland_hwnd_dmabuf_surface *surface,
                                                       const struct wayland_hwnd_dmabuf_geometry *geometry,
                                                       struct wl_surface *above)
{
    wl_subsurface_set_position(surface->wl_subsurface, geometry->x, geometry->y);
    wl_subsurface_place_above(surface->wl_subsurface, above);
    wp_viewport_set_source(surface->wp_viewport,
                           wl_fixed_from_double(geometry->source_x),
                           wl_fixed_from_double(geometry->source_y),
                           wl_fixed_from_double(geometry->source_width),
                           wl_fixed_from_double(geometry->source_height));
    wp_viewport_set_destination(surface->wp_viewport, geometry->width, geometry->height);
    wayland_hwnd_dmabuf_surface_set_opaque(surface, geometry->width, geometry->height);
}

static BOOL wayland_hwnd_dmabuf_surface_configure(struct wayland_surface *parent,
                                                  struct wayland_hwnd_dmabuf_surface *surface,
                                                  const hwnd_dmabuf_frame_info_t *info,
                                                  struct wl_surface *above)
{
    struct wayland_hwnd_dmabuf_geometry geometry;

    if (!wayland_hwnd_dmabuf_surface_compute_geometry(parent, surface, info, &geometry))
        return FALSE;

    wayland_hwnd_dmabuf_surface_apply_geometry(surface, &geometry, above);
    return TRUE;
}

/* Claim this child's consumer channel end once. The producer mints it lazily. Retry. */
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

/* Receive one frame. Returns 1 on success, 0 if empty, -1 on EOF. */
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
    unsigned int i, plane_count = desc->plane_count;

    if (plane_count < 1) plane_count = 1;
    if (plane_count > HWND_DMABUF_MAX_PLANES) plane_count = HWND_DMABUF_MAX_PLANES;

    if (!(params = zwp_linux_dmabuf_v1_create_params(process_wayland.zwp_linux_dmabuf_v1)))
        return NULL;
    /* all planes share the one exported memory fd */
    for (i = 0; i < plane_count; i++)
        zwp_linux_buffer_params_v1_add(params, fd, i, desc->plane_offsets[i], desc->plane_strides[i],
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
        WARN("hwnd=%p failed to create dmabuf buffer size=%ux%u fourcc=%#x "
             "modifier=0x%08x%08x stride=%u alpha=%u\n",
             surface->hwnd, desc->width, desc->height, desc->fourcc,
             (unsigned int)(desc->modifier >> 32), (unsigned int)desc->modifier,
             desc->stride, desc->alpha_mode);
        free(buffer);
        return NULL;
    }
    buffer->ref = 1;  /* owner ref. A compositor ref is added per attach in the update loop */
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

/* Make buffer the surface's current frame. Publishing is separate so direct
 * parent mode can validate the frame before it attaches anything. */
static void wayland_hwnd_dmabuf_set_frame(struct wayland_hwnd_dmabuf_surface *surface,
        struct wayland_hwnd_dmabuf_buffer *buffer, const hwnd_dmabuf_frame_desc_t *desc)
{
    if (wayland_hwnd_dmabuf_buffer_exchange_release_token(buffer, desc->release_token))
        WARN("hwnd=%p slot=%u overwrote an unreleased token\n", surface->hwnd, desc->image_id);
    buffer->alpha_mode = desc->alpha_mode;
    buffer->release_flags = HWND_DMABUF_RELEASE_ORPHANED;
    surface->current = buffer;
    surface->frame_seq = desc->frame_seq;
    surface->current_committed = FALSE;
}

static void wayland_hwnd_dmabuf_attach_current(struct wayland_hwnd_dmabuf_surface *surface)
{
    struct wayland_hwnd_dmabuf_buffer *buffer = surface->current;

    if (!buffer) return;
    if (!surface->logged_first_attach)
    {
        TRACE("hwnd=%p attaching dmabuf slot=%u size=%dx%d fourcc=%#x alpha=%u\n",
              surface->hwnd, buffer->image_id, buffer->width, buffer->height,
              buffer->fourcc, buffer->alpha_mode);
        surface->logged_first_attach = TRUE;
    }
    buffer->release_flags = HWND_DMABUF_RELEASE_PRESENTED;
    wl_surface_attach(surface->wl_surface, buffer->wl_buffer, 0, 0);
    wl_surface_damage_buffer(surface->wl_surface, 0, 0, buffer->width, buffer->height);
    wp_viewport_set_source(surface->wp_viewport, 0, 0,
                           wl_fixed_from_int(buffer->width), wl_fixed_from_int(buffer->height));
}

static void wayland_hwnd_dmabuf_drop_current_frame(struct wayland_hwnd_dmabuf_surface *surface,
                                                   unsigned int flags)
{
    struct wayland_hwnd_dmabuf_buffer *buffer = surface->current;

    if (!buffer) return;
    wayland_hwnd_dmabuf_buffer_send_release(buffer, flags, buffer->stable_slot);
    surface->current = NULL;
    surface->current_committed = FALSE;
    if (!buffer->stable_slot) wayland_hwnd_dmabuf_buffer_reap(buffer);
}

/* Stable-slot path (HWND_DMABUF_FLAG_STABLE_SLOT): ensure this slot is imported and cached
 * (no attach). The producer sends a slot's dmabuf fd only once, then fd-less references:
 *  - cache hit (exact layout match): reuse the cached wl_buffer.
 *  - fd-bearing miss: import and cache it.
 *  - fd-less miss: the slot is not cached -> return NULL. The failed release clears the
 *    producer's cache state so it resends the fd.
 * Stale slots (new producer/ring generation or a changed layout) are reaped first. */
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
        return buffer;
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
 * later reference resolves. Then release the present so the producer can recycle the slot.
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

static BOOL wayland_hwnd_dmabuf_desc_is_valid(const hwnd_dmabuf_frame_desc_t *desc)
{
    return desc->version == HWND_DMABUF_DESC_VERSION_V1 && desc->width && desc->height &&
           desc->stride && desc->fourcc && desc->release_token;
}

/* Drain the channel, importing every fd-bearing slot so none is lost, then show the newest
 * frame. Stable-slot producers send a slot's fd once and fd-less references after. The fd
 * is dup'd + passed via SCM_RIGHTS once per slot, not once per frame. created_buffer reports
 * a freshly imported wl_buffer. attached_frame reports a frame was published. */
static struct wayland_hwnd_dmabuf_buffer *wayland_hwnd_dmabuf_surface_import_buffer(
        struct wayland_hwnd_dmabuf_surface *surface, const hwnd_dmabuf_frame_info_t *info,
        BOOL *created_buffer, BOOL *attached_frame)
{
    struct wayland_hwnd_dmabuf_buffer *buffer = NULL;
    hwnd_dmabuf_frame_desc_t pdesc, desc;
    BOOL have_pending = FALSE;
    BOOL retried = FALSE;
    int pfd = -1, fd, r;

    *created_buffer = FALSE;
    *attached_frame = FALSE;
    memset(&pdesc, 0, sizeof(pdesc));

retry:
    wayland_hwnd_dmabuf_surface_claim_channel(surface);
    if (surface->channel_fd < 0) return surface->current;

    while ((r = wayland_hwnd_dmabuf_channel_recv_one(surface->channel_fd, &desc, &fd)) > 0)
    {
        BOOL desc_valid = wayland_hwnd_dmabuf_desc_is_valid(&desc);
        BOOL format_supported = desc_valid &&
                wayland_dmabuf_format_supported(desc.fourcc, desc.modifier);

        if (!desc_valid || !format_supported)
        {
            WARN("hwnd=%p import rejected info_seq=%u desc_seq=%u version=%u "
                 "size=%ux%u stride=%u offset=%u fourcc=%#x (%c%c%c%c) "
                 "modifier=0x%08x%08x token=0x%08x%08x supported=%u\n",
                 surface->hwnd, info->frame_seq, desc.frame_seq, desc.version,
                 desc.width, desc.height, desc.stride, desc.offset, desc.fourcc,
                 desc.fourcc & 0xff, (desc.fourcc >> 8) & 0xff,
                 (desc.fourcc >> 16) & 0xff, (desc.fourcc >> 24) & 0xff,
                 (unsigned int)(desc.modifier >> 32), (unsigned int)desc.modifier,
                 (unsigned int)(desc.release_token >> 32),
                 (unsigned int)desc.release_token, format_supported);
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
    if (r < 0)
    {
        close(surface->channel_fd);
        surface->channel_fd = -1;
        if (!have_pending && !retried)
        {
            retried = TRUE;
            goto retry;
        }
    }

    if (!have_pending) return surface->current;

    /* Show the newest drained frame. */
    if (pdesc.flags & HWND_DMABUF_FLAG_STABLE_SLOT)
        buffer = wayland_hwnd_dmabuf_cache_slot(surface, &pdesc, pfd, created_buffer);
    else if ((buffer = wayland_hwnd_dmabuf_create_buffer(surface, &pdesc, pfd, FALSE)))
    {
        *created_buffer = TRUE;
        if (!surface->logged_first_import)
        {
            TRACE("hwnd=%p imported dmabuf seq=%u slot=%u size=%ux%u "
                  "fourcc=%#x modifier=0x%08x%08x alpha=%u flags=%#x\n",
                  surface->hwnd, pdesc.frame_seq, pdesc.image_id,
                  pdesc.width, pdesc.height, pdesc.fourcc,
                  (unsigned int)(pdesc.modifier >> 32),
                  (unsigned int)pdesc.modifier, pdesc.alpha_mode, pdesc.flags);
            surface->logged_first_import = TRUE;
        }
    }

    if (pfd >= 0) close(pfd);

    if (!buffer)
    {
        wayland_hwnd_dmabuf_send_release(surface, pdesc.producer_unique_id, pdesc.release_token,
                                         HWND_DMABUF_RELEASE_FAILED,
                                         pdesc.image_id, pdesc.ring_generation);
        return surface->current;
    }
    wayland_hwnd_dmabuf_set_frame(surface, buffer, &pdesc);
    *attached_frame = TRUE;
    return buffer;
}

static BOOL wayland_surface_client_fills_window(struct wayland_surface *surface)
{
    return EqualRect(&surface->window.rect, &surface->window.client_rect);
}

static BOOL wayland_hwnd_dmabuf_frame_covers_client(struct wayland_surface *surface,
                                                    const hwnd_dmabuf_frame_info_t *info)
{
    RECT rect = wine_server_get_rect(info->client);
    int width = surface->window.client_rect.right - surface->window.client_rect.left;
    int height = surface->window.client_rect.bottom - surface->window.client_rect.top;

    return width > 0 && height > 0 &&
           rect.left <= 0 && rect.top <= 0 &&
           rect.right >= width && rect.bottom >= height;
}

static BOOL wayland_surface_has_region_constraints(struct wayland_surface *surface)
{
    return surface->shaped || surface->occlusion_clipped;
}

static BOOL wayland_surface_direct_dmabuf_candidate(struct wayland_surface *surface,
                                                    struct wayland_win_data *data,
                                                    const hwnd_dmabuf_frame_info_t *frames,
                                                    unsigned int count)
{
    if (count != 1) return FALSE;
    if (!wayland_surface_is_toplevel(surface)) return FALSE;
    if (!data || data->client_surface) return FALSE;
    if (data->window_contents) return FALSE;
    if (!wl_list_empty(&surface->child_overlays)) return FALSE;
    if (wayland_surface_has_region_constraints(surface))
    {
        TRACE("hwnd=%p direct parent dmabuf rejected for visual constraints\n", surface->hwnd);
        return FALSE;
    }
    if (!wayland_surface_client_fills_window(surface)) return FALSE;
    return wayland_hwnd_dmabuf_frame_covers_client(surface, &frames[0]);
}

static BOOL wayland_surface_replace_direct_dmabuf_with_shm(struct wayland_surface *surface,
                                                           struct wayland_win_data *data)
{
    struct wayland_hwnd_dmabuf_surface *direct = surface->direct_dmabuf_surface;

    if (!direct || !data || !data->window_contents || !wayland_surface_reconfigure(surface))
        return FALSE;

    wl_surface_set_opaque_region(surface->wl_surface, NULL);
    wayland_surface_attach_shm(surface, data->window_contents,
                               data->window_contents->damage_region);
    wl_surface_commit(surface->wl_surface);
    wayland_hwnd_dmabuf_surface_destroy(direct);
    return TRUE;
}

static void transparent_carrier_buffer_release(void *data, struct wl_buffer *buffer)
{
    struct wayland_shm_buffer *shm_buffer = data;

    TRACE("shm_buffer=%p\n", shm_buffer);
    shm_buffer->busy = FALSE;
    wayland_shm_buffer_unref(shm_buffer);
}

static const struct wl_buffer_listener transparent_carrier_buffer_listener =
{
    transparent_carrier_buffer_release
};

static BOOL wayland_surface_replace_direct_dmabuf_with_transparent_shm(struct wayland_surface *surface)
{
    struct wayland_hwnd_dmabuf_surface *direct = surface->direct_dmabuf_surface;
    struct wayland_shm_buffer *shm_buffer;
    int width, height;

    if (!direct || !wayland_surface_reconfigure(surface)) return FALSE;

    /* Keep a pure-dmabuf toplevel mapped when it leaves direct mode before any
     * real GDI buffer exists; do not leave the stale producer frame as base. */
    width = max(1, surface->window.rect.right - surface->window.rect.left);
    height = max(1, surface->window.rect.bottom - surface->window.rect.top);
    if (!(shm_buffer = wayland_shm_buffer_create(width, height, WL_SHM_FORMAT_ARGB8888)))
        return FALSE;

    memset(shm_buffer->map_data, 0, shm_buffer->map_size);
    wl_buffer_add_listener(shm_buffer->wl_buffer, &transparent_carrier_buffer_listener, shm_buffer);
    wl_surface_set_opaque_region(surface->wl_surface, NULL);
    wayland_surface_attach_shm(surface, shm_buffer, shm_buffer->damage_region);
    wl_surface_commit(surface->wl_surface);
    wayland_shm_buffer_unref(shm_buffer);
    wayland_hwnd_dmabuf_surface_destroy(direct);
    return TRUE;
}

static void wayland_surface_clear_direct_dmabuf(struct wayland_surface *surface,
                                                struct wayland_win_data *data)
{
    if (surface->direct_dmabuf_surface)
    {
        struct wayland_hwnd_dmabuf_surface *direct = surface->direct_dmabuf_surface;

        if (wayland_surface_replace_direct_dmabuf_with_shm(surface, data))
            return;
        if (data && direct->current_committed &&
            wayland_surface_replace_direct_dmabuf_with_transparent_shm(surface))
            return;

        wl_surface_set_opaque_region(surface->wl_surface, NULL);
        wayland_hwnd_dmabuf_surface_destroy(direct);
    }
}

void wayland_surface_prepare_direct_dmabuf_shm_commit(struct wayland_surface *surface)
{
    if (surface->direct_dmabuf_surface)
        wl_surface_set_opaque_region(surface->wl_surface, NULL);
}

void wayland_surface_finish_direct_dmabuf_shm_commit(struct wayland_surface *surface)
{
    if (surface->direct_dmabuf_surface)
        wayland_hwnd_dmabuf_surface_destroy(surface->direct_dmabuf_surface);
}

static void wayland_surface_clear_dmabuf_children(struct wayland_surface *surface)
{
    struct wayland_hwnd_dmabuf_surface *dmabuf_surface, *next;

    wl_list_for_each_safe(dmabuf_surface, next, &surface->hwnd_dmabuf_surfaces, link)
        wayland_hwnd_dmabuf_surface_destroy(dmabuf_surface);
    surface->dmabuf_top = NULL;
}

static BOOL wayland_surface_update_direct_dmabuf(struct wayland_surface *surface,
                                                 struct wayland_win_data *data,
                                                 const hwnd_dmabuf_frame_info_t *frames,
                                                 unsigned int count,
                                                 unsigned long long now)
{
    struct wayland_hwnd_dmabuf_surface *direct;
    struct wayland_hwnd_dmabuf_buffer *buffer, *buffer_next;
    BOOL created_buffer, attached_frame = FALSE;
    int width, height;

    if (!wayland_surface_direct_dmabuf_candidate(surface, data, frames, count))
    {
        wayland_surface_clear_direct_dmabuf(surface, data);
        return FALSE;
    }

    if (!frames[0].opened)
    {
        if ((direct = surface->direct_dmabuf_surface) && direct->current)
        {
            direct->seen = TRUE;
            direct->last_seen_ms = now;
            goto commit_current;
        }
        wayland_surface_clear_direct_dmabuf(surface, data);
        return FALSE;
    }

    wayland_surface_clear_dmabuf_children(surface);

    if (!(direct = surface->direct_dmabuf_surface) &&
        !(direct = wayland_hwnd_dmabuf_surface_create_direct(surface, (HWND)(UINT_PTR)frames[0].hwnd)))
        return FALSE;

    direct->seen = TRUE;
    direct->last_seen_ms = now;

    wl_list_for_each_safe(buffer, buffer_next, &direct->buffers, link)
        if (buffer->released)
            wayland_hwnd_dmabuf_buffer_reap(buffer);

    wayland_hwnd_dmabuf_surface_import_buffer(direct, &frames[0], &created_buffer, &attached_frame);
    (void)created_buffer;
    assert(!attached_frame || !direct->current_committed);
    if (!direct->current) return TRUE;

commit_current:
    if (direct->current->alpha_mode != HWND_DMABUF_ALPHA_MODE_IGNORE)
    {
        wayland_hwnd_dmabuf_drop_current_frame(direct, HWND_DMABUF_RELEASE_DROPPED);
        wayland_surface_clear_direct_dmabuf(surface, data);
        return FALSE;
    }

    if (!wayland_surface_reconfigure(surface))
    {
        if (!direct->current_committed)
        {
            wayland_surface_clear_direct_dmabuf(surface, data);
            return FALSE;
        }
        return TRUE;
    }

    wayland_surface_coords_from_window(surface,
                                       surface->window.rect.right - surface->window.rect.left,
                                       surface->window.rect.bottom - surface->window.rect.top,
                                       &width, &height);
    width = max(1, width);
    height = max(1, height);

    if (!attached_frame && direct->current_committed &&
        direct->committed_width == width && direct->committed_height == height)
        return TRUE;

    if (!direct->current_committed)
        wayland_hwnd_dmabuf_attach_current(direct);
    wayland_hwnd_dmabuf_surface_set_opaque(direct, width, height);
    wl_surface_commit(surface->wl_surface);
    if (!direct->current_committed)
    {
        InterlockedIncrement(&direct->current->ref);
        direct->current_committed = TRUE;
    }
    direct->committed_width = width;
    direct->committed_height = height;
    surface->content_width = surface->window.rect.right - surface->window.rect.left;
    surface->content_height = surface->window.rect.bottom - surface->window.rect.top;
    return TRUE;
}

BOOL wayland_surface_has_hwnd_dmabuf_content(struct wayland_surface *surface)
{
    struct wayland_hwnd_dmabuf_surface *dmabuf_surface;

    if (surface->direct_dmabuf_surface && surface->direct_dmabuf_surface->current_committed)
        return TRUE;
    wl_list_for_each(dmabuf_surface, &surface->hwnd_dmabuf_surfaces, link)
        if (dmabuf_surface->current_committed)
            return TRUE;
    return FALSE;
}

static BOOL wayland_surface_try_direct_dmabuf(HWND hwnd)
{
    hwnd_dmabuf_frame_info_t stack_frames[16], *frames = stack_frames;
    unsigned int total = 0, count = 0;
    enum hwnd_dmabuf_status status;
    struct wayland_win_data *data;
    BOOL had_direct = FALSE, may_try = FALSE, ret = FALSE;

    if (!process_wayland.zwp_linux_dmabuf_v1) return FALSE;

    if ((data = wayland_win_data_get(hwnd)))
    {
        struct wayland_surface *surface = data->wayland_surface;

        had_direct = surface && surface->direct_dmabuf_surface;
        may_try = surface && (had_direct ||
                              (wayland_surface_is_toplevel(surface) &&
                               !data->window_contents &&
                               !data->client_surface &&
                               wl_list_empty(&surface->child_overlays) &&
                               wayland_surface_client_fills_window(surface)));
        wayland_win_data_release(data);
    }
    if (!may_try) return FALSE;

    if (had_direct)
    {
        if ((data = wayland_win_data_get(hwnd)))
        {
            if (data->wayland_surface)
            {
                wayland_surface_update_hwnd_dmabufs(data->wayland_surface);
                ret = wayland_surface_has_hwnd_dmabuf_content(data->wayland_surface);
            }
            wayland_win_data_release(data);
        }
        if (ret) wl_display_flush(process_wayland.wl_display);
        return ret;
    }

    status = wine_hwnd_dmabuf_list(hwnd, frames, ARRAY_SIZE(stack_frames), &total, &count);
    if (status != HWND_DMABUF_OK) return FALSE;
    if (total > count)
    {
        if (!(frames = calloc(total, sizeof(*frames)))) return FALSE;
        status = wine_hwnd_dmabuf_list(hwnd, frames, total, &total, &count);
        if (status != HWND_DMABUF_OK) count = 0;
    }

    if ((data = wayland_win_data_get(hwnd)))
    {
        if (data->wayland_surface)
            ret = wayland_surface_update_direct_dmabuf(data->wayland_surface, data, frames, count,
                                                       wayland_time_ms());
        wayland_win_data_release(data);
    }
    if (frames != stack_frames) free(frames);
    if (ret) wl_display_flush(process_wayland.wl_display);
    return ret;
}

static void wayland_surface_update_hwnd_dmabufs(struct wayland_surface *surface)
{
    hwnd_dmabuf_frame_info_t stack_frames[16], *frames = stack_frames;
    unsigned int total = 0, count = 0, i;
    struct wayland_hwnd_dmabuf_buffer *buffer, *buffer_next;
    struct wayland_hwnd_dmabuf_surface *dmabuf_surface, *next;
    enum hwnd_dmabuf_status status;
    struct wayland_win_data *data;
    struct wl_surface *above;
    unsigned long long now = wayland_time_ms();
    BOOL any_new = FALSE;

    if (!process_wayland.zwp_linux_dmabuf_v1) return;
    /* dmabuf children are composited for primary surfaces. */
    if (!wayland_surface_is_toplevel(surface) && !wayland_surface_is_popup(surface) &&
        !wayland_surface_is_layer(surface))
        return;

    /* Import is driven by producer wakes (WM_WAYLAND_DMABUF_FRAME) at the producer's
     * self-paced rate, regardless of whether our toplevel is presented. We do not pace
     * to a frame callback on our surface: the compositor stops firing it when the surface
     * is not in front, stalling delivery while another window (e.g. a game) is active. */

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

    data = wayland_win_data_get_nolock(surface->hwnd);
    if (wayland_surface_update_direct_dmabuf(surface, data, frames, count, now))
    {
        if (frames != stack_frames) free(frames);
        return;
    }

    /* Anchor the dmabuf chain above the client subsurface, not the main surface.
     * Both used to place_above() main. Whichever committed last won the top,
     * a per-frame z-order race (the flicker). A fixed sibling anchor makes
     * [main, client, dmabuf...] resolve the same regardless. (win_data_mutex held.) */
    above = surface->wl_surface;
    if (data &&
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

        if (!frames[i].opened)
        {
            if ((dmabuf_surface = wayland_hwnd_dmabuf_surface_get(surface, hwnd)))
            {
                dmabuf_surface->seen = TRUE;
                dmabuf_surface->last_seen_ms = now;
                if (dmabuf_surface->current &&
                    wayland_hwnd_dmabuf_surface_configure(surface, dmabuf_surface, &frames[i], above))
                {
                    wl_surface_commit(dmabuf_surface->wl_surface);
                    above = dmabuf_surface->wl_surface;
                    any_new = TRUE;
                }
            }
            continue;
        }

        if (!(dmabuf_surface = wayland_hwnd_dmabuf_surface_get(surface, hwnd)) &&
            !(dmabuf_surface = wayland_hwnd_dmabuf_surface_create(surface, hwnd)))
            continue;

        dmabuf_surface->seen = TRUE;
        dmabuf_surface->last_seen_ms = now;
        /* Reap uncached buffers the compositor has released. Cached stable-slot
         * buffers set no released flag and stay cached for reuse. */
        wl_list_for_each_safe(buffer, buffer_next, &dmabuf_surface->buffers, link)
            if (buffer->released)
                wayland_hwnd_dmabuf_buffer_reap(buffer);

        wayland_hwnd_dmabuf_surface_import_buffer(dmabuf_surface, &frames[i],
                                                  &created_buffer, &attached_frame);
        assert(!attached_frame || !dmabuf_surface->current_committed);
        if (attached_frame) any_new = TRUE;
        if (!dmabuf_surface->current) continue;
        if (attached_frame) wayland_hwnd_dmabuf_attach_current(dmabuf_surface);
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
        /* One compositor ref per attached frame. The wl_buffer.release handler drops
         * it. Cached slots re-attach the same wl_buffer each new frame, hence the ref
         * is per-attach. The busy gate keeps at most one attach of a slot outstanding. */
        if (attached_frame)
        {
            InterlockedIncrement(&dmabuf_surface->current->ref);
            dmabuf_surface->current_committed = TRUE;
        }
        above = dmabuf_surface->wl_surface;
    }

    /* record the chain top for the GDI overlay anchor, see wayland_surface_overlay_anchor */
    surface->dmabuf_top = above;

    /* A child absent from the descendant list: hide its stale overlay at once so the
     * content behind it (e.g. the client surface) is not occluded. Keep the dmabuf
     * cache for a grace window so a brief gap does not force a re-import. Reap the
     * cache only once the grace expires. */
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

    /* Commit and arm the next vsync only when something changed. An idle window costs nothing. */
    /* Commit the parent to apply subsurface stacking. No frame callback: import is
     * paced by the producer, not by our toplevel being presented. */
    if (any_new)
        wl_surface_commit(surface->wl_surface);
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
        /* if a decoration change occured during the initial configure, avoid
         * a double ack as that would cause a protocol error */
        if (surface->processing.serial != surface->current.serial)
            xdg_surface_ack_configure(surface->xdg_surface, surface->processing.serial);
        surface->current = surface->processing;
        memset(&surface->processing, 0, sizeof(surface->processing));
    }
    /* If this is the initial configure, and we have a compatible requested
     * config, use that, in order to draw windows that don't go through the
     * message loop (e.g., some splash screens). */
    else if (!surface->current.serial && surface->requested.serial &&
             wayland_surface_config_is_compatible(&surface->requested,
                                                  width, height,
                                                  window->state))
    {
        if (surface->requested.serial != surface->current.serial)
            xdg_surface_ack_configure(surface->xdg_surface, surface->requested.serial);
        surface->current = surface->requested;
        memset(&surface->requested, 0, sizeof(surface->requested));
        /* decoration changes must go through the message loop */
        surface->requested.decor = surface->current.decor;
        surface->current.decor = 0;
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

static BOOL wayland_surface_reconfigure_layer(struct wayland_surface *surface)
{
    if (surface->processing.serial && surface->processing.processed)
    {
        if (surface->processing.serial != surface->current.serial)
            zwlr_layer_surface_v1_ack_configure(surface->zwlr_layer_surface_v1,
                                                surface->processing.serial);
        surface->current = surface->processing;
        memset(&surface->processing, 0, sizeof(surface->processing));
    }
    else if (!surface->current.serial)
    {
        return FALSE;
    }

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
    case WAYLAND_SURFACE_ROLE_POPUP:
        if (!surface->xdg_surface) break; /* surface role has been cleared */
        /* Popups share the xdg_surface ack path. The buffer waits for the first ack. */
        if (!wayland_surface_reconfigure_xdg(surface, width, height)) return FALSE;
        break;
    case WAYLAND_SURFACE_ROLE_LAYER:
        if (!surface->zwlr_layer_surface_v1) break; /* surface role has been cleared */
        if (!wayland_surface_reconfigure_layer(surface)) return FALSE;
        break;
    case WAYLAND_SURFACE_ROLE_SUBSURFACE:
        if (!surface->wl_subsurface) break; /* surface role has been cleared */
        wayland_surface_reconfigure_subsurface(surface);
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

    TRACE("%s\n", debugstr_client_surface(client));

    if ((data = wayland_win_data_get(client->hwnd)))
    {
        if (data->client_surface == surface) data->client_surface = NULL;
        wayland_client_surface_attach(surface, NULL);
        /* the occluder is gone, GDI children show through the base again */
        if (data->wayland_surface && !wl_list_empty(&data->wayland_surface->child_overlays))
        {
            wayland_surface_clear_child_overlays(data->wayland_surface);
            wl_surface_commit(data->wayland_surface->wl_surface);
        }
        wayland_win_data_release(data);
    }
}

static void wayland_client_surface_update(struct client_surface *client)
{
    struct wayland_client_surface *surface = impl_from_client_surface(client);

    TRACE("%s\n", debugstr_client_surface(client));

    set_client_surface(client->hwnd, surface);
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

    /* A dmabuf producer presents through the hwnd-dmabuf path, not this
     * client subsurface. Still mark it presented for lifecycle state. */
    if (wayland_client_surface_is_hwnd_dmabuf_producer(surface))
    {
        surface->has_presented = TRUE;
        return;
    }

    set_client_surface(hwnd, surface);
    ensure_window_surface_contents(toplevel);
    surface->has_presented = TRUE;
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

static BOOL wayland_surface_has_live_role(struct wayland_surface *surface)
{
    switch (surface->role)
    {
    case WAYLAND_SURFACE_ROLE_TOPLEVEL:
        return surface->xdg_surface != NULL && surface->xdg_toplevel != NULL;
    case WAYLAND_SURFACE_ROLE_POPUP:
        return surface->xdg_surface != NULL && surface->xdg_popup != NULL;
    case WAYLAND_SURFACE_ROLE_LAYER:
        return surface->zwlr_layer_surface_v1 != NULL;
    case WAYLAND_SURFACE_ROLE_SUBSURFACE:
        return surface->wl_subsurface != NULL;
    case WAYLAND_SURFACE_ROLE_NONE:
        return FALSE;
    }

    return FALSE;
}

void wayland_client_surface_attach(struct wayland_client_surface *client, HWND toplevel)
{
    struct wayland_win_data *toplevel_data;
    struct wayland_surface *surface;
    HWND hwnd = client->client.hwnd;
    RECT client_rect, dst;
    struct wayland_child_visibility_info visibility;

    if (!toplevel)
    {
        if (client->wl_subsurface)
        {
            wl_subsurface_destroy(client->wl_subsurface);
            client->wl_subsurface = NULL;
            client->toplevel_wl_surface = NULL;
            TRACE("Detached %s\n", debugstr_client_surface(&client->client));
        }

        client->toplevel = 0;
        return;
    }

    if (!(toplevel_data = wayland_win_data_get_nolock(toplevel)) ||
        !(surface = toplevel_data->wayland_surface) ||
        !wayland_surface_has_live_role(surface))
    {
        wayland_client_surface_attach(client, NULL);
        return;
    }

    /* if the toplevel's role changes (e.g becomes unmanaged popup/subsurface)
     * we are left with an invalid parent, so we need to reparent */
    if (client->toplevel != toplevel || client->toplevel_wl_surface != surface->wl_surface)
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
        client->toplevel_wl_surface = surface->wl_surface;
        SetRect(&client->rect, 0, 0, -1, -1);

        TRACE("Created subsurface for toplevel=%p\n", toplevel);
    }

    if (hwnd == toplevel)
    {
        /* Keep own-toplevel client surfaces in the same geometry generation as
         * the parent surface during xdg configure / rawpos transitions. */
        SetRect(&client_rect, 0, 0,
                surface->window.client_rect.right - surface->window.client_rect.left,
                surface->window.client_rect.bottom - surface->window.client_rect.top);
    }
    else
    {
        NtUserGetClientRect(hwnd, &client_rect, NtUserGetWinMonitorDpi(hwnd, MDT_RAW_DPI));
        NtUserMapWindowPoints(hwnd, toplevel, (POINT *)&client_rect, 2, NtUserGetWinMonitorDpi(hwnd, MDT_RAW_DPI));
    }

    dst = client_rect;
    OffsetRect(&dst, surface->window.client_rect.left - surface->window.rect.left,
               surface->window.client_rect.top - surface->window.rect.top);
    wayland_surface_classify_child_visibility(surface, &dst, &visibility);
    wayland_surface_trace_child_visibility(surface, hwnd, &dst, &visibility,
                                           &client->visual_constraint_trace);

    if (wayland_surface_reconfigure_client(surface, client, &client_rect))
    {
        /* Commit to apply subsurface positioning. */
        wl_surface_commit(surface->wl_surface);
    }
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

/**********************************************************************
 *          wayland_surface_ensure_contents
 *
 * Import any direct or child dmabuf content. Parent SHM contents normally come
 * from the real window surface expose path; a transparent carrier is used only
 * to replace an already-mapped direct frame when child compositing takes over.
 */
void wayland_surface_ensure_contents(struct wayland_surface *surface,
                                     struct wayland_client_surface *client)
{
    (void)client;
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
