/*
 * Native tests for DMA-BUF channel readiness
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

#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

static int recv_error, recv_errors_remaining;

static ssize_t interrupted_recvmsg(int fd, struct msghdr *msg, int flags)
{
    if (recv_errors_remaining)
    {
        recv_errors_remaining--;
        errno = recv_error;
        return -1;
    }
    return recvmsg(fd, msg, flags);
}

/* Test the production receiver, including its EINTR/error handling. */
#define recvmsg interrupted_recvmsg
#include "../dmabuf.h"
#undef recvmsg

static unsigned int open_fd_count(void)
{
    struct dirent *entry;
    unsigned int count = 0;
    DIR *dir = opendir("/proc/self/fd");

    assert(dir);
    while ((entry = readdir(dir)))
        if (entry->d_name[0] != '.') count++;
    closedir(dir);
    return count;
}

static void create_channel(int pair[2])
{
    assert(!socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, pair));
}

static hwnd_dmabuf_frame_desc_t frame(unsigned int seq)
{
    return (hwnd_dmabuf_frame_desc_t){.version = HWND_DMABUF_DESC_VERSION_V1,
            .flags = HWND_DMABUF_FLAG_STABLE_SLOT, .width = 16, .height = 16,
            .stride = 64, .fourcc = HWND_DMABUF_SHM_FORMAT_XRGB8888,
            .frame_seq = seq, .producer_unique_id = 123, .release_token = seq + 1};
}

static void send_packet(int sock, const void *data, size_t size, const int *fds, unsigned int count)
{
    union { struct cmsghdr align; char data[CMSG_SPACE(4 * sizeof(int))]; } control;
    struct iovec iov = {.iov_base = (void *)data, .iov_len = size};
    struct msghdr msg = {.msg_iov = &iov, .msg_iovlen = 1};
    ssize_t ret;

    assert(count <= 4);
    if (count)
    {
        struct cmsghdr *cmsg;

        msg.msg_control = control.data;
        msg.msg_controllen = CMSG_SPACE(count * sizeof(int));
        cmsg = CMSG_FIRSTHDR(&msg);
        cmsg->cmsg_level = SOL_SOCKET;
        cmsg->cmsg_type = SCM_RIGHTS;
        cmsg->cmsg_len = CMSG_LEN(count * sizeof(int));
        memcpy(CMSG_DATA(cmsg), fds, count * sizeof(int));
    }
    do ret = sendmsg(sock, &msg, MSG_NOSIGNAL | MSG_DONTWAIT);
    while (ret < 0 && errno == EINTR);
    assert(ret == (ssize_t)size);
}

static int watch(int fd, uint64_t identity)
{
    struct epoll_event event = {.events = EPOLLIN | EPOLLET, .data.u64 = identity};
    int epfd = epoll_create1(EPOLL_CLOEXEC);

    assert(epfd >= 0);
    assert(!epoll_ctl(epfd, EPOLL_CTL_ADD, fd, &event));
    return epfd;
}

static void expect_ready(int epfd, uint64_t identity)
{
    struct epoll_event event;

    assert(epoll_wait(epfd, &event, 1, 0) == 1);
    assert(event.data.u64 == identity);
}

static void expect_quiet(int epfd)
{
    struct epoll_event event;

    assert(!epoll_wait(epfd, &event, 1, 0));
}

static void test_coalesce(void)
{
    const uint64_t a = (uint64_t)1 << 32 | 0x10020;
    const uint64_t b = (uint64_t)2 << 32 | 0x10020; /* Same HWND, new surface. */
    const uint64_t c = (uint64_t)1 << 32 | 0x10022;
    struct epoll_event events[] = {
        {.events = EPOLLIN, .data.u64 = a}, {.events = EPOLLIN, .data.u64 = b},
        {.events = EPOLLHUP, .data.u64 = a}, {.events = EPOLLIN, .data.u64 = c},
        {.events = EPOLLERR, .data.u64 = b}, {.events = EPOLLIN, .data.u64 = a}};

    assert(!wayland_dmabuf_coalesce_events(events, 0));
    assert(wayland_dmabuf_coalesce_events(events, ARRAY_SIZE(events)) == 3);
    assert(events[0].data.u64 == a && events[0].events == (EPOLLIN | EPOLLHUP));
    assert(events[1].data.u64 == b && events[1].events == (EPOLLIN | EPOLLERR));
    assert(events[2].data.u64 == c);
    /* Coalescing must not suppress a later batch for the same host. */
    assert(wayland_dmabuf_coalesce_events(events, 3) == 3);
}

