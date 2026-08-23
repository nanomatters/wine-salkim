/*
 * X11 input proxy window for the native Steam overlay
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#if 0
#pragma makedep unix
#endif

#include "config.h"

#include <dlfcn.h>
#include <limits.h>
#include <link.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "waylanddrv.h"
#include "wine/debug.h"
#include "wine/wayland_vulkan_proxy.h"

/* X11 headers last: X.h defines macros (ControlMask, ...) that collide with
 * struct members in the Windows headers, and Xlib typedefs Status. */
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>

WINE_DEFAULT_DEBUG_CHANNEL(vulkan);

static pthread_once_t proxy_once = PTHREAD_ONCE_INIT;
static pthread_once_t translator_once = PTHREAD_ONCE_INIT;
static pthread_mutex_t proxy_mutex = PTHREAD_MUTEX_INITIALIZER;
static void *x11_handle;
static void *translator_handle;
static Display *proxy_display;
static Window proxy_window;
static wine_wayland_vk_register_surface_func p_register_surface;

static int (*p_XInitThreads)(void);
static Display *(*p_XOpenDisplay)(const char *);
static int (*p_XCloseDisplay)(Display *);
static Window (*p_XDefaultRootWindow)(Display *);
static Window (*p_XCreateWindow)(Display *, Window, int, int, unsigned int, unsigned int,
                                 unsigned int, int, unsigned int, Visual *, unsigned long,
                                 XSetWindowAttributes *);
static int (*p_XDestroyWindow)(Display *, Window);
static int (*p_XMapWindow)(Display *, Window);
static int (*p_XSync)(Display *, Bool);
static int (*p_XSetClassHint)(Display *, Window, XClassHint *);
static int (*p_XStoreName)(Display *, Window, const char *);
static Atom (*p_XInternAtom)(Display *, const char *, Bool);
static int (*p_XChangeProperty)(Display *, Window, Atom, Atom, int, int,
                                const unsigned char *, int);

static void wayland_vk_proxy_init(void)
{
    XSetWindowAttributes attrs = {0};
    char class_name[128];
    const char *appid;
    XClassHint class_hint;
    unsigned long pid;
    Atom net_wm_pid;

    /* This initializer may run on lsteamclient's native pthread, which has no
     * Wine thread state for debug logging. */
    if (!(x11_handle = dlopen("libX11.so.6", RTLD_NOW | RTLD_LOCAL))) return;

#define LOAD_FUNCPTR(f) if (!(p_##f = dlsym(RTLD_DEFAULT, #f))) goto failed
    LOAD_FUNCPTR(XInitThreads);
    LOAD_FUNCPTR(XOpenDisplay);
    LOAD_FUNCPTR(XCloseDisplay);
    LOAD_FUNCPTR(XDefaultRootWindow);
    LOAD_FUNCPTR(XCreateWindow);
    LOAD_FUNCPTR(XDestroyWindow);
    LOAD_FUNCPTR(XMapWindow);
    LOAD_FUNCPTR(XSync);
    LOAD_FUNCPTR(XSetClassHint);
    LOAD_FUNCPTR(XStoreName);
    LOAD_FUNCPTR(XInternAtom);
    LOAD_FUNCPTR(XChangeProperty);
#undef LOAD_FUNCPTR

    p_XInitThreads();

    if (!(proxy_display = p_XOpenDisplay(NULL))) goto failed;

    /* Resolve Xlib through RTLD_DEFAULT so the overlay observes the same
     * display and window that it later records for the Vulkan surface. The
     * window is presentable Xlib state, but rendering is translated to the
     * Wayland surface before reaching the driver. */
    attrs.override_redirect = True;
    attrs.event_mask = FocusChangeMask | KeyPressMask | KeyReleaseMask |
                       PointerMotionMask | ButtonPressMask | ButtonReleaseMask;
    proxy_window = p_XCreateWindow(proxy_display, p_XDefaultRootWindow(proxy_display),
                                   -10000, -10000, 32, 32, 0, CopyFromParent, InputOutput,
                                   CopyFromParent, CWOverrideRedirect | CWEventMask, &attrs);
    if (!proxy_window) goto failed;

    if ((appid = getenv("SteamAppId")) && *appid)
        snprintf(class_name, sizeof(class_name), "steam_app_%s", appid);
    else
        snprintf(class_name, sizeof(class_name), "steam_proton");
    class_hint.res_name = class_name;
    class_hint.res_class = class_name;
    p_XSetClassHint(proxy_display, proxy_window, &class_hint);
    p_XStoreName(proxy_display, proxy_window, class_name);

    pid = getpid();
    net_wm_pid = p_XInternAtom(proxy_display, "_NET_WM_PID", False);
    p_XChangeProperty(proxy_display, proxy_window, net_wm_pid, XA_CARDINAL, 32,
                      PropModeReplace, (const unsigned char *)&pid, 1);

    p_XMapWindow(proxy_display, proxy_window);
    p_XSync(proxy_display, False);

    return;

failed:
    if (proxy_window && proxy_display && p_XDestroyWindow)
        p_XDestroyWindow(proxy_display, proxy_window);
    if (proxy_display && p_XCloseDisplay) p_XCloseDisplay(proxy_display);
    proxy_window = 0;
    proxy_display = NULL;
    dlclose(x11_handle);
    x11_handle = NULL;
}

