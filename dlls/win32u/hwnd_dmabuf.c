/* Wine-internal HWND dmabuf producer channel helpers. */

#if 0
#pragma makedep unix
#endif

#include <unistd.h>
#include <string.h>
#include <sys/socket.h>
#include <errno.h>

#include "ntgdi_private.h"
#include "ntuser_private.h"
#include "wine/hwnd_dmabuf.h"
#include "wine/server.h"

#define WM_WINE_HWND_DMABUF_FRAME (WM_WINE_FIRST_DRIVER_MSG + 3)

/* Wake the toplevel so winewayland recomposites dmabuf children. */
void hwnd_dmabuf_post_wake( HWND hwnd, unsigned int flags )
{
    HWND toplevel = NtUserGetAncestor( hwnd, GA_ROOT );

    if (toplevel) NtUserPostMessage( toplevel, WM_WINE_HWND_DMABUF_FRAME, flags, 0 );
}

unsigned int hwnd_dmabuf_set_pending( HWND hwnd, BOOL pending )
{
    return wine_hwnd_dmabuf_set_pending( hwnd, pending );
}

/* Open the producer end of hwnd's channel. */
int hwnd_dmabuf_open_channel( HWND hwnd )
{
    HANDLE handle = 0;
    int fd = -1;

    if (wine_hwnd_dmabuf_get_channel( hwnd, &handle ) != HWND_DMABUF_OK || !handle)
        return -1;
    if (wine_server_handle_to_fd( handle, FILE_READ_DATA | FILE_WRITE_DATA, &fd, NULL ))
    {
        fd = -1;
        wine_hwnd_dmabuf_release_channel( hwnd );
    }
    NtClose( handle );
    return fd;
}

/* Open an exclusive producer channel for cross-module GL publishers. */
int hwnd_dmabuf_open_channel_exclusive( HWND hwnd )
{
    HANDLE handle = 0;
    int fd = -1;

    if (wine_hwnd_dmabuf_get_channel_exclusive( hwnd, &handle ) != HWND_DMABUF_OK || !handle)
        return -1;
    if (wine_server_handle_to_fd( handle, FILE_READ_DATA | FILE_WRITE_DATA, &fd, NULL ))
    {
        fd = -1;
        wine_hwnd_dmabuf_release_channel( hwnd );
    }
    NtClose( handle );
    return fd;
}

void hwnd_dmabuf_close_channel( HWND hwnd, int channel_fd )
{
    if (channel_fd >= 0) close( channel_fd );
    wine_hwnd_dmabuf_release_channel( hwnd );
}

unsigned int hwnd_dmabuf_release_channel( HWND hwnd )
{
    return wine_hwnd_dmabuf_release_channel( hwnd );
}

/* Send one frame. The caller must reclaim the slot on error. */
int hwnd_dmabuf_channel_send( int channel_fd, const void *desc, int dmabuf_fd, int sync_fd )
{
    hwnd_dmabuf_frame_desc_t frame = *(const hwnd_dmabuf_frame_desc_t *)desc;
    char control[CMSG_SPACE( 2 * sizeof(int) )];
    struct msghdr msg = { 0 };
    struct iovec iov;
    int fds[2], fd_count = 0;
    ssize_t n;

    if (dmabuf_fd >= 0)
        fds[fd_count++] = dmabuf_fd;
    if (sync_fd >= 0)
    {
        frame.sync_fd_kind = HWND_DMABUF_SYNC_FILE;
        fds[fd_count++] = sync_fd;
    }
    else frame.sync_fd_kind = HWND_DMABUF_SYNC_NONE;

    iov.iov_base = &frame;
    iov.iov_len = sizeof(frame);
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    if (fd_count)
    {
        struct cmsghdr *cmsg;
        msg.msg_control = control;
        msg.msg_controllen = CMSG_SPACE( fd_count * sizeof(int) );
        cmsg = CMSG_FIRSTHDR( &msg );
        cmsg->cmsg_level = SOL_SOCKET;
        cmsg->cmsg_type = SCM_RIGHTS;
        cmsg->cmsg_len = CMSG_LEN( fd_count * sizeof(int) );
        memcpy( CMSG_DATA( cmsg ), fds, fd_count * sizeof(int) );
    }

    do n = sendmsg( channel_fd, &msg, MSG_NOSIGNAL | MSG_DONTWAIT );
    while (n < 0 && errno == EINTR);
    if (dmabuf_fd >= 0) close( dmabuf_fd );
    if (sync_fd >= 0) close( sync_fd );
    return n == sizeof(frame) ? 0 : n < 0 ? errno : EMSGSIZE;
}