static void test_full_batch(void)
{
    struct epoll_event events[32];
    unsigned int i;

    for (i = 0; i < ARRAY_SIZE(events); i++)
        events[i] = (struct epoll_event){.events = EPOLLIN, .data.u64 = i};
    assert(wayland_dmabuf_coalesce_events(events, ARRAY_SIZE(events)) == ARRAY_SIZE(events));
    for (i = 0; i < ARRAY_SIZE(events); i++) events[i].data.u64 = 7;
    assert(wayland_dmabuf_coalesce_events(events, ARRAY_SIZE(events)) == 1);
    assert(events[0].data.u64 == 7);
}

static void test_consumer_state(void)
{
    const unsigned int active = HWND_DMABUF_RELEASE_CONSUMER_ACTIVE;
    const unsigned int suspended = HWND_DMABUF_RELEASE_CONSUMER_SUSPENDED;
    const unsigned int readiness = HWND_DMABUF_RELEASE_CAP_FD_READINESS;
    static const unsigned int irrelevant[] = {0, HWND_DMABUF_RELEASE_CACHED,
        HWND_DMABUF_RELEASE_PRESENTED, HWND_DMABUF_RELEASE_CAP_ALPHA_MODIFIER};
    enum hwnd_dmabuf_consumer_state state;
    unsigned int i;

    assert(!hwnd_dmabuf_consumer_active(HWND_DMABUF_CONSUMER_UNKNOWN));
    assert(!hwnd_dmabuf_consumer_active(HWND_DMABUF_CONSUMER_SUSPENDED));
    for (i = 0; i < ARRAY_SIZE(irrelevant); i++)
    {
        /* Cached buffers or a capability alone do not establish an active watch. */
        assert(hwnd_dmabuf_consumer_state_from_flags(irrelevant[i]) == HWND_DMABUF_CONSUMER_UNKNOWN);
        assert(hwnd_dmabuf_consumer_state_from_flags(readiness | irrelevant[i]) == HWND_DMABUF_CONSUMER_UNKNOWN);
        state = hwnd_dmabuf_consumer_state_from_flags(active | irrelevant[i]);
        assert(state == HWND_DMABUF_CONSUMER_ACTIVE && hwnd_dmabuf_consumer_active(state));
        state = hwnd_dmabuf_consumer_state_from_flags(active | readiness | irrelevant[i]);
        assert(state == HWND_DMABUF_CONSUMER_ACTIVE_FD && hwnd_dmabuf_consumer_active(state));
    }
    /* Suspend wins, including over a conflicting/stale active indication. */
    assert(hwnd_dmabuf_consumer_state_from_flags(suspended | active | readiness) == HWND_DMABUF_CONSUMER_SUSPENDED);
    state = hwnd_dmabuf_consumer_state_from_flags(suspended);
    assert(!hwnd_dmabuf_consumer_active(state));
    /* A replacement legacy consumer restores posted notifications, not sticky FD mode. */
    state = hwnd_dmabuf_consumer_state_from_flags(active);
    assert(state == HWND_DMABUF_CONSUMER_ACTIVE);
}

static void test_empty_closed(void)
{
    hwnd_dmabuf_frame_desc_t desc;
    int pair[2], fd = 0, sync_fd = 0;

    create_channel(pair);
    assert(!wayland_dmabuf_channel_recv(pair[1], &desc, &fd, &sync_fd));
    assert(fd == -1 && sync_fd == -1);
    close(pair[0]);
    assert(wayland_dmabuf_channel_recv(pair[1], &desc, &fd, &sync_fd) == -1);
    close(pair[1]);
}

static void test_late_registration(void)
{
    hwnd_dmabuf_frame_desc_t desc = frame(7), received;
    int pair[2], epfd, fd, sync_fd;

    create_channel(pair);
    send_packet(pair[0], &desc, sizeof(desc), NULL, 0);
    epfd = watch(pair[1], 17); /* Data arrived before discovery/registration. */
    expect_ready(epfd, 17);
    assert(wayland_dmabuf_channel_recv(pair[1], &received, &fd, &sync_fd) == 1);
    assert(received.frame_seq == 7 && fd == -1 && sync_fd == -1);
    assert(!wayland_dmabuf_channel_recv(pair[1], &received, &fd, &sync_fd));
    expect_quiet(epfd);
    close(epfd);
    close(pair[0]);
    close(pair[1]);
}