static BOOL wayland_vulkan_proxy_get(void **display, UINT64 *window)
{
    pthread_once(&proxy_once, wayland_vk_proxy_init);
    if (!proxy_window) return FALSE;
    *display = proxy_display;
    *window = proxy_window;
    return TRUE;
}

struct translator_search
{
    const char *name;
    char path[PATH_MAX];
};

static int find_translator_module(struct dl_phdr_info *info, size_t size, void *arg)
{
    struct translator_search *search = arg;
    const char *name;

    (void)size;
    if (!(name = strrchr(info->dlpi_name, '/'))) name = info->dlpi_name;
    else name++;
    if (strcmp(name, search->name)) return 0;
    snprintf(search->path, sizeof(search->path), "%s", info->dlpi_name);
    return 1;
}

static void wayland_vk_translator_init(void)
{
#if defined(__x86_64__)
    struct translator_search search = {.name = "libVkLayer_WINELAND_translate_x86_64.so"};
#elif defined(__i386__)
    struct translator_search search = {.name = "libVkLayer_WINELAND_translate_i386.so"};
#else
    struct translator_search search = {0};
#endif

    if (!search.name || !dl_iterate_phdr(find_translator_module, &search) ||
        !(translator_handle = dlopen(search.path, RTLD_NOW | RTLD_LOCAL | RTLD_NOLOAD)) ||
        !(p_register_surface = dlsym(translator_handle,
                                     WINE_WAYLAND_VK_REGISTER_SURFACE_SYMBOL)))
    {
        WARN("The Wineland Vulkan surface translation layer is unavailable\n");
        if (translator_handle) dlclose(translator_handle);
        translator_handle = NULL;
        p_register_surface = NULL;
    }
}

BOOL wayland_vulkan_proxy_create_surface(const struct vulkan_instance *instance,
                                         struct wl_surface *wl_surface,
                                         VkSurfaceKHR *surface, VkResult *result)
{
    VkXlibSurfaceCreateInfoKHR create_info =
    {
        .sType = VK_STRUCTURE_TYPE_XLIB_SURFACE_CREATE_INFO_KHR,
    };
    VkResult register_result;
    UINT64 window;

    pthread_once(&translator_once, wayland_vk_translator_init);
    if (!p_register_surface || !instance->p_vkCreateXlibSurfaceKHR ||
        !wayland_vulkan_proxy_get((void **)&create_info.dpy, &window))
        return FALSE;
    create_info.window = window;

    pthread_mutex_lock(&proxy_mutex);
    register_result = p_register_surface(instance->host.instance, create_info.dpy,
                                         create_info.window, process_wayland.wl_display,
                                         wl_surface);
    if (register_result == VK_SUCCESS)
    {
        *result = instance->p_vkCreateXlibSurfaceKHR(instance->host.instance, &create_info,
                                                     NULL /* allocator */, surface);
        p_register_surface(instance->host.instance, create_info.dpy, create_info.window,
                           NULL, NULL);
    }
    else *result = register_result;
    pthread_mutex_unlock(&proxy_mutex);
    return TRUE;
}

/* The input bridge must use this exact Display connection because X event
 * queues and input contexts are connection-local. */
DECLSPEC_EXPORT BOOL __wine_wayland_vulkan_proxy_get_v1(
        struct wine_wayland_vulkan_proxy *proxy)
{
    void *display;
    UINT64 window;

    if (!proxy || proxy->size < sizeof(*proxy) ||
        proxy->version != WINE_WAYLAND_VULKAN_PROXY_VERSION ||
        !wayland_vulkan_proxy_get(&display, &window))
        return FALSE;

    proxy->display = display;
    proxy->window = window;
    return TRUE;
}
