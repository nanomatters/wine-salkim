/*
 * Wayland window handling
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

#include "ntstatus.h"
#define WIN32_NO_STATUS

#include "waylanddrv.h"

#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(waylanddrv);


static int wayland_win_data_cmp_rb(const void *key,
                                   const struct rb_entry *entry)
{
    HWND key_hwnd = (HWND)key; /* cast to work around const */
    const struct wayland_win_data *entry_win_data =
        RB_ENTRY_VALUE(entry, const struct wayland_win_data, entry);

    if (key_hwnd < entry_win_data->hwnd) return -1;
    if (key_hwnd > entry_win_data->hwnd) return 1;
    return 0;
}

static pthread_mutex_t win_data_mutex = PTHREAD_MUTEX_INITIALIZER;
static struct rb_tree win_data_rb = { wayland_win_data_cmp_rb };
static BOOL window_surface_configure_blocks_dmabuf(HWND hwnd);
static BOOL window_surface_has_requested_configure(HWND hwnd);
static BOOL window_surface_has_hwnd_dmabuf_content(HWND hwnd);

static const WCHAR frameless_window_prop[] =
    {'_','_','w','i','n','e','_','w','i','n','3','2','u','_','f','r','a','m','e','l','e','s','s',0};
static const WCHAR layer_menu_hwnd_prop[] =
    {'_','_','w','i','n','e','_','w','a','y','l','a','n','d','_','l','a','y','e','r','_','m','e','n','u',0};
static const WCHAR layer_menu_restore_hwnd_prop[] =
    {'_','_','w','i','n','e','_','w','a','y','l','a','n','d','_','l','a','y','e','r','_','m','e','n','u','_','r','e','s','t','o','r','e',0};

/***********************************************************************
 *           wayland_win_data_create
 *
 * Create a data window structure for an existing window.
 */
static struct wayland_win_data *wayland_win_data_create(HWND hwnd, const struct window_rects *rects)
{
    struct wayland_win_data *data;
    struct rb_entry *rb_entry;
    HWND parent;

    /* Don't create win data for desktop or HWND_MESSAGE windows. */
    if (!(parent = NtUserGetAncestor(hwnd, GA_PARENT))) return NULL;
    if (parent != NtUserGetDesktopWindow() && !NtUserGetAncestor(parent, GA_PARENT))
        return NULL;

    if (!(data = calloc(1, sizeof(*data)))) return NULL;

    data->hwnd = hwnd;
    data->rects = *rects;
    data->ime_enabled = FALSE;
    data->num_ime_children = 0;
    data->alpha_multiplier = UINT32_MAX;

    pthread_mutex_lock(&win_data_mutex);

    /* Check that another thread hasn't already created the wayland_win_data. */
    if ((rb_entry = rb_get(&win_data_rb, hwnd)))
    {
        free(data);
        return RB_ENTRY_VALUE(rb_entry, struct wayland_win_data, entry);
    }

    rb_put(&win_data_rb, hwnd, &data->entry);

    TRACE("hwnd=%p\n", data->hwnd);

    return data;
}

/***********************************************************************
 *           wayland_win_data_destroy
 */
static void wayland_win_data_destroy(struct wayland_win_data *data)
{
    TRACE("hwnd=%p\n", data->hwnd);

    rb_remove(&win_data_rb, &data->entry);

    pthread_mutex_unlock(&win_data_mutex);

    if (data->stashed_client) client_surface_release(&data->stashed_client->client);
    if (data->wayland_surface) wayland_surface_destroy(data->wayland_surface);
    if (data->window_contents) wayland_shm_buffer_unref(data->window_contents);
    free(data);
}

/***********************************************************************
 *           wayland_win_data_get_nolock
 *
 * Return the data structure associated with a window. This function does
 * not lock the win_data_mutex, so it must be externally synchronized.
 */
struct wayland_win_data *wayland_win_data_get_nolock(HWND hwnd)
{
    struct rb_entry *rb_entry;

    if ((rb_entry = rb_get(&win_data_rb, hwnd)))
        return RB_ENTRY_VALUE(rb_entry, struct wayland_win_data, entry);

    return NULL;
}

/***********************************************************************
 *           wayland_win_data_get
 *
 * Lock and return the data structure associated with a window.
 */
struct wayland_win_data *wayland_win_data_get(HWND hwnd)
{
    struct wayland_win_data *data;

    pthread_mutex_lock(&win_data_mutex);
    if ((data = wayland_win_data_get_nolock(hwnd))) return data;
    pthread_mutex_unlock(&win_data_mutex);

    return NULL;
}

/***********************************************************************
 *           wayland_win_data_release
 *
 * Release the data returned by wayland_win_data_get.
 */
void wayland_win_data_release(struct wayland_win_data *data)
{
    assert(data);
    pthread_mutex_unlock(&win_data_mutex);
}

static void wayland_win_data_queue_state_update(struct wayland_win_data *data,
                                                UINT state_cmd, const RECT *rect)
{
    data->state_update_cmd = state_cmd;
    data->state_update_swp_flags = 0;
    data->state_update_rect = *rect;
    data->state_update_foreground = NULL;
}

static BOOL wayland_win_data_is_fullscreen(struct wayland_win_data *data, DWORD style)
{
    RECT rect;

    if (!data->is_fullscreen) return FALSE;
    if (!(style & WS_POPUP) &&
        (style & (WS_MAXIMIZE | WS_THICKFRAME)) == (WS_MAXIMIZE | WS_THICKFRAME))
        return FALSE;
    if (NtUserGetPresentRect(data->hwnd, &rect, -1)) return TRUE;
    return !(style & (WS_CAPTION | WS_THICKFRAME));
}

static BOOL wayland_win_data_get_pending_config_state(struct wayland_win_data *data,
                                                      enum wayland_surface_config_state *state)
{
    struct wayland_surface *surface = data->wayland_surface;

    if (!surface || surface->role != WAYLAND_SURFACE_ROLE_TOPLEVEL ||
        !surface->xdg_surface || !surface->processing.serial)
        return FALSE;

    *state = surface->processing.state &
             (WAYLAND_SURFACE_CONFIG_STATE_MAXIMIZED |
              WAYLAND_SURFACE_CONFIG_STATE_TILED |
              WAYLAND_SURFACE_CONFIG_STATE_FULLSCREEN);
    return TRUE;
}

static void wayland_win_data_get_config(struct wayland_win_data *data,
                                        struct wayland_window_config *conf)
{
    enum wayland_surface_config_state window_state = 0;
    enum wayland_surface_config_state pending_state;
    DWORD style;

    conf->rect = data->rects.visible;
    conf->window_rect = data->rects.window;
    conf->client_rect = data->rects.client;
    style = NtUserGetWindowLongW(data->hwnd, GWL_STYLE);

    TRACE("window=%s style=%#x\n", wine_dbgstr_rect(&conf->rect), style);

    conf->minimized = FALSE;

    if (style & WS_MINIMIZE)
    {
        conf->minimized = TRUE;
    }
    else if (wayland_win_data_get_pending_config_state(data, &pending_state))
    {
        window_state |= pending_state;
    }
    /* The fullscreen state is implied by the window position and style. */
    else if (wayland_win_data_is_fullscreen(data, style))
    {
        if ((style & WS_MAXIMIZE) && (style & WS_CAPTION) == WS_CAPTION)
            window_state |= WAYLAND_SURFACE_CONFIG_STATE_MAXIMIZED;
        else if (!(style & WS_MINIMIZE))
            window_state |= WAYLAND_SURFACE_CONFIG_STATE_FULLSCREEN;
    }
    else if (style & WS_MAXIMIZE)
    {
        window_state |= WAYLAND_SURFACE_CONFIG_STATE_MAXIMIZED;
    }

    conf->resizeable = !!(style & WS_THICKFRAME);
    conf->state = window_state;
    if (process_wayland.wp_fractional_scale_manager_v1)
        conf->scale = conf->fractional_scale;
    else
        conf->scale = NtUserGetSystemDpiForProcess(0) / 96.0;
    conf->visible = (style & WS_VISIBLE) == WS_VISIBLE;
    conf->managed = data->managed;
}

static void reapply_cursor_clipping(void)
{
    RECT rect;
    UINT context = NtUserSetThreadDpiAwarenessContext(NTUSER_DPI_PER_MONITOR_AWARE);
    if (NtUserGetClipCursor(&rect)) NtUserClipCursor(&rect);
    NtUserSetThreadDpiAwarenessContext(context);
}

