/*
 * Private contract between winewayland.drv and the Wineland Vulkan
 * translation layer.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#ifndef __WINE_WAYLAND_VULKAN_PROXY_H
#define __WINE_WAYLAND_VULKAN_PROXY_H

#include <stdint.h>

/* Private entry point exported by the loaded translation layer. Registration
 * is scoped to the following vkCreateXlibSurfaceKHR call. */
#define WINE_WAYLAND_VK_REGISTER_SURFACE_SYMBOL "__wine_wayland_vulkan_register_surface_v1"

typedef int32_t (*wine_wayland_vk_register_surface_func)(
        void *instance, void *x11_display, uint64_t x11_window,
        void *wl_display, void *wl_surface);

#define WINE_WAYLAND_VULKAN_PROXY_VERSION 1
#define WINE_WAYLAND_VULKAN_PROXY_SYMBOL "__wine_wayland_vulkan_proxy_get_v1"

struct wine_wayland_vulkan_proxy
{
    uint32_t size;
    uint32_t version;
    void *display;
    uint64_t window;
};

#endif /* __WINE_WAYLAND_VULKAN_PROXY_H */
