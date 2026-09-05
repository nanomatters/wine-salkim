/*
 * Wayland pointer handling
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

#include <linux/input.h>
#undef SW_MAX /* Also defined in winuser.rh */
#include <math.h>
#include <stdlib.h>
#include <time.h>

#define OEMRESOURCE

#include "waylanddrv.h"
#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(cursor);

/* The cursor-shape-v1 protocol file references the zwp_tablet_tool_v2
 * interface object. Since we don't currently use the tablet protocol,
 * provide a dummy object here to avoid linking errors. */
void *zwp_tablet_tool_v2_interface = NULL;

struct system_cursors
{
    WORD id;
    enum wp_cursor_shape_device_v1_shape shape;
};

static const struct system_cursors user32_cursors[] =
{
    {OCR_NORMAL,      WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_DEFAULT},
    {OCR_IBEAM,       WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_TEXT},
    {OCR_WAIT,        WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_WAIT},
    {OCR_CROSS,       WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_CROSSHAIR},
    {OCR_SIZE,        WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_ALL_RESIZE},
    {OCR_SIZENWSE,    WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NWSE_RESIZE},
    {OCR_SIZENESW,    WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NESW_RESIZE},
    {OCR_SIZEWE,      WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_EW_RESIZE},
    {OCR_SIZENS,      WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NS_RESIZE},
    {OCR_SIZEALL,     WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_ALL_RESIZE},
    {OCR_NO,          WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NOT_ALLOWED},
    {OCR_HAND,        WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_POINTER},
    {OCR_APPSTARTING, WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_PROGRESS},
    {OCR_HELP,        WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_HELP},
    {OCR_RDR2DIM,     WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_ALL_SCROLL},
    {0}
};

static const struct system_cursors comctl32_cursors[] =
{
    {102, WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_MOVE},
    {104, WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_COPY},
    {105, WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_DEFAULT},
    {106, WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_COL_RESIZE},
    {107, WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_COL_RESIZE},
    {108, WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_POINTER},
    {135, WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_ROW_RESIZE},
    {0}
};

static const struct system_cursors ole32_cursors[] =
{
    {1, WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NO_DROP},
    {2, WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_MOVE},
    {3, WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_COPY},
    {4, WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_ALIAS},
    {0}
};

static const struct system_cursors riched20_cursors[] =
{
    {105, WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_POINTER},
    {109, WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_COPY},
    {110, WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_MOVE},
    {111, WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NO_DROP},
    {0}
};

static const struct
{
    const struct system_cursors *cursors;
    WCHAR name[16];
} module_cursors[] =
{
    {user32_cursors, {'u','s','e','r','3','2','.','d','l','l',0}},
    {comctl32_cursors, {'c','o','m','c','t','l','3','2','.','d','l','l',0}},
    {ole32_cursors, {'o','l','e','3','2','.','d','l','l',0}},
    {riched20_cursors, {'r','i','c','h','e','d','2','0','.','d','l','l',0}}
};

static HWND wayland_pointer_get_focus(struct wl_surface **wl_surface)
{
    struct wayland_pointer *pointer = &process_wayland.pointer;
    HWND hwnd;

    pthread_mutex_lock(&pointer->mutex);
    hwnd = pointer->focused_hwnd;
    if (wl_surface) *wl_surface = pointer->focused_wl_surface;
    pthread_mutex_unlock(&pointer->mutex);

    return hwnd;
}

static HWND wayland_pointer_get_focused_hwnd(void)
{
    return wayland_pointer_get_focus(NULL);
}

static void wayland_pointer_reset_frame(void)
{
    struct wayland_pointer_frame *frame = &process_wayland.pointer.frame;

    frame->dx = frame->dy = 0.0;
    frame->external_dx = frame->external_dy = 0.0;
    frame->dx_raw = frame->dy_raw = 0.0;
    frame->horz_scroll = frame->scroll = 0.0;
    frame->horz_axis = frame->axis = 0.0;
    frame->flags = 0;
}

static void pointer_handle_motion_internal(wl_fixed_t sx, wl_fixed_t sy)
{
    struct wayland_pointer *pointer = &process_wayland.pointer;
    struct wayland_pointer_frame *frame = &pointer->frame;
    double screen_x, screen_y;
    LONG external_x = 0, external_y = 0, external_width = 0, external_height = 0;
    RECT input_rect;
    BOOL external_input_active;
    HWND hwnd;
    POINT screen;
    struct wl_surface *focused_wl_surface;
    struct wayland_surface *surface;
    struct wayland_win_data *data;

    if (!(hwnd = wayland_pointer_get_focus(&focused_wl_surface))) return;
    external_input_active = wayland_external_input_is_active();
    if ((data = wayland_win_data_get(hwnd)))
    {
        if (!(surface = data->wayland_surface))
        {
            wayland_win_data_release(data);
            return;
        }

        input_rect = wayland_surface_get_input_rect(surface, data);
        if (external_input_active)
            wayland_surface_coords_to_external_input(surface, data,
                    wl_fixed_to_double(sx), wl_fixed_to_double(sy),
                    &external_x, &external_y, &external_width, &external_height);
        wayland_surface_coords_to_screen(surface, data, wl_fixed_to_double(sx),
                                         wl_fixed_to_double(sy), &screen_x, &screen_y);
        screen.x = round(screen_x);
        screen.y = round(screen_y);
        wayland_win_data_release(data);
    }
    else if (!wayland_hwnd_dmabuf_surface_coords_to_screen(
                     focused_wl_surface, wl_fixed_to_double(sx),
                     wl_fixed_to_double(sy), &screen, &input_rect))
        return;
    else if (external_input_active)
    {
        external_x = screen.x - input_rect.left;
        external_y = screen.y - input_rect.top;
        external_width = input_rect.right - input_rect.left;
        external_height = input_rect.bottom - input_rect.top;
    }

    /* Sometimes, due to rounding, we may end up with pointer coordinates
     * slightly outside the target window, so bring them within bounds. */
    if (screen.x >= input_rect.right) screen.x = input_rect.right - 1;
    else if (screen.x < input_rect.left) screen.x = input_rect.left;
    if (screen.y >= input_rect.bottom) screen.y = input_rect.bottom - 1;
    else if (screen.y < input_rect.top) screen.y = input_rect.top;

    pthread_mutex_lock(&pointer->mutex);

    if (pointer->external_input_active != external_input_active)
    {
        pthread_mutex_unlock(&pointer->mutex);
        return;
    }
    frame->x = screen.x;
    frame->y = screen.y;
    if (external_input_active)
    {
        frame->external_x = external_x;
        frame->external_y = external_y;
        frame->external_width = external_width;
        frame->external_height = external_height;
    }
    frame->flags |= WAYLAND_POINTER_FRAME_ABSOLUTE;

    pthread_mutex_unlock(&pointer->mutex);

    if (external_input_active)
        TRACE("hwnd=%p wayland_xy=%.2f,%.2f screen_xy=%d,%d external_xy=%d,%d size=%dx%d\n",
              hwnd, wl_fixed_to_double(sx), wl_fixed_to_double(sy), screen.x, screen.y,
              external_x, external_y, external_width, external_height);
    else
        TRACE("hwnd=%p wayland_xy=%.2f,%.2f screen_xy=%d,%d\n",
              hwnd, wl_fixed_to_double(sx), wl_fixed_to_double(sy), screen.x, screen.y);
}

static void pointer_handle_motion(void *data, struct wl_pointer *wl_pointer,
                                  uint32_t time, wl_fixed_t sx, wl_fixed_t sy)
{
    struct wayland_pointer *pointer = &process_wayland.pointer;

    /* Ignore absolute motion events if in relative mode. */
    if (pointer->relative_mode) return;

    pointer_handle_motion_internal(sx, sy);
}