static BOOL rect_intersects_virtual_screen(const RECT *rect)
{
    RECT virtual_rect, intersect;

    virtual_rect.left = NtUserGetSystemMetrics(SM_XVIRTUALSCREEN);
    virtual_rect.top = NtUserGetSystemMetrics(SM_YVIRTUALSCREEN);
    virtual_rect.right = virtual_rect.left + NtUserGetSystemMetrics(SM_CXVIRTUALSCREEN);
    virtual_rect.bottom = virtual_rect.top + NtUserGetSystemMetrics(SM_CYVIRTUALSCREEN);

    return intersect_rect(&intersect, rect, &virtual_rect);
}

static BOOL should_keep_toplevel_mapped(struct wayland_surface *surface,
                                        DWORD style, UINT swp_flags)
{
    if (!surface || surface->role != WAYLAND_SURFACE_ROLE_TOPLEVEL) return FALSE;
    if (swp_flags & SWP_HIDEWINDOW) return FALSE;

    /* Invisible without an explicit hide is presumed transient; visible
     * windows map through the normal path. */
    if (!(style & WS_MINIMIZE)) return !(style & WS_VISIBLE);

    return !surface->current.caps ||
           (surface->current.caps & WAYLAND_SURFACE_WM_CAPS_MINIMIZE);
}

static BOOL window_or_root_minimized(HWND hwnd)
{
    HWND root = NtUserGetAncestor(hwnd, GA_ROOT);

    if (NtUserGetWindowLongW(hwnd, GWL_STYLE) & WS_MINIMIZE) return TRUE;
    return root && root != hwnd && (NtUserGetWindowLongW(root, GWL_STYLE) & WS_MINIMIZE);
}

static void detach_client_surfaces_for_toplevel(HWND toplevel)
{
    struct wayland_win_data *data;

    RB_FOR_EACH_ENTRY(data, &win_data_rb, struct wayland_win_data, entry)
    {
        if (data->client_surface && data->client_surface->toplevel == toplevel)
            wayland_client_surface_attach(data->client_surface, NULL);
    }
}

static BOOL is_menu_popup_candidate_style(DWORD style, DWORD exstyle)
{
    if (!(style & WS_POPUP)) return FALSE;
    if ((style & WS_CAPTION) == WS_CAPTION) return FALSE;
    if (style & WS_SYSMENU) return FALSE;
    if (exstyle & WS_EX_APPWINDOW) return FALSE;
    return TRUE;
}

static BOOL should_defer_ownerless_menu_popup(DWORD style, DWORD exstyle, BOOL fullscreen)
{
    if (!(exstyle & WS_EX_TOOLWINDOW)) return FALSE;
    if (fullscreen) return FALSE;
    return is_menu_popup_candidate_style(style, exstyle);
}

static BOOL wayland_win_data_create_wayland_surface(struct wayland_win_data *data,
                                                    struct wayland_surface *toplevel_surface,
                                                    struct wayland_surface *owner_surface,
                                                    BOOL use_layer_shell,
                                                    struct window_surface *window_surface,
                                                    UINT swp_flags)
{
    struct wayland_client_surface *client = data->client_surface;
    struct wayland_surface *surface;
    enum wayland_surface_role role;
    HWND focused;
    BOOL visible, layer_set, keep_mapped, server_decor = FALSE;
    DWORD exstyle = NtUserGetWindowLongW(data->hwnd, GWL_EXSTYLE);
    DWORD style = NtUserGetWindowLongW(data->hwnd, GWL_STYLE);

    TRACE("hwnd=%p style=%x exstyle=%x\n", data->hwnd, style, exstyle);

    surface = data->wayland_surface;
    keep_mapped = should_keep_toplevel_mapped(surface, style, swp_flags);

    layer_set = !(exstyle & WS_EX_LAYERED) || data->layered_attribs_set;
    visible = ((style & WS_VISIBLE) == WS_VISIBLE) && layer_set;

    /* State changes can transiently clear WS_VISIBLE before the driver sees
     * the final minimized/restored state. Keep an existing toplevel mapped
     * unless this is an explicit hide request. */
    if (keep_mapped) visible = TRUE;

    /* if a window is layered and visible but doesn't have attributes set,
     * that only delays when it gets mapped: it doesn't cause the window to get unmapped. */
    if (surface && !layer_set &&
        surface->role != WAYLAND_SURFACE_ROLE_NONE &&
        ((style & WS_VISIBLE) == WS_VISIBLE))
    {
        visible = TRUE;
    }

    if (visible && !owner_surface && !use_layer_shell && !toplevel_surface &&
        !rect_intersects_virtual_screen(&data->rects.window) &&
        !keep_mapped)
        visible = FALSE;

    /* If the toplevel has no observable area, make it roleless. */
    if (!visible) role = WAYLAND_SURFACE_ROLE_NONE;
    else if (owner_surface) role = WAYLAND_SURFACE_ROLE_POPUP;
    else if (use_layer_shell && !IsRectEmpty(&data->rects.window)) role = WAYLAND_SURFACE_ROLE_LAYER;
    else if (toplevel_surface) role = WAYLAND_SURFACE_ROLE_SUBSURFACE;
    else if (should_defer_ownerless_menu_popup(style, exstyle, data->is_fullscreen)) role = WAYLAND_SURFACE_ROLE_NONE;
    else if (!IsRectEmpty(&data->rects.window)) role = WAYLAND_SURFACE_ROLE_TOPLEVEL;
    else role = WAYLAND_SURFACE_ROLE_NONE;

    if (surface && role == WAYLAND_SURFACE_ROLE_LAYER &&
        (surface->role == WAYLAND_SURFACE_ROLE_NONE ||
         (surface->role == WAYLAND_SURFACE_ROLE_LAYER && !surface->zwlr_layer_surface_v1)))
    {
        if (client) wayland_client_surface_attach(client, NULL);
        wayland_surface_destroy(data->wayland_surface);
        data->wayland_surface = NULL;
        surface = NULL;
    }

    /* we can temporarily clear the role of a surface but cannot assign a different one after it's set */
    if (surface && role && surface->role && surface->role != role)
    {
        if (client) wayland_client_surface_attach(client, NULL);
        wayland_surface_destroy(data->wayland_surface);
        data->wayland_surface = NULL;
    }

    if (!(surface = data->wayland_surface) && !(surface = wayland_surface_create(data->hwnd))) return FALSE;

    if (!EqualRect(&data->rects.visible, &data->rects.window)
        && is_decoration_enabled(style, exstyle))
    {
        server_decor = TRUE;
    }

    pthread_mutex_lock(&process_wayland.keyboard.mutex);
    focused = process_wayland.keyboard.focused_hwnd;
    pthread_mutex_unlock(&process_wayland.keyboard.mutex);

    /* Force recreate the toplevel if it is now unminimized.
     * This resets the toplevel state, effectively making it unminimized.
     * Ensure that this restoration is not user driven,
     * as this case is already handled by the compositor.
     * If we don't have caps yet, assume that minimize is supported. */
    if (surface->window.minimized && focused != data->hwnd &&
        role == surface->role && role == WAYLAND_SURFACE_ROLE_TOPLEVEL &&
        (!surface->current.caps ||
          surface->current.caps & WAYLAND_SURFACE_WM_CAPS_MINIMIZE) &&
        !(style & WS_MINIMIZE))
    {
        TRACE("restoring hwnd %p\n", data->hwnd);
        wayland_surface_clear_role(surface);
    }

    if (!layer_set) data->alpha_multiplier = surface->alpha_multiplier = UINT32_MAX;
    else surface->alpha_multiplier = data->alpha_multiplier;

    /* If the window is a visible toplevel make it a wayland
     * xdg_toplevel. Otherwise keep it role-less to avoid polluting the
     * compositor with empty xdg_toplevels. */
    switch (role)
    {
    case WAYLAND_SURFACE_ROLE_NONE:
        if (surface->role) wayland_surface_clear_role(surface);
        break;
    case WAYLAND_SURFACE_ROLE_POPUP:
        wayland_surface_make_popup(surface, owner_surface, &data->rects.window);
        break;
    case WAYLAND_SURFACE_ROLE_LAYER:
        wayland_surface_make_layer(surface, &data->rects.window);
        break;
    case WAYLAND_SURFACE_ROLE_TOPLEVEL:
        wayland_surface_make_toplevel(surface, server_decor);
        break;
    case WAYLAND_SURFACE_ROLE_SUBSURFACE:
        wayland_surface_make_subsurface(surface, toplevel_surface);
        break;
    }

    if (client)
    {
        if (role != WAYLAND_SURFACE_ROLE_NONE && !window_or_root_minimized(data->hwnd))
            wayland_client_surface_attach(client, data->hwnd);
        else wayland_client_surface_attach(client, NULL);
    }
    wayland_win_data_get_config(data, &surface->window);
    if (surface->role == WAYLAND_SURFACE_ROLE_TOPLEVEL && surface->window.minimized)
        detach_client_surfaces_for_toplevel(data->hwnd);
    wayland_surface_sync_window_regions(surface, window_surface);

    /* Size/position changes affect the effective pointer constraint, so update
     * it as needed. */
    if (data->hwnd == NtUserGetForegroundWindow()) reapply_cursor_clipping();

    TRACE("hwnd=%p surface=%p=>%p\n", data->hwnd, data->wayland_surface, surface);
    data->wayland_surface = surface;
    return TRUE;
}

