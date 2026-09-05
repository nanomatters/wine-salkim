/*
 * Private native OpenGL overlay swap hook. EGL objects stay opaque here;
 * the supplied callback performs Wine's real EGL swap exactly once.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#ifndef __WINE_WAYLAND_OPENGL_OVERLAY_H
#define __WINE_WAYLAND_OPENGL_OVERLAY_H

#include <stdint.h>

#define WINE_WAYLAND_OPENGL_OVERLAY_SYMBOL "__wineland_overlay_gl_swap_buffers_v1"

typedef uint32_t (*wine_wayland_egl_swap_func)(void *display, void *surface);
typedef uint32_t (*wine_wayland_opengl_overlay_swap_func)(
        void *display, void *surface, int width, int height,
        wine_wayland_egl_swap_func swap);

#endif /* __WINE_WAYLAND_OPENGL_OVERLAY_H */
