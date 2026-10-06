/*
 * WAYLANDDRV initialization code
 *
 * Copyright 2020 Alexandre Frantzis for Collabora Ltd
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
#include <errno.h>
#include <poll.h>
#include <sys/epoll.h>
#include <unistd.h>

#include "ntstatus.h"
#define WIN32_NO_STATUS

#include "waylanddrv.h"
#include "dmabuf.h"

WINE_DEFAULT_DEBUG_CHANNEL(waylanddrv);

char *process_name = NULL;
static char *process_activate_token;
static int dmabuf_epoll_fd = -1;
static pthread_mutex_t reader_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t reader_cond = PTHREAD_COND_INITIALIZER;
static HWND reader_desktop;
static BOOL reader_ready;

static void wayland_read_events_thread(void *arg);

static void WAYLAND_SetDesktopWindow(HWND hwnd)
{
    pthread_mutex_lock(&reader_mutex);
    if (!reader_desktop) reader_desktop = hwnd;
    pthread_cond_signal(&reader_cond);
    pthread_mutex_unlock(&reader_mutex);
}

/* The event contains an identity, not a surface pointer: it may outlive both
 * the registration and the HWND. Callers serialize changes with win_data_mutex. */
BOOL wayland_surface_monitor_fd(struct wayland_surface *surface, int fd)
{
    struct epoll_event event = {.events = EPOLLIN | EPOLLET};

    event.data.u64 = (UINT64)(UINT32)surface->serial << 32 | HandleToULong(surface->hwnd);
    if (!epoll_ctl(dmabuf_epoll_fd, EPOLL_CTL_ADD, fd, &event)) return TRUE;
    ERR("Failed to monitor frame fd %d for hwnd %p: %s\n", fd, surface->hwnd, strerror(errno));
    return FALSE;
}

void wayland_surface_unmonitor_fd(int fd)
{
    /* Explicitly remove it: buffer/feedback objects may retain dup'd fds. */
    epoll_ctl(dmabuf_epoll_fd, EPOLL_CTL_DEL, fd, NULL);
}

static UINT WAYLAND_GetForeignGdiSurfaceCaps(void)
{
    return process_wayland.wl_shm ? WINE_GDI_FOREIGN_SURFACE_SHM : 0;
}

static const struct user_driver_funcs waylanddrv_funcs =
{
    .pActivateWindow = WAYLAND_ActivateWindow,
    .pClipboardWindowProc = WAYLAND_ClipboardWindowProc,
    .pClipCursor = WAYLAND_ClipCursor,
    .pDesktopWindowProc = WAYLAND_DesktopWindowProc,
    .pDestroyWindow = WAYLAND_DestroyWindow,
    .pFlashWindowEx = WAYLAND_FlashWindowEx,
    .pSetIMECompositionRect = WAYLAND_SetIMECompositionRect,
    .pSetIMEEnabled = WAYLAND_SetIMEEnabled,
    .pKbdLayerDescriptor = WAYLAND_KbdLayerDescriptor,
    .pReleaseKbdTables = WAYLAND_ReleaseKbdTables,
    .pSetCursor = WAYLAND_SetCursor,
    .pSetCursorPos = WAYLAND_SetCursorPos,
    .pSetDesktopWindow = WAYLAND_SetDesktopWindow,
    .pSetLayeredWindowAttributes = WAYLAND_SetLayeredWindowAttributes,
    .pSetWindowIcons = WAYLAND_SetWindowIcons,
    .pSetWindowStyle = WAYLAND_SetWindowStyle,
    .pSetWindowText = WAYLAND_SetWindowText,
    .pShowWindow = WAYLAND_ShowWindow,
    .pSysCommand = WAYLAND_SysCommand,
    .pUpdateLayeredWindow = WAYLAND_UpdateLayeredWindow,
    .pUpdateDisplayDevices = WAYLAND_UpdateDisplayDevices,
    .pWindowMessage = WAYLAND_WindowMessage,
    .pWindowPosChanged = WAYLAND_WindowPosChanged,
    .pWindowPosChanging = WAYLAND_WindowPosChanging,
    .pCreateWindowSurface = WAYLAND_CreateWindowSurface,
    .pGetForeignGdiSurfaceCaps = WAYLAND_GetForeignGdiSurfaceCaps,
    .pGetWindowStyleMasks = WAYLAND_GetWindowStyleMasks,
    .pGetWindowStateUpdates = WAYLAND_GetWindowStateUpdates,
    .pGetWindowMaxTrackSize = WAYLAND_GetWindowMaxTrackSize,
    .pHasWindowManager = WAYLAND_HasWindowManager,
    .pVulkanInit = WAYLAND_VulkanInit,
    .pOpenGLInit = WAYLAND_OpenGLInit,
};