static BOOL wayland_surface_has_pending_state(struct wayland_surface *surface,
                                              enum wayland_surface_config_state state)
{
    if (surface->requested.serial && (surface->requested.state & state)) return TRUE;
    if (surface->processing.serial && (surface->processing.state & state)) return TRUE;
    return FALSE;
}

static void wayland_surface_update_state_toplevel(struct wayland_surface *surface)
{
    const RECT *rect = &surface->window.rect;
    BOOL processing_config = surface->processing.serial;

    TRACE("hwnd=%p window_state=%#x minimized=%u %s->state=%#x\n",
          surface->hwnd, surface->window.state, surface->window.minimized,
          processing_config ? "processing" : "current",
          processing_config ? surface->processing.state : surface->current.state);

    if (processing_config)
    {
        /* Keep compositor configures authoritative until promotion. */
        surface->processing.processed = TRUE;
        return;
    }

    /* Use window state for Wayland requests. Unset states first. */
    if (!(surface->window.state & WAYLAND_SURFACE_CONFIG_STATE_MAXIMIZED) &&
        (surface->current.state & WAYLAND_SURFACE_CONFIG_STATE_MAXIMIZED))
    {
        xdg_toplevel_unset_maximized(surface->xdg_toplevel);
    }
    if (!(surface->window.state & WAYLAND_SURFACE_CONFIG_STATE_FULLSCREEN) &&
        (surface->current.state & WAYLAND_SURFACE_CONFIG_STATE_FULLSCREEN))
    {
        xdg_toplevel_unset_fullscreen(surface->xdg_toplevel);
        wayland_surface_shortcut_control(surface, FALSE);
        surface->requested_output = NULL;
    }

    if ((surface->window.state & WAYLAND_SURFACE_CONFIG_STATE_MAXIMIZED) &&
        !(surface->current.state & WAYLAND_SURFACE_CONFIG_STATE_MAXIMIZED) &&
        !wayland_surface_has_pending_state(surface,
                                           WAYLAND_SURFACE_CONFIG_STATE_MAXIMIZED))
    {
        xdg_toplevel_set_maximized(surface->xdg_toplevel);
    }
    if (surface->window.state & WAYLAND_SURFACE_CONFIG_STATE_FULLSCREEN)
    {
        struct wayland_output *wayland_output;
        struct wl_output *output = NULL;
        pthread_mutex_lock(&process_wayland.output_mutex);

        if ((wayland_output = wayland_output_for_rect(rect)))
            output = wayland_output->wl_output;

        if (surface->current.state & WAYLAND_SURFACE_CONFIG_STATE_FULLSCREEN)
        {
            if (surface->requested_output != output)
            {
                xdg_toplevel_unset_fullscreen(surface->xdg_toplevel);
                wl_display_flush(process_wayland.wl_display);
            }
            else
                goto skip_fullscreen;
        }
        else if (surface->requested_output == output &&
                 wayland_surface_has_pending_state(surface,
                                                   WAYLAND_SURFACE_CONFIG_STATE_FULLSCREEN))
        {
            goto skip_fullscreen;
        }

        xdg_toplevel_set_fullscreen(surface->xdg_toplevel, output);
        wayland_surface_shortcut_control(surface, TRUE);
        surface->requested_output = output;

        skip_fullscreen:
        pthread_mutex_unlock(&process_wayland.output_mutex);
    }
    if (surface->window.minimized)
    {
        xdg_toplevel_set_minimized(surface->xdg_toplevel);
    }
}

static void wayland_win_data_update_wayland_state(struct wayland_win_data *data)
{
    struct wayland_surface *surface = data->wayland_surface;

    switch (surface->role)
    {
    case WAYLAND_SURFACE_ROLE_NONE:
    /* popups do not have any state to update */
    case WAYLAND_SURFACE_ROLE_POPUP:
    case WAYLAND_SURFACE_ROLE_LAYER:
        break;
    case WAYLAND_SURFACE_ROLE_TOPLEVEL:
        if (!surface->xdg_surface) break; /* surface role has been cleared */
        wayland_surface_update_state_toplevel(surface);
        break;
    case WAYLAND_SURFACE_ROLE_SUBSURFACE:
        TRACE("hwnd=%p subsurface parent=%p\n", surface->hwnd, surface->toplevel_hwnd);
        /* Although subsurfaces don't have a dedicated surface config mechanism,
         * we use the config fields to mark them as updated. */
        surface->processing.serial = 1;
        surface->processing.processed = TRUE;
        break;
    }

    wl_display_flush(process_wayland.wl_display);
}

static BOOL is_managed(HWND hwnd)
{
    struct wayland_win_data *data = wayland_win_data_get(hwnd);
    BOOL ret = data && data->managed;
    if (data) wayland_win_data_release(data);
    return ret;
}

static HWND *build_hwnd_list(void)
{
    NTSTATUS status;
    HWND *list;
    ULONG count = 128;

    for (;;)
    {
        if (!(list = malloc(count * sizeof(*list)))) return NULL;
        status = NtUserBuildHwndList(0, 0, 0, 0, 0, count, list, &count);
        if (!status) return list;
        free(list);
        if (status != STATUS_BUFFER_TOO_SMALL) return NULL;
    }
}

static BOOL has_owned_popups(HWND hwnd)
{
    HWND *list;
    UINT i;
    BOOL ret = FALSE;

    if (!(list = build_hwnd_list())) return FALSE;

    for (i = 0; list[i] != HWND_BOTTOM; i++)
    {
        if (list[i] == hwnd) break;  /* popups are always above owner */
        if (NtUserGetWindowRelative(list[i], GW_OWNER) != hwnd) continue;
        if ((ret = is_managed(list[i]))) break;
    }

    free(list);
    return ret;
}

BOOL wayland_is_popup_menu_class(HWND hwnd)
{
    static const WCHAR popup_menu_classW[] = {'#','3','2','7','6','8',0};
    WCHAR buffer[16];
    UNICODE_STRING name;
    INT len;

    name.Buffer = buffer;
    name.MaximumLength = sizeof(buffer);
    if (!(len = NtUserGetClassName(hwnd, FALSE, &name))) return FALSE;
    return len == sizeof(popup_menu_classW) / sizeof(WCHAR) - 1 &&
           !memcmp(name.Buffer, popup_menu_classW, len * sizeof(WCHAR));
}

static HWND layer_menu_restore_fg;

static HWND get_layer_menu_hwnd(void)
{
    return NtUserGetProp(NtUserGetDesktopWindow(), layer_menu_hwnd_prop);
}

static BOOL same_window_process(HWND hwnd, HWND other)
{
    DWORD pid = 0, other_pid = 0;

    if (!hwnd || !other) return FALSE;
    NtUserGetWindowThread(hwnd, &pid);
    NtUserGetWindowThread(other, &other_pid);
    return pid && pid == other_pid;
}

static HWND get_layer_menu_restore_target(HWND menu)
{
    HWND foreground;

    if (layer_menu_restore_fg && NtUserIsWindow(layer_menu_restore_fg) &&
        same_window_process(layer_menu_restore_fg, menu))
        return layer_menu_restore_fg;

    foreground = NtUserGetForegroundWindow();
    if (foreground && foreground != menu && NtUserIsWindow(foreground) &&
        same_window_process(foreground, menu))
        return foreground;

    return NULL;
}

void wayland_set_layer_menu_hwnd(HWND hwnd)
{
    HWND desktop = NtUserGetDesktopWindow();
    HWND restore = get_layer_menu_restore_target(hwnd);

    NtUserSetProp(desktop, layer_menu_hwnd_prop, hwnd);
    if (restore)
        NtUserSetProp(desktop, layer_menu_restore_hwnd_prop, restore);
    else
        NtUserRemoveProp(desktop, layer_menu_restore_hwnd_prop);
}

