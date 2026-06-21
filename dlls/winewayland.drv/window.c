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

static pthread_mutex_t win_data_mutex;
static struct rb_tree win_data_rb = { wayland_win_data_cmp_rb };
static BOOL window_surface_needs_dmabuf_overlay_refresh(HWND hwnd);

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
 *           wayland_win_data_get
 *
 * Lock and return the data structure associated with a window.
 */
struct wayland_win_data *wayland_win_data_get_nolock(HWND hwnd)
{
    struct rb_entry *entry;

    if ((entry = rb_get(&win_data_rb, hwnd)))
        return RB_ENTRY_VALUE(entry, struct wayland_win_data, entry);
    return NULL;
}

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

static BOOL wayland_win_data_is_fullscreen(struct wayland_win_data *data, DWORD style)
{
    RECT rect;

    if (!data->is_fullscreen) return FALSE;
    if (style & WS_MAXIMIZE) return TRUE;
    if (NtUserGetPresentRect(data->hwnd, &rect, -1)) return TRUE;
    return !(style & (WS_CAPTION | WS_THICKFRAME));
}

static void wayland_win_data_get_config(struct wayland_win_data *data,
                                        struct wayland_window_config *conf)
{
    enum wayland_surface_config_state window_state = 0;
    DWORD style, exstyle;

    conf->rect = data->rects.visible;
    conf->window_rect = data->rects.window;
    conf->client_rect = data->rects.client;
    style = NtUserGetWindowLongW(data->hwnd, GWL_STYLE);
    exstyle = NtUserGetWindowLongW(data->hwnd, GWL_EXSTYLE);

    TRACE("window=%s style=%#x exstyle=%#x\n", wine_dbgstr_rect(&conf->rect), style, exstyle);

    conf->minimized = FALSE;