static void wayland_init_process_name(void)
{
    WCHAR *p, *appname;
    WCHAR appname_lower[MAX_PATH];
    DWORD appname_len;
    DWORD appnamez_size;
    DWORD utf8_size;
    int i;

    appname = NtCurrentTeb()->Peb->ProcessParameters->ImagePathName.Buffer;
    if ((p = wcsrchr(appname, '/'))) appname = p + 1;
    if ((p = wcsrchr(appname, '\\'))) appname = p + 1;
    appname_len = lstrlenW(appname);

    if (appname_len == 0 || appname_len >= MAX_PATH) return;

    for (i = 0; appname[i]; i++) appname_lower[i] = RtlDowncaseUnicodeChar(appname[i]);
    appname_lower[i] = 0;

    appnamez_size = (appname_len + 1) * sizeof(WCHAR);

    if (!RtlUnicodeToUTF8N(NULL, 0, &utf8_size, appname_lower, appnamez_size) &&
        (process_name = malloc(utf8_size)))
    {
        RtlUnicodeToUTF8N(process_name, utf8_size, &utf8_size, appname_lower, appnamez_size);
    }
}

static void wayland_init_activation_token(void)
{
    const char *env;

    if (!(env = getenv("XDG_ACTIVATION_TOKEN"))) env = getenv("DESKTOP_STARTUP_ID");
    if (env) process_activate_token = strdup(env);
    unsetenv("XDG_ACTIVATION_TOKEN");
    unsetenv("DESKTOP_STARTUP_ID");

    TRACE("inherited activation token %s\n", process_activate_token ? "found" : "not found");
}

char *wayland_take_process_activation_token(void)
{
    char *token;

    pthread_mutex_lock(&process_wayland.activation_mutex);
    token = process_activate_token;
    process_activate_token = NULL;
    pthread_mutex_unlock(&process_wayland.activation_mutex);
    return token;
}

BOOL wayland_process_activation_token_pending(void)
{
    BOOL pending;

    pthread_mutex_lock(&process_wayland.activation_mutex);
    pending = process_activate_token != NULL;
    pthread_mutex_unlock(&process_wayland.activation_mutex);
    return pending;
}

static NTSTATUS waylanddrv_unix_init(void *arg)
{
    HANDLE thread;
    NTSTATUS status;

    wayland_init_process_name();
    wayland_init_activation_token();

    if ((dmabuf_epoll_fd = epoll_create1(EPOLL_CLOEXEC)) < 0) goto err;
    if (!wayland_process_init()) goto err;

    /* Normally the thread loading the driver already knows its desktop. The
     * desktop process itself can load us earlier, before creating that window. */
    WAYLAND_SetDesktopWindow(ULongToHandle(NtUserGetThreadInfo()->top_window));

    /* Event dispatch is driver-internal work, not an application thread.
     * A system thread has its own TEB without running PE loader callbacks. */
    if ((status = PsCreateSystemThread(&thread, THREAD_ALL_ACCESS, NULL, 0, NULL,
                                      wayland_read_events_thread, NULL)))
    {
        ERR("Failed to create event thread, status %#x\n", (unsigned int)status);
        goto err;
    }
    NtClose(thread);

    /* Other threads can call the driver as soon as it is published. Finish
     * initializing the window mutex and Wayland objects before exposing it. */
    __wine_set_user_driver(&waylanddrv_funcs, WINE_GDI_DRIVER_VERSION);

    pthread_mutex_lock(&reader_mutex);
    reader_ready = TRUE;
    pthread_cond_signal(&reader_cond);
    pthread_mutex_unlock(&reader_mutex);

    return 0;

err:
    if (dmabuf_epoll_fd >= 0) close(dmabuf_epoll_fd);
    dmabuf_epoll_fd = -1;
    return STATUS_UNSUCCESSFUL;
}