void wayland_clear_layer_menu_hwnd(HWND hwnd)
{
    HWND desktop = NtUserGetDesktopWindow();

    if (!hwnd || NtUserGetProp(desktop, layer_menu_hwnd_prop) == hwnd)
    {
        NtUserRemoveProp(desktop, layer_menu_hwnd_prop);
        NtUserRemoveProp(desktop, layer_menu_restore_hwnd_prop);
    }
}

BOOL wayland_is_layer_menu_hwnd(HWND hwnd)
{
    return hwnd && get_layer_menu_hwnd() == hwnd;
}

static HWND get_layer_menu_restore_hwnd(HWND menu)
{
    HWND desktop = NtUserGetDesktopWindow();
    HWND restore;

    if (NtUserGetProp(desktop, layer_menu_hwnd_prop) != menu) return NULL;
    restore = NtUserGetProp(desktop, layer_menu_restore_hwnd_prop);
    return restore && NtUserIsWindow(restore) ? restore : NtUserGetDesktopWindow();
}

void wayland_cancel_layer_menu(HWND hwnd)
{
    HWND desktop = NtUserGetDesktopWindow();
    HWND menu = hwnd ? hwnd : NtUserGetProp(desktop, layer_menu_hwnd_prop);
    HWND restore;

    if (!menu) return;

    restore = get_layer_menu_restore_hwnd(menu);
    TRACE("dismissing layer menu %p restore %p\n", menu, restore);
    NtUserPostMessage(menu, WM_WAYLAND_SET_FOREGROUND, TRUE, (LPARAM)restore);
}

void wayland_cancel_layer_menu_if_needed(HWND hwnd)
{
    HWND menu = get_layer_menu_hwnd();

    if (!menu) return;
    if (hwnd && (hwnd == menu ||
                 NtUserGetAncestor(hwnd, GA_ROOT) == menu ||
                 NtUserGetAncestor(hwnd, GA_ROOTOWNER) == menu))
        return;

    wayland_cancel_layer_menu(menu);
}

static inline HWND get_active_window(void)
{
    GUITHREADINFO info;
    info.cbSize = sizeof(info);
    return NtUserGetGUIThreadInfo(GetCurrentThreadId(), &info) ? info.hwndActive : 0;
}

/***********************************************************************
 *		is_window_managed
 *
 * Check if a given window should be managed
 */
/* Owned, caption-less popups (menus, dropdowns, tooltips) become xdg_popups
 * anchored to their owner. A Wayland toplevel has no client-set position. */
/* Reject owner cycles that can briefly appear during transition states. */
static BOOL has_owner_cycle(HWND hwnd, struct wayland_surface *owner)
{
    struct wayland_win_data *grandparent_data;
    struct wayland_surface *grandparent;

    if (!wayland_surface_is_popup(owner)) return FALSE;
    if (owner->owner_hwnd == hwnd) return TRUE;

    if (!(grandparent_data = wayland_win_data_get_nolock(owner->owner_hwnd))) return FALSE;
    if (!(grandparent = grandparent_data->wayland_surface)) return FALSE;
    return has_owner_cycle(hwnd, grandparent);
}

/* Same for a subsurface's parent (toplevel_hwnd) chain. */
static BOOL has_parent_cycle(HWND hwnd, struct wayland_surface *parent)
{
    struct wayland_win_data *grandparent_data;
    struct wayland_surface *grandparent;

    if (parent->role != WAYLAND_SURFACE_ROLE_SUBSURFACE) return FALSE;
    if (parent->toplevel_hwnd == hwnd) return TRUE;

    if (!(grandparent_data = wayland_win_data_get_nolock(parent->toplevel_hwnd))) return FALSE;
    if (!(grandparent = grandparent_data->wayland_surface)) return FALSE;
    return has_parent_cycle(hwnd, grandparent);
}

/* Caption-less, sysmenu-less popups are menu-like transient windows. When they
 * have an owner, force them unmanaged so they become xdg_popups, not
 * compositor-positioned toplevels. */
BOOL wayland_is_menu_popup_candidate(HWND hwnd)
{
    DWORD style = NtUserGetWindowLongW(hwnd, GWL_STYLE);
    DWORD exstyle = NtUserGetWindowLongW(hwnd, GWL_EXSTYLE);

    return is_menu_popup_candidate_style(style, exstyle);
}

static HWND get_menu_popup_owner(HWND hwnd, HWND owner_hint)
{
    HWND owner = NtUserGetWindowRelative(hwnd, GW_OWNER);

    if (!owner) owner = owner_hint;
    if (!owner || owner == hwnd || owner == NtUserGetDesktopWindow() ||
        !NtUserIsWindow(owner))
        return NULL;

    owner = NtUserGetAncestor(owner, GA_ROOT);
    if (!owner || owner == hwnd || owner == NtUserGetDesktopWindow() ||
        !NtUserIsWindow(owner))
        return NULL;

    return owner;
}

BOOL wayland_is_menu_popup(HWND hwnd)
{
    if (!wayland_is_menu_popup_candidate(hwnd)) return FALSE;
    return !!get_menu_popup_owner(hwnd, NULL);
}

static BOOL is_layer_shell_menu_popup(HWND hwnd)
{
    return wayland_is_menu_popup_candidate(hwnd) && wayland_is_popup_menu_class(hwnd);
}

static BOOL is_window_managed(HWND hwnd, HWND menu_popup_owner, UINT swp_flags, BOOL fullscreen)
{
    DWORD style, ex_style;

    /* child windows are not managed */
    style = NtUserGetWindowLongW(hwnd, GWL_STYLE);
    if ((style & (WS_CHILD|WS_POPUP)) == WS_CHILD) return FALSE;
    if (wayland_is_menu_popup_candidate(hwnd) && menu_popup_owner) return FALSE;
    /* activated windows are managed */
    if (!(swp_flags & (SWP_NOACTIVATE|SWP_HIDEWINDOW))) return TRUE;
    if (hwnd == get_active_window()) return TRUE;
    /* windows with caption are managed */
    if ((style & WS_CAPTION) == WS_CAPTION) return TRUE;
    /* windows with thick frame are managed */
    if (style & WS_THICKFRAME) return TRUE;
    if (style & WS_POPUP)
    {
        /* popup with sysmenu == caption are managed */
        if (style & WS_SYSMENU) return TRUE;
        /* full-screen popup windows are managed */
        if (fullscreen) return TRUE;
    }
    /* application windows are managed */
    ex_style = NtUserGetWindowLongW(hwnd, GWL_EXSTYLE);
    if (ex_style & WS_EX_APPWINDOW) return TRUE;
    /* windows that own popups are managed */
    if (has_owned_popups(hwnd)) return TRUE;
    /* default: not managed */
    return FALSE;
}

/***********************************************************************
 *           WAYLAND_DestroyWindow
 */
void WAYLAND_DestroyWindow(HWND hwnd)
{
    struct wayland_win_data *data;

    TRACE("%p\n", hwnd);

    if (!(data = wayland_win_data_get(hwnd))) return;
    wayland_win_data_destroy(data);
}

/***********************************************************************
 *           WAYLAND_WindowPosChanging
 */
BOOL WAYLAND_WindowPosChanging(HWND hwnd, UINT swp_flags, BOOL shaped, const struct window_rects *rects)
{
    struct wayland_win_data *data = wayland_win_data_get(hwnd);

    TRACE("hwnd %p, swp_flags %04x, shaped %u, rects %s\n", hwnd, swp_flags, shaped, debugstr_window_rects(rects));

    if (!data && !(data = wayland_win_data_create(hwnd, rects))) return FALSE;

    wayland_win_data_release(data);

    return TRUE;
}

/***********************************************************************
 *           WAYLAND_WindowPosChanged
 */