static int hwnd_dmabuf_channel_result_from_errno( int err )
{
    switch (err)
    {
        case EPIPE:
        case ECONNRESET:
        case ENOTCONN:
        case ECONNABORTED:
#ifdef ESHUTDOWN
        case ESHUTDOWN:
#endif
        case EBADF:
            return HWND_DMABUF_CHANNEL_CLOSED;
        default:
            return HWND_DMABUF_CHANNEL_ERROR;
    }
}

int hwnd_dmabuf_channel_publish( HWND hwnd, int channel_fd, const void *desc,
                                 int dmabuf_fd, int sync_fd )
{
    const hwnd_dmabuf_frame_desc_t *frame = desc;
    int send_fd = -1, send_sync_fd = -1, ret;

    if (dmabuf_fd < 0 && (!frame || !(frame->flags & HWND_DMABUF_FLAG_STABLE_SLOT)))
        return HWND_DMABUF_CHANNEL_ERROR;
    if (dmabuf_fd >= 0 && (send_fd = dup( dmabuf_fd )) < 0)
        return hwnd_dmabuf_channel_result_from_errno( errno );
    if (sync_fd >= 0 && (send_sync_fd = dup( sync_fd )) < 0)
    {
        if (send_fd >= 0) close( send_fd );
        return hwnd_dmabuf_channel_result_from_errno( errno );
    }

    ret = hwnd_dmabuf_channel_send( channel_fd, desc, send_fd, send_sync_fd );
    if (!ret) hwnd_dmabuf_post_wake( hwnd, 0 );
    return ret ? hwnd_dmabuf_channel_result_from_errno( ret ) : HWND_DMABUF_CHANNEL_OK;
}

int hwnd_dmabuf_channel_recv_release( int channel_fd, void *release )
{
    ssize_t n;

    do n = recv( channel_fd, release, sizeof(hwnd_dmabuf_release_t), MSG_DONTWAIT );
    while (n < 0 && errno == EINTR);

    if (n == sizeof(hwnd_dmabuf_release_t)) return HWND_DMABUF_CHANNEL_OK;
#if EAGAIN == EWOULDBLOCK
    if (n < 0 && errno == EAGAIN) return HWND_DMABUF_CHANNEL_EMPTY;
#else
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return HWND_DMABUF_CHANNEL_EMPTY;
#endif
    if (!n) return HWND_DMABUF_CHANNEL_CLOSED;
    return n < 0 ? hwnd_dmabuf_channel_result_from_errno( errno ) : HWND_DMABUF_CHANNEL_ERROR;
}

int WINAPI NtUserHwndDmaBufOpenProducer( HWND hwnd )
{
    return hwnd_dmabuf_open_channel_exclusive( hwnd );
}

void WINAPI NtUserHwndDmaBufCloseProducer( HWND hwnd, int channel_fd )
{
    hwnd_dmabuf_close_channel( hwnd, channel_fd );
}

int WINAPI NtUserHwndDmaBufPublish( HWND hwnd, int channel_fd, const void *desc,
                                    int dmabuf_fd, int sync_fd )
{
    if (!desc) return HWND_DMABUF_CHANNEL_ERROR;
    return hwnd_dmabuf_channel_publish( hwnd, channel_fd, desc, dmabuf_fd, sync_fd );
}

int WINAPI NtUserHwndDmaBufDrainRelease( int channel_fd, void *release )
{
    if (!release) return HWND_DMABUF_CHANNEL_ERROR;
    return hwnd_dmabuf_channel_recv_release( channel_fd, release );
}