static void wayland_set_cursor(HWND hwnd, HCURSOR hcursor, BOOL use_hcursor);
static void pointer_handle_frame(void *data, struct wl_pointer *wl_pointer);

static void pointer_handle_enter(void *data, struct wl_pointer *wl_pointer,
                                 uint32_t serial, struct wl_surface *wl_surface,
                                 wl_fixed_t sx, wl_fixed_t sy)
{
    struct wayland_pointer *pointer = &process_wayland.pointer;
    HWND hwnd;

    InterlockedExchange(&process_wayland.input_serial, serial);

    if (!wl_surface) return;
    /* The wl_surface user data remains valid and immutable for the whole
     * lifetime of the object, so it's safe to access without locking. */
    hwnd = wl_surface_get_user_data(wl_surface);
    wayland_activation_set_serial(WAYLAND_ACTIVATION_SERIAL_POINTER_FOCUS, hwnd, serial);

    TRACE("hwnd=%p\n", hwnd);

    pthread_mutex_lock(&pointer->mutex);
    pointer->focused_wl_surface = wl_surface;
    pointer->focused_hwnd = hwnd;
    pointer->enter_serial = serial;
    wayland_pointer_reset_frame();
    pthread_mutex_unlock(&pointer->mutex);

    /* The cursor is undefined at every enter, so we set it again with
     * the latest information we have. */
    wayland_set_cursor(hwnd, NULL, FALSE);

    /* Handle the enter as a motion, to account for cases where the
     * window first appears beneath the pointer and won't get a separate
     * motion event. */
    pointer_handle_motion_internal(sx, sy);
    pointer_handle_frame(NULL, pointer->wl_pointer);
}

static void pointer_handle_leave(void *data, struct wl_pointer *wl_pointer,
                                 uint32_t serial, struct wl_surface *wl_surface)
{
    struct wayland_pointer *pointer = &process_wayland.pointer;

    InterlockedExchange(&process_wayland.input_serial, serial);

    if (!wl_surface) return;

    wayland_activation_clear_serial(WAYLAND_ACTIVATION_SERIAL_POINTER_FOCUS,
                                    wl_surface_get_user_data(wl_surface));
    TRACE("hwnd=%p\n", wl_surface_get_user_data(wl_surface));

    pthread_mutex_lock(&pointer->mutex);
    if (pointer->focused_wl_surface == wl_surface)
    {
        pointer->focused_wl_surface = NULL;
        pointer->focused_hwnd = NULL;
        pointer->enter_serial = 0;
    }
    pthread_mutex_unlock(&pointer->mutex);
}

static void pointer_handle_button(void *data, struct wl_pointer *wl_pointer,
                                  uint32_t serial, uint32_t time, uint32_t button,
                                  uint32_t state)
{
    struct wayland_pointer *pointer = &process_wayland.pointer;
    struct wine_wayland_external_input_event event;
    struct wayland_win_data *win_data;
    INPUT input = {0};
    BOOL external_input_active;
    POINT screen;
    double surface_x, surface_y;
    HWND hwnd;

    InterlockedExchange(&process_wayland.input_serial, serial);

    if (!(hwnd = wayland_pointer_get_focused_hwnd())) return;

    if (state == WL_POINTER_BUTTON_STATE_PRESSED)
        wayland_activation_set_serial(WAYLAND_ACTIVATION_SERIAL_INPUT, hwnd, serial);

    input.type = INPUT_MOUSE;

    switch (button)
    {
    case BTN_LEFT: input.mi.dwFlags = MOUSEEVENTF_LEFTDOWN; break;
    case BTN_RIGHT: input.mi.dwFlags = MOUSEEVENTF_RIGHTDOWN; break;
    case BTN_MIDDLE: input.mi.dwFlags = MOUSEEVENTF_MIDDLEDOWN; break;
    case BTN_SIDE:
    case BTN_BACK:
        input.mi.dwFlags = MOUSEEVENTF_XDOWN;
        input.mi.mouseData = XBUTTON1;
        break;
    case BTN_EXTRA:
    case BTN_FORWARD:
        input.mi.dwFlags = MOUSEEVENTF_XDOWN;
        input.mi.mouseData = XBUTTON2;
        break;
    default: break;
    }

    if (state == WL_POINTER_BUTTON_STATE_RELEASED) input.mi.dwFlags <<= 1;
    else wayland_cancel_layer_menu_if_needed(hwnd);

    pthread_mutex_lock(&pointer->mutex);
    pointer->button_serial = state == WL_POINTER_BUTTON_STATE_PRESSED ?
                             serial : 0;
    external_input_active = pointer->external_input_active;
    if (external_input_active)
    {
        screen.x = pointer->frame.x;
        screen.y = pointer->frame.y;
        event = (struct wine_wayland_external_input_event)
        {
            .size = sizeof(event),
            .type = WINE_WAYLAND_EXTERNAL_INPUT_POINTER_BUTTON,
            .time = time,
            .flags = WINE_WAYLAND_EXTERNAL_INPUT_ABSOLUTE,
            .x = pointer->frame.external_x,
            .y = pointer->frame.external_y,
            .width = pointer->frame.external_width,
            .height = pointer->frame.external_height,
            .code = button,
            .state = state,
        };
    }
    if (state == WL_POINTER_BUTTON_STATE_PRESSED)
    {
        pointer->popup_serial = serial;
        pointer->popup_serial_hwnd = hwnd;
        pointer->popup_serial_time = wayland_time_ms();
    }
    pthread_mutex_unlock(&pointer->mutex);

    /* A click can be the first pointer event after overlay activation. Derive
     * its coordinates and current presentation size without requiring motion. */
    if (external_input_active && (win_data = wayland_win_data_get(hwnd)))
    {
        if (win_data->wayland_surface)
        {
            wayland_surface_coords_from_screen(win_data->wayland_surface, win_data,
                    screen.x, screen.y, &surface_x, &surface_y);
            wayland_surface_coords_to_external_input(win_data->wayland_surface, win_data,
                    surface_x, surface_y, &event.x, &event.y, &event.width, &event.height);
        }
        wayland_win_data_release(win_data);
    }

    TRACE("hwnd=%p button=%#x state=%u\n", hwnd, button, state);

    if (!external_input_active || !wayland_external_input_emit(&event))
        NtUserSendHardwareInput(hwnd, 0, &input, 0);
}

static void pointer_handle_axis(void *user_data, struct wl_pointer *wl_pointer,
                                uint32_t time, uint32_t axis, wl_fixed_t value)
{
    HWND hwnd;
    struct wayland_pointer *pointer = &process_wayland.pointer;
    struct wayland_pointer_frame *frame = &pointer->frame;
    /* on KWin one tick is 15.0 of whatever unit this is */
    double scroll = wl_fixed_to_double(value) / 15.0;

    if (!(hwnd = wayland_pointer_get_focused_hwnd())) return;

    pthread_mutex_lock(&pointer->mutex);
    switch (axis)
    {
        case WL_POINTER_AXIS_VERTICAL_SCROLL:
            frame->axis += -scroll * WHEEL_DELTA;
            frame->flags |= WAYLAND_POINTER_FRAME_AXIS;
            break;
        case WL_POINTER_AXIS_HORIZONTAL_SCROLL:
            frame->horz_axis += scroll * WHEEL_DELTA;
            frame->flags |= WAYLAND_POINTER_FRAME_AXIS_HORZ;
            break;
        default: break;
    }
    pthread_mutex_unlock(&pointer->mutex);

    TRACE("hwnd %p scroll %.3f\n", hwnd, scroll);
}