static void test_empty_packet_rights(void)
{
    hwnd_dmabuf_frame_desc_t desc;
    int pair[2], fd, sync_fd, backing = eventfd(0, EFD_CLOEXEC);
    unsigned int before;

    assert(backing >= 0);
    create_channel(pair);
    before = open_fd_count();
    send_packet(pair[0], NULL, 0, &backing, 1);
    assert(wayland_dmabuf_channel_recv(pair[1], &desc, &fd, &sync_fd) == -1);
    assert(fd == -1 && sync_fd == -1);
    assert(open_fd_count() == before);
    close(backing);
    close(pair[0]);
    close(pair[1]);
}

static void test_short_packet_drain(void)
{
    hwnd_dmabuf_frame_desc_t desc = frame(9), received;
    int pair[2], epfd, fd, sync_fd, backing = eventfd(0, EFD_CLOEXEC);
    unsigned int before;

    assert(backing >= 0);
    create_channel(pair);
    epfd = watch(pair[1], 21);
    before = open_fd_count();
    send_packet(pair[0], &desc, 1, &backing, 1);
    send_packet(pair[0], &desc, sizeof(desc), NULL, 0);
    expect_ready(epfd, 21);
    assert(wayland_dmabuf_channel_recv(pair[1], &received, &fd, &sync_fd) == 1);
    assert(!received.version && !received.release_token && fd == -1 && sync_fd == -1);
    assert(open_fd_count() == before);
    /* There is no second edge for the already queued valid packet. */
    expect_quiet(epfd);
    assert(wayland_dmabuf_channel_recv(pair[1], &received, &fd, &sync_fd) == 1);
    assert(received.frame_seq == 9);
    assert(!wayland_dmabuf_channel_recv(pair[1], &received, &fd, &sync_fd));
    desc = frame(10);
    send_packet(pair[0], &desc, sizeof(desc), NULL, 0);
    expect_ready(epfd, 21);
    assert(wayland_dmabuf_channel_recv(pair[1], &received, &fd, &sync_fd) == 1);
    assert(received.frame_seq == 10);
    close(backing);
    close(epfd);
    close(pair[0]);
    close(pair[1]);
}

static void test_bad_packet(unsigned int kind)
{
    hwnd_dmabuf_frame_desc_t desc = frame(1), received;
    char oversized[sizeof(desc) + 1];
    int pair[2], fds[4], fd, sync_fd;
    unsigned int before, i;

    create_channel(pair);
    for (i = 0; i < ARRAY_SIZE(fds); i++) assert((fds[i] = eventfd(0, EFD_CLOEXEC)) >= 0);
    before = open_fd_count();
    if (kind == 0)
    {
        memcpy(oversized, &desc, sizeof(desc));
        oversized[sizeof(desc)] = 0;
        send_packet(pair[0], oversized, sizeof(oversized), fds, 1);
    }
    else if (kind == 1) send_packet(pair[0], &desc, sizeof(desc), fds, 4);
    else if (kind == 2) send_packet(pair[0], &desc, sizeof(desc), fds, 2);
    else
    {
        desc.sync_fd_kind = HWND_DMABUF_SYNC_FILE;
        send_packet(pair[0], &desc, sizeof(desc), NULL, 0);
    }
    assert(wayland_dmabuf_channel_recv(pair[1], &received, &fd, &sync_fd) == 1);
    assert(!received.version && fd == -1 && sync_fd == -1);
    assert(received.release_token == (kind == 0 ? 0 : desc.release_token));
    assert(open_fd_count() == before);
    for (i = 0; i < ARRAY_SIZE(fds); i++) close(fds[i]);
    close(pair[0]);
    close(pair[1]);
}

static void test_oversized(void) { test_bad_packet(0); }
static void test_truncated_rights(void) { test_bad_packet(1); }
static void test_extra_rights(void) { test_bad_packet(2); }
static void test_missing_fence(void) { test_bad_packet(3); }