    if (style & WS_MINIMIZE)
    {
        conf->minimized = TRUE;
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

    conf->resizeable = data->resizeable;
    conf->state = window_state;
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

static BOOL wayland_win_data_create_wayland_surface(struct wayland_win_data *data,
                                                    struct wayland_surface *toplevel_surface,
                                                    struct wayland_surface *owner_surface,
                                                    BOOL use_layer_shell)
{
    struct wayland_client_surface *client = data->client_surface;
    struct wayland_surface *surface;
    enum wayland_surface_role role;
    BOOL visible, layer_set, server_decor = FALSE;
    DWORD exstyle = NtUserGetWindowLongW(data->hwnd, GWL_EXSTYLE);
    DWORD style = NtUserGetWindowLongW(data->hwnd, GWL_STYLE);
    struct wl_region *input_region;

    TRACE("hwnd=%p\n", data->hwnd);

    layer_set = !(exstyle & WS_EX_LAYERED) || data->layered_attribs_set;
    visible = ((style & WS_VISIBLE) == WS_VISIBLE);

    /* if a window is layered and visible but doesn't have attributes set,
     * that only delays when it gets mapped: it doesn't cause the window to get unmapped. */
    if (!(surface = data->wayland_surface) || !surface->window.visible)
        visible = visible && layer_set;

    if (visible && !owner_surface && !use_layer_shell && !toplevel_surface &&
        !rect_intersects_virtual_screen(&data->rects.window))
        visible = FALSE;

    /* If the toplevel has no observable area, make it roleless. */
    if (!visible) role = WAYLAND_SURFACE_ROLE_NONE;
    else if (owner_surface) role = WAYLAND_SURFACE_ROLE_POPUP;
    else if (use_layer_shell && !IsRectEmpty(&data->rects.window)) role = WAYLAND_SURFACE_ROLE_LAYER;
    else if (toplevel_surface) role = WAYLAND_SURFACE_ROLE_SUBSURFACE;
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
        /* Make sure any attached client surface is detached before we destroy the surface.
         * They will be reattached when win32u updates them again after WindowPosChanged.
         */
        data->wayland_surface = NULL;
        update_client_surfaces(data->hwnd);
        wayland_surface_destroy(surface);
    }

    if (!(surface = data->wayland_surface) && !(surface = wayland_surface_create(data->hwnd))) return FALSE;

    /* Pass through mouse events for layered, transparent windows, to match
     * Windows behavior. */
    input_region = ((exstyle & WS_EX_TRANSPARENT) && (exstyle & WS_EX_LAYERED)) ?
                   wl_compositor_create_region(process_wayland.wl_compositor) :
                   NULL;
    wl_surface_set_input_region(surface->wl_surface, input_region);
    if (input_region) wl_region_destroy(input_region);

    surface->ensured_contents = WAYLAND_SURFACE_NOT_ENSURED;

    if (!EqualRect(&data->rects.visible, &data->rects.window)
        && is_decoration_enabled(style, exstyle))
    {
        server_decor = TRUE;
    }

    surface->window.visible = visible;
    wayland_win_data_get_config(data, &surface->window);

    /* If the window is a visible toplevel make it a wayland
     * xdg_toplevel. Otherwise keep it role-less to avoid polluting the
     * compositor with empty xdg_toplevels. */
    switch (role)
    {
    case WAYLAND_SURFACE_ROLE_NONE:
        wayland_surface_clear_role(surface);
        break;
    case WAYLAND_SURFACE_ROLE_POPUP:
        wayland_surface_make_popup(surface, owner_surface);
        break;
    case WAYLAND_SURFACE_ROLE_LAYER:
        wayland_surface_make_layer(surface, &data->rects.window);
        break;
    case WAYLAND_SURFACE_ROLE_SUBSURFACE:
        wayland_surface_make_subsurface(surface, toplevel_surface);
        break;
    case WAYLAND_SURFACE_ROLE_TOPLEVEL:
        wayland_surface_make_toplevel(surface, server_decor);
        break;
    }

    /* Size/position changes affect the effective pointer constraint, so update
     * it as needed. */
    if (data->hwnd == NtUserGetForegroundWindow()) reapply_cursor_clipping();

    TRACE("hwnd=%p surface=%p=>%p\n", data->hwnd, data->wayland_surface, surface);
    data->wayland_surface = surface;
    return TRUE;
}

static void wayland_surface_update_state_toplevel(struct wayland_surface *surface)
{
    const RECT *rect = &surface->window.rect;
    BOOL processing_config = surface->processing.serial &&
                             !surface->processing.processed;

    TRACE("hwnd=%p window_state=%#x %s->state=%#x\n",
          surface->hwnd, surface->window.state,
          processing_config ? "processing" : "current",
          processing_config ? surface->processing.state : surface->current.state);

    /* update the parent here as well to ensure that its not stale if the owner is updated
     * with no new contents comitted or state change */
    wayland_surface_update_toplevel_parent(surface);

    /* If we are not processing a compositor requested config, use the
     * window state to determine and update the Wayland state. */
    if (!processing_config)
    {
         /* First do all state unsettings, before setting new state. Some
          * Wayland compositors misbehave if the order is reversed. */
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
           !(surface->current.state & WAYLAND_SURFACE_CONFIG_STATE_MAXIMIZED))
        {
            xdg_toplevel_set_maximized(surface->xdg_toplevel);
        }
        if (surface->window.state & WAYLAND_SURFACE_CONFIG_STATE_FULLSCREEN)
        {
            struct wayland_output *output;
            struct wl_output *wl_output = NULL;

            if ((output = wayland_output_for_rect(rect)))
                wl_output = output->wl_output;

            if (surface->current.state & WAYLAND_SURFACE_CONFIG_STATE_FULLSCREEN)
            {
                if (surface->requested_output != wl_output)
                {
                    xdg_toplevel_unset_fullscreen(surface->xdg_toplevel);
                    wl_display_flush(process_wayland.wl_display);
                }
                else
                    goto skip_fullscreen;
            }

            xdg_toplevel_set_fullscreen(surface->xdg_toplevel, wl_output);
            wayland_surface_shortcut_control(surface, TRUE);
            surface->requested_output = wl_output;

        skip_fullscreen:
            if (output) wayland_output_release(output);
        }
        if (surface->window.minimized && !surface->comitted.minimized)
        {
            xdg_toplevel_set_minimized(surface->xdg_toplevel);
        }

        surface->comitted.minimized = surface->window.minimized;

        /* reset the size hint since we don't want to poison the next configure event with it */
        xdg_toplevel_set_min_size(surface->xdg_toplevel, 0, 0);
        xdg_toplevel_set_max_size(surface->xdg_toplevel, 0, 0);
    }
    else
    {
        surface->processing.processed = TRUE;
    }
}