static void pointer_handle_frame(void *data, struct wl_pointer *wl_pointer)
{
    struct wine_wayland_external_input_event event;
    INPUT absolute = {.type = INPUT_MOUSE}, relative = {.type = INPUT_MOUSE};
    INPUT raw = {.type = INPUT_MOUSE}, wheel = {.type = INPUT_MOUSE};
    INPUT horizontal_wheel = {.type = INPUT_MOUSE};
    BOOL have_absolute = FALSE, have_relative = FALSE, have_raw = FALSE;
    BOOL have_wheel = FALSE, have_horizontal_wheel = FALSE, consumed;
    BOOL external_input_active;
    HWND hwnd;
    struct wayland_pointer *pointer = &process_wayland.pointer;
    struct wayland_pointer_frame *frame = &pointer->frame;

    if (!(hwnd = wayland_pointer_get_focused_hwnd())) return;

    TRACE("hwnd %p\n", hwnd);

    pthread_mutex_lock(&pointer->mutex);

    external_input_active = pointer->external_input_active;
    if (external_input_active)
    {
        event = (struct wine_wayland_external_input_event)
        {
            .size = sizeof(event),
            .type = WINE_WAYLAND_EXTERNAL_INPUT_POINTER_MOTION,
            .time = wayland_time_ms(),
        };
    }

    if (frame->flags & WAYLAND_POINTER_FRAME_ABSOLUTE)
    {
        absolute.mi.dwFlags = MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_MOVE;
        absolute.mi.dx = frame->x;
        absolute.mi.dy = frame->y;
        have_absolute = TRUE;
        if (external_input_active)
        {
            event.flags |= WINE_WAYLAND_EXTERNAL_INPUT_ABSOLUTE;
            event.x = frame->external_x;
            event.y = frame->external_y;
        }
    }

    if (frame->flags & WAYLAND_POINTER_FRAME_RELATIVE)
    {
        relative.mi.dwFlags = MOUSEEVENTF_MOVE;
        relative.mi.dx = round(frame->dx);
        relative.mi.dy = round(frame->dy);
        frame->dx -= relative.mi.dx;
        frame->dy -= relative.mi.dy;
        have_relative = pointer->relative_mode && (relative.mi.dx || relative.mi.dy);

        raw.mi.dwFlags = MOUSEEVENTF_MOVE;
        raw.mi.dx = round(frame->dx_raw);
        raw.mi.dy = round(frame->dy_raw);
        frame->dx_raw -= raw.mi.dx;
        frame->dy_raw -= raw.mi.dy;
        have_raw = raw.mi.dx || raw.mi.dy;

        if (external_input_active)
        {
            event.flags |= WINE_WAYLAND_EXTERNAL_INPUT_RELATIVE;
            event.dx = round(frame->external_dx);
            event.dy = round(frame->external_dy);
            frame->external_dx -= event.dx;
            frame->external_dy -= event.dy;
        }
    }

    if (frame->flags & WAYLAND_POINTER_FRAME_DISCRETE_WHEEL)
    {
        wheel.mi.mouseData = frame->scroll;
        have_wheel = wheel.mi.mouseData != 0;
    }
    else if (frame->flags & WAYLAND_POINTER_FRAME_AXIS)
    {
        /* many apps cannot handle high resolution scrolling */
        wheel.mi.mouseData = trunc(frame->axis / WHEEL_DELTA) * WHEEL_DELTA;
        frame->axis -= (int)wheel.mi.mouseData;
        have_wheel = wheel.mi.mouseData != 0;
    }
    wheel.mi.dwFlags = MOUSEEVENTF_WHEEL;

    if (frame->flags & WAYLAND_POINTER_FRAME_DISCRETE_WHEEL_HORZ)
    {
        horizontal_wheel.mi.mouseData = frame->horz_scroll;
        have_horizontal_wheel = horizontal_wheel.mi.mouseData != 0;
    }
    else if (frame->flags & WAYLAND_POINTER_FRAME_AXIS_HORZ)
    {
        /* many apps cannot handle high resolution scrolling */
        horizontal_wheel.mi.mouseData = trunc(frame->horz_axis / WHEEL_DELTA) * WHEEL_DELTA;
        frame->horz_axis -= (int)horizontal_wheel.mi.mouseData;
        have_horizontal_wheel = horizontal_wheel.mi.mouseData != 0;
    }
    horizontal_wheel.mi.dwFlags = MOUSEEVENTF_HWHEEL;

    if (frame->flags & WAYLAND_POINTER_FRAME_AXIS_STOP)
        frame->axis = 0.0;

    if (frame->flags & WAYLAND_POINTER_FRAME_AXIS_HORZ_STOP)
        frame->horz_axis = 0.0;

    if (external_input_active)
    {
        event.width = frame->external_width;
        event.height = frame->external_height;
    }
    frame->flags = 0;
    frame->scroll = frame->horz_scroll = 0;
    pthread_mutex_unlock(&pointer->mutex);

    consumed = external_input_active && event.flags && wayland_external_input_emit(&event);
    if (!consumed)
    {
        if (have_absolute) NtUserSendHardwareInput(hwnd, SEND_HWMSG_NO_RAW, &absolute, 0);
        if (have_relative) NtUserSendHardwareInput(hwnd, SEND_HWMSG_NO_RAW, &relative, 0);
        if (have_raw) NtUserSendHardwareInput(0, SEND_HWMSG_NO_MSG, &raw, 0);
    }

    if (have_wheel)
    {
        if (external_input_active)
        {
            event.type = WINE_WAYLAND_EXTERNAL_INPUT_POINTER_AXIS;
            event.code = WL_POINTER_AXIS_VERTICAL_SCROLL;
            event.value = wheel.mi.mouseData;
        }
        if (!external_input_active || !wayland_external_input_emit(&event))
            NtUserSendHardwareInput(hwnd, 0, &wheel, 0);
    }
    if (have_horizontal_wheel)
    {
        if (external_input_active)
        {
            event.type = WINE_WAYLAND_EXTERNAL_INPUT_POINTER_AXIS;
            event.code = WL_POINTER_AXIS_HORIZONTAL_SCROLL;
            event.value = horizontal_wheel.mi.mouseData;
        }
        if (!external_input_active || !wayland_external_input_emit(&event))
            NtUserSendHardwareInput(hwnd, 0, &horizontal_wheel, 0);
    }
}

static void pointer_handle_axis_source(void *data, struct wl_pointer *wl_pointer,
                                       uint32_t axis_source)
{
}

static void pointer_handle_axis_stop(void *data, struct wl_pointer *wl_pointer,
                                     uint32_t time, uint32_t axis)
{
    struct wayland_pointer *pointer = &process_wayland.pointer;
    struct wayland_pointer_frame *frame = &pointer->frame;
    HWND hwnd;

    if (!(hwnd = wayland_pointer_get_focused_hwnd())) return;

    pthread_mutex_lock(&pointer->mutex);
    switch (axis)
    {
        case WL_POINTER_AXIS_HORIZONTAL_SCROLL:
            frame->flags |= WAYLAND_POINTER_FRAME_AXIS_HORZ_STOP;
            break;
        case WL_POINTER_AXIS_VERTICAL_SCROLL:
            frame->flags |= WAYLAND_POINTER_FRAME_AXIS_STOP;
            break;
        default: break;
    }
    pthread_mutex_unlock(&pointer->mutex);

    TRACE("hwnd %p axis %u\n", hwnd, axis);
}