static int dispatch_events(void)
{
    struct wl_display *display = process_wayland.wl_display;
    struct wl_event_queue *queue = process_wayland.wl_event_queue;
    struct pollfd fds[2] = {{wl_display_get_fd(display), POLLIN}, {dmabuf_epoll_fd, POLLIN}};
    struct epoll_event events[32];
    int ret, count, i;

    while (wl_display_prepare_read_queue(display, queue))
        if (wl_display_dispatch_queue_pending(display, queue) < 0) return -1;

    if (wl_display_flush(display) < 0)
    {
        if (errno != EAGAIN)
        {
            wl_display_cancel_read(display);
            return -1;
        }
        fds[0].events |= POLLOUT;
    }

    ret = poll(fds, ARRAY_SIZE(fds), wayland_clipboard_dispatch_timeout());
    if (ret < 0 || !(fds[0].revents & (POLLIN | POLLERR | POLLHUP)))
        wl_display_cancel_read(display);
    else if (wl_display_read_events(display) < 0)
        return -1;
    if (ret < 0) return errno == EINTR ? 0 : -1;

    /* Release the prepared read before dispatching anything that can take
     * driver locks or issue Wayland requests, including frame imports. */
    wayland_clipboard_cleanup_thread();
    if (wl_display_dispatch_queue_pending(display, queue) < 0) return -1;
    if (!(fds[1].revents & POLLIN)) return 0;

    count = epoll_wait(dmabuf_epoll_fd, events, ARRAY_SIZE(events), 0);
    if (count < 0) return errno == EINTR ? 0 : -1;
    count = wayland_dmabuf_coalesce_events(events, count);
    for (i = 0; i < count; i++)
        wayland_surface_dispatch_dmabuf(ULongToHandle((UINT32)events[i].data.u64),
                                        events[i].data.u64 >> 32);
    return 0;
}

static void wayland_read_events_thread(void *arg)
{
    int error;
    uint32_t id, proto_err;
    const struct wl_interface *interface;

    /* A Unix reader must not initialize the desktop or builtin classes through
     * PE callbacks. Wait for win32u to supply an existing desktop, then seed the
     * reader's own desktop cache before any handler can call back into win32u.
     * This also keeps event dispatch behind driver publication. */
    pthread_mutex_lock(&reader_mutex);
    while (!reader_ready || !reader_desktop)
        pthread_cond_wait(&reader_cond, &reader_mutex);
    NtUserGetThreadInfo()->top_window = HandleToULong(reader_desktop);
    pthread_mutex_unlock(&reader_mutex);

    while (dispatch_events() != -1) continue;
    /* This function only returns on a fatal error, e.g., if our connection
     * to the Wayland server is lost. */

    error = wl_display_get_error(process_wayland.wl_display);
    if (!error) error = errno;

    if (error == EPROTO)
    {
        proto_err = wl_display_get_protocol_error(process_wayland.wl_display,
                                                  &interface, &id);
        ERR("Protocol error on %s#%u with code %u\n",
            (interface && interface->name) ? interface->name : "Unknown", id, proto_err);
    }
    else ERR("%s when dispatching event queue\n", strerror(error));

    /* Losing the display connection is fatal, just as for the PE reader. */
    NtTerminateProcess(NtCurrentProcess(), 1);
}

static NTSTATUS waylanddrv_unix_init_clipboard(void *arg)
{
    /* If the compositor supports zwlr_data_control_manager_v1, we don't need
     * per-process clipboard window and handling, we can use the default clipboard
     * window from the desktop process. */
    if (process_wayland.zwlr_data_control_manager_v1) return STATUS_UNSUCCESSFUL;
    if (process_wayland.ext_data_control_manager_v1) return STATUS_UNSUCCESSFUL;
    return STATUS_SUCCESS;
}

const unixlib_entry_t __wine_unix_call_funcs[] =
{
    waylanddrv_unix_init,
    waylanddrv_unix_init_clipboard,
};

C_ASSERT(ARRAYSIZE(__wine_unix_call_funcs) == waylanddrv_unix_func_count);

#ifdef _WIN64

const unixlib_entry_t __wine_unix_call_wow64_funcs[] =
{
    waylanddrv_unix_init,
    waylanddrv_unix_init_clipboard,
};

C_ASSERT(ARRAYSIZE(__wine_unix_call_wow64_funcs) == waylanddrv_unix_func_count);

#endif /* _WIN64 */