static void wayland_win_data_update_wayland_state(struct wayland_win_data *data)
{
    struct wayland_surface *surface = data->wayland_surface;

    switch (surface->role)
    {
    case WAYLAND_SURFACE_ROLE_NONE:
    /* popups do not have any state to update */
    case WAYLAND_SURFACE_ROLE_SUBSURFACE:
    case WAYLAND_SURFACE_ROLE_POPUP:
    case WAYLAND_SURFACE_ROLE_LAYER:
        break;
    case WAYLAND_SURFACE_ROLE_TOPLEVEL:
        if (!surface->xdg_surface) break; /* surface role has been cleared */
        wayland_surface_update_state_toplevel(surface);
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

/* An owned, caption-less, sysmenu-less popup is a menu. Force it unmanaged so it
 * becomes an xdg_popup, not a compositor-centered toplevel. */
BOOL wayland_is_menu_popup(HWND hwnd)
{
    DWORD style = NtUserGetWindowLongW(hwnd, GWL_STYLE);

    if (!(style & WS_POPUP)) return FALSE;
    if ((style & WS_CAPTION) == WS_CAPTION) return FALSE;
    if (style & WS_SYSMENU) return FALSE;
    if (NtUserGetWindowLongW(hwnd, GWL_EXSTYLE) & WS_EX_APPWINDOW) return FALSE;
    /* must be owned so there is an owner toplevel/popup to anchor against */
    if (NtUserGetAncestor(hwnd, GA_ROOTOWNER) == hwnd) return FALSE;
    return TRUE;
}

static BOOL is_layer_shell_menu_popup(HWND hwnd)
{
    DWORD style = NtUserGetWindowLongW(hwnd, GWL_STYLE);

    if (!(style & WS_POPUP)) return FALSE;
    if ((style & WS_CAPTION) == WS_CAPTION) return FALSE;
    if (style & WS_SYSMENU) return FALSE;
    if (NtUserGetWindowLongW(hwnd, GWL_EXSTYLE) & WS_EX_APPWINDOW) return FALSE;
    return wayland_is_popup_menu_class(hwnd);
}

static BOOL is_window_managed(HWND hwnd, UINT swp_flags, BOOL fullscreen)
{
    DWORD style, ex_style;

    /* child windows are not managed */
    style = NtUserGetWindowLongW(hwnd, GWL_STYLE);
    if ((style & (WS_CHILD|WS_POPUP)) == WS_CHILD) return FALSE;
    if (wayland_is_menu_popup(hwnd)) return FALSE;
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
    HWND toplevel = NtUserGetAncestor(hwnd, GA_ROOT);
    struct wayland_win_data *data, *toplevel_data;

    TRACE("%p\n", hwnd);

    if (!(data = wayland_win_data_get(hwnd))) return;
    /* drop this child's overlay (if any) from its toplevel */
    if (toplevel && toplevel != hwnd &&
        (toplevel_data = wayland_win_data_get_nolock(toplevel)) &&
        toplevel_data->wayland_surface)
        wayland_surface_remove_child_overlay(toplevel_data->wayland_surface, hwnd);
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
    HWND toplevel = NtUserGetAncestor(hwnd, GA_ROOT), owner = NULL;
    /* GA_ROOT host of any GDI child overlays, resolved independently below. */
    HWND overlay_toplevel = toplevel;
    struct child_overlay_snapshot *snapshot = NULL;
    struct wayland_surface *toplevel_surface = NULL, *owner_surface = NULL, *overlay_surface;
    struct wayland_client_surface *client;
    struct wayland_win_data *data, *toplevel_data, *owner_data, *overlay_data;
    BOOL managed, fullscreen = swp_flags & WINE_SWP_FULLSCREEN;
    BOOL tray_menu = swp_flags & WINE_SWP_TRAY_MENU;
    BOOL use_layer_shell = FALSE;

    /* Get the managed state with win_data unlocked, as is_window_managed
     * may need to query win_data information about other HWNDs and thus
     * acquire the lock itself internally. */
    managed = is_window_managed(hwnd, swp_flags, fullscreen);
    if (tray_menu && surface && process_wayland.zwlr_layer_shell_v1)
    {
        managed = FALSE;
        toplevel = NULL;
        use_layer_shell = TRUE;
    }
    else if (!managed && surface)
    {
        toplevel = NULL;
        owner = owner_hint;
        use_layer_shell = owner && process_wayland.zwlr_layer_shell_v1 &&
                          is_layer_shell_menu_popup(hwnd);
    }

    TRACE("hwnd %p owner %p new_rects %s after %p flags %08x\n", hwnd,
          owner, debugstr_window_rects(new_rects), insert_after, swp_flags);

    /* A child moved, restacked or changed visibility. Snapshot the toplevel's
     * overlay stack outside win_data_mutex so overlays track the window tree
     * without waiting for the next GDI flush. */
    if (overlay_toplevel && overlay_toplevel != hwnd &&
        window_surface_needs_child_overlays(overlay_toplevel))
        snapshot = child_overlays_snapshot(overlay_toplevel);

    if (!(data = wayland_win_data_get(hwnd)))
    {
        free(snapshot);
        return;
    }
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

    /* Reposition and restack overlays only. Content refresh stays flush-driven. */
    if (snapshot && overlay_toplevel && overlay_toplevel != hwnd &&
        (overlay_data = wayland_win_data_get_nolock(overlay_toplevel)) &&
        (overlay_surface = overlay_data->wayland_surface))
    {
        wayland_surface_apply_child_overlays(overlay_surface, NULL, snapshot);
        wl_surface_commit(overlay_surface->wl_surface);
    }
    free(snapshot);

    data->rects = *new_rects;
    data->is_fullscreen = fullscreen;
    data->resizeable = swp_flags & WINE_SWP_RESIZABLE;
    data->managed = managed;

    if (!surface)
    {
        if ((client = data->client_surface))
        {
            if (toplevel && NtUserIsWindowVisible(hwnd))
                wayland_client_surface_attach(client, toplevel);
            else
            {
                wayland_client_surface_attach(client, NULL);
                data->client_surface = NULL;
            }
        }
        if (data->wayland_surface)
        {
            wayland_surface_destroy(data->wayland_surface);
            data->wayland_surface = NULL;
        }
    }
    else if (wayland_win_data_create_wayland_surface(data, toplevel_surface, owner_surface,
                                                     use_layer_shell))
    {
        wayland_win_data_update_wayland_state(data);
    }

    if (owner_data) wayland_win_data_release(owner_data);
    wayland_win_data_release(data);
}

static void wayland_configure_window(HWND hwnd)
{
    struct wayland_surface *surface;
    INT width, height, window_width, window_height, offset_x, offset_y;
    UINT flags = 0;
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
        TRACE("hwnd=%p requested configure event already handled, returning\n", hwnd);
        wayland_win_data_release(data);
        return;
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
        width = surface->processing.rect.right - surface->processing.rect.left;
        height = surface->processing.rect.bottom - surface->processing.rect.top;
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

    /* If the window is already fullscreen and its size is compatible with what
     * the compositor is requesting, don't force a resize, since some applications
     * are very insistent on a particular fullscreen size (which may not match
     * the monitor size). */
    if ((surface->window.state & WAYLAND_SURFACE_CONFIG_STATE_FULLSCREEN) &&
        wayland_surface_config_is_compatible(&surface->processing, surface->window.rect,
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
    if (!IsRectEmpty(&rect)) rect = window_rect_from_visible(&data->rects, rect);

    wayland_win_data_release(data);

    TRACE("hwnd=%p processing=%s,%#x\n", hwnd, wine_dbgstr_rect(&rect), state);

    if (needs_enter_size_move) send_message(hwnd, WM_ENTERSIZEMOVE, 0, 0);
    if (needs_exit_size_move) send_message(hwnd, WM_EXITSIZEMOVE, 0, 0);

    flags |= SWP_NOACTIVATE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_NOMOVE;
    if (IsRectEmpty(&rect)) flags |= SWP_NOSIZE;

    style = NtUserGetWindowLongW(hwnd, GWL_STYLE);
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

    NtUserSetRawWindowPos(hwnd, rect, flags, FALSE);
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
        struct child_overlay_snapshot *snapshot = NULL;

        /* A producer published a frame for a descendant of this toplevel.
         * Import it in the process that owns the toplevel's wayland surface.
         * If GDI child overlays were captured before the producer existed,
         * refresh them once with a producer-aware snapshot so stale host
         * overlays do not stay stacked above the live dmabuf. */
        if (window_surface_needs_dmabuf_overlay_refresh(hwnd))
            snapshot = child_overlays_snapshot(hwnd);
        ensure_window_surface_contents(hwnd);
        if (snapshot)
        {
            struct wayland_win_data *data;

            if ((data = wayland_win_data_get(hwnd)))
            {
                if (data->wayland_surface)
                {
                    wayland_surface_apply_child_overlays(data->wayland_surface, NULL, snapshot);
                    wl_surface_commit(data->wayland_surface->wl_surface);
                    wl_display_flush(process_wayland.wl_display);
                }
                wayland_win_data_release(data);
            }
            free(snapshot);
        }
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
 *		WAYLAND_SetLayeredWindowAttributes
 */
void WAYLAND_SetLayeredWindowAttributes(HWND hwnd, COLORREF key, BYTE alpha, DWORD flags)
{
    struct wayland_win_data *data;
    struct wayland_surface *surface;

    if (!(data = wayland_win_data_get(hwnd))) return;

    if ((surface = data->wayland_surface))
        wayland_surface_set_opacity(surface, alpha, flags);
    data->layered_attribs_set = TRUE;

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
    struct wayland_surface *surface;
    DWORD changed = style->styleNew ^ style->styleOld;

    if (hwnd == NtUserGetDesktopWindow()) return;
    if (!(data = wayland_win_data_get(hwnd))) return;

    /* Changing WS_EX_LAYERED resets attributes */
    if (offset == GWL_EXSTYLE && (changed & WS_EX_LAYERED))
    {
        if ((surface = data->wayland_surface))
            wayland_surface_set_opacity(surface, 0, 0);
        data->layered_attribs_set = FALSE;
    }

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
 *          WAYLAND_UpdateLayeredWindow
 */
void WAYLAND_UpdateLayeredWindow(HWND hwnd, BYTE alpha, UINT flags)
{
    struct wayland_win_data *data;
    struct wayland_surface *surface;

    if (!(data = wayland_win_data_get(hwnd))) return;

    if ((surface = data->wayland_surface))
        wayland_surface_set_opacity(surface, alpha, flags);

    wayland_win_data_release(data);
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
            wayland_surface_activate(data->wayland_surface, FALSE);
        wayland_win_data_release(data);
    }
}

/***********************************************************************
 *           WAYLAND_ActivateWindow
 */
void WAYLAND_ActivateWindow(HWND hwnd, HWND previous)
{
    struct wayland_win_data *data;

    TRACE("hwnd=%p previous=%p\n", hwnd, previous);

    if (hwnd == previous) return;

    if ((data = wayland_win_data_get(hwnd)))
    {
        if (data->wayland_surface)
            wayland_surface_activate(data->wayland_surface, TRUE);
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

    if (ret && (ret = is_decoration_enabled(style, ex_style)))
    {
        *style_mask |= WS_CAPTION | WS_DLGFRAME | WS_THICKFRAME;
        *ex_style_mask |= WS_EX_DLGMODALFRAME;
    }

    return ret;
}

/***********************************************************************
 *           WAYLAND_GetWindowStateUpdates
 */
BOOL WAYLAND_GetWindowStateUpdates(HWND hwnd, UINT *state_cmd, UINT *swp_flags,
                                   RECT *rect, HWND *foreground)
{
    struct wayland_keyboard *keyboard = &process_wayland.keyboard;
    DWORD style = NtUserGetWindowLongW(hwnd, GWL_STYLE);
    HWND focused_hwnd, old_foreground = NtUserGetForegroundWindow();

    /* in these cases we dont need to update the window focus, borrowed from winemac */
    if (!(style & WS_VISIBLE)) return FALSE;
    if ((style & (WS_POPUP | WS_CHILD)) == WS_CHILD) return FALSE;
    if (style & WS_DISABLED) return FALSE;

    pthread_mutex_lock(&keyboard->mutex);
    focused_hwnd = keyboard->focused_hwnd;
    pthread_mutex_unlock(&keyboard->mutex);

    /* if the foreground window is not the hwnd then this is a stale focus loss */
    if (!focused_hwnd && old_foreground == hwnd)
        focused_hwnd = NtUserGetDesktopWindow();
    else if (focused_hwnd != hwnd) focused_hwnd = NULL;

    if (old_foreground != focused_hwnd) *foreground = focused_hwnd;

    /* we can't track if the host window is minimized or unminimized, but
     * if we have keyboard focus on this window we can treat it as restored. */
    if ((style & WS_MINIMIZE) && focused_hwnd == hwnd && old_foreground != hwnd)
        *state_cmd = MAKELONG(SC_RESTORE, 1);

    TRACE("hwnd=%p foreground=%p state_low=%#x\n", hwnd, *foreground, LOWORD(*state_cmd));

    return TRUE;
}

/* True when the client surface occludes this window's GDI buffer. A detached
 * or reparented client does not count. */
static BOOL window_client_surface_attached(struct wayland_win_data *data)
{
    return data->client_surface && data->client_surface->wl_subsurface &&
           data->client_surface->toplevel == data->hwnd;
}

/* Whether the flush path should snapshot child geometry for overlays before
 * taking win_data_mutex in set_window_surface_contents. Overlays exist only
 * while an attached client surface occludes the whole GDI buffer. Dmabuf
 * children do NOT count: they cover only their own rects, the base stays
 * visible and a copy overlay would occlude their content (Steam black window). */
BOOL window_surface_needs_child_overlays(HWND hwnd)
{
    struct wayland_win_data *data;
    BOOL ret;

    if (!(data = wayland_win_data_get(hwnd))) return FALSE;
    ret = window_client_surface_attached(data);
    wayland_win_data_release(data);

    return ret;
}

static BOOL window_surface_needs_dmabuf_overlay_refresh(HWND hwnd)
{
    struct wayland_win_data *data;
    BOOL ret;

    if (!(data = wayland_win_data_get(hwnd))) return FALSE;
    ret = data->wayland_surface && data->wayland_surface->child_overlays_need_dmabuf_refresh;
    wayland_win_data_release(data);

    return ret;
}

BOOL set_window_surface_contents(HWND hwnd, struct wayland_shm_buffer *shm_buffer, HRGN damage_region,
                                 const struct child_overlay_snapshot *overlay_snapshot)
{
    struct wayland_surface *wayland_surface;
    struct wayland_win_data *data;
    struct wayland_shm_buffer *dummy_buffer = NULL;
    HRGN dummy_damage = NULL;
    BOOL committed = FALSE;

    if (!(data = wayland_win_data_get(hwnd))) return FALSE;

    if ((wayland_surface = data->wayland_surface))
    {
        if (wayland_surface_reconfigure(wayland_surface))
        {
            /* sync the alpha multiplier if it has changed due to SLWA/ULW */
            if (data->alpha_multiplier != wayland_surface->alpha_multiplier)
            {
                wayland_surface->alpha_multiplier = data->alpha_multiplier;
                wayland_surface_sync_alpha(wayland_surface);
            }

            if (window_client_surface_attached(data) && !data->client_surface->has_presented)
                dummy_buffer = wayland_surface_create_dummy_buffer(wayland_surface,
                                                                   WL_SHM_FORMAT_ARGB8888,
                                                                   &dummy_damage);

            wayland_surface_attach_shm(wayland_surface, dummy_buffer ? dummy_buffer : shm_buffer,
                                       dummy_buffer ? dummy_damage : damage_region);
            wayland_surface->client_placeholder = dummy_buffer != NULL;
            if (window_client_surface_attached(data))
                wayland_surface_apply_child_overlays(wayland_surface, shm_buffer, overlay_snapshot);
            else if (!wl_list_empty(&wayland_surface->child_overlays))
                wayland_surface_clear_child_overlays(wayland_surface);
            wl_surface_commit(wayland_surface->wl_surface);
            if (dummy_buffer) wayland_shm_buffer_unref(dummy_buffer);
            wayland_surface->ensured_contents = WAYLAND_SURFACE_ENSURED_FLUSH;
            committed = TRUE;
        }
        else
        {
            TRACE("Wayland surface not configured yet, not flushing\n");
        }
    }

    /* An attached client stays attached on GDI commits. Detaching would flash
     * the empty GDI buffer. GDI children show via overlays instead. Only a
     * no-longer-attached client is cleaned up here. */
    if (committed && data->client_surface && !window_client_surface_attached(data))
    {
        wayland_client_surface_attach(data->client_surface, NULL);
        data->client_surface = NULL;
    }
    /* Update the latest window buffer for the wayland surface. Note that we
     * only care whether the buffer contains the latest window contents,
     * it's irrelevant if it was actually committed or not. */
    if (data->window_contents)
        wayland_shm_buffer_unref(data->window_contents);
    wayland_shm_buffer_ref((data->window_contents = shm_buffer));

    if (dummy_damage) NtGdiDeleteObjectApp(dummy_damage);
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

void wayland_window_init(void)
{
    pthread_mutexattr_t attr;

    pthread_mutexattr_init(&attr);
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&win_data_mutex, &attr);
    pthread_mutexattr_destroy(&attr);
}

void ensure_window_surface_contents(HWND hwnd)
{
    BOOL expose = FALSE;
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

        if (wayland_surface->window.visible)
        {
            if (data->window_contents)
            {
                /* Handle any processed configure request, to ensure the related
                * surface state is applied by the compositor. */
                if (wayland_surface->processing.serial &&
                    wayland_surface->processing.processed &&
                    wayland_surface_reconfigure(wayland_surface))
                {
                    wl_surface_commit(wayland_surface->wl_surface);
                }
            }
            else expose = TRUE;
        }

        /* Flush queued commits now: the dmabuf present path has no other flush
         * point. They would otherwise sit in the output buffer until the event
         * thread next wakes, throttling presentation to that rate. */
        wl_display_flush(process_wayland.wl_display);
    }

    wayland_win_data_release(data);

    if (expose) NtUserExposeWindowSurface(hwnd, 0, NULL, 0);
}