static void pointer_handle_axis_value120(void *data, struct wl_pointer *wl_pointer,
                                         uint32_t axis, int32_t value120)
{
    struct wayland_pointer *pointer = &process_wayland.pointer;
    struct wayland_pointer_frame *frame = &pointer->frame;
    HWND hwnd;

    if (!(hwnd = wayland_pointer_get_focused_hwnd())) return;

    pthread_mutex_lock(&pointer->mutex);
    switch (axis)
    {
        case WL_POINTER_AXIS_VERTICAL_SCROLL:
            frame->flags |= WAYLAND_POINTER_FRAME_DISCRETE_WHEEL;
            frame->scroll += -value120;
            break;
        case WL_POINTER_AXIS_HORIZONTAL_SCROLL:
            frame->flags |= WAYLAND_POINTER_FRAME_DISCRETE_WHEEL_HORZ;
            frame->horz_scroll += value120;
            break;
        default: break;
    }
    pthread_mutex_unlock(&pointer->mutex);

    TRACE("hwnd=%p axis=%u value120=%d\n", hwnd, axis, value120);
}

static void pointer_handle_axis_discrete(void *data, struct wl_pointer *wl_pointer,
                                         uint32_t axis, int32_t discrete)
{
    pointer_handle_axis_value120(data, wl_pointer, axis, WHEEL_DELTA * discrete);
}

static const struct wl_pointer_listener pointer_listener =
{
    pointer_handle_enter,
    pointer_handle_leave,
    pointer_handle_motion,
    pointer_handle_button,
    pointer_handle_axis,
    pointer_handle_frame,
    pointer_handle_axis_source,
    pointer_handle_axis_stop,
    pointer_handle_axis_discrete,
#ifdef WL_POINTER_AXIS_VALUE120_SINCE_VERSION
    pointer_handle_axis_value120
#endif
};

static void relative_pointer_v1_relative_motion(void *private,
                                                struct zwp_relative_pointer_v1 *zwp_relative_pointer_v1,
                                                uint32_t utime_hi, uint32_t utime_lo,
                                                wl_fixed_t dx, wl_fixed_t dy,
                                                wl_fixed_t dx_unaccel, wl_fixed_t dy_unaccel)
{
    HWND hwnd;
    struct wayland_win_data *data;
    double screen_x = 0.0, screen_y = 0.0;
    double raw_x = 0.0, raw_y = 0.0;
    LONG external_width = 0, external_height = 0;
    double external_x = 0.0, external_y = 0.0;
    BOOL external_input_active;
    struct wayland_pointer *pointer = &process_wayland.pointer;
    struct wayland_pointer_frame *frame = &pointer->frame;

    if (!(hwnd = wayland_pointer_get_focused_hwnd())) return;
    if (!(data = wayland_win_data_get(hwnd))) return;

    external_input_active = wayland_external_input_is_active();
    if (external_input_active)
        wayland_surface_delta_to_external_input(data->wayland_surface, data,
                wl_fixed_to_double(dx), wl_fixed_to_double(dy),
                &external_x, &external_y, &external_width, &external_height);

    wayland_surface_delta_to_screen(data->wayland_surface, data,
                                    wl_fixed_to_double(dx),
                                    wl_fixed_to_double(dy),
                                    &screen_x, &screen_y);
    wayland_win_data_release(data);

    raw_x = wl_fixed_to_double(dx_unaccel);
    raw_y = wl_fixed_to_double(dy_unaccel);

    pthread_mutex_lock(&pointer->mutex);

    if (pointer->external_input_active != external_input_active)
    {
        pthread_mutex_unlock(&pointer->mutex);
        return;
    }
    frame->dx_raw += raw_x;
    frame->dy_raw += raw_y;
    frame->dx += screen_x;
    frame->dy += screen_y;
    if (external_input_active)
    {
        frame->external_dx += external_x;
        frame->external_dy += external_y;
        frame->external_width = external_width;
        frame->external_height = external_height;
    }

    frame->flags |= WAYLAND_POINTER_FRAME_RELATIVE;

    pthread_mutex_unlock(&pointer->mutex);

    TRACE("hwnd=%p screen=%.2f,%.2f raw=%.2f,%.2f accum=%.2f,%.2f\n",
          hwnd, screen_x, screen_y, raw_x, raw_y, frame->dx_raw, frame->dy_raw);
}

static const struct zwp_relative_pointer_v1_listener relative_pointer_v1_listener =
{
    relative_pointer_v1_relative_motion
};

static void confined_pointer_v1_confined(void *data,
                                         struct zwp_confined_pointer_v1 *confined_pointer)
{
    struct wayland_pointer *pointer = &process_wayland.pointer;

    pthread_mutex_lock(&pointer->mutex);
    if (pointer->zwp_confined_pointer_v1 == confined_pointer)
    {
        pointer->constraint_state = WAYLAND_POINTER_CONSTRAINT_ACTIVE;
        TRACE("Confinement activated for hwnd=%p\n", pointer->constraint_hwnd);
    }
    pthread_mutex_unlock(&pointer->mutex);
}

static void confined_pointer_v1_unconfined(void *data,
                                           struct zwp_confined_pointer_v1 *confined_pointer)
{
    struct wayland_pointer *pointer = &process_wayland.pointer;

    pthread_mutex_lock(&pointer->mutex);
    if (pointer->zwp_confined_pointer_v1 == confined_pointer)
    {
        pointer->constraint_state = WAYLAND_POINTER_CONSTRAINT_INACTIVE;
        TRACE("Confinement deactivated for hwnd=%p\n", pointer->constraint_hwnd);
    }
    pthread_mutex_unlock(&pointer->mutex);
}

static const struct zwp_confined_pointer_v1_listener confined_pointer_v1_listener =
{
    confined_pointer_v1_confined,
    confined_pointer_v1_unconfined,
};

static void locked_pointer_v1_locked(void *data,
                                     struct zwp_locked_pointer_v1 *locked_pointer)
{
    struct wayland_pointer *pointer = &process_wayland.pointer;

    pthread_mutex_lock(&pointer->mutex);
    if (pointer->zwp_locked_pointer_v1 == locked_pointer)
    {
        pointer->constraint_state = WAYLAND_POINTER_CONSTRAINT_ACTIVE;
        pointer->relative_mode = !pointer->cursor.wl_surface &&
                                 !pointer->wp_cursor_shape_device_v1 &&
                                 pointer->constraint_wl_surface ==
                                 pointer->focused_wl_surface;
        TRACE("Pointer lock activated for hwnd=%p\n", pointer->constraint_hwnd);
    }
    pthread_mutex_unlock(&pointer->mutex);
}

static void locked_pointer_v1_unlocked(void *data,
                                       struct zwp_locked_pointer_v1 *locked_pointer)
{
    struct wayland_pointer *pointer = &process_wayland.pointer;

    pthread_mutex_lock(&pointer->mutex);
    if (pointer->zwp_locked_pointer_v1 == locked_pointer)
    {
        pointer->constraint_state = WAYLAND_POINTER_CONSTRAINT_INACTIVE;
        pointer->relative_mode = FALSE;
        TRACE("Pointer lock deactivated for hwnd=%p\n", pointer->constraint_hwnd);
    }
    pthread_mutex_unlock(&pointer->mutex);
}

static const struct zwp_locked_pointer_v1_listener locked_pointer_v1_listener =
{
    locked_pointer_v1_locked,
    locked_pointer_v1_unlocked,
};

static void wayland_pointer_destroy_constraint(struct wayland_pointer *pointer);

void wayland_pointer_init(struct wl_pointer *wl_pointer)
{
    struct wayland_pointer *pointer = &process_wayland.pointer;

    pthread_mutex_lock(&pointer->mutex);
    pointer->wl_pointer = wl_pointer;
    pointer->focused_wl_surface = NULL;
    pointer->constraint_wl_surface = NULL;
    pointer->focused_hwnd = NULL;
    pointer->constraint_state = WAYLAND_POINTER_CONSTRAINT_NONE;
    pointer->confine_rect_valid = FALSE;
    pointer->enter_serial = 0;
    if (process_wayland.zwp_relative_pointer_manager_v1)
    {
        pointer->zwp_relative_pointer_v1 =
            zwp_relative_pointer_manager_v1_get_relative_pointer(
                process_wayland.zwp_relative_pointer_manager_v1,
                pointer->wl_pointer);
    }
    pthread_mutex_unlock(&pointer->mutex);

    wl_pointer_add_listener(pointer->wl_pointer, &pointer_listener, NULL);
    if (pointer->zwp_relative_pointer_v1)
    {
        zwp_relative_pointer_v1_add_listener(pointer->zwp_relative_pointer_v1,
                                             &relative_pointer_v1_listener,
                                             NULL);
    }
}