void WAYLAND_WindowPosChanged(HWND hwnd, HWND insert_after, HWND owner_hint, UINT swp_flags,
                              const struct window_rects *new_rects, struct window_surface *surface)
{
    HWND toplevel = NtUserGetAncestor(hwnd, GA_ROOT), owner = NULL, menu_popup_owner = NULL;
    struct wayland_surface *toplevel_surface = NULL, *owner_surface = NULL;
    struct wayland_client_surface *client;
    struct wayland_win_data *data, *toplevel_data, *owner_data;
    BOOL managed, fullscreen = swp_flags & WINE_SWP_FULLSCREEN;
    BOOL tray_menu = swp_flags & WINE_SWP_TRAY_MENU;
    BOOL use_layer_shell = FALSE;

    /* Get the managed state with win_data unlocked, as is_window_managed
     * may need to query win_data information about other HWNDs and thus
     * acquire the lock itself internally. */
    menu_popup_owner = wayland_is_menu_popup_candidate(hwnd) ? get_menu_popup_owner(hwnd, owner_hint) : NULL;
    managed = is_window_managed(hwnd, menu_popup_owner, swp_flags, fullscreen);
    if (tray_menu && surface && process_wayland.zwlr_layer_shell_v1)
    {
        managed = FALSE;
        toplevel = NULL;
        use_layer_shell = TRUE;
    }
    else if (!managed && surface)
    {
        toplevel = NULL;
        owner = menu_popup_owner ? menu_popup_owner : owner_hint;
        use_layer_shell = owner && process_wayland.zwlr_layer_shell_v1 &&
                          is_layer_shell_menu_popup(hwnd);
    }

    TRACE("hwnd %p toplevel %p owner %p owner_hint %p menu_owner %p new_rects %s after %p flags %08x\n",
          hwnd, toplevel, owner, owner_hint, menu_popup_owner, debugstr_window_rects(new_rects),
          insert_after, swp_flags);

    if (!(data = wayland_win_data_get(hwnd))) return;
    toplevel_data = toplevel && toplevel != hwnd ? wayland_win_data_get_nolock(toplevel) : NULL;
    toplevel_surface = toplevel_data ? toplevel_data->wayland_surface : NULL;
    owner_data = owner && owner != hwnd ? wayland_win_data_get_nolock(owner) : NULL;
    owner_surface = owner_data ? owner_data->wayland_surface : NULL;
    /* for it to be a popup, we need a valid xdg surface.
     * Demote to subsurface instead if this condition is not met. */
    if (owner_surface && !owner_surface->xdg_surface)
    {
        /* if the window is unmanaged and the owner
         * is not visible, we cannot use a subsurface. */
        if (owner_surface->role != WAYLAND_SURFACE_ROLE_NONE)
            toplevel_surface = owner_surface;
        owner_surface = NULL;
    }
    if (owner_surface) use_layer_shell = FALSE;
    /* Cycles can occur during some transition states.
     * They can be corrected the next time their position updates (after the toplevel gets its role)
     * otherwise these windows will remain as toplevels. */
    if (owner_surface && has_owner_cycle(hwnd, owner_surface))
    {
        ERR("hwnd=%p owner=%p forms a cycle!\n", hwnd, owner);
        owner_surface = NULL;
    }
    if (toplevel_surface && has_parent_cycle(hwnd, toplevel_surface))
    {
        ERR("hwnd=%p parent=%p forms a cycle!\n", hwnd, toplevel);
        toplevel_surface = NULL;
    }

    data->rects = *new_rects;
    data->is_fullscreen = fullscreen;
    data->managed = managed;

    if (!surface)
    {
        if ((client = data->client_surface))
        {
            if (toplevel && NtUserIsWindowVisible(hwnd) && !window_or_root_minimized(hwnd))
                wayland_client_surface_attach(client, toplevel);
            else
                wayland_client_surface_attach(client, NULL);
        }

        if (data->wayland_surface)
        {
            wayland_surface_destroy(data->wayland_surface);
            data->wayland_surface = NULL;
        }
    }
    else if (wayland_win_data_create_wayland_surface(data, toplevel_surface, owner_surface,
                                                     use_layer_shell, surface, swp_flags))
    {
        wayland_win_data_update_wayland_state(data);
    }

    wayland_win_data_release(data);
}

static void wayland_configure_window(HWND hwnd)
{
    struct wayland_surface *surface;
    INT width, height, window_width, window_height;
    INT window_surf_width, window_surf_height, offset_x, offset_y;
    UINT flags = 0;
    UINT state_cmd = 0;
    uint32_t state;
    DWORD style;
    BOOL needs_enter_size_move = FALSE;
    BOOL needs_exit_size_move = FALSE;
    struct wayland_win_data *data;
    RECT rect;

    if (!(data = wayland_win_data_get(hwnd))) return;
    if (!(surface = data->wayland_surface))
    {
        wayland_win_data_release(data);
        return;
    }

    if (!wayland_surface_is_toplevel(surface))
    {
        TRACE("missing xdg_toplevel, returning\n");
        wayland_win_data_release(data);
        return;
    }

    if (!surface->requested.serial)
    {
        /* leftover decor from an initial configure */
        if (surface->requested.decor && surface->current.serial &&
            surface->requested.decor != surface->current.decor)
        {
            int decor = surface->requested.decor;
            surface->requested = surface->current;
            surface->requested.decor = decor;
            surface->requested.processed = FALSE;
        }
        else
        {
            TRACE("hwnd=%p requested configure event already handled, returning\n", hwnd);
            wayland_win_data_release(data);
            return;
        }
    }

    surface->processing = surface->requested;
    memset(&surface->requested, 0, sizeof(surface->requested));

    state = surface->processing.state;
    /* Ignore size hints if we don't have a state that requires strict
     * size adherence, in order to avoid spurious resizes.
     * The tiled and maximized states have a strict size adherance, so
     * their sizes cannot change while we are still processing the new config.
     * This allows us to respect the size hint on transitions of maximized/tiled
     * to a stateless regular window. */
    if (state || surface->current.state &
        (WAYLAND_SURFACE_CONFIG_STATE_MAXIMIZED |
               WAYLAND_SURFACE_CONFIG_STATE_TILED))
    {
        width = surface->processing.width;
        height = surface->processing.height;
    }
    else
    {
        width = height = 0;
    }

    if ((state & WAYLAND_SURFACE_CONFIG_STATE_RESIZING) && !surface->resizing)
    {
        surface->resizing = TRUE;
        needs_enter_size_move = TRUE;
    }

    if (!(state & WAYLAND_SURFACE_CONFIG_STATE_RESIZING) && surface->resizing)
    {
        surface->resizing = FALSE;
        needs_exit_size_move = TRUE;
    }

    /* Transitions between normal/max/fullscreen may entail a frame change. */
    if ((state ^ surface->current.state) &
        (WAYLAND_SURFACE_CONFIG_STATE_MAXIMIZED |
         WAYLAND_SURFACE_CONFIG_STATE_FULLSCREEN))
    {
        flags |= SWP_FRAMECHANGED;
    }
    /* Transitions between decoration modes may entail a frame change. */
    else if (surface->processing.decor != surface->current.decor)
    {
        flags |= SWP_FRAMECHANGED;
    }

    wayland_surface_coords_from_window(surface,
                                       surface->window.rect.right -
                                           surface->window.rect.left,
                                       surface->window.rect.bottom -
                                           surface->window.rect.top,
                                       &window_surf_width, &window_surf_height);

    /* If the window is already fullscreen and its size is compatible with what
     * the compositor is requesting, don't force a resize, since some applications
     * are very insistent on a particular fullscreen size (which may not match
     * the monitor size). */
    if ((surface->window.state & WAYLAND_SURFACE_CONFIG_STATE_FULLSCREEN) &&
        wayland_surface_config_is_compatible(&surface->processing,
                                             window_surf_width, window_surf_height,
                                             surface->window.state))
    {
        flags |= SWP_NOSIZE;
    }

    wayland_surface_coords_to_window(surface, width, height,
                                     &window_width, &window_height);
    offset_x = ((surface->window.rect.left - surface->window.window_rect.left) +
                (surface->window.window_rect.right - surface->window.rect.right));
    offset_y = ((surface->window.rect.top - surface->window.window_rect.top) +
                (surface->window.window_rect.bottom - surface->window.rect.bottom));

    flags |= SWP_NOACTIVATE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_NOMOVE;
    if (window_width == 0 || window_height == 0) flags |= SWP_NOSIZE;
    /* avoid any behavior differences when server side decorations is disabled */
    else if (surface->processing.decor == ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE)
    {
        window_height += offset_y;
        window_width += offset_x;
    }
    else if (NtUserGetProp(hwnd, frameless_window_prop))
    {
        window_width += (data->rects.client.left - data->rects.window.left) +
                        (data->rects.window.right - data->rects.client.right);
        window_height += (data->rects.client.top - data->rects.window.top) +
                         (data->rects.window.bottom - data->rects.client.bottom);
    }

    SetRect(&rect, 0, 0, window_width, window_height);
    OffsetRect(&rect, data->rects.window.left, data->rects.window.top);

    style = NtUserGetWindowLongW(hwnd, GWL_STYLE);
    if (!(state & WAYLAND_SURFACE_CONFIG_STATE_FULLSCREEN))
    {
        if ((state & WAYLAND_SURFACE_CONFIG_STATE_MAXIMIZED) && !(style & WS_MAXIMIZE))
            state_cmd = SC_MAXIMIZE;
        else if (!(state & (WAYLAND_SURFACE_CONFIG_STATE_MAXIMIZED |
                            WAYLAND_SURFACE_CONFIG_STATE_TILED)) &&
                 (style & WS_MAXIMIZE) && window_width && window_height)
            state_cmd = SC_RESTORE;
    }
    if (state_cmd) wayland_win_data_queue_state_update(data, state_cmd, &rect);

    wayland_win_data_release(data);

    TRACE("hwnd=%p processing=%dx%d,%#x\n", hwnd, width, height, state);

    if (needs_enter_size_move) send_message(hwnd, WM_ENTERSIZEMOVE, 0, 0);
    if (needs_exit_size_move) send_message(hwnd, WM_EXITSIZEMOVE, 0, 0);

    if (state_cmd)
    {
        TRACE("hwnd=%p queueing state update %#x rect=%s\n", hwnd, state_cmd,
              wine_dbgstr_rect(&rect));
        NtUserPostMessage(hwnd, WM_WINE_WINDOW_STATE_CHANGED, 0, 0);
        return;
    }

    if (!(state & WAYLAND_SURFACE_CONFIG_STATE_MAXIMIZED) != !(style & WS_MAXIMIZE)
        && !(state & WAYLAND_SURFACE_CONFIG_STATE_FULLSCREEN))
        NtUserSetWindowLong(hwnd, GWL_STYLE, style ^ WS_MAXIMIZE, FALSE);

    /* The Wayland maximized and fullscreen states are very strict about
     * surface size, so don't let the application override it. The tiled state
     * is not as strict, but it indicates a strong size preference, so try to
     * respect it. */
    if (state & (WAYLAND_SURFACE_CONFIG_STATE_MAXIMIZED |
                 WAYLAND_SURFACE_CONFIG_STATE_FULLSCREEN |
                 WAYLAND_SURFACE_CONFIG_STATE_TILED))
    {
        flags |= SWP_NOSENDCHANGING;
    }

    /* Mark pending configures processed before rawpos can flush. */
    if ((data = wayland_win_data_get(hwnd)))
    {
        surface = data->wayland_surface;
        if (surface && surface->role == WAYLAND_SURFACE_ROLE_TOPLEVEL &&
            surface->xdg_surface && surface->processing.serial &&
            !surface->processing.processed)
            wayland_win_data_update_wayland_state(data);
        wayland_win_data_release(data);
    }

    NtUserSetRawWindowPos(hwnd, rect, flags, FALSE);

    /* Ack/promote the processed configure if rawpos did not flush it. */
    if ((data = wayland_win_data_get(hwnd)))
    {
        surface = data->wayland_surface;
        if (surface && surface->role == WAYLAND_SURFACE_ROLE_TOPLEVEL &&
            surface->xdg_surface && surface->processing.serial &&
            surface->processing.processed)
        {
            wayland_win_data_release(data);
            /* Preserve flush lock order: surface before win_data. */
            ensure_window_surface_contents(hwnd);
        }
        else wayland_win_data_release(data);
    }
}

