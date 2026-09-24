/*
 * DMA-BUF channel event helpers
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

#ifndef __WINE_WAYLAND_DMABUF_H
#define __WINE_WAYLAND_DMABUF_H

#include <errno.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

#include "wine/hwnd_dmabuf.h"

static inline int wayland_hwnd_dmabuf_channel_send_release(int channel_fd,
                                                          const hwnd_dmabuf_release_t *rel)
{
    ssize_t n;

    do n = send(channel_fd, rel, sizeof(*rel), MSG_DONTWAIT | MSG_NOSIGNAL);
    while (n < 0 && errno == EINTR);
    return n == sizeof(*rel) ? 0 : n < 0 ? errno : EMSGSIZE;
}

/* Teardown cannot retry a lost state message. Shut down all duplicates of the
 * endpoint on failure, including wineserver's retained copy, so blocked
 * producers see EOF and recreate instead of waiting for image releases. */
static inline int wayland_dmabuf_channel_suspend(int channel_fd)
{
    hwnd_dmabuf_release_t rel = {.flags = HWND_DMABUF_RELEASE_CONSUMER_SUSPENDED};
    int ret = wayland_hwnd_dmabuf_channel_send_release(channel_fd, &rel);

    if (ret) shutdown(channel_fd, SHUT_RDWR);
    return ret;
}

enum wayland_dmabuf_update_mode
{
    WAYLAND_DMABUF_UPDATE_BLOCKED,
    WAYLAND_DMABUF_UPDATE_CHILDREN,
    WAYLAND_DMABUF_UPDATE_ALL,
};

/* A configured parent can keep presenting child buffers while its window
 * thread handles a resize. Buffers attached to the parent itself still need
 * a compatible configuration; never promote a child in this interval. */
static inline enum wayland_dmabuf_update_mode wayland_dmabuf_get_update_mode(
        BOOL reconfigured, BOOL configured_toplevel, BOOL direct_buffer)
{
    if (reconfigured) return WAYLAND_DMABUF_UPDATE_ALL;
    if (configured_toplevel && !direct_buffer) return WAYLAND_DMABUF_UPDATE_CHILDREN;
    return WAYLAND_DMABUF_UPDATE_BLOCKED;
}

/* Each dispatch drains all channels for one host. Coalesce only this batch,
 * retaining the surface serial so a recycled HWND is a different identity. */
static inline unsigned int wayland_dmabuf_coalesce_events(struct epoll_event *events,
                                                         unsigned int count)
{
    unsigned int i, j, unique = 0;

    for (i = 0; i < count; i++)
    {
        for (j = 0; j < unique; j++)
            if (events[j].data.u64 == events[i].data.u64) break;
        if (j == unique) events[unique++] = events[i];
        else events[j].events |= events[i].events;
    }
    return unique;
}

/* Return 1 for a packet (invalid packets have version 0), 0 only for an empty
 * channel, and -1 for a closed/broken channel. EPOLLET requires draining past
 * malformed packets and interrupted reads, all the way to EAGAIN. */
static inline int wayland_dmabuf_channel_recv(int channel_fd, hwnd_dmabuf_frame_desc_t *desc,
                                             int *out_fd, int *out_sync_fd)
{
    union
    {
        struct cmsghdr align;
        char data[CMSG_SPACE(2 * sizeof(int))];
    } control;
    struct iovec iov = {.iov_base = desc, .iov_len = sizeof(*desc)};
    struct msghdr msg;
    struct cmsghdr *cmsg;
    int fds[2];
    unsigned int fd_count = 0, expected, i;
    ssize_t n;

    *out_fd = *out_sync_fd = -1;
    do
    {
        msg = (struct msghdr){.msg_iov = &iov, .msg_iovlen = 1,
                             .msg_control = control.data, .msg_controllen = sizeof(control.data)};
        n = recvmsg(channel_fd, &msg, MSG_DONTWAIT | MSG_CMSG_CLOEXEC);
    } while (n < 0 && errno == EINTR);
#if EAGAIN == EWOULDBLOCK
    if (n < 0) return errno == EAGAIN ? 0 : -1;
#else
    if (n < 0) return errno == EAGAIN || errno == EWOULDBLOCK ? 0 : -1;
#endif
    for (cmsg = CMSG_FIRSTHDR(&msg); cmsg; cmsg = CMSG_NXTHDR(&msg, cmsg))
        if (cmsg->cmsg_level == SOL_SOCKET && cmsg->cmsg_type == SCM_RIGHTS &&
            cmsg->cmsg_len >= CMSG_LEN(0))
        {
            unsigned int count = (cmsg->cmsg_len - CMSG_LEN(0)) / sizeof(int);
            int *received = (int *)CMSG_DATA(cmsg);

            for (i = 0; i < count; i++)
            {
                if (fd_count < ARRAY_SIZE(fds)) fds[fd_count++] = received[i];
                else close(received[i]);
            }
        }

    if (!n)
    {
        /* Even a zero-length seqpacket can carry descriptors. */
        for (i = 0; i < fd_count; i++) close(fds[i]);
        return -1;
    }

    if (n != sizeof(*desc) || (msg.msg_flags & (MSG_TRUNC | MSG_CTRUNC)))
    {
        for (i = 0; i < fd_count; i++) close(fds[i]);
        /* Preserve a complete descriptor's token so the caller can return a
         * failed release, but never interpret a partial/oversized payload. */
        if (n != sizeof(*desc) || (msg.msg_flags & MSG_TRUNC)) memset(desc, 0, sizeof(*desc));
        else desc->version = 0;
        return 1;
    }

    if (desc->sync_fd_kind == HWND_DMABUF_SYNC_FILE)
    {
        expected = fd_count == 2 ? 2 : 1;
        if (fd_count == 2) *out_fd = fds[0];
        if (fd_count) *out_sync_fd = fds[fd_count - 1];
    }
    else
    {
        expected = fd_count ? 1 : 0;
        if (fd_count) *out_fd = fds[0];
    }

    if (fd_count != expected ||
        (desc->sync_fd_kind == HWND_DMABUF_SYNC_FILE && *out_sync_fd < 0))
    {
        for (i = 0; i < fd_count; i++) close(fds[i]);
        *out_fd = *out_sync_fd = -1;
        desc->version = 0;
    }
    return 1;
}

#endif /* __WINE_WAYLAND_DMABUF_H */