void wayland_pointer_deinit(void)
{
    struct wayland_pointer *pointer = &process_wayland.pointer;

    pthread_mutex_lock(&pointer->mutex);
    wayland_pointer_destroy_constraint(pointer);
    if (pointer->zwp_relative_pointer_v1)
    {
        zwp_relative_pointer_v1_destroy(pointer->zwp_relative_pointer_v1);
        pointer->zwp_relative_pointer_v1 = NULL;
    }
    if (pointer->wp_cursor_shape_device_v1)
    {
        wp_cursor_shape_device_v1_destroy(pointer->wp_cursor_shape_device_v1);
        pointer->wp_cursor_shape_device_v1 = NULL;
    }
    wl_pointer_release(pointer->wl_pointer);
    pointer->wl_pointer = NULL;
    pointer->focused_wl_surface = NULL;
    pointer->focused_hwnd = NULL;
    pointer->enter_serial = 0;
    pthread_mutex_unlock(&pointer->mutex);
}

/***********************************************************************
 *           create_mono_cursor_buffer
 *
 * Create a wayland_shm_buffer for a monochrome cursor bitmap.
 *
 * Adapted from wineandroid.drv code.
 */
static struct wayland_shm_buffer *create_mono_cursor_buffer(HBITMAP bmp)
{
    struct wayland_shm_buffer *shm_buffer = NULL;
    BITMAP bm;
    char *mask = NULL;
    unsigned int i, j, stride, mask_size, *ptr;

    if (!NtGdiExtGetObjectW(bmp, sizeof(bm), &bm)) goto done;
    stride = ((bm.bmWidth + 15) >> 3) & ~1;
    mask_size = stride * bm.bmHeight;
    if (!(mask = malloc(mask_size))) goto done;
    if (!NtGdiGetBitmapBits(bmp, mask_size, mask)) goto done;

    bm.bmHeight /= 2;
    shm_buffer = wayland_shm_buffer_create(bm.bmWidth, bm.bmHeight,
                                           WL_SHM_FORMAT_ARGB8888);
    if (!shm_buffer) goto done;

    ptr = shm_buffer->map_data;
    for (i = 0; i < bm.bmHeight; i++)
    {
        for (j = 0; j < bm.bmWidth; j++, ptr++)
        {
            int and = ((mask[i * stride + j / 8] << (j % 8)) & 0x80);
            int xor = ((mask[(i + bm.bmHeight) * stride + j / 8] << (j % 8)) & 0x80);
            if (!xor && and)
                *ptr = 0;
            else if (xor && !and)
                *ptr = 0xffffffff;
            else
                /* we can't draw "invert" pixels, so render them as black instead */
                *ptr = 0xff000000;
        }
    }

done:
    free(mask);
    return shm_buffer;
}

/***********************************************************************
 *           get_icon_info
 *
 * Local GetIconInfoExW helper implementation.
 */
static BOOL get_icon_info(HICON handle, ICONINFOEXW *ret)
{
    UNICODE_STRING module, res_name;
    ICONINFO info;

    module.Buffer = ret->szModName;
    module.MaximumLength = sizeof(ret->szModName) - sizeof(WCHAR);
    res_name.Buffer = ret->szResName;
    res_name.MaximumLength = sizeof(ret->szResName) - sizeof(WCHAR);
    if (!NtUserGetIconInfo(handle, &info, &module, &res_name, NULL, 0)) return FALSE;
    ret->fIcon = info.fIcon;
    ret->xHotspot = info.xHotspot;
    ret->yHotspot = info.yHotspot;
    ret->hbmColor = info.hbmColor;
    ret->hbmMask = info.hbmMask;
    ret->wResID = res_name.Length ? 0 : LOWORD(res_name.Buffer);
    ret->szModName[module.Length] = 0;
    ret->szResName[res_name.Length] = 0;
    return TRUE;
}

static BOOL cursor_buffer_is_transparent(struct wayland_shm_buffer *shm_buffer)
{
    uint32_t *pixel = shm_buffer->map_data;
    uint32_t *end = pixel + shm_buffer->map_size / WINEWAYLAND_BYTES_PER_PIXEL;

    for (; pixel < end; ++pixel)
        if ((*pixel & 0xff000000) != 0) return FALSE;

    return TRUE;
}

static void wayland_pointer_update_cursor_buffer(HCURSOR hcursor, double scale)
{
    struct wayland_cursor *cursor = &process_wayland.pointer.cursor;
    ICONINFOEXW info = {0};

    if (!hcursor) goto clear_cursor;

    /* Create a new buffer for the specified cursor. */
    if (cursor->shm_buffer)
    {
        wayland_shm_buffer_unref(cursor->shm_buffer);
        cursor->shm_buffer = NULL;
    }

    if (!get_icon_info(hcursor, &info))
    {
        ERR("Failed to get icon info for cursor=%p\n", hcursor);
        goto clear_cursor;
    }

    if (info.hbmColor)
    {
        HDC hdc = NtGdiCreateCompatibleDC(0);
        cursor->shm_buffer =
            wayland_shm_buffer_from_color_bitmaps(hdc, info.hbmColor, info.hbmMask, FALSE);
        NtGdiDeleteObjectApp(hdc);
    }
    else
    {
        cursor->shm_buffer = create_mono_cursor_buffer(info.hbmMask);
    }

    if (info.hbmColor) NtGdiDeleteObjectApp(info.hbmColor);
    if (info.hbmMask) NtGdiDeleteObjectApp(info.hbmMask);

    cursor->hotspot_x = info.xHotspot;
    cursor->hotspot_y = info.yHotspot;

    if (!cursor->shm_buffer)
    {
        ERR("Failed to create shm_buffer for cursor=%p\n", hcursor);
        goto clear_cursor;
    }

    if (cursor_buffer_is_transparent(cursor->shm_buffer))
        goto clear_cursor;

    /* Make sure the hotspot is valid. */
    if (cursor->hotspot_x >= cursor->shm_buffer->width ||
        cursor->hotspot_y >= cursor->shm_buffer->height)
    {
        cursor->hotspot_x = cursor->shm_buffer->width / 2;
        cursor->hotspot_y = cursor->shm_buffer->height / 2;
    }

    cursor->hotspot_x = round(cursor->hotspot_x / scale);
    cursor->hotspot_y = round(cursor->hotspot_y / scale);

    return;

clear_cursor:
    if (cursor->shm_buffer)
    {
        wayland_shm_buffer_unref(cursor->shm_buffer);
        cursor->shm_buffer = NULL;
    }
}

static void wayland_pointer_clear_cursor_surface(void)
{
    struct wayland_cursor *cursor = &process_wayland.pointer.cursor;

    if (cursor->wp_viewport)
    {
        wp_viewport_destroy(cursor->wp_viewport);
        cursor->wp_viewport = NULL;
    }
    if (cursor->wl_surface)
    {
        wl_surface_destroy(cursor->wl_surface);
        cursor->wl_surface = NULL;
    }
    if (cursor->shm_buffer)
    {
        wayland_shm_buffer_unref(cursor->shm_buffer);
        cursor->shm_buffer = NULL;
    }
}