BOOL WAYLAND_GetWindowStateUpdates(HWND hwnd, UINT *state_cmd, UINT *swp_flags,
                                   RECT *rect, HWND *foreground)
{
    struct wayland_win_data *data;
    BOOL ret;

    *state_cmd = *swp_flags = 0;
    *foreground = NULL;
    SetRectEmpty(rect);

    if (!(data = wayland_win_data_get(hwnd))) return FALSE;

    *state_cmd = data->state_update_cmd;
    *swp_flags = data->state_update_swp_flags;
    *rect = data->state_update_rect;
    *foreground = data->state_update_foreground;
    ret = *state_cmd || *swp_flags || *foreground;

    data->state_update_cmd = 0;
    data->state_update_swp_flags = 0;
    SetRectEmpty(&data->state_update_rect);
    data->state_update_foreground = NULL;

    wayland_win_data_release(data);

    TRACE("hwnd=%p returning state_cmd %#x, swp_flags %#x, rect %s, foreground %p\n",
          hwnd, *state_cmd, *swp_flags, wine_dbgstr_rect(rect), *foreground);
    return ret;
}

BOOL WAYLAND_GetWindowMaxTrackSize(HWND hwnd, SIZE *size)
{
    struct wayland_win_data *data;
    BOOL ret = FALSE;

    if (!(data = wayland_win_data_get(hwnd))) return FALSE;

    if (data->wayland_surface)
        ret = wayland_surface_get_max_track_size(data->wayland_surface, size);

    wayland_win_data_release(data);
    return ret;
}

/**********************************************************************
 *           WAYLAND_WindowMessage
 */
LRESULT WAYLAND_WindowMessage(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg)
    {
    case WM_WAYLAND_INIT_DISPLAY_DEVICES:
        NtUserCallNoParam(NtUserCallNoParam_DisplayModeChanged);
        return 0;
    case WM_WAYLAND_CONFIGURE:
        wayland_configure_window(hwnd);
        return 0;
    case WM_WAYLAND_SET_FOREGROUND:
    {
        BOOL layer_menu;
        HWND focused, restore;
        pthread_mutex_lock(&process_wayland.keyboard.mutex);
        focused = process_wayland.keyboard.focused_hwnd;
        pthread_mutex_unlock(&process_wayland.keyboard.mutex);
        layer_menu = wayland_is_layer_menu_hwnd(hwnd);
        /* Layer menu cancellation is explicit. Other focus-loss messages must
         * match a compositor keyboard leave. */
        if (wp && layer_menu && wayland_is_popup_menu_class(hwnd))
        {
            if (NtUserGetForegroundWindow() == hwnd)
            {
                restore = (HWND)lp;
                if (!restore || !NtUserIsWindow(restore)) restore = NtUserGetDesktopWindow();
                NtUserSetForegroundWindowInternal(restore);
            }
            if (NtUserEndMenu()) wayland_clear_layer_menu_hwnd(hwnd);
        }
        else if (wp && NtUserGetForegroundWindow() == hwnd && (focused != hwnd || layer_menu))
        {
            restore = (HWND)lp;
            if (!restore || !NtUserIsWindow(restore)) restore = NtUserGetDesktopWindow();
            if (NtUserSetForegroundWindowInternal(restore) && layer_menu)
                wayland_clear_layer_menu_hwnd(hwnd);
        }
        /* the same applies here */
        else if (!wp && focused == hwnd)
        {
            if (NtUserGetWindowLongW(hwnd, GWL_STYLE) & WS_MINIMIZE)
                NtUserPostMessage(hwnd, WM_SYSCOMMAND, SC_RESTORE, 0);
            NtUserSetForegroundWindowInternal(hwnd);
            /* remember where to hand foreground back when an ownerless layer
             * menu is dismissed so dismissal does not strand the process. */
            if (!wayland_is_layer_menu_hwnd(hwnd)) layer_menu_restore_fg = hwnd;
        }
        else
            WARN("focused %p hwnd %p, Ignoring stale %s message\n",
                 focused, hwnd, wp ? "focus loss" : "focus gain");
        return 0;
    }
    case WM_WAYLAND_DMABUF_FRAME:
    {
        BOOL had_dmabuf_content = FALSE;

        if (window_surface_has_requested_configure(hwnd))
            wayland_configure_window(hwnd);
        if (window_surface_configure_blocks_dmabuf(hwnd))
            return 0;

        /* A producer published a frame for a descendant of this toplevel.
         * Import it in the process that owns the toplevel's wayland surface. */
        had_dmabuf_content = window_surface_has_hwnd_dmabuf_content(hwnd);
        ensure_window_surface_contents(hwnd);
        if (had_dmabuf_content != window_surface_has_hwnd_dmabuf_content(hwnd))
            NtUserPostMessage(hwnd, WM_WINE_UPDATEWINDOWSTATE, 0, 0);
        return 0;
    }
    case WM_WAYLAND_EXPOSE:
        /* Event-thread callbacks use this for exposes that must follow the
         * window-thread lock order. */
        NtUserExposeWindowSurface(hwnd, 0, NULL, 0);
        return 0;
    default:
        FIXME("got window msg %x hwnd %p wp %lx lp %lx\n", msg, hwnd, (long)wp, lp);
        return 0;
    }
}