static void test_rights(void)
{
    hwnd_dmabuf_frame_desc_t desc = frame(2), received;
    int pair[2], pipes[2], fds[2], fd, sync_fd;
    struct stat original, imported;
    unsigned int before;

    create_channel(pair);
    assert(!pipe2(pipes, O_CLOEXEC));
    fds[0] = pipes[0];
    assert((fds[1] = eventfd(0, EFD_CLOEXEC)) >= 0);
    before = open_fd_count();
    desc.sync_fd_kind = HWND_DMABUF_SYNC_FILE;
    send_packet(pair[0], &desc, sizeof(desc), fds, 2);
    assert(wayland_dmabuf_channel_recv(pair[1], &received, &fd, &sync_fd) == 1);
    assert(received.version == desc.version && fd >= 0 && sync_fd >= 0);
    assert(fcntl(fd, F_GETFD) & FD_CLOEXEC);
    assert(fcntl(sync_fd, F_GETFD) & FD_CLOEXEC);
    assert(!fstat(pipes[0], &original) && !fstat(fd, &imported));
    assert(original.st_dev == imported.st_dev && original.st_ino == imported.st_ino);
    close(fd);
    close(sync_fd);
    assert(open_fd_count() == before);
    /* A cached slot can supply just a new acquire fence. */
    send_packet(pair[0], &desc, sizeof(desc), &fds[1], 1);
    assert(wayland_dmabuf_channel_recv(pair[1], &received, &fd, &sync_fd) == 1);
    assert(fd == -1 && sync_fd >= 0 && received.version == desc.version);
    close(sync_fd);
    desc.sync_fd_kind = HWND_DMABUF_SYNC_NONE;
    send_packet(pair[0], &desc, sizeof(desc), fds, 1);
    assert(wayland_dmabuf_channel_recv(pair[1], &received, &fd, &sync_fd) == 1);
    assert(fd >= 0 && sync_fd == -1);
    close(fd);
    assert(open_fd_count() == before);
    close(fds[1]);
    close(pipes[0]);
    close(pipes[1]);
    close(pair[0]);
    close(pair[1]);
}

static void test_receive_errors(void)
{
    hwnd_dmabuf_frame_desc_t desc = frame(3), received;
    int pair[2], fd, sync_fd;

    create_channel(pair);
    send_packet(pair[0], &desc, sizeof(desc), NULL, 0);
    recv_error = EINTR;
    recv_errors_remaining = 2;
    assert(wayland_dmabuf_channel_recv(pair[1], &received, &fd, &sync_fd) == 1);
    assert(received.frame_seq == 3 && !recv_errors_remaining);
    recv_error = ECONNRESET;
    recv_errors_remaining = 1;
    assert(wayland_dmabuf_channel_recv(pair[1], &received, &fd, &sync_fd) == -1);
    assert(fd == -1 && sync_fd == -1);
    close(pair[0]);
    close(pair[1]);
}

static void test_multiple_channels(void)
{
    hwnd_dmabuf_frame_desc_t desc = frame(4), received;
    struct epoll_event event = {.events = EPOLLIN | EPOLLET, .data.u64 = 42}, events[4];
    int channels[4][2], epfd = epoll_create1(EPOLL_CLOEXEC), count, fd, sync_fd;
    unsigned int i;

    assert(epfd >= 0);
    for (i = 0; i < ARRAY_SIZE(channels); i++)
    {
        create_channel(channels[i]);
        assert(!epoll_ctl(epfd, EPOLL_CTL_ADD, channels[i][1], &event));
        send_packet(channels[i][0], &desc, sizeof(desc), NULL, 0);
    }
    count = epoll_wait(epfd, events, ARRAY_SIZE(events), 0);
    assert(count == 4 && wayland_dmabuf_coalesce_events(events, count) == 1);
    /* One host dispatch drains all four channels. */
    for (i = 0; i < ARRAY_SIZE(channels); i++)
    {
        assert(wayland_dmabuf_channel_recv(channels[i][1], &received, &fd, &sync_fd) == 1);
        assert(!wayland_dmabuf_channel_recv(channels[i][1], &received, &fd, &sync_fd));
    }
    expect_quiet(epfd);
    send_packet(channels[2][0], &desc, sizeof(desc), NULL, 0);
    expect_ready(epfd, 42);
    for (i = 0; i < ARRAY_SIZE(channels); i++)
    {
        close(channels[i][0]);
        close(channels[i][1]);
    }
    close(epfd);
}