static void wayland_pointer_update_cursor_surface(double scale)
{
    struct wayland_cursor *cursor = &process_wayland.pointer.cursor;

    if (!cursor->shm_buffer) goto clear_cursor;

    if (!cursor->wl_surface)
    {
        cursor->wl_surface =
            wl_compositor_create_surface(process_wayland.wl_compositor);
        if (!cursor->wl_surface)
        {
            ERR("Failed to create wl_surface for cursor\n");
            goto clear_cursor;
        }
    }

    if (!cursor->wp_viewport)
    {
        cursor->wp_viewport =
            wp_viewporter_get_viewport(process_wayland.wp_viewporter,
                                       cursor->wl_surface);
        if (!cursor->wp_viewport)
        {
            ERR("Failed to create wp_viewport for cursor\n");
            goto clear_cursor;
        }
    }

    /* Commit the cursor buffer to the cursor surface. */
    wl_surface_attach(cursor->wl_surface,
                      cursor->shm_buffer->wl_buffer, 0, 0);
    wl_surface_damage_buffer(cursor->wl_surface, 0, 0,
                             cursor->shm_buffer->width,
                             cursor->shm_buffer->height);
    /* Setting only the viewport is enough, but some compositors don't
     * support wp_viewport for cursor surfaces, so also set the buffer
     * scale. Note that setting the viewport destination overrides
     * the buffer scale, so it's fine to set both. */
    wl_surface_set_buffer_scale(cursor->wl_surface, round(scale));
    wp_viewport_set_destination(cursor->wp_viewport,
                                round(cursor->shm_buffer->width / scale),
                                round(cursor->shm_buffer->height / scale));
    wl_surface_commit(cursor->wl_surface);

    return;

clear_cursor:
    wayland_pointer_clear_cursor_surface();
}

static void reapply_cursor_clipping(void)
{
    RECT rect;
    UINT context = NtUserSetThreadDpiAwarenessContext(NTUSER_DPI_PER_MONITOR_AWARE);
    if (NtUserGetClipCursor(&rect)) NtUserClipCursor(&rect);
    NtUserSetThreadDpiAwarenessContext(context);
}

static enum wp_cursor_shape_device_v1_shape cursor_shape_from_info(ICONINFOEXW *info,
                                                                   uint32_t proto_version)
{
    const struct system_cursors *cursors;
    const WCHAR *module;
    unsigned int i;
    enum wp_cursor_shape_device_v1_shape shape = 0;

    if (!info->szModName[0]) return 0;
    if ((module = wcsrchr(info->szModName, '\\'))) module++;
    else module = info->szModName;
    for (i = 0; i < ARRAY_SIZE(module_cursors); i++)
        if (!wcsicmp(module, module_cursors[i].name)) break;
    if (i == ARRAY_SIZE(module_cursors)) return 0;

    cursors = module_cursors[i].cursors;
    for (i = 0; cursors[i].id; i++)
    {
        if (cursors[i].id == info->wResID)
        {
            shape = cursors[i].shape;
            break;
        }
    }

    if (shape >= WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_DND_ASK && proto_version < 2)
        shape = 0;

    return shape;
}

static BOOL wayland_pointer_set_cursor_shape(HCURSOR hcursor)
{
    struct wayland_pointer *pointer = &process_wayland.pointer;
    ICONINFOEXW info = {0};
    enum wp_cursor_shape_device_v1_shape shape = 0;
    uint32_t proto_version;

    if (!process_wayland.wp_cursor_shape_manager_v1) return FALSE;
    if (!hcursor) return FALSE;
    if (!get_icon_info(hcursor, &info)) return FALSE;
    proto_version = wp_cursor_shape_manager_v1_get_version(
        process_wayland.wp_cursor_shape_manager_v1);
    shape = cursor_shape_from_info(&info, proto_version);

    if (info.hbmColor) NtGdiDeleteObjectApp(info.hbmColor);
    if (info.hbmMask) NtGdiDeleteObjectApp(info.hbmMask);

    if (!shape) return FALSE;

    if (!pointer->wp_cursor_shape_device_v1)
    {
        pointer->wp_cursor_shape_device_v1 =
            wp_cursor_shape_manager_v1_get_pointer(
                process_wayland.wp_cursor_shape_manager_v1, pointer->wl_pointer);
        if (!pointer->wp_cursor_shape_device_v1) return FALSE;
    }

    wp_cursor_shape_device_v1_set_shape(pointer->wp_cursor_shape_device_v1,
                                        pointer->enter_serial, shape);

    return TRUE;
}

static void wayland_pointer_clear_cursor_shape(void)
{
    struct wayland_pointer *pointer = &process_wayland.pointer;

    if (pointer->wp_cursor_shape_device_v1)
    {
        wp_cursor_shape_device_v1_destroy(pointer->wp_cursor_shape_device_v1);
        pointer->wp_cursor_shape_device_v1 = NULL;
    }
}

static BOOL wayland_pointer_set_external_cursor(struct wayland_pointer *pointer)
{
    if (!pointer->wl_pointer || !pointer->enter_serial ||
        !process_wayland.wp_cursor_shape_manager_v1)
        return FALSE;

    if (!pointer->wp_cursor_shape_device_v1)
        pointer->wp_cursor_shape_device_v1 =
            wp_cursor_shape_manager_v1_get_pointer(
                process_wayland.wp_cursor_shape_manager_v1, pointer->wl_pointer);
    if (!pointer->wp_cursor_shape_device_v1) return FALSE;

    wayland_pointer_clear_cursor_surface();
    wp_cursor_shape_device_v1_set_shape(pointer->wp_cursor_shape_device_v1,
                                        pointer->enter_serial,
                                        WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_DEFAULT);
    return TRUE;
}

static void wayland_set_cursor(HWND hwnd, HCURSOR hcursor, BOOL use_hcursor)
{
    struct wayland_pointer *pointer = &process_wayland.pointer;
    struct wayland_surface *surface;
    struct wayland_win_data *data;
    double scale;
    BOOL reapply_clip = FALSE;

    if ((data = wayland_win_data_get(hwnd)))
    {
        if (!(surface = data->wayland_surface))
        {
            wayland_win_data_release(data);
            return;
        }
        scale = surface->window.scale;
        if (use_hcursor) surface->hcursor = hcursor;
        else hcursor = surface->hcursor;
        use_hcursor = TRUE;
        wayland_win_data_release(data);
    }
    else
    {
        scale = 1.0;
    }

    pthread_mutex_lock(&pointer->mutex);
    if (pointer->focused_hwnd == hwnd)
    {
        if (pointer->external_input_active)
        {
            wayland_pointer_set_external_cursor(pointer);
        }
        else if ((!use_hcursor && pointer->wp_cursor_shape_device_v1) ||
            (use_hcursor && hcursor && wayland_pointer_set_cursor_shape(hcursor)))
        {
            wayland_pointer_clear_cursor_surface();
        }
        else
        {
            if (use_hcursor) wayland_pointer_update_cursor_buffer(hcursor, scale);
            wayland_pointer_update_cursor_surface(scale);
            wl_pointer_set_cursor(pointer->wl_pointer,
                                  pointer->enter_serial,
                                  pointer->cursor.wl_surface,
                                  pointer->cursor.hotspot_x,
                                  pointer->cursor.hotspot_y);
            wayland_pointer_clear_cursor_shape();
        }
        wl_display_flush(process_wayland.wl_display);
        reapply_clip = TRUE;
    }
    pthread_mutex_unlock(&pointer->mutex);

    /* Reapply cursor clip since cursor visibility affects pointer constraint
     * behavior. */
    if (reapply_clip) reapply_cursor_clipping();
}

/**********************************************************************
 *          wayland_surface_calc_confine
 *
 * Calculates the pointer confine rect (in surface-local coords)
 * for the specified clip rectangle (in screen coords using thread dpi).
 */
