/* Wine-internal HWND dmabuf producer channel helpers. */

#if 0
#pragma makedep unix
#endif

#include <unistd.h>
#include <string.h>
#include <sys/socket.h>

#include "ntgdi_private.h"
#include "ntuser_private.h"
#include "wine/debug.h"
#include "wine/hwnd_dmabuf.h"
#include "wine/server.h"

WINE_DEFAULT_DEBUG_CHANNEL(hwnd_dmabuf);

#define WM_WINE_HWND_DMABUF_FRAME (WM_WINE_FIRST_DRIVER_MSG + 3)

/* Wake the toplevel that owns hwnd so winewayland recomposites its dmabuf children. */
void hwnd_dmabuf_post_wake( HWND hwnd )
{
    HWND toplevel = NtUserGetAncestor( hwnd, GA_ROOT );

    if (toplevel) NtUserPostMessage( toplevel, WM_WINE_HWND_DMABUF_FRAME, 0, 0 );
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