static void test_unregister_and_replace(void)
{
    hwnd_dmabuf_frame_desc_t desc = frame(5);
    int pair[2], duplicate, epfd;

    create_channel(pair);
    epfd = watch(pair[1], 100);
    assert((duplicate = dup(pair[1])) >= 0);
    /* Buffer/release objects may keep the endpoint alive after the surface dies. */
    assert(!epoll_ctl(epfd, EPOLL_CTL_DEL, pair[1], NULL));
    close(pair[1]);
    send_packet(pair[0], &desc, sizeof(desc), NULL, 0);
    expect_quiet(epfd);
    {
        struct epoll_event event = {.events = EPOLLIN | EPOLLET, .data.u64 = 101};
        assert(!epoll_ctl(epfd, EPOLL_CTL_ADD, duplicate, &event));
    }
    expect_ready(epfd, 101);
    close(duplicate);
    close(pair[0]);
    close(epfd);
}

static void test_deferred_fence(void)
{
    uint64_t signal = 1;
    int fence = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK), epfd;

    assert(fence >= 0);
    epfd = watch(fence, 123);
    expect_quiet(epfd);
    /* Models the pollable completion FD without requiring a GPU. */
    assert(write(fence, &signal, sizeof(signal)) == sizeof(signal));
    expect_ready(epfd, 123);
    assert(!epoll_ctl(epfd, EPOLL_CTL_DEL, fence, NULL));
    expect_quiet(epfd);
    close(fence);
    close(epfd);
}

static unsigned int fill_reply_queue(int fd)
{
    hwnd_dmabuf_release_t rel = {.flags = HWND_DMABUF_RELEASE_CONSUMER_ACTIVE |
                                        HWND_DMABUF_RELEASE_CAP_FD_READINESS};
    unsigned int count = 0;
    int ret;

    while (!(ret = wayland_hwnd_dmabuf_channel_send_release(fd, &rel))) count++;
    assert(count && (ret == EAGAIN || ret == EWOULDBLOCK));
    return count;
}

static void test_suspend_reuse(void)
{
    hwnd_dmabuf_release_t rel;
    hwnd_dmabuf_frame_desc_t desc = frame(1), received;
    struct pollfd pfd;
    int pair[2], retained, consumer, fd, sync_fd;

    create_channel(pair);
    assert((retained = dup(pair[1])) >= 0);
    assert(!wayland_dmabuf_channel_suspend(pair[1]));
    close(pair[1]);
    assert(recv(pair[0], &rel, sizeof(rel), MSG_DONTWAIT) == sizeof(rel));
    assert(hwnd_dmabuf_consumer_state_from_flags(rel.flags) == HWND_DMABUF_CONSUMER_SUSPENDED);
    pfd = (struct pollfd){.fd = pair[0], .events = POLLIN};
    assert(!poll(&pfd, 1, 0));

    /* Normal hide/show keeps the retained channel usable. */
    assert((consumer = dup(retained)) >= 0);
    send_packet(pair[0], &desc, sizeof(desc), NULL, 0);
    assert(wayland_dmabuf_channel_recv(consumer, &received, &fd, &sync_fd) == 1);
    assert(received.release_token == desc.release_token && fd == -1 && sync_fd == -1);
    close(consumer);
    close(retained);
    close(pair[0]);
}