static void wayland_surface_calc_confine(struct wayland_surface *surface,
                                         const struct wayland_win_data *data,
                                         const RECT *clip, RECT *confine)
{
    double left, top, right, bottom;
    RECT input_rect, window_clip;

    input_rect = wayland_surface_get_input_rect(surface, data);

    TRACE("hwnd=%p clip=%s input=%s\n",
          surface->hwnd, wine_dbgstr_rect(clip),
          wine_dbgstr_rect(&input_rect));

    /* FIXME: surface->window.(client_)rect is in window dpi, whereas
     * clip is in thread dpi. */

    if (!intersect_rect(&window_clip, clip, &input_rect))
    {
        SetRectEmpty(confine);
        return;
    }

    wayland_surface_coords_from_screen(surface, data, window_clip.left,
                                       window_clip.top, &left, &top);
    wayland_surface_coords_from_screen(surface, data, window_clip.right,
                                       window_clip.bottom, &right, &bottom);
    SetRect(confine, round(left), round(top), round(right), round(bottom));
}

static BOOL wayland_clip_covers_vscreen(const RECT *clip)
{
    RECT vscreen = NtUserGetVirtualScreenRect(MDT_RAW_DPI);

    return clip->left <= vscreen.left && clip->top <= vscreen.top &&
           clip->right >= vscreen.right && clip->bottom >= vscreen.bottom;
}

/***********************************************************************
 *           wayland_pointer_update_constraint
 *
 *  Enables/disables pointer confinement.
 */
static BOOL wayland_pointer_needs_lock(struct wayland_pointer *pointer,
                                       struct wl_surface *wl_surface,
                                       const RECT *confine_rect,
                                       BOOL covers_vscreen, BOOL force_lock)
{
    BOOL is_visible = pointer->cursor.wl_surface || pointer->wp_cursor_shape_device_v1;

    return wl_surface && (((confine_rect || covers_vscreen) && !is_visible) || force_lock) &&
           pointer->wl_pointer;
}

static void wayland_pointer_destroy_constraint(struct wayland_pointer *pointer)
{
    if (pointer->zwp_confined_pointer_v1)
    {
        zwp_confined_pointer_v1_destroy(pointer->zwp_confined_pointer_v1);
        pointer->zwp_confined_pointer_v1 = NULL;
    }
    if (pointer->zwp_locked_pointer_v1)
    {
        zwp_locked_pointer_v1_destroy(pointer->zwp_locked_pointer_v1);
        pointer->zwp_locked_pointer_v1 = NULL;
    }
    pointer->constraint_hwnd = NULL;
    pointer->constraint_wl_surface = NULL;
    pointer->constraint_state = WAYLAND_POINTER_CONSTRAINT_NONE;
    pointer->confine_rect_valid = FALSE;
    pointer->relative_mode = FALSE;
}

static BOOL wayland_pointer_update_constraint(struct wl_surface *wl_surface,
                                              const RECT *confine_rect,
                                              BOOL covers_vscreen,
                                              BOOL force_lock)
{
    struct wayland_pointer *pointer = &process_wayland.pointer;
    BOOL commit_region = FALSE, needs_relative, needs_lock, needs_confine, is_visible;
    static unsigned int once;

    if (!process_wayland.zwp_pointer_constraints_v1)
    {
        if (!once++)
            ERR("This function requires zwp_pointer_constraints_v1\n");
        return FALSE;
    }

    is_visible = pointer->cursor.wl_surface || pointer->wp_cursor_shape_device_v1;
    needs_lock = wayland_pointer_needs_lock(pointer, wl_surface, confine_rect,
                                            covers_vscreen, force_lock);
    needs_confine = wl_surface && confine_rect && is_visible && !force_lock &&
                    pointer->wl_pointer;

    if (!needs_confine && pointer->zwp_confined_pointer_v1)
    {
        TRACE("Unconfining from hwnd=%p\n", pointer->constraint_hwnd);
        wayland_pointer_destroy_constraint(pointer);
    }

    if (!needs_lock && pointer->zwp_locked_pointer_v1)
    {
        TRACE("Unlocking from hwnd=%p\n", pointer->constraint_hwnd);
        wayland_pointer_destroy_constraint(pointer);
    }

    if (needs_confine)
    {
        HWND hwnd = wl_surface_get_user_data(wl_surface);
        BOOL recreate = !pointer->zwp_confined_pointer_v1 ||
                        pointer->constraint_wl_surface != wl_surface ||
                        (pointer->constraint_state == WAYLAND_POINTER_CONSTRAINT_INACTIVE &&
                         pointer->focused_wl_surface == wl_surface);
        BOOL region_changed = !pointer->confine_rect_valid ||
                              !EqualRect(&pointer->confine_rect, confine_rect);
        struct wl_region *region = NULL;

        if (recreate || region_changed)
        {
            region = wl_compositor_create_region(process_wayland.wl_compositor);
            wl_region_add(region, confine_rect->left, confine_rect->top,
                          confine_rect->right - confine_rect->left,
                          confine_rect->bottom - confine_rect->top);
        }

        if (recreate)
        {
            wayland_pointer_destroy_constraint(pointer);
            pointer->zwp_confined_pointer_v1 =
                zwp_pointer_constraints_v1_confine_pointer(
                    process_wayland.zwp_pointer_constraints_v1,
                    wl_surface,
                    pointer->wl_pointer,
                    region,
                    ZWP_POINTER_CONSTRAINTS_V1_LIFETIME_PERSISTENT);
            pointer->constraint_hwnd = hwnd;
            pointer->constraint_wl_surface = wl_surface;
            pointer->constraint_state = WAYLAND_POINTER_CONSTRAINT_PENDING;
            zwp_confined_pointer_v1_add_listener(pointer->zwp_confined_pointer_v1,
                                                 &confined_pointer_v1_listener,
                                                 NULL);
        }
        else if (region_changed)
        {
            zwp_confined_pointer_v1_set_region(pointer->zwp_confined_pointer_v1,
                                               region);
            commit_region = TRUE;
        }

        pointer->confine_rect = *confine_rect;
        pointer->confine_rect_valid = TRUE;

        TRACE("Confining to hwnd=%p wayland=%d,%d+%d,%d state=%u%s\n",
              pointer->constraint_hwnd,
              confine_rect->left, confine_rect->top,
              confine_rect->right - confine_rect->left,
              confine_rect->bottom - confine_rect->top,
              pointer->constraint_state, recreate ? " recreated" : "");

        if (region) wl_region_destroy(region);
    }
    else if (needs_lock)
    {
        HWND hwnd = wl_surface_get_user_data(wl_surface);
        BOOL recreate = !pointer->zwp_locked_pointer_v1 ||
                        pointer->constraint_wl_surface != wl_surface ||
                        (pointer->constraint_state == WAYLAND_POINTER_CONSTRAINT_INACTIVE &&
                         pointer->focused_wl_surface == wl_surface);

        if (recreate)
        {
            wayland_pointer_destroy_constraint(pointer);
            pointer->zwp_locked_pointer_v1 =
                zwp_pointer_constraints_v1_lock_pointer(
                    process_wayland.zwp_pointer_constraints_v1,
                    wl_surface,
                    pointer->wl_pointer,
                    NULL,
                    ZWP_POINTER_CONSTRAINTS_V1_LIFETIME_PERSISTENT);
            pointer->constraint_hwnd = hwnd;
            pointer->constraint_wl_surface = wl_surface;
            pointer->constraint_state = WAYLAND_POINTER_CONSTRAINT_PENDING;
            zwp_locked_pointer_v1_add_listener(pointer->zwp_locked_pointer_v1,
                                               &locked_pointer_v1_listener,
                                               NULL);
            TRACE("Locking to hwnd=%p\n", pointer->constraint_hwnd);
        }
    }

    if (!process_wayland.zwp_relative_pointer_manager_v1)
    {
        if (!once++)
            ERR("zwp_relative_pointer_manager_v1 isn't supported, skipping relative motion\n");
        return commit_region;
    }

    needs_relative = !is_visible && pointer->zwp_locked_pointer_v1 &&
                     pointer->constraint_state == WAYLAND_POINTER_CONSTRAINT_ACTIVE &&
                     pointer->constraint_wl_surface == pointer->focused_wl_surface;

    if (needs_relative != pointer->relative_mode)
    {
        pointer->relative_mode = needs_relative;
        TRACE("%s relative motion\n", needs_relative ? "Enabling" : "Disabling");
    }

    return commit_region;
}

