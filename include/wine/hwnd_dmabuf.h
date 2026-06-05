/* Wine-internal HWND dmabuf bridge helpers. */

#ifndef __WINE_HWND_DMABUF_H
#define __WINE_HWND_DMABUF_H

#include <wine/server.h>

#define HWND_DMABUF_HOST_CAPS_VERSION_V1       1
#define HWND_DMABUF_HOST_CAPS_VERSION_V2       2

#define HWND_DMABUF_CAPS_HAS_MOD_INVALID       0x00000004

typedef struct
{
    unsigned int     fourcc;
    unsigned int     tranche_index;
    unsigned int     tranche_flags;
    unsigned __int64 modifier;
} hwnd_dmabuf_format_modifier_t;

typedef struct
{
    unsigned int     version;
    unsigned int     feedback_gen;
    unsigned int     dmabuf_protocol_version;
    unsigned int     caps_source;
    unsigned int     caps_flags;
    unsigned int     has_drm_syncobj;
    unsigned int     has_zwp_explicit_sync;
    unsigned int     main_device_major;
    unsigned int     main_device_minor;
    unsigned int     format_modifier_count;
    const hwnd_dmabuf_format_modifier_t *format_modifiers;
} hwnd_dmabuf_host_caps_t;

/* Release token sent from the consumer back to the producer over the channel. */
typedef struct
{
    unsigned __int64 producer_unique_id;
    unsigned __int64 release_token;
} hwnd_dmabuf_release_t;

static inline enum hwnd_dmabuf_status wine_hwnd_dmabuf_list( HWND host_hwnd,
                                                             hwnd_dmabuf_frame_info_t *frames,
                                                             unsigned int max_frames,
                                                             unsigned int *total_count,
                                                             unsigned int *copied_count )
{
    enum hwnd_dmabuf_status status = HWND_DMABUF_INVALID_ARGS;
    data_size_t reply_size = 0;

    if (total_count) *total_count = 0;
    if (copied_count) *copied_count = 0;
    if (max_frames && !frames) return status;
    if (max_frames > (data_size_t)-1 / sizeof(*frames)) return status;

    SERVER_START_REQ( hwnd_list_dmabuf_frames )
    {
        req->host_hwnd = wine_server_user_handle( host_hwnd );
        reply_size = max_frames * sizeof(*frames);
        wine_server_set_reply( req, frames, reply_size );
        if (!wine_server_call_err( req ))
        {
            status = reply->status;
            if (total_count) *total_count = reply->count;
            if (copied_count) *copied_count = wine_server_reply_size( reply ) / sizeof(*frames);
        }
    }
    SERVER_END_REQ;

    return status;
}

static inline enum hwnd_dmabuf_status wine_hwnd_dmabuf_get_channel( HWND hwnd, HANDLE *channel_handle )
{
    enum hwnd_dmabuf_status status = HWND_DMABUF_INVALID_ARGS;

    if (!channel_handle) return status;
    *channel_handle = 0;

    SERVER_START_REQ( hwnd_dmabuf_get_channel )
    {
        req->hwnd = wine_server_user_handle( hwnd );
        if (!wine_server_call_err( req ))
        {
            status = reply->status;
            if (status == HWND_DMABUF_OK)
                *channel_handle = wine_server_ptr_handle( reply->channel_handle );
        }
    }
    SERVER_END_REQ;

    return status;
}

static inline enum hwnd_dmabuf_status wine_hwnd_dmabuf_claim_channel( HWND hwnd, HANDLE *channel_handle )
{
    enum hwnd_dmabuf_status status = HWND_DMABUF_INVALID_ARGS;

    if (!channel_handle) return status;
    *channel_handle = 0;

    SERVER_START_REQ( hwnd_dmabuf_claim_channel )
    {
        req->hwnd = wine_server_user_handle( hwnd );
        if (!wine_server_call_err( req ))
        {
            status = reply->status;
            if (status == HWND_DMABUF_OK)
                *channel_handle = wine_server_ptr_handle( reply->channel_handle );
        }
    }
    SERVER_END_REQ;

    return status;
}

#endif /* __WINE_HWND_DMABUF_H */