static void check_failed_suspend(BOOL fill_frames)
{
    hwnd_dmabuf_frame_desc_t desc = frame(1);
    hwnd_dmabuf_release_t rel;
    struct stat old_stat, duplicate_stat, new_stat;
    struct pollfd pfd;
    int pair[2], replacement[2], retained[2], ret;
    unsigned int i, replies;

    create_channel(pair);
    /* The server retains both endpoints; closing the consumer alone cannot
     * wake a producer waiting for buffer release or presentation feedback. */
    assert((retained[0] = dup(pair[0])) >= 0);
    assert((retained[1] = dup(pair[1])) >= 0);
    replies = fill_reply_queue(pair[1]);
    for (i = 0; i < 8; i++)
    {
        desc = frame(i + 1);
        send_packet(pair[0], &desc, sizeof(desc), NULL, 0);
    }
    if (fill_frames)
    {
        while (send(pair[0], &desc, sizeof(desc), MSG_DONTWAIT | MSG_NOSIGNAL) == sizeof(desc)) {}
        assert(errno == EAGAIN || errno == EWOULDBLOCK);
    }
    else
    {
        pfd = (struct pollfd){.fd = pair[0], .events = POLLOUT};
        assert(poll(&pfd, 1, 0) == 1 && (pfd.revents & POLLOUT));
    }
    /* Otherwise the image ring is exhausted before the frame socket: no
     * failed frame send is needed for teardown to become observable. */
    pfd = (struct pollfd){.fd = pair[0]};
    assert(!poll(&pfd, 1, 0));
    ret = wayland_dmabuf_channel_suspend(pair[1]);
    assert(ret == EAGAIN || ret == EWOULDBLOCK);
    close(pair[1]);
    assert(poll(&pfd, 1, 0) == 1 && (pfd.revents & POLLHUP));
    /* Queued ACTIVE_FD records cannot hide EOF from the release drain. */
    for (i = 0; i < replies; i++)
    {
        assert(recv(pair[0], &rel, sizeof(rel), MSG_DONTWAIT) == sizeof(rel));
        assert(hwnd_dmabuf_consumer_state_from_flags(rel.flags) == HWND_DMABUF_CONSUMER_ACTIVE_FD);
    }
    assert(!recv(pair[0], &rel, sizeof(rel), MSG_DONTWAIT));
    assert(send(pair[0], &desc, sizeof(desc), MSG_DONTWAIT | MSG_NOSIGNAL) == -1 && errno == EPIPE);

    /* Reopen while old producers still exist: duplicate descriptors share
     * consumer state, but a replacement socket must start discovery afresh. */
    create_channel(replacement);
    assert(!fstat(pair[0], &old_stat) && !fstat(retained[0], &duplicate_stat));
    assert(old_stat.st_dev == duplicate_stat.st_dev && old_stat.st_ino == duplicate_stat.st_ino);
    assert(!fstat(replacement[0], &new_stat));
    assert(old_stat.st_dev != new_stat.st_dev || old_stat.st_ino != new_stat.st_ino);
    close(retained[0]);
    close(retained[1]);
    close(pair[0]);
    pfd = (struct pollfd){.fd = replacement[0], .events = POLLIN};
    assert(!poll(&pfd, 1, 0));
    assert(!wayland_dmabuf_channel_suspend(replacement[1]));
    assert(recv(replacement[0], &rel, sizeof(rel), MSG_DONTWAIT) == sizeof(rel));
    close(replacement[0]);
    close(replacement[1]);
}

static void test_suspend_ring_full(void)
{
    check_failed_suspend(FALSE);
}

static void test_suspend_socket_full(void)
{
    check_failed_suspend(TRUE);
}

int main(void)
{
    static const struct { const char *name; void (*run)(void); } tests[] = {
        {"coalesce identities", test_coalesce}, {"full event batch", test_full_batch},
        {"consumer notification states", test_consumer_state}, {"empty and closed channel", test_empty_closed},
        {"suspend and reuse", test_suspend_reuse}, {"suspend with full image ring", test_suspend_ring_full},
        {"suspend with full frame socket", test_suspend_socket_full},
        {"late registration", test_late_registration}, {"empty packet rights", test_empty_packet_rights},
        {"short packet drain", test_short_packet_drain},
        {"oversized packet", test_oversized}, {"truncated rights", test_truncated_rights},
        {"extra rights", test_extra_rights}, {"missing fence", test_missing_fence},
        {"descriptor transfer", test_rights}, {"receive errors", test_receive_errors},
        {"multiple channels", test_multiple_channels}, {"unregister and replace", test_unregister_and_replace},
        {"deferred fence", test_deferred_fence}};
    unsigned int i, before;

    setbuf(stdout, NULL);
    printf("TAP version 13\n1..%u\n", (unsigned int)ARRAY_SIZE(tests));
    for (i = 0; i < ARRAY_SIZE(tests); i++)
    {
        before = open_fd_count();
        tests[i].run();
        assert(open_fd_count() == before);
        printf("ok %u - %s\n", i + 1, tests[i].name);
    }
    return 0;
}
