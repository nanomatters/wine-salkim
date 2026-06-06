/* Wine-internal HWND dmabuf producer syscall helpers. */

#if 0
#pragma makedep unix
#endif

#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <sys/socket.h>

#include "ntgdi_private.h"
#include "ntuser_private.h"
#include "wine/debug.h"
#include "wine/hwnd_dmabuf.h"
#include "wine/list.h"
#include "wine/server.h"

WINE_DEFAULT_DEBUG_CHANNEL(hwnd_dmabuf);

#define WM_WINE_HWND_DMABUF_FRAME (WM_WINE_FIRST_DRIVER_MSG + 3)

/* Wake the toplevel that owns hwnd so winewayland recomposites its dmabuf children. */
void hwnd_dmabuf_post_wake( HWND hwnd )
{
    HWND toplevel = NtUserGetAncestor( hwnd, GA_ROOT );

    if (toplevel) NtUserPostMessage( toplevel, WM_WINE_HWND_DMABUF_FRAME, 0, 0 );
}

/* Per-hwnd producer channel cache for the GL syscall producer (no swapchain to hold the fd). */
struct hwnd_dmabuf_channel
{
    struct list entry;
    HWND hwnd;
    int fd;
};

static struct list hwnd_dmabuf_channels = LIST_INIT( hwnd_dmabuf_channels );
static pthread_mutex_t hwnd_dmabuf_channels_lock = PTHREAD_MUTEX_INITIALIZER;

/* Cached producer channel fd for hwnd, opened on first use. Caller holds the lock. */
static int hwnd_dmabuf_cached_channel( HWND hwnd )
{
    struct hwnd_dmabuf_channel *chan;
    int fd;

    LIST_FOR_EACH_ENTRY( chan, &hwnd_dmabuf_channels, struct hwnd_dmabuf_channel, entry )
        if (chan->hwnd == hwnd) return chan->fd;

    if ((fd = hwnd_dmabuf_open_channel( hwnd )) < 0) return -1;
    if (!(chan = malloc( sizeof(*chan) ))) { close( fd ); return -1; }
    chan->hwnd = hwnd;
    chan->fd = fd;
    list_add_tail( &hwnd_dmabuf_channels, &chan->entry );
    return fd;
}

/* Drain and discard releases: the GL producer recycles by ring depth, not by token. */
static void hwnd_dmabuf_drain_releases( int channel_fd )
{
    hwnd_dmabuf_release_t rel;
    while (recv( channel_fd, &rel, sizeof(rel), MSG_DONTWAIT ) == (ssize_t)sizeof(rel)) ;
}

/* Close and forget the cached channel for hwnd (called from destroy_window). */
void hwnd_dmabuf_drop_channel( HWND hwnd )
{
    struct hwnd_dmabuf_channel *chan, *next;

    pthread_mutex_lock( &hwnd_dmabuf_channels_lock );
    LIST_FOR_EACH_ENTRY_SAFE( chan, next, &hwnd_dmabuf_channels, struct hwnd_dmabuf_channel, entry )
        if (chan->hwnd == hwnd)
        {
            list_remove( &chan->entry );
            close( chan->fd );
            free( chan );
        }
    pthread_mutex_unlock( &hwnd_dmabuf_channels_lock );
}

/* Send a rendered frame for hwnd over its channel; the GL producer's NtUserPublishHwndDmabuf shim. */
UINT hwnd_dmabuf_publish( HWND hwnd, int dmabuf_fd, int acquire_sync_fd,
                          const void *desc_ptr, UINT *frame_seq )
{
    const hwnd_dmabuf_frame_desc_t *desc = desc_ptr;
    int channel_fd;

    TRACE("hwnd %p, dmabuf_fd %d, acquire_sync_fd %d, desc %p.\n",
          hwnd, dmabuf_fd, acquire_sync_fd, desc);

    if (frame_seq) *frame_seq = 0;
    /* acquire_sync unused: the producer waits for its own render before exporting. */
    if (acquire_sync_fd >= 0) close( acquire_sync_fd );

    if (!hwnd || dmabuf_fd < 0 || !desc)
    {
        if (dmabuf_fd >= 0) close( dmabuf_fd );
        return HWND_DMABUF_INVALID_ARGS;
    }

    pthread_mutex_lock( &hwnd_dmabuf_channels_lock );
    if ((channel_fd = hwnd_dmabuf_cached_channel( hwnd )) >= 0)
    {
        hwnd_dmabuf_drain_releases( channel_fd );
        hwnd_dmabuf_channel_send( channel_fd, desc, dmabuf_fd ); /* closes dmabuf_fd */
    }
    pthread_mutex_unlock( &hwnd_dmabuf_channels_lock );

    if (channel_fd < 0)
    {
        close( dmabuf_fd );
        return HWND_DMABUF_NOT_FOUND;
    }

    hwnd_dmabuf_post_wake( hwnd );
    if (frame_seq) *frame_seq = desc->frame_seq;
    return HWND_DMABUF_OK;
}

UINT WINAPI NtUserPublishHwndDmabuf( HWND hwnd, int dmabuf_fd, int acquire_sync_fd,
                                     const void *desc_ptr, UINT *frame_seq )
{
    return hwnd_dmabuf_publish( hwnd, dmabuf_fd, acquire_sync_fd, desc_ptr, frame_seq );
}

/* Open (server-mint) the producer end of hwnd's channel; returns a socket fd or -1. */
int hwnd_dmabuf_open_channel( HWND hwnd )
{
    HANDLE handle = 0;
    int fd = -1;

    if (wine_hwnd_dmabuf_get_channel( hwnd, &handle ) != HWND_DMABUF_OK || !handle)
        return -1;
    if (wine_server_handle_to_fd( handle, FILE_READ_DATA | FILE_WRITE_DATA, &fd, NULL ))
        fd = -1;
    NtClose( handle );
    return fd;
}

/* Send one frame (desc + dmabuf fd via SCM_RIGHTS) over the channel; closes the fd, drops if full. */
void hwnd_dmabuf_channel_send( int channel_fd, const void *desc, int dmabuf_fd )
{
    char control[CMSG_SPACE( sizeof(int) )];
    struct msghdr msg = { 0 };
    struct cmsghdr *cmsg;
    struct iovec iov;

    iov.iov_base = (void *)desc;
    iov.iov_len = sizeof(hwnd_dmabuf_frame_desc_t);
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = control;
    msg.msg_controllen = sizeof(control);
    cmsg = CMSG_FIRSTHDR( &msg );
    cmsg->cmsg_level = SOL_SOCKET;
    cmsg->cmsg_type = SCM_RIGHTS;
    cmsg->cmsg_len = CMSG_LEN( sizeof(int) );
    memcpy( CMSG_DATA( cmsg ), &dmabuf_fd, sizeof(int) );

    sendmsg( channel_fd, &msg, MSG_NOSIGNAL | MSG_DONTWAIT );
    close( dmabuf_fd );
}