/**********************************************************************
 *           WAYLAND_DesktopWindowProc
 */
LRESULT WAYLAND_DesktopWindowProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    return NtUserMessageCall(hwnd, msg, wp, lp, 0, NtUserDefWindowProc, FALSE);
}

/*****************************************************************
 *		WAYLAND_UpdateLayeredWindow
 */
void WAYLAND_UpdateLayeredWindow(HWND hwnd, BYTE alpha, UINT flags)
{
    struct wayland_win_data *data;

    TRACE("hwnd=%p alpha=%u flags=%#x\n", hwnd, alpha, flags);

    if (!(data = wayland_win_data_get(hwnd))) return;
    if (!(flags & LWA_ALPHA)) alpha = UINT8_MAX;
    data->alpha_multiplier = (UINT32)alpha * (UINT32_MAX / UINT8_MAX);

    wayland_win_data_release(data);
}

/*****************************************************************
 *		WAYLAND_SetLayeredWindowAttributes
 */
void WAYLAND_SetLayeredWindowAttributes(HWND hwnd, COLORREF key, BYTE alpha, DWORD flags)
{
    struct wayland_win_data *data;

    TRACE("hwnd=%p alpha=%u flags=%#x\n", hwnd, alpha, flags);

    if (!(data = wayland_win_data_get(hwnd))) return;
    data->layered_attribs_set = TRUE;
    if (!(flags & LWA_ALPHA)) alpha = UINT8_MAX;
    data->alpha_multiplier = (UINT32)alpha * (UINT32_MAX / UINT8_MAX);
    wayland_win_data_release(data);
}

static enum xdg_toplevel_resize_edge hittest_to_resize_edge(WPARAM hittest)
{
    switch (hittest)
    {
    case WMSZ_LEFT:        return XDG_TOPLEVEL_RESIZE_EDGE_LEFT;
    case WMSZ_RIGHT:       return XDG_TOPLEVEL_RESIZE_EDGE_RIGHT;
    case WMSZ_TOP:         return XDG_TOPLEVEL_RESIZE_EDGE_TOP;
    case WMSZ_TOPLEFT:     return XDG_TOPLEVEL_RESIZE_EDGE_TOP_LEFT;
    case WMSZ_TOPRIGHT:    return XDG_TOPLEVEL_RESIZE_EDGE_TOP_RIGHT;
    case WMSZ_BOTTOM:      return XDG_TOPLEVEL_RESIZE_EDGE_BOTTOM;
    case WMSZ_BOTTOMLEFT:  return XDG_TOPLEVEL_RESIZE_EDGE_BOTTOM_LEFT;
    case WMSZ_BOTTOMRIGHT: return XDG_TOPLEVEL_RESIZE_EDGE_BOTTOM_RIGHT;
    default:               return XDG_TOPLEVEL_RESIZE_EDGE_NONE;
    }
}

/*****************************************************************
 *		WAYLAND_SetWindowIcons
 */
void WAYLAND_SetWindowIcons(HWND hwnd, HICON icon, const ICONINFO *ii, HICON icon_small, const ICONINFO *ii_small)
{
    struct wayland_surface *surface;
    struct wayland_win_data *data;

    TRACE("hwnd=%p icon=%p ii=%p icon_small=%p ii_small=%p\n", hwnd, icon, ii, icon_small, ii_small);

    if (process_wayland.xdg_toplevel_icon_manager_v1)
    {
        if ((data = wayland_win_data_get(hwnd)))
        {
            if ((surface = data->wayland_surface))
            {
                wayland_surface_set_icon_buffer(surface, ICON_BIG, ii);
                if (icon_small) wayland_surface_set_icon_buffer(surface, ICON_SMALL, ii_small);
                if (wayland_surface_is_toplevel(surface)) wayland_surface_assign_icon(surface);
            }
            wayland_win_data_release(data);
        }
    }
}

/***********************************************************************
 *		WAYLAND_SetWindowStyle
 */
void WAYLAND_SetWindowStyle(HWND hwnd, INT offset, STYLESTRUCT *style)
{
    struct wayland_win_data *data;
    DWORD changed = style->styleNew ^ style->styleOld;

    if (hwnd == NtUserGetDesktopWindow()) return;
    if (!(data = wayland_win_data_get(hwnd))) return;

    /* Changing WS_EX_LAYERED resets attributes */
    if (offset == GWL_EXSTYLE && (changed & WS_EX_LAYERED))
        data->layered_attribs_set = FALSE;

    wayland_win_data_release(data);
}

/*****************************************************************
 *		WAYLAND_SetWindowText
 */
void WAYLAND_SetWindowText(HWND hwnd, LPCWSTR text)
{
    struct wayland_surface *surface;
    struct wayland_win_data *data;

    TRACE("hwnd=%p text=%s\n", hwnd, wine_dbgstr_w(text));

    if ((data = wayland_win_data_get(hwnd)))
    {
        if ((surface = data->wayland_surface) && wayland_surface_is_toplevel(surface))
            wayland_surface_set_title(surface, text);
        wayland_win_data_release(data);
    }
}

/***********************************************************************
 *          WAYLAND_SysCommand
 */
LRESULT WAYLAND_SysCommand(HWND hwnd, WPARAM wparam, LPARAM lparam, const POINT *pos)
{
    LRESULT ret = -1;
    WPARAM command = wparam & 0xfff0;
    uint32_t button_serial;
    struct wl_seat *wl_seat;
    struct wayland_surface *surface;
    struct wayland_win_data *data;

    TRACE("cmd=%lx hwnd=%p, %lx, %lx\n",
          (long)command, hwnd, (long)wparam, lparam);

    pthread_mutex_lock(&process_wayland.pointer.mutex);
    if (process_wayland.pointer.focused_hwnd == hwnd)
        button_serial = process_wayland.pointer.button_serial;
    else
        button_serial = 0;
    pthread_mutex_unlock(&process_wayland.pointer.mutex);

    if (command == SC_MOVE || command == SC_SIZE)
    {
        if ((data = wayland_win_data_get(hwnd)))
        {
            pthread_mutex_lock(&process_wayland.seat.mutex);
            wl_seat = process_wayland.seat.wl_seat;
            if (wl_seat && (surface = data->wayland_surface) &&
                wayland_surface_is_toplevel(surface) && button_serial)
            {
                if (command == SC_MOVE)
                {
                    xdg_toplevel_move(surface->xdg_toplevel, wl_seat, button_serial);
                }
                else if (command == SC_SIZE)
                {
                    xdg_toplevel_resize(surface->xdg_toplevel, wl_seat, button_serial,
                                        hittest_to_resize_edge(wparam & 0x0f));
                }
            }
            pthread_mutex_unlock(&process_wayland.seat.mutex);
            wayland_win_data_release(data);
            ret = 0;
        }
    }

    wl_display_flush(process_wayland.wl_display);
    return ret;
}

/***********************************************************************
 *          WAYLAND_HasWindowManager
 */
BOOL WAYLAND_HasWindowManager(const char *name)
{
    static int once;
    const char *env = getenv("XDG_CURRENT_DESKTOP");

    if (!once++) TRACE("DE: %s\n", debugstr_a(env));

    if (!strcmp("waylanddrv", name)) return TRUE;
    if (env && !strcmp(env, name)) return TRUE;

    return FALSE;
}

/**********************************************************************
 *          WAYLAND_FlashWindowEx
 */
void WAYLAND_FlashWindowEx(FLASHWINFO *info)
{
    struct wayland_win_data *data;

    TRACE("hwnd %p flags %u\n", info->hwnd, info->dwFlags);

    if ((data = wayland_win_data_get(info->hwnd)))
    {
        if (data->wayland_surface && info->dwFlags)
            wayland_surface_activate(data->wayland_surface);
        wayland_win_data_release(data);
    }
}

/***********************************************************************
 *           WAYLAND_GetWindowStyleMasks
 */