void wayland_pointer_clear_constraint(void)
{
    wayland_pointer_update_constraint(NULL, NULL, FALSE, FALSE);
}

void wayland_pointer_set_external_input_active(BOOL active)
{
    struct wayland_pointer *pointer = &process_wayland.pointer;
    HWND hwnd;

    pthread_mutex_lock(&pointer->mutex);
    if (pointer->external_input_active == active)
    {
        pthread_mutex_unlock(&pointer->mutex);
        return;
    }

    pointer->external_input_active = active;
    hwnd = pointer->focused_hwnd;
    if (active)
    {
        wayland_pointer_destroy_constraint(pointer);
        pointer->pending_warp = FALSE;
        wayland_pointer_reset_frame();
        wayland_pointer_set_external_cursor(pointer);
    }
    pthread_mutex_unlock(&pointer->mutex);

    if (active)
        wl_display_flush(process_wayland.wl_display);
    else
    {
        if (hwnd) wayland_set_cursor(hwnd, NULL, FALSE);
        reapply_cursor_clipping();
    }

    TRACE("external input is now %s\n", active ? "active" : "inactive");
}

/***********************************************************************
 *           WAYLAND_SetCursor
 */
void WAYLAND_SetCursor(HWND hwnd, HCURSOR hcursor)
{
    TRACE("hwnd=%p hcursor=%p\n", hwnd, hcursor);

    wayland_set_cursor(hwnd, hcursor, TRUE);
}

/***********************************************************************
 *           WAYLAND_SetCursorPos
 */
BOOL WAYLAND_SetCursorPos(INT x, INT y)
{
    struct wayland_pointer *pointer = &process_wayland.pointer;

    pthread_mutex_lock(&pointer->mutex);
    if (pointer->external_input_active)
    {
        pointer->pending_warp = FALSE;
        pthread_mutex_unlock(&pointer->mutex);
        return TRUE;
    }
    if (pointer->relative_mode)
    {
        pthread_mutex_unlock(&pointer->mutex);
        return FALSE;
    }
    pointer->pending_warp = TRUE;
    pointer->warp.x = x;
    pointer->warp.y = y;
    pthread_mutex_unlock(&pointer->mutex);

    TRACE("warping to %d,%d\n", x, y);
    reapply_cursor_clipping();
    return TRUE;
}

static void wayland_pointer_commit_surface_state(HWND hwnd, struct wl_surface *wl_surface)
{
    struct wayland_surface *surface;
    struct wayland_win_data *data;

    if (!(data = wayland_win_data_get(hwnd))) return;
    surface = data->wayland_surface;
    if (surface && surface->wl_surface == wl_surface)
    {
        wayland_surface_mark_pending_commit(surface);
        wayland_surface_commit_pending_state(surface);
    }
    wayland_win_data_release(data);
}

/***********************************************************************
 *	     WAYLAND_ClipCursor
 */
BOOL WAYLAND_ClipCursor(const RECT *clip, BOOL reset)
{
    struct wayland_pointer *pointer = &process_wayland.pointer;
    HWND hwnd;
    struct wl_surface *wl_surface = NULL;
    struct wayland_surface *surface = NULL;
    struct wayland_win_data *data;
    BOOL commit_constraint_region, commit_position_hint = FALSE, covers_vscreen = FALSE;
    const RECT *confine_rect = NULL;
    RECT surface_clip;
    POINT cursor_pos, warp;
    double warp_x, warp_y;

    TRACE("clip=%s reset=%d\n", wine_dbgstr_rect(clip), reset);

    if (wayland_external_input_is_active())
        return TRUE;

    NtUserGetCursorPos(&cursor_pos);
    hwnd = NtUserGetForegroundWindow();

    /* the cursor pos may have changed between SetCursorPos and ClipCursor calls */
    pthread_mutex_lock(&pointer->mutex);
    if (pointer->pending_warp) cursor_pos = pointer->warp;
    pthread_mutex_unlock(&pointer->mutex);

    if (!(data = wayland_win_data_get(hwnd))) return FALSE;
    if ((surface = data->wayland_surface))
    {
        wl_surface = surface->wl_surface;
        if (clip && !wayland_clip_covers_vscreen(clip))
        {
            wayland_surface_calc_confine(surface, data, clip, &surface_clip);
            confine_rect = &surface_clip;
        }
        covers_vscreen = wayland_win_data_is_fullscreen(data) &&
                          wayland_win_data_covers_virtual_screen(data);
        wayland_surface_coords_from_screen(surface, data, cursor_pos.x,
                                           cursor_pos.y, &warp_x, &warp_y);
        warp.x = round(warp_x);
        warp.y = round(warp_y);
    }
    wayland_win_data_release(data);

    pthread_mutex_lock(&pointer->mutex);
    if (wl_surface && pointer->pending_warp)
    {
        if (process_wayland.wp_pointer_warp_v1)
        {
            wp_pointer_warp_v1_warp_pointer(
                    process_wayland.wp_pointer_warp_v1,
                    wl_surface,
                    pointer->wl_pointer,
                    wl_fixed_from_int(warp.x),
                    wl_fixed_from_int(warp.y),
                    pointer->enter_serial);
            TRACE("warp_pointer hwnd=%p wayland_xy=%s screen_xy=%s\n",
                    hwnd, wine_dbgstr_point(&warp), wine_dbgstr_point(&cursor_pos));
        }
        else
        {
            wayland_pointer_update_constraint(wl_surface, NULL, FALSE, TRUE);
        }
        pointer->pending_warp = FALSE;
    }

    if (wl_surface && wl_surface == pointer->constraint_wl_surface &&
        pointer->zwp_locked_pointer_v1)
    {
        zwp_locked_pointer_v1_set_cursor_position_hint(
                pointer->zwp_locked_pointer_v1,
                wl_fixed_from_int(warp.x),
                wl_fixed_from_int(warp.y));
        commit_position_hint =
                !wayland_pointer_needs_lock(pointer, wl_surface,
                                            confine_rect,
                                            covers_vscreen, FALSE);
    }
    pthread_mutex_unlock(&pointer->mutex);

    if (commit_position_hint)
    {
        wayland_pointer_commit_surface_state(hwnd, wl_surface);
        TRACE("position hint hwnd=%p wayland_xy=%s screen_xy=%s\n",
                hwnd, wine_dbgstr_point(&warp), wine_dbgstr_point(&cursor_pos));
    }

    pthread_mutex_lock(&pointer->mutex);

    /* Since we are running in the context of the foreground thread we know
    * that the wl_surface of the foreground HWND will not be invalidated,
    * so we can access it without having the win data lock. */
    commit_constraint_region = wayland_pointer_update_constraint(wl_surface, confine_rect,
                                                                 covers_vscreen, FALSE);
    pthread_mutex_unlock(&pointer->mutex);

    if (commit_constraint_region)
        wayland_pointer_commit_surface_state(hwnd, wl_surface);

    wl_display_flush(process_wayland.wl_display);

    return TRUE;
}
