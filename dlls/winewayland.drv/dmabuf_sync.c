/*
 * Wayland dma-buf synchronization
 *
 * Copyright 2026 Erhan Bilgili
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
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>

#if defined(__linux__) && defined(HAVE_SYS_EPOLL_H) && defined(HAVE_SYS_EVENTFD_H) && \
    defined(HAVE_XF86DRM_H) && defined(HAVE_DRMSYNCOBJEVENTFD)
#include <linux/dma-buf.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <xf86drm.h>
#define HAVE_WAYLAND_SYNCOBJ 1
#endif

#include "waylanddrv.h"
#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(waylanddrv);

#ifdef HAVE_WAYLAND_SYNCOBJ

struct wayland_syncobj_timeline
{
    uint32_t handle;
    uint32_t import_handle;
    UINT64 next_point;
    struct wp_linux_drm_syncobj_timeline_v1 *proxy;
};

struct wayland_syncobj_release
{
    struct wayland_syncobj_attachment *attachment;
    wayland_syncobj_release_func callback;
    void *data;
    UINT64 point;
    LONG committed;
};

struct wayland_syncobj_attachment
{
    struct wl_list buffer_link;
    struct wl_list monitor_link;
    struct wl_surface *surface;
    struct wayland_syncobj_timeline timeline;
    struct wayland_syncobj_release *pending;
    UINT64 monitor_id;
    int event_fd;
    BOOL retired;
};

struct wayland_syncobj_buffer
{
    struct wayland_syncobj_timeline acquire;
    struct wl_list attachments;
    BOOL wayland_destroyed;
};

static struct
{
    pthread_mutex_t mutex;
    struct wl_list attachments;
    UINT64 next_id;
    int epoll_fd;
    BOOL running;
} release_monitor =
{
    .mutex = PTHREAD_MUTEX_INITIALIZER,
    .attachments = {&release_monitor.attachments, &release_monitor.attachments},
    .epoll_fd = -1,
};

static int syncobj_drm_fd = -1;
static BOOL syncobj_ready;

static void syncobj_attachment_destroy(struct wayland_syncobj_attachment *attachment);

static struct wayland_syncobj_attachment *release_monitor_find(UINT64 id)
{
    struct wayland_syncobj_attachment *attachment;

    wl_list_for_each(attachment, &release_monitor.attachments, monitor_link)
        if (attachment->monitor_id == id) return attachment;
    return NULL;
}

static void *release_monitor_thread(void *arg)
{
    struct epoll_event events[16];

    (void)arg;
    for (;;)
    {
        int count = epoll_wait(release_monitor.epoll_fd, events, ARRAY_SIZE(events), -1);
        int i;

        if (count < 0)
        {
            if (errno == EINTR) continue;
            ERR("syncobj release monitor failed: %s\n", strerror(errno));
            return NULL;
        }

        for (i = 0; i < count; i++)
        {
            struct wayland_syncobj_attachment *attachment;
            struct wayland_syncobj_release *release = NULL;
            BOOL destroy_attachment = FALSE;
            uint64_t value;

            pthread_mutex_lock(&release_monitor.mutex);
            if ((attachment = release_monitor_find(events[i].data.u64)))
            {
                ssize_t ret;

                do ret = read(attachment->event_fd, &value, sizeof(value));
                while (ret < 0 && errno == EINTR);
                if (ret == sizeof(value))
                {
                    release = attachment->pending;
                    attachment->pending = NULL;
                    if (attachment->retired)
                    {
                        epoll_ctl(release_monitor.epoll_fd, EPOLL_CTL_DEL,
                                  attachment->event_fd, NULL);
                        wl_list_remove(&attachment->monitor_link);
                        wl_list_remove(&attachment->buffer_link);
                        destroy_attachment = TRUE;
                    }
                }
            }
            pthread_mutex_unlock(&release_monitor.mutex);

            if (destroy_attachment) syncobj_attachment_destroy(attachment);
            if (release)
            {
                release->callback(release->data, ReadAcquire(&release->committed));
                free(release);
            }
        }
    }
}

static BOOL release_monitor_start(void)
{
    pthread_t thread;

    pthread_mutex_lock(&release_monitor.mutex);
    if (release_monitor.running)
    {
        pthread_mutex_unlock(&release_monitor.mutex);
        return TRUE;
    }
    if ((release_monitor.epoll_fd = epoll_create1(EPOLL_CLOEXEC)) < 0 ||
        pthread_create(&thread, NULL, release_monitor_thread, NULL))
    {
        if (release_monitor.epoll_fd >= 0) close(release_monitor.epoll_fd);
        release_monitor.epoll_fd = -1;
        pthread_mutex_unlock(&release_monitor.mutex);
        return FALSE;
    }
    pthread_detach(thread);
    release_monitor.running = TRUE;
    pthread_mutex_unlock(&release_monitor.mutex);
    return TRUE;
}

static BOOL syncobj_timeline_init(struct wayland_syncobj_timeline *timeline, BOOL import_sync_file)
{
    int fd = -1;

    if (drmSyncobjCreate(syncobj_drm_fd, 0, &timeline->handle)) return FALSE;
    if (import_sync_file && drmSyncobjCreate(syncobj_drm_fd, 0, &timeline->import_handle)) goto failed;
    if (drmSyncobjHandleToFD(syncobj_drm_fd, timeline->handle, &fd)) goto failed;
    timeline->proxy = wp_linux_drm_syncobj_manager_v1_import_timeline(
            process_wayland.wp_linux_drm_syncobj_manager_v1, fd);
    close(fd);
    if (!timeline->proxy) goto failed;
    return TRUE;

failed:
    if (fd >= 0) close(fd);
    if (timeline->import_handle) drmSyncobjDestroy(syncobj_drm_fd, timeline->import_handle);
    if (timeline->handle) drmSyncobjDestroy(syncobj_drm_fd, timeline->handle);
    memset(timeline, 0, sizeof(*timeline));
    return FALSE;
}

static void syncobj_timeline_destroy_wayland(struct wayland_syncobj_timeline *timeline)
{
    if (timeline->proxy)
    {
        wp_linux_drm_syncobj_timeline_v1_destroy(timeline->proxy);
        timeline->proxy = NULL;
    }
}

static void syncobj_timeline_destroy(struct wayland_syncobj_timeline *timeline)
{
    if (timeline->import_handle) drmSyncobjDestroy(syncobj_drm_fd, timeline->import_handle);
    if (timeline->handle) drmSyncobjDestroy(syncobj_drm_fd, timeline->handle);
}

static void syncobj_attachment_destroy(struct wayland_syncobj_attachment *attachment)
{
    assert(!attachment->timeline.proxy);
    close(attachment->event_fd);
    syncobj_timeline_destroy(&attachment->timeline);
    free(attachment);
}

static BOOL syncobj_probe_eventfd(int fd)
{
    struct pollfd pollfd = {.events = POLLIN};
    uint32_t handle = 0;
    uint64_t point = 1, value;
    BOOL ret = FALSE;

    if (drmSyncobjCreate(fd, 0, &handle)) return FALSE;
    if ((pollfd.fd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK)) < 0) goto done;
    if (drmSyncobjEventfd(fd, handle, point, pollfd.fd, 0)) goto done;
    if (drmSyncobjTimelineSignal(fd, &handle, &point, 1)) goto done;
    if (poll(&pollfd, 1, 100) != 1 || !(pollfd.revents & POLLIN)) goto done;
    ret = read(pollfd.fd, &value, sizeof(value)) == sizeof(value);

done:
    if (pollfd.fd >= 0) close(pollfd.fd);
    drmSyncobjDestroy(fd, handle);
    return ret;
}

BOOL wayland_syncobj_init(void)
{
    struct wayland_dmabuf_feedback *feedback = &process_wayland.dmabuf_default_feedback;
    drmDevicePtr device = NULL;
    const char *node = NULL;
    uint64_t cap;
    int fd = -1;

    if (syncobj_ready) return TRUE;
    if (!process_wayland.wp_linux_drm_syncobj_manager_v1 || !feedback->has_main_device)
        return FALSE;
    if (getenv("WINE_WAYLAND_NO_SYNCOBJ")) return FALSE;
    if (drmGetDeviceFromDevId(feedback->main_device, 0, &device)) return FALSE;
    if (device->available_nodes & (1 << DRM_NODE_RENDER)) node = device->nodes[DRM_NODE_RENDER];
    else if (device->available_nodes & (1 << DRM_NODE_PRIMARY)) node = device->nodes[DRM_NODE_PRIMARY];
    if (node) fd = open(node, O_RDWR | O_CLOEXEC);
    drmFreeDevice(&device);
    if (fd < 0) return FALSE;

    if (drmGetCap(fd, DRM_CAP_SYNCOBJ, &cap) || !cap ||
        drmGetCap(fd, DRM_CAP_SYNCOBJ_TIMELINE, &cap) || !cap ||
        !syncobj_probe_eventfd(fd) || !release_monitor_start())
    {
        close(fd);
        return FALSE;
    }

    syncobj_drm_fd = fd;
    syncobj_ready = TRUE;
    TRACE("using linux-drm-syncobj for HWND dma-buf synchronization\n");
    return TRUE;
}

BOOL wayland_syncobj_available(void)
{
    return syncobj_ready;
}

struct wayland_syncobj_buffer *wayland_syncobj_buffer_create(void)
{
    struct wayland_syncobj_buffer *buffer;

    if (!syncobj_ready || !(buffer = calloc(1, sizeof(*buffer)))) return NULL;
    wl_list_init(&buffer->attachments);
    return buffer;
}

void wayland_syncobj_buffer_destroy_wayland(struct wayland_syncobj_buffer *buffer)
{
    struct wayland_syncobj_attachment *attachment;

    if (!buffer || buffer->wayland_destroyed) return;
    syncobj_timeline_destroy_wayland(&buffer->acquire);
    pthread_mutex_lock(&release_monitor.mutex);
    wl_list_for_each(attachment, &buffer->attachments, buffer_link)
        syncobj_timeline_destroy_wayland(&attachment->timeline);
    pthread_mutex_unlock(&release_monitor.mutex);
    buffer->wayland_destroyed = TRUE;
}

void wayland_syncobj_buffer_destroy(struct wayland_syncobj_buffer *buffer)
{
    struct wayland_syncobj_attachment *attachment, *next;

    if (!buffer) return;
    assert(buffer->wayland_destroyed);
    wl_list_for_each_safe(attachment, next, &buffer->attachments, buffer_link)
    {
        pthread_mutex_lock(&release_monitor.mutex);
        assert(!attachment->pending);
        epoll_ctl(release_monitor.epoll_fd, EPOLL_CTL_DEL, attachment->event_fd, NULL);
        wl_list_remove(&attachment->monitor_link);
        wl_list_remove(&attachment->buffer_link);
        pthread_mutex_unlock(&release_monitor.mutex);
        syncobj_attachment_destroy(attachment);
    }
    syncobj_timeline_destroy(&buffer->acquire);
    free(buffer);
}

void wayland_syncobj_buffer_remove_surface(struct wayland_syncobj_buffer *buffer,
                                           struct wl_surface *surface)
{
    struct wayland_syncobj_attachment *attachment, *next;
    struct wl_list destroy_list;

    if (!buffer) return;

    wl_list_init(&destroy_list);
    pthread_mutex_lock(&release_monitor.mutex);
    wl_list_for_each_safe(attachment, next, &buffer->attachments, buffer_link)
    {
        if (attachment->surface != surface) continue;
        syncobj_timeline_destroy_wayland(&attachment->timeline);
        attachment->surface = NULL;
        attachment->retired = TRUE;
        if (!attachment->pending)
        {
            epoll_ctl(release_monitor.epoll_fd, EPOLL_CTL_DEL,
                      attachment->event_fd, NULL);
            wl_list_remove(&attachment->monitor_link);
            wl_list_remove(&attachment->buffer_link);
            wl_list_insert(destroy_list.prev, &attachment->buffer_link);
        }
    }
    pthread_mutex_unlock(&release_monitor.mutex);

    wl_list_for_each_safe(attachment, next, &destroy_list, buffer_link)
    {
        wl_list_remove(&attachment->buffer_link);
        syncobj_attachment_destroy(attachment);
    }
}

BOOL wayland_syncobj_prepare_acquire(struct wayland_syncobj_buffer *buffer, int sync_fd,
                                     UINT64 *point)
{
    if (!buffer->acquire.handle && !syncobj_timeline_init(&buffer->acquire, TRUE)) return FALSE;
    if (!++buffer->acquire.next_point) return FALSE;

    if (sync_fd >= 0)
    {
        if (drmSyncobjReset(syncobj_drm_fd, &buffer->acquire.import_handle, 1) ||
            drmSyncobjImportSyncFile(syncobj_drm_fd, buffer->acquire.import_handle, sync_fd) ||
            drmSyncobjTransfer(syncobj_drm_fd, buffer->acquire.handle,
                               buffer->acquire.next_point, buffer->acquire.import_handle, 0, 0))
            return FALSE;
    }
    else if (drmSyncobjTimelineSignal(syncobj_drm_fd, &buffer->acquire.handle,
                                      &buffer->acquire.next_point, 1))
        return FALSE;

    *point = buffer->acquire.next_point;
    return TRUE;
}

static struct wayland_syncobj_attachment *syncobj_get_attachment(
        struct wayland_syncobj_buffer *buffer, struct wl_surface *surface)
{
    struct wayland_syncobj_attachment *attachment;
    struct epoll_event event = {.events = EPOLLIN};

    pthread_mutex_lock(&release_monitor.mutex);
    wl_list_for_each(attachment, &buffer->attachments, buffer_link)
        if (attachment->surface == surface && !attachment->pending)
        {
            pthread_mutex_unlock(&release_monitor.mutex);
            return attachment;
        }
    pthread_mutex_unlock(&release_monitor.mutex);

    if (!(attachment = calloc(1, sizeof(*attachment)))) return NULL;
    attachment->surface = surface;
    attachment->event_fd = -1;
    if (!syncobj_timeline_init(&attachment->timeline, FALSE)) goto failed;
    if ((attachment->event_fd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK)) < 0) goto failed;

    pthread_mutex_lock(&release_monitor.mutex);
    attachment->monitor_id = ++release_monitor.next_id;
    if (!attachment->monitor_id) attachment->monitor_id = ++release_monitor.next_id;
    event.data.u64 = attachment->monitor_id;
    if (epoll_ctl(release_monitor.epoll_fd, EPOLL_CTL_ADD, attachment->event_fd, &event))
    {
        pthread_mutex_unlock(&release_monitor.mutex);
        goto failed;
    }
    wl_list_insert(buffer->attachments.prev, &attachment->buffer_link);
    wl_list_insert(release_monitor.attachments.prev, &attachment->monitor_link);
    pthread_mutex_unlock(&release_monitor.mutex);
    return attachment;

failed:
    if (attachment->event_fd >= 0) close(attachment->event_fd);
    syncobj_timeline_destroy_wayland(&attachment->timeline);
    syncobj_timeline_destroy(&attachment->timeline);
    free(attachment);
    return NULL;
}

struct wayland_syncobj_release *wayland_syncobj_prepare_release(
        struct wayland_syncobj_buffer *buffer, struct wl_surface *surface,
        wayland_syncobj_release_func callback, void *data)
{
    struct wayland_syncobj_attachment *attachment;
    struct wayland_syncobj_release *release;

    if (!(attachment = syncobj_get_attachment(buffer, surface))) return NULL;
    if (!(release = calloc(1, sizeof(*release)))) return NULL;
    release->attachment = attachment;
    release->callback = callback;
    release->data = data;
    if (!(release->point = ++attachment->timeline.next_point)) goto failed;

    pthread_mutex_lock(&release_monitor.mutex);
    if (attachment->pending ||
        drmSyncobjEventfd(syncobj_drm_fd, attachment->timeline.handle,
                          release->point, attachment->event_fd, 0))
    {
        pthread_mutex_unlock(&release_monitor.mutex);
        goto failed;
    }
    attachment->pending = release;
    pthread_mutex_unlock(&release_monitor.mutex);
    return release;

failed:
    free(release);
    return NULL;
}

BOOL wayland_syncobj_surface_set_points(
        struct wp_linux_drm_syncobj_surface_v1 **syncobj_surface,
        struct wl_surface *surface, struct wayland_syncobj_buffer *buffer,
        UINT64 acquire_point, struct wayland_syncobj_release *release)
{
    struct wayland_syncobj_timeline *acquire = &buffer->acquire;
    struct wayland_syncobj_timeline *release_timeline = &release->attachment->timeline;

    if (!*syncobj_surface &&
        !(*syncobj_surface = wp_linux_drm_syncobj_manager_v1_get_surface(
                process_wayland.wp_linux_drm_syncobj_manager_v1, surface)))
        return FALSE;

    wp_linux_drm_syncobj_surface_v1_set_acquire_point(
            *syncobj_surface, acquire->proxy, acquire_point >> 32, (uint32_t)acquire_point);
    wp_linux_drm_syncobj_surface_v1_set_release_point(
            *syncobj_surface, release_timeline->proxy,
            release->point >> 32, (uint32_t)release->point);
    return TRUE;
}

void wayland_syncobj_release_commit(struct wayland_syncobj_release *release)
{
    InterlockedExchange(&release->committed, TRUE);
}

void wayland_syncobj_release_cancel(struct wayland_syncobj_release *release)
{
    struct wayland_syncobj_attachment *attachment;
    BOOL complete = FALSE;
    int ret;

    if (!release) return;
    attachment = release->attachment;
    pthread_mutex_lock(&release_monitor.mutex);
    if (attachment->pending == release &&
        (ret = drmSyncobjTimelineSignal(syncobj_drm_fd, &attachment->timeline.handle,
                                        &release->point, 1)))
    {
        ERR("Failed to cancel syncobj release point: %d\n", ret);
        attachment->pending = NULL;
        complete = TRUE;
    }
    pthread_mutex_unlock(&release_monitor.mutex);

    if (complete)
    {
        release->callback(release->data, FALSE);
        free(release);
    }
}

BOOL wayland_dmabuf_import_sync_file(int dmabuf_fd, int sync_fd)
{
#ifdef DMA_BUF_IOCTL_IMPORT_SYNC_FILE
    struct dma_buf_import_sync_file data = {.flags = DMA_BUF_SYNC_WRITE, .fd = sync_fd};

    return ioctl(dmabuf_fd, DMA_BUF_IOCTL_IMPORT_SYNC_FILE, &data) == 0;
#else
    return FALSE;
#endif
}

#else

BOOL wayland_syncobj_init(void)
{
    return FALSE;
}

BOOL wayland_syncobj_available(void)
{
    return FALSE;
}

struct wayland_syncobj_buffer *wayland_syncobj_buffer_create(void)
{
    return NULL;
}

void wayland_syncobj_buffer_destroy_wayland(struct wayland_syncobj_buffer *buffer)
{
}

void wayland_syncobj_buffer_destroy(struct wayland_syncobj_buffer *buffer)
{
}

void wayland_syncobj_buffer_remove_surface(struct wayland_syncobj_buffer *buffer,
                                           struct wl_surface *surface)
{
}

BOOL wayland_syncobj_prepare_acquire(struct wayland_syncobj_buffer *buffer, int sync_fd,
                                     UINT64 *point)
{
    return FALSE;
}

struct wayland_syncobj_release *wayland_syncobj_prepare_release(
        struct wayland_syncobj_buffer *buffer, struct wl_surface *surface,
        wayland_syncobj_release_func callback, void *data)
{
    return NULL;
}

BOOL wayland_syncobj_surface_set_points(
        struct wp_linux_drm_syncobj_surface_v1 **syncobj_surface,
        struct wl_surface *surface, struct wayland_syncobj_buffer *buffer,
        UINT64 acquire_point, struct wayland_syncobj_release *release)
{
    return FALSE;
}

void wayland_syncobj_release_commit(struct wayland_syncobj_release *release)
{
}

void wayland_syncobj_release_cancel(struct wayland_syncobj_release *release)
{
}

BOOL wayland_dmabuf_import_sync_file(int dmabuf_fd, int sync_fd)
{
    return FALSE;
}

#endif

BOOL wayland_sync_file_wait(int sync_fd)
{
    struct pollfd pfd = {.fd = sync_fd, .events = POLLIN};
    int ret;

    do ret = poll(&pfd, 1, -1);
    while (ret < 0 && errno == EINTR);
    return ret == 1 && !(pfd.revents & POLLNVAL);
}