BOOL WAYLAND_GetWindowStyleMasks(HWND hwnd, UINT style, UINT ex_style, UINT *style_mask, UINT *ex_style_mask)
{
    BOOL ret = TRUE;
    struct wayland_win_data *data;
    struct wayland_surface *surface;

    TRACE("%p %x %x %p %p\n", hwnd, style, ex_style, style_mask, ex_style_mask);

    *style_mask = *ex_style_mask = 0;

    if (!process_wayland.zxdg_decoration_manager_v1) return FALSE;

    if (!(data = wayland_win_data_get(hwnd))) return FALSE;

    if ((surface = data->wayland_surface) && wayland_surface_is_toplevel(surface))
    {
        if (!data->managed) ret = FALSE;
        else if (surface->current.decor == ZXDG_TOPLEVEL_DECORATION_V1_MODE_CLIENT_SIDE)
            ret = FALSE;
    }

    wayland_win_data_release(data);

    if (ret) ret = is_decoration_enabled(style, ex_style);

    if (ret)
    {
        *style_mask |= WS_CAPTION | WS_DLGFRAME | WS_THICKFRAME;
        *ex_style_mask |= WS_EX_DLGMODALFRAME;
    }

    return ret;
}

void set_client_surface(HWND hwnd, struct wayland_client_surface *new_client)
{
    HWND toplevel = NtUserGetAncestor(hwnd, GA_ROOT);
    struct wayland_client_surface *old_client;
    struct wayland_win_data *data;
    BOOL visible;

    /* ownership is shared with the callers, the last caller to release
     * its reference will also destroy it and clear our pointer. */

    if (!(data = wayland_win_data_get(hwnd)))
    {
        TRACE("hwnd=%p no wayland data for client surface\n", hwnd);
        return;
    }

    visible = NtUserIsWindowVisible(hwnd) && !window_or_root_minimized(hwnd);

    if (new_client && new_client != data->client_surface && data->client_surface &&
        data->client_surface->has_presented && !new_client->has_presented)
    {
        goto done;
    }

    if (new_client != data->client_surface)
    {
        if ((old_client = data->client_surface))
            wayland_client_surface_attach(old_client, NULL);

        data->client_surface = new_client;
    }

    if (data->client_surface)
    {
        if (toplevel && visible)
            wayland_client_surface_attach(data->client_surface, toplevel);
        else
            wayland_client_surface_attach(data->client_surface, NULL);
    }

done:
    wayland_win_data_release(data);
}

/* True when the client surface occludes this window's GDI buffer. A detached
 * or reparented client does not count. */
static BOOL window_client_surface_attached(struct wayland_win_data *data)
{
    return data->client_surface && data->client_surface->wl_subsurface &&
           data->client_surface->toplevel == data->hwnd;
}

static BOOL window_client_surface_pending_first_frame(struct wayland_win_data *data)
{
    return data->client_surface && !data->client_surface->has_presented;
}

static BOOL window_surface_has_requested_configure(HWND hwnd)
{
    struct wayland_win_data *data;
    struct wayland_surface *surface;
    BOOL ret = FALSE;

    if (!(data = wayland_win_data_get(hwnd))) return FALSE;
    surface = data->wayland_surface;
    if (surface && surface->role == WAYLAND_SURFACE_ROLE_TOPLEVEL &&
        surface->xdg_surface && surface->requested.serial)
        ret = TRUE;
    wayland_win_data_release(data);

    return ret;
}

static BOOL window_surface_configure_blocks_dmabuf(HWND hwnd)
{
    struct wayland_win_data *data;
    struct wayland_surface *surface;
    BOOL ret = FALSE;

    if (!(data = wayland_win_data_get(hwnd))) return FALSE;
    surface = data->wayland_surface;
    if (surface && surface->role == WAYLAND_SURFACE_ROLE_TOPLEVEL && surface->xdg_surface)
    {
        /* Keep producer frames flowing while a maximize/restore SysCommand is
         * pending. Only an actual unconfigured parent blocks dmabuf commits. */
        ret = surface->requested.serial ||
              (surface->processing.serial && !surface->processing.processed &&
               !surface->current.serial);
    }
    wayland_win_data_release(data);

    return ret;
}

static BOOL window_surface_has_hwnd_dmabuf_content(HWND hwnd)
{
    struct wayland_win_data *data;
    BOOL ret = FALSE;

    if (!(data = wayland_win_data_get(hwnd))) return FALSE;
    ret = data->wayland_surface && wayland_surface_has_hwnd_dmabuf_content(data->wayland_surface);
    wayland_win_data_release(data);

    return ret;
}

BOOL set_window_surface_contents(HWND hwnd, struct wayland_shm_buffer *shm_buffer, HRGN damage_region)
{
    struct wayland_surface *wayland_surface;
    struct wayland_win_data *data;
    BOOL committed = FALSE;

    if (!(data = wayland_win_data_get(hwnd))) return FALSE;

    if ((wayland_surface = data->wayland_surface))
    {
        if (wayland_surface->role == WAYLAND_SURFACE_ROLE_TOPLEVEL &&
            wayland_surface->window.minimized)
        {
            wayland_win_data_release(data);
            return TRUE;
        }

        if (wayland_surface_reconfigure(wayland_surface))
        {
            /* sync the alpha multiplier if it has changed due to SLWA/ULW */
            if (data->alpha_multiplier != wayland_surface->alpha_multiplier)
            {
                wayland_surface->alpha_multiplier = data->alpha_multiplier;
                wayland_surface_sync_alpha(wayland_surface);
            }

            if (shm_buffer)
            {
                wayland_surface_prepare_direct_dmabuf_shm_commit(wayland_surface);
                wayland_surface_attach_shm(wayland_surface, shm_buffer, damage_region);
                wayland_surface->transparent_carrier_attached = FALSE;
                wl_surface_commit(wayland_surface->wl_surface);
                wayland_surface_finish_direct_dmabuf_shm_commit(wayland_surface);
                wayland_surface_update_hwnd_dmabufs(wayland_surface);
                committed = TRUE;
            }
            else committed = wayland_surface_attach_transparent_carrier(wayland_surface);
        }
        else
        {
            TRACE("Wayland surface not configured yet, not flushing\n");
        }
    }

    /* An attached client stays attached on GDI commits. Detaching would flash
     * the empty GDI buffer. Lifecycle callbacks clear dead client surfaces. */
    if (committed && shm_buffer && data->client_surface && !window_client_surface_attached(data) &&
        !window_client_surface_pending_first_frame(data))
        wayland_client_surface_attach(data->client_surface, NULL);

    /* Update the latest window buffer for the wayland surface. Note that we
     * only care whether the buffer contains the latest window contents,
     * it's irrelevant if it was actually committed or not. */
    if (data->window_contents)
        wayland_shm_buffer_unref(data->window_contents);
    data->window_contents = NULL;
    if (shm_buffer)
    {
        wayland_shm_buffer_ref(shm_buffer);
        data->window_contents = shm_buffer;
    }

    wayland_win_data_release(data);

    return committed;
}

struct wayland_shm_buffer *get_window_surface_contents(HWND hwnd)
{
    struct wayland_shm_buffer *shm_buffer;
    struct wayland_win_data *data;

    if (!(data = wayland_win_data_get(hwnd))) return NULL;
    if ((shm_buffer = data->window_contents)) wayland_shm_buffer_ref(shm_buffer);
    wayland_win_data_release(data);

    return shm_buffer;
}


void ensure_window_surface_contents(HWND hwnd)
{
    BOOL has_dmabuf_content = FALSE, expose = FALSE;
    struct wayland_surface *wayland_surface;
    struct wayland_win_data *data;

    if (!(data = wayland_win_data_get(hwnd)))
    {
        TRACE("hwnd=%p no wayland data for ensure contents\n", hwnd);
        return;
    }

    TRACE("hwnd=%p wayland_surface=%p\n", hwnd, data->wayland_surface);

    if ((wayland_surface = data->wayland_surface))
    {
        wayland_surface_ensure_contents(wayland_surface, data->client_surface);
        has_dmabuf_content = wayland_surface_has_hwnd_dmabuf_content(wayland_surface);

        if (wayland_surface->window.visible)
        {
            if (data->window_contents)
            {
                if (wayland_surface->processing.serial &&
                    wayland_surface->processing.processed &&
                    wayland_surface_reconfigure(wayland_surface))
                {
                    /* Handle any processed configure request, to ensure the
                     * related surface state is applied by the compositor. */
                    wl_surface_commit(wayland_surface->wl_surface);
                }
            }
            /* Producer content already visible: do not create a fallback
             * window surface that could replace its carrier with default pixels. */
            else if (!has_dmabuf_content) expose = TRUE;
        }

        /* Flush queued commits now: the dmabuf present path has no other flush
         * point. They would otherwise sit in the output buffer until the event
         * thread next wakes, throttling presentation to that rate. */
        wl_display_flush(process_wayland.wl_display);
    }

    wayland_win_data_release(data);

    if (expose) NtUserExposeWindowSurface(hwnd, 0, NULL, 0);
}
