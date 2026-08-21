/*
 * Process-local external input interface for winewayland.drv
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#ifndef __WINE_WAYLAND_EXTERNAL_INPUT_H
#define __WINE_WAYLAND_EXTERNAL_INPUT_H

#include <stdint.h>

#define WINE_WAYLAND_EXTERNAL_INPUT_VERSION 1
#define WINE_WAYLAND_EXTERNAL_INPUT_SYMBOL "__wine_wayland_external_input_v1"

enum wine_wayland_external_input_type
{
    WINE_WAYLAND_EXTERNAL_INPUT_POINTER_MOTION,
    WINE_WAYLAND_EXTERNAL_INPUT_POINTER_BUTTON,
    WINE_WAYLAND_EXTERNAL_INPUT_POINTER_AXIS,
    WINE_WAYLAND_EXTERNAL_INPUT_KEY,
    WINE_WAYLAND_EXTERNAL_INPUT_FOCUS,
};

enum wine_wayland_external_input_flags
{
    WINE_WAYLAND_EXTERNAL_INPUT_ABSOLUTE = 1u << 0,
    WINE_WAYLAND_EXTERNAL_INPUT_RELATIVE = 1u << 1,
};

struct wine_wayland_external_input_event
{
    uint32_t size;
    uint32_t type;
    uint32_t time;
    uint32_t flags;
    int32_t x;
    int32_t y;
    int32_t dx;
    int32_t dy;
    int32_t width;
    int32_t height;
    int32_t value;
    uint32_t code;
    uint32_t state;
};

struct wine_wayland_external_input_handler
{
    uint32_t size;
    uint32_t version;
    void *context;
    int (*event)(void *context, const struct wine_wayland_external_input_event *event);
};

struct wine_wayland_external_input_api
{
    uint32_t size;
    uint32_t version;
    int (*set_handler)(const struct wine_wayland_external_input_handler *handler);
    int (*set_active)(int active);
};

#endif /* __WINE_WAYLAND_EXTERNAL_INPUT_H */
