/*
 * Wayland driver
 *
 * Copyright 2020 Alexandros Frantzis for Collabora Ltd
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

#ifndef __WINE_WAYLANDDRV_H
#define __WINE_WAYLANDDRV_H

#ifndef __WINE_CONFIG_H
# error You must include config.h to use this header
#endif

#include <pthread.h>
#include <sys/types.h>
#include <wayland-client.h>
#include <xkbcommon/xkbcommon.h>
#include <xkbcommon/xkbregistry.h>
#ifdef HAVE_XKBCOMMON_XKBCOMMON_COMPOSE_H
#include <xkbcommon/xkbcommon-compose.h>
#else
struct xkb_compose_table;
#endif
#include "cursor-shape-v1-client-protocol.h"
#include "pointer-constraints-unstable-v1-client-protocol.h"
#include "relative-pointer-unstable-v1-client-protocol.h"
#include "text-input-unstable-v3-client-protocol.h"
#include "viewporter-client-protocol.h"
#include "linux-dmabuf-unstable-v1-client-protocol.h"
#include "linux-drm-syncobj-v1-client-protocol.h"
#include "linux-explicit-synchronization-unstable-v1-client-protocol.h"
#include "xdg-output-unstable-v1-client-protocol.h"
#include "xdg-shell-client-protocol.h"
#include "wlr-data-control-unstable-v1-client-protocol.h"
#include "ext-data-control-v1-client-protocol.h"
#include "xdg-toplevel-icon-v1-client-protocol.h"
#include "pointer-warp-v1-client-protocol.h"
#include "alpha-modifier-v1-client-protocol.h"
#include "fractional-scale-v1-client-protocol.h"
#include "xdg-toplevel-tag-v1-client-protocol.h"
#include "content-type-v1-client-protocol.h"
#include "xdg-decoration-unstable-v1-client-protocol.h"
#include "color-management-v1-client-protocol.h"
#include "xdg-activation-v1-client-protocol.h"
#include "keyboard-shortcuts-inhibit-unstable-v1-client-protocol.h"
#include "wlr-layer-shell-unstable-v1-client-protocol.h"

#include "windef.h"
#include "winbase.h"
#include "ntgdi.h"
#include "wine/gdi_driver.h"
#include "wine/list.h"
#include "wine/rbtree.h"
#include "wine/wayland_external_input.h"
#include "wine/vulkan_driver.h"

#include "unixlib.h"

/* We only use 4 byte formats. */
#define WINEWAYLAND_BYTES_PER_PIXEL 4

/**********************************************************************
 *          Globals
 */

extern char *process_name;
extern struct wayland process_wayland;

/**********************************************************************
 *          Definitions for wayland types
 */

enum wayland_window_message
{
    WM_WAYLAND_INIT_DISPLAY_DEVICES = WM_WINE_FIRST_DRIVER_MSG,
    WM_WAYLAND_CONFIGURE,
    WM_WAYLAND_SET_FOREGROUND,
    WM_WAYLAND_DMABUF_FRAME,
    WM_WAYLAND_EXPOSE,
};

#define WAYLAND_ACTIVATION_TOKEN_MAGIC 0x54434158 /* XACT */
#define WAYLAND_ACTIVATION_TOKEN_MAX_SIZE 65536

enum wayland_activation_serial_kind
{
    WAYLAND_ACTIVATION_SERIAL_INPUT,
    WAYLAND_ACTIVATION_SERIAL_POINTER_FOCUS,
    WAYLAND_ACTIVATION_SERIAL_KEYBOARD_FOCUS,
    WAYLAND_ACTIVATION_SERIAL_COUNT,
};

struct wayland_activation_serial
{
    HWND hwnd;
    uint32_t serial;
    UINT64 generation;
};

enum wayland_surface_config_state
{
    WAYLAND_SURFACE_CONFIG_STATE_MAXIMIZED = (1 << 0),
    WAYLAND_SURFACE_CONFIG_STATE_RESIZING = (1 << 1),
    WAYLAND_SURFACE_CONFIG_STATE_TILED = (1 << 2),
    WAYLAND_SURFACE_CONFIG_STATE_FULLSCREEN = (1 << 3),
    WAYLAND_SURFACE_CONFIG_STATE_SUSPENDED = (1 << 4)
};

enum wayland_surface_role
{
    WAYLAND_SURFACE_ROLE_NONE,
    WAYLAND_SURFACE_ROLE_TOPLEVEL,
    WAYLAND_SURFACE_ROLE_SUBSURFACE,
    WAYLAND_SURFACE_ROLE_POPUP,
    WAYLAND_SURFACE_ROLE_LAYER,
};

enum wayland_surface_wm_caps
{
    WAYLAND_SURFACE_WM_CAPS_CHANGED = (1 << 0),
    WAYLAND_SURFACE_WM_CAPS_SHOW_WINDOW = (1 << 1),
    WAYLAND_SURFACE_WM_CAPS_MAXIMIZE = (1 << 2),
    WAYLAND_SURFACE_WM_CAPS_FULLSCREEN = (1 << 3),
    WAYLAND_SURFACE_WM_CAPS_MINIMIZE = (1 << 4)
};

enum wayland_surface_ensure_type
{
    WAYLAND_SURFACE_NOT_ENSURED = 0,
    WAYLAND_SURFACE_ENSURED_FLUSH,
    WAYLAND_SURFACE_ENSURED_DUMMY_BUFFER,
};

struct wayland_keyboard
{
    struct wl_keyboard *wl_keyboard;
    struct xkb_context *xkb_context;
    struct xkb_state *xkb_state;
    HWND focused_hwnd;
    pthread_mutex_t mutex;
    /* scancode -> keystate mapping. The keyboard state is the same across every instance of a seat.
     * We bind to the first seat each time, and this first bind seat should be the same every time. */
    unsigned char keystate[0x300];
    HWND key_hwnds[0x300];
};

struct wayland_cursor
{
    struct wayland_shm_buffer *shm_buffer;
    struct wl_surface *wl_surface;
    struct wp_viewport *wp_viewport;
    int hotspot_x, hotspot_y;
};

enum wayland_pointer_frame_flags
{
    WAYLAND_POINTER_FRAME_RELATIVE = (1 << 0),
    WAYLAND_POINTER_FRAME_ABSOLUTE = (1 << 1),
    WAYLAND_POINTER_FRAME_DISCRETE_WHEEL = (1 << 2),
    WAYLAND_POINTER_FRAME_DISCRETE_WHEEL_HORZ = (1 << 3),
    WAYLAND_POINTER_FRAME_AXIS = (1 << 4),
    WAYLAND_POINTER_FRAME_AXIS_HORZ = (1 << 5),
    WAYLAND_POINTER_FRAME_AXIS_STOP = (1 << 6),
    WAYLAND_POINTER_FRAME_AXIS_HORZ_STOP = (1 << 7)
};

struct wayland_pointer_frame
{
    LONG x, y;
    LONG external_x, external_y;
    LONG external_width, external_height;
    double dx, dy;
    double external_dx, external_dy;
    double dx_raw, dy_raw;
    double axis, horz_axis;
    LONG scroll, horz_scroll;

    enum wayland_pointer_frame_flags flags;
};

enum wayland_pointer_constraint_state
{
    WAYLAND_POINTER_CONSTRAINT_NONE,
    WAYLAND_POINTER_CONSTRAINT_PENDING,
    WAYLAND_POINTER_CONSTRAINT_ACTIVE,
    WAYLAND_POINTER_CONSTRAINT_INACTIVE,
};

struct wayland_pointer
{
    struct wl_pointer *wl_pointer;
    struct zwp_confined_pointer_v1 *zwp_confined_pointer_v1;
    struct zwp_locked_pointer_v1 *zwp_locked_pointer_v1;
    struct zwp_relative_pointer_v1 *zwp_relative_pointer_v1;
    struct wp_cursor_shape_device_v1 *wp_cursor_shape_device_v1;
    struct wl_surface *focused_wl_surface;
    struct wl_surface *constraint_wl_surface;
    HWND focused_hwnd;
    HWND constraint_hwnd;
    enum wayland_pointer_constraint_state constraint_state;
    RECT confine_rect;
    BOOL confine_rect_valid;
    BOOL pending_warp;
    POINT warp;
    BOOL relative_mode;
    BOOL external_input_active;
    uint32_t enter_serial;
    uint32_t button_serial;
    uint32_t popup_serial;
    HWND popup_serial_hwnd;
    UINT64 popup_serial_time;
    struct wayland_cursor cursor;
    struct wayland_pointer_frame frame;
    pthread_mutex_t mutex;
};

struct wayland_touch_point
{
    struct wl_list link;
    struct wl_surface *focused_wl_surface;
    LPARAM xy;
    HWND focused_hwnd;
    int32_t id;
};

struct wayland_touch
{
    struct wl_touch *wl_touch;
    struct wl_list touch_points;
};

struct wayland_text_input
{
    struct zwp_text_input_v3 *zwp_text_input_v3;
    struct
    {
        WCHAR *string;
        DWORD cursor_pos;
    } preedit, current_preedit;
    WCHAR *commit_string;
    HWND focused_hwnd;
    BOOL enabled;
    pthread_mutex_t mutex;
};

struct wayland_seat
{
    struct wl_seat *wl_seat;
    uint32_t global_id;
    pthread_mutex_t mutex;
};

struct wayland_data_device
{
    union
    {
        struct
        {
            struct zwlr_data_control_device_v1 *zwlr_data_control_device_v1;
            struct zwlr_data_control_source_v1 *zwlr_data_control_source_v1;
            struct zwlr_data_control_offer_v1 *clipboard_zwlr_data_control_offer_v1;
        };
        struct
        {
            struct ext_data_control_device_v1 *ext_data_control_device_v1;
            struct ext_data_control_source_v1 *ext_data_control_source_v1;
            struct ext_data_control_offer_v1 *clipboard_ext_data_control_offer_v1;
        };
        struct
        {
            struct wl_data_device *wl_data_device;
            struct wl_data_source *wl_data_source;
            struct wl_data_offer *clipboard_wl_data_offer;
        };
    };
    pthread_mutex_t mutex;
};

struct wayland_dmabuf_format
{
    struct wl_list link;
    uint32_t format;
    uint32_t tranche_index;
    uint32_t tranche_flags;
    uint64_t modifier;
};

#define DRM_FORMAT_MOD_INVALID 0x00ffffffffffffffull

struct wayland_dmabuf_feedback_format
{
    uint32_t format;
    uint32_t padding;
    uint64_t modifier;
};

struct wayland_dmabuf_feedback
{
    struct zwp_linux_dmabuf_feedback_v1 *zwp_linux_dmabuf_feedback_v1;
    struct wayland_dmabuf_feedback_format *format_table;
    size_t format_table_count;
    struct wl_list pending_formats;
    uint32_t tranche_index;
    uint32_t tranche_flags;
    dev_t main_device;
    BOOL has_main_device;
    BOOL valid;
};

struct wayland
{
    BOOL initialized;
    struct wl_display *wl_display;
    struct wl_event_queue *wl_event_queue;
    struct wl_registry *wl_registry;
    struct zxdg_output_manager_v1 *zxdg_output_manager_v1;
    struct wl_compositor *wl_compositor;
    struct xdg_wm_base *xdg_wm_base;
    struct wl_shm *wl_shm;
    struct wp_viewporter *wp_viewporter;
    struct wl_subcompositor *wl_subcompositor;
    struct zwp_linux_dmabuf_v1 *zwp_linux_dmabuf_v1;
    struct wp_linux_drm_syncobj_manager_v1 *wp_linux_drm_syncobj_manager_v1;
    struct zwp_linux_explicit_synchronization_v1 *zwp_linux_explicit_synchronization_v1;
    struct wayland_dmabuf_feedback dmabuf_default_feedback;
    struct wl_list dmabuf_formats;
    pthread_mutex_t dmabuf_mutex;
    struct wp_fractional_scale_manager_v1 *wp_fractional_scale_manager_v1;
    struct zwp_pointer_constraints_v1 *zwp_pointer_constraints_v1;
    struct zwp_relative_pointer_manager_v1 *zwp_relative_pointer_manager_v1;
    struct zwp_text_input_manager_v3 *zwp_text_input_manager_v3;
    struct zwlr_data_control_manager_v1 *zwlr_data_control_manager_v1;
    struct ext_data_control_manager_v1 *ext_data_control_manager_v1;
    struct wl_data_device_manager *wl_data_device_manager;
    struct xdg_toplevel_icon_manager_v1 *xdg_toplevel_icon_manager_v1;
    struct wp_cursor_shape_manager_v1 *wp_cursor_shape_manager_v1;
    struct wp_pointer_warp_v1 *wp_pointer_warp_v1;
    struct wp_alpha_modifier_v1 *wp_alpha_modifier_v1;
    struct xdg_toplevel_tag_manager_v1 *xdg_toplevel_tag_manager_v1;
    struct wp_content_type_manager_v1 *wp_content_type_manager_v1;
    struct zxdg_decoration_manager_v1 *zxdg_decoration_manager_v1;
    struct wp_color_manager_v1 *wp_color_manager_v1;
    struct xdg_activation_v1 *xdg_activation_v1;
    struct zwp_keyboard_shortcuts_inhibit_manager_v1* zwp_keyboard_shortcuts_inhibit_manager_v1;
    struct zwlr_layer_shell_v1 *zwlr_layer_shell_v1;
    struct wayland_seat seat;
    struct wayland_keyboard keyboard;
    struct wayland_pointer pointer;
    struct wayland_touch touch;
    struct wayland_text_input text_input;
    struct wayland_data_device data_device;
    struct wl_list output_list;
    struct wl_array output_info_array;
    /* Protects the output_list, output_info_array, and the wayland_output.current states. */
    pthread_mutex_t output_mutex;
    LONG input_serial;
    pthread_mutex_t activation_mutex;
    /* Serials remain associated with the window that received them. */
    struct wayland_activation_serial activation_serials[WAYLAND_ACTIVATION_SERIAL_COUNT];
    UINT64 activation_serial_generation;
    BOOL supports_parametric;
    BOOL supports_pq;
    BOOL supports_win_scrgb;
    BOOL supports_windows_bt2100;
    BOOL supports_set_primaries;
    BOOL supports_set_luminances;
    BOOL supports_bt2020_primaries;
    BOOL supports_extended_volume;
};

struct wayland_syncobj_buffer;
struct wayland_syncobj_release;

typedef void (*wayland_syncobj_release_func)(void *data, BOOL released);

BOOL wayland_syncobj_init(void);
BOOL wayland_syncobj_available(void);
struct wayland_syncobj_buffer *wayland_syncobj_buffer_create(void);
void wayland_syncobj_buffer_destroy_wayland(struct wayland_syncobj_buffer *buffer);
void wayland_syncobj_buffer_destroy(struct wayland_syncobj_buffer *buffer);
void wayland_syncobj_buffer_remove_surface(struct wayland_syncobj_buffer *buffer,
                                           struct wl_surface *surface);
BOOL wayland_syncobj_prepare_acquire(struct wayland_syncobj_buffer *buffer, int sync_fd,
                                     UINT64 *point);
struct wayland_syncobj_release *wayland_syncobj_prepare_release(
        struct wayland_syncobj_buffer *buffer, struct wl_surface *surface,
        wayland_syncobj_release_func callback, void *data);
BOOL wayland_syncobj_surface_set_points(
        struct wp_linux_drm_syncobj_surface_v1 **syncobj_surface,
        struct wl_surface *surface, struct wayland_syncobj_buffer *buffer,
        UINT64 acquire_point, struct wayland_syncobj_release *release);
void wayland_syncobj_release_commit(struct wayland_syncobj_release *release);
void wayland_syncobj_release_cancel(struct wayland_syncobj_release *release);
BOOL wayland_dmabuf_import_sync_file(int dmabuf_fd, int sync_fd);
BOOL wayland_sync_file_wait(int sync_fd);

struct wayland_output_mode
{
    struct rb_entry entry;
    int32_t width;
    int32_t height;
    int32_t refresh;
};

struct wayland_primaries
{
    int32_t r_x;
    int32_t r_y;
    int32_t g_x;
    int32_t g_y;
    int32_t b_x;
    int32_t b_y;
    int32_t w_x;
    int32_t w_y;
};

struct wayland_output_state
{
    int modes_count;
    struct rb_tree modes;
    struct wayland_output_mode *current_mode;
    struct wayland_primaries primaries;
    char *name;
    char *make;
    char *model;
    int logical_x, logical_y;
    int logical_w, logical_h;
    int physical_w, physical_h;
    int transform;
    uint32_t max_fall;
    uint32_t max_cll;
    uint32_t max_target_lum;
    uint32_t ref_lum;
    BOOL supports_hdr;
};

struct wayland_output
{
    struct wl_list link;
    struct wl_output *wl_output;
    struct zxdg_output_v1 *zxdg_output_v1;
    struct wp_image_description_v1 *wp_image_description_v1;
    struct wp_image_description_info_v1 *wp_image_description_info_v1;
    struct wp_color_management_output_v1 *wp_color_management_output_v1;
    uint32_t global_id;
    unsigned int pending_flags;
    LONG ref;
    struct wayland_output_state pending, current;
};

struct output_info
{
    int x, y;
    BOOL is_primary;
    struct wayland_output_state *output;
};

struct wayland_surface_config
{
    RECT rect;
    /* Raw configure size in surface-local units, plus the scale used when
     * converting it to rect. */
    int32_t surface_width, surface_height;
    double scale;
    int32_t bounds_width, bounds_height;
    enum wayland_surface_config_state state;
    enum zxdg_toplevel_decoration_v1_mode decor;
    enum wayland_surface_wm_caps caps;
    uint32_t serial;
    BOOL processed;
    BOOL bounds_set;
};

struct wayland_toplevel_size_limits
{
    int min_width, min_height;
    int max_width, max_height;
    BOOL valid;
};

enum wayland_child_visibility
{
    WAYLAND_CHILD_VISIBILITY_AS_IS,
    WAYLAND_CHILD_VISIBILITY_CROPPED,
    WAYLAND_CHILD_VISIBILITY_UNMASKABLE,
};

struct wayland_visual_constraint
{
    BOOL valid;
    enum wayland_child_visibility visibility;
    RECT dst;
    RECT rect;
    unsigned int rect_count;
};

struct wayland_window_config
{
    RECT rect;
    RECT window_rect;
    RECT client_rect;
    /* Window-surface buffer pixels presented by the parent wl_surface. */
    RECT shm_source;
    enum wayland_surface_config_state state;
    /* The scale (i.e., normalized dpi) the window is rendering at. */
    double scale;
    BOOL visible;
    BOOL managed;
    BOOL resizeable;
    BOOL minimized;
    BOOL preserve_fullscreen_size;
};

struct wayland_retired_wl_surface
{
    UINT64 host_surface;
    struct wl_surface *wl_surface;
    struct wl_subsurface *handoff_subsurface;
};

enum wayland_image_description_color_space
{
    WAYLAND_IMAGE_DESCRIPTION_DEFAULT,
    WAYLAND_IMAGE_DESCRIPTION_SCRGB,
    WAYLAND_IMAGE_DESCRIPTION_BT2100,
};

enum wayland_image_description_status
{
    WAYLAND_IMAGE_DESCRIPTION_UNINITIALIZED,
    WAYLAND_IMAGE_DESCRIPTION_UNSUPPORTED,
    WAYLAND_IMAGE_DESCRIPTION_PENDING,
    WAYLAND_IMAGE_DESCRIPTION_READY,
    WAYLAND_IMAGE_DESCRIPTION_FAILED,
};

struct wayland_image_description_state
{
    enum wayland_image_description_color_space color_space;
    const struct wl_surface *wl_surface;
};

struct wayland_fullscreen_request
{
    struct list entry;
    UINT64 owner;
    RECT rect;
    enum vulkan_surface_fullscreen_target target;
};

struct wayland_client_surface
{
    struct client_surface client;
    HWND toplevel;
    HANDLE throttle;
    struct wl_callback *wl_callback;
    BOOL hwnd_dmabuf_producer;
    LONG direct_toplevel;
    LONG direct_toplevel_invalidated;
    BOOL owns_wl_surface;
    BOOL owns_direct_wl_surface;
    UINT64 direct_host_surface;
    RECT rect;
    struct wl_surface *wl_surface;
    struct wl_surface *direct_wl_surface;
    /* wl_surfaces retired by direct-toplevel promotions and demotions. They
     * remain live until their matching host VkSurfaceKHR is destroyed. */
    struct wayland_retired_wl_surface *retired_wl_surfaces;
    unsigned int retired_wl_surface_count;
    const struct wl_surface *toplevel_wl_surface;
    struct wl_subsurface *wl_subsurface;
    BOOL stack_above_parent;
    /* Protected by client.presentation_mutex. */
    struct wp_color_management_surface_v1 *wp_color_management_surface_v1;
    struct wayland_image_description_state image_description_state;
    LONG has_image_description;
    struct wp_viewport *wp_viewport;
    struct wp_content_type_v1 *wp_content_type_v1;
    LONG opaque_region_state;
    /* if true then the client surface has an alpha channel controlling transparency */
    LONG has_alpha;
    LONG has_presented;
    LONG attachment_generation;
    LONG updated_attachment_generation;
    struct list fullscreen_requests;
    UINT64 fullscreen_active_owner;
    struct wayland_visual_constraint visual_constraint;
};

struct wayland_shm_buffer
{
    struct wl_list link;
    struct wl_buffer *wl_buffer;
    int width, height;
    uint32_t format;
    void *map_data;
    size_t map_size;
    BOOL busy;
    LONG ref;
    HRGN damage_region;
};

struct wayland_hwnd_dmabuf_surface;
struct wayland_win_data;

struct wayland_surface
{
    HWND hwnd;
    unsigned int serial;

    struct wl_surface *wl_surface;
    LONG pending_commit;
    struct wp_viewport *wp_viewport;
    struct wp_viewport *configured_wp_viewport;
    int viewport_dest_width, viewport_dest_height;
    struct wp_fractional_scale_v1 *wp_fractional_scale_v1;
    struct zwp_keyboard_shortcuts_inhibitor_v1* zwp_keyboard_shortcuts_inhibitor_v1;
    struct wayland_shm_buffer *small_icon_buffer;
    struct wayland_shm_buffer *big_icon_buffer;

    enum wayland_surface_role role;

    struct xdg_surface *xdg_surface;
    HWND owner_hwnd;

    union
    {
        struct
        {
            struct xdg_toplevel *xdg_toplevel;
            struct xdg_toplevel_icon_v1 *xdg_toplevel_icon;
            struct zxdg_toplevel_decoration_v1 *zxdg_toplevel_decoration_v1;
            /* Fullscreen requested by Wine and not rejected by the compositor. */
            const struct wl_output *requested_output;
            BOOL fullscreen_requested;
        };
        struct
        {
            struct xdg_popup *xdg_popup;
            BOOL xdg_popup_grabbed;
        };
        struct
        {
            struct wl_subsurface *wl_subsurface;
            HWND toplevel_hwnd;
            unsigned int parent_serial;
        };
        struct
        {
            struct zwlr_layer_surface_v1 *zwlr_layer_surface_v1;
            struct wl_output *layer_output;
        };
    };
    struct wp_alpha_modifier_surface_v1 *wp_alpha_modifier_surface_v1;

    struct wayland_surface_config pending, queued, processing, current;
    struct wayland_toplevel_size_limits toplevel_size_limits;
    BOOL resizing;
    enum wayland_surface_ensure_type ensured_contents;
    struct wl_list hwnd_dmabuf_surfaces;
    struct wayland_hwnd_dmabuf_surface *direct_dmabuf_surface;
    struct wp_linux_drm_syncobj_surface_v1 *direct_dmabuf_syncobj_surface;
    /* The direct-toplevel client surface currently borrowing wl_surface for
     * external WSI presentation, if any. Role changes and destruction must
     * evict the borrower (transferring wl_surface ownership to it) first. */
    struct wayland_client_surface *direct_client;
    /* Bottom of the below-main dmabuf subsurface chain. Client subsurfaces
     * anchor below this so children stay above their parent's client content. */
    struct wl_surface *dmabuf_bottom;
    BOOL carrier_attached;
    BOOL carrier_opaque;
    BOOL carrier_single_pixel;
    int carrier_width, carrier_height;
    HRGN child_region;
    HWND clip_producer;
    BOOL shaped;
    BOOL occlusion_clipped;
    struct wayland_window_config window, comitted;
    RECT geometry;
    int content_width, content_height;
    UINT32 alpha_multiplier;
    HCURSOR hcursor;
};

BOOL wayland_dmabuf_format_supported(uint32_t format, uint64_t modifier);
BOOL wayland_vulkan_proxy_create_surface(const struct vulkan_instance *instance,
                                         struct wl_surface *wl_surface,
                                         VkSurfaceKHR *surface, VkResult *result);

/**********************************************************************
 *          Wayland initialization
 */

BOOL wayland_process_init(void);
BOOL wayland_external_input_emit(const struct wine_wayland_external_input_event *event);
void wayland_external_input_set_keyboard_focus(BOOL focused);
extern int wayland_external_input_active;
extern int wayland_external_input_registered;
static inline BOOL wayland_external_input_is_active(void)
{
    return __atomic_load_n(&wayland_external_input_active, __ATOMIC_ACQUIRE);
}
static inline BOOL wayland_external_input_is_registered(void)
{
    return __atomic_load_n(&wayland_external_input_registered, __ATOMIC_ACQUIRE);
}
void wayland_pointer_set_external_input_active(BOOL active);

/**********************************************************************
 *          Wayland output
 */

void wayland_output_add_ref(struct wayland_output *output);
BOOL wayland_output_create(uint32_t id, uint32_t version);
void wayland_output_release(struct wayland_output *output);
void wayland_output_remove(struct wayland_output *output);
void wayland_output_use_xdg_extension(struct wayland_output *output);
void wayland_output_use_image_description(struct wayland_output *output);
struct wayland_output *wayland_output_for_rect(const RECT *rect, RECT *output_rect,
                                               double *output_scale);
BOOL wayland_output_get_layout_rect(const struct wl_output *wl_output, RECT *rect);
BOOL wayland_output_layout_intersects_rect(const RECT *rect);
void output_info_array_update(void);
BOOL wayland_output_edid_is_valid(const unsigned char *edid, UINT edid_len);
BOOL wayland_output_edid_supports_hdr(const unsigned char *edid, UINT edid_len);
UINT wayland_generic_output_get_edid_override(const char *output_name, unsigned char **edid);
UINT wayland_generic_output_get_edid_sysfs(const char *output_name, unsigned char **edid);
UINT wayland_generic_output_get_edid(const struct wayland_output_state *output,
                                     BOOL hdr_supported, unsigned char **edid);
void wayland_color_manager_init(void);
BOOL wayland_color_manager_can_present_bt2100(void);
BOOL wayland_color_manager_may_support_hdr(void);
struct wp_image_description_v1 *wayland_color_manager_create_windows_bt2100(void);
enum wayland_image_description_status wayland_color_manager_get_image_description(
        enum wayland_image_description_color_space color_space,
        struct wp_image_description_v1 **description);

/**********************************************************************
 *          Wayland surface
 */

unsigned long long wayland_time_ms(void);
struct wayland_surface *wayland_surface_create(HWND hwnd, BYTE alpha, DWORD flags);
void wayland_surface_destroy(struct wayland_surface *surface);
BOOL wayland_surface_make_toplevel(struct wayland_surface *surface, BOOL server_decor,
                                   HWND owner, LPCWSTR title);
void wayland_surface_make_subsurface(struct wayland_surface *surface,
                                     struct wayland_surface *parent);
void wayland_surface_make_popup(struct wayland_surface *surface,
                                struct wayland_surface *owner);
void wayland_surface_make_layer(struct wayland_surface *surface, const RECT *rect);
BOOL wayland_surface_clear_role(struct wayland_surface *surface);
BOOL wayland_surface_unmap(struct wayland_surface *surface);
void wayland_surface_attach_shm(struct wayland_surface *surface,
                                struct wayland_shm_buffer *shm_buffer,
                                HRGN surface_damage_region);
BOOL wayland_surface_reconfigure(struct wayland_surface *surface);
BOOL wayland_surface_has_external_commit_owner(const struct wayland_surface *surface);
void wayland_surface_mark_pending_commit(struct wayland_surface *surface);
void wayland_surface_commit(struct wayland_surface *surface);
void wayland_surface_commit_pending_state(struct wayland_surface *surface);
BOOL wayland_surface_config_is_compatible(struct wayland_surface_config *conf, RECT rect,
                                          enum wayland_surface_config_state state,
                                          BOOL preserve_fullscreen_size);
void wayland_surface_update_toplevel_parent(struct wayland_surface *surface);
RECT map_rect_to_surface(struct wayland_surface *surface, RECT rect);
POINT map_point_to_surface(struct wayland_surface *surface, POINT point);
RECT map_rect_from_surface(struct wayland_surface *surface, RECT rect);
POINT map_point_from_surface(struct wayland_surface *surface, POINT point);
RECT wayland_surface_get_input_rect(struct wayland_surface *surface,
                                    const struct wayland_win_data *data);
void wayland_surface_coords_to_screen(struct wayland_surface *surface,
                                      const struct wayland_win_data *data,
                                      double surface_x, double surface_y,
                                      double *screen_x, double *screen_y);
void wayland_surface_delta_to_screen(struct wayland_surface *surface,
                                     const struct wayland_win_data *data,
                                     double surface_x, double surface_y,
                                     double *screen_x, double *screen_y);
void wayland_surface_coords_to_external_input(struct wayland_surface *surface,
                                              const struct wayland_win_data *data,
                                              double surface_x, double surface_y,
                                              LONG *input_x, LONG *input_y,
                                              LONG *input_width, LONG *input_height);
void wayland_surface_delta_to_external_input(struct wayland_surface *surface,
                                             const struct wayland_win_data *data,
                                             double surface_x, double surface_y,
                                             double *input_x, double *input_y,
                                             LONG *input_width, LONG *input_height);
BOOL wayland_hwnd_dmabuf_surface_coords_to_screen(struct wl_surface *wl_surface,
                                                   double surface_x, double surface_y,
                                                   POINT *screen, RECT *input_rect);
void wayland_surface_coords_from_screen(struct wayland_surface *surface,
                                        const struct wayland_win_data *data,
                                        double screen_x, double screen_y,
                                        double *surface_x, double *surface_y);
BOOL wayland_surface_get_max_track_size(struct wayland_surface *surface, SIZE *size);
BOOL wayland_surface_has_hwnd_dmabuf_content(struct wayland_surface *surface);
BOOL wayland_surface_has_direct_dmabuf_content(struct wayland_surface *surface);
BOOL wayland_surface_client_is_unmaskable(struct wayland_surface *surface);
void wayland_surface_sync_window_regions(struct wayland_surface *surface,
                                         struct window_surface *window_surface, DWORD exstyle);
BOOL wayland_surface_attach_transparent_carrier(struct wayland_surface *surface);
void wayland_surface_prepare_direct_dmabuf_shm_commit(struct wayland_surface *surface);
void wayland_surface_finish_direct_dmabuf_shm_commit(struct wayland_surface *surface);
void wayland_surface_reannounce_hwnd_dmabuf_consumers(struct wayland_surface *surface);
void wayland_surface_update_hwnd_dmabufs(struct wayland_surface *surface);
void wayland_surface_coords_from_window(struct wayland_surface *surface,
                                        int window_x, int window_y,
                                        int *surface_x, int *surface_y);
void wayland_surface_coords_to_window(struct wayland_surface *surface,
                                      double surface_x, double surface_y,
                                      int *window_x, int *window_y);
struct wayland_client_surface *wayland_client_surface_create(HWND hwnd);
struct wl_surface *wayland_client_surface_prepare_direct_promotion(struct client_surface *client,
                                                                   HWND hwnd, const char **reason);
BOOL wayland_client_surface_finish_direct_promotion(struct client_surface *client, HWND hwnd,
                                                    struct wl_surface *toplevel_wl_surface,
                                                    UINT64 old_host_surface,
                                                    UINT64 new_host_surface,
                                                    const char **reason);
BOOL wayland_client_surface_bind_direct_toplevel(struct client_surface *client, HWND hwnd,
                                                 UINT64 host_surface);
struct wl_surface *wayland_client_surface_prepare_demotion(struct client_surface *client,
                                                           HWND hwnd, const char **reason,
                                                           BOOL *needed);
BOOL wayland_client_surface_finish_demotion(struct client_surface *client, HWND hwnd,
                                            struct wl_surface *new_wl_surface,
                                            UINT64 old_host_surface);
void wayland_client_surface_release_vulkan_surface(struct client_surface *client,
                                                   UINT64 host_surface);
void wayland_client_surface_attach(struct wayland_client_surface *client, HWND toplevel);
BOOL wayland_client_surface_scales_presentation(struct wayland_surface *surface,
                                                struct wayland_client_surface *client,
                                                BOOL content_over_producer);
BOOL wayland_client_surface_set_image_description(
        struct client_surface *client,
        enum wayland_image_description_color_space color_space);
struct wayland_client_surface *impl_from_client_surface(struct client_surface *client);
void wayland_client_surface_set_alpha(struct client_surface *client, BOOL alpha);
BOOL wayland_client_surface_get_fullscreen_rect(struct wayland_client_surface *client,
                                                BOOL active, RECT *rect);
BOOL wayland_client_surface_update_fullscreen_target(struct wayland_client_surface *client,
                                                     const RECT *window_rect);
void wayland_surface_ensure_contents(struct wayland_surface *surface,
                                     struct wayland_client_surface *client);
void wayland_surface_set_title(struct wayland_surface *surface, LPCWSTR title);
void wayland_surface_assign_icon(struct wayland_surface *surface);
void wayland_surface_set_icon_buffer(struct wayland_surface *surface, UINT type, const ICONINFO *ii);
void wayland_surface_set_opacity(struct wayland_surface *surface, BYTE alpha, UINT flags);
UINT32 wayland_alpha_multiplier(BYTE alpha, UINT flags);
void wayland_activation_set_serial(enum wayland_activation_serial_kind kind,
                                   HWND hwnd, uint32_t serial);
void wayland_activation_clear_serial(enum wayland_activation_serial_kind kind, HWND hwnd);
uint32_t wayland_activation_get_serial(HWND hwnd);
void wayland_request_activation(HWND target, struct wayland_surface *requester,
                                BOOL foreground, uint32_t serial, BOOL set_app_id);
void wayland_activation_apply_token(struct wayland_win_data *data, const char *token,
                                    BOOL defer);
void wayland_activation_apply_pending(struct wayland_win_data *data, BOOL foreground);
char *wayland_take_process_activation_token(void);
BOOL wayland_process_activation_token_pending(void);
void wayland_surface_shortcut_control(struct wayland_surface *surface, BOOL inhibit);
void wayland_surface_sync_alpha(struct wayland_surface *surface);
BOOL wayland_is_popup_menu_class(HWND hwnd);
BOOL wayland_is_menu_popup_candidate(HWND hwnd);
BOOL wayland_is_menu_popup(HWND hwnd);
BOOL wayland_window_is_externally_hosted(HWND hwnd, HWND *host);
BOOL wayland_window_get_effective_alpha(HWND hwnd, BYTE *alpha);
HWND wayland_keyboard_get_input_hwnd(HWND surface_hwnd, HWND foreground);
void wayland_window_surface_set_external_host(struct window_surface *surface, HWND host);
BOOL wayland_is_layer_menu_hwnd(HWND hwnd);
void wayland_set_layer_menu_hwnd(HWND hwnd);
void wayland_clear_layer_menu_hwnd(HWND hwnd);
void wayland_cancel_layer_menu(HWND hwnd);
void wayland_cancel_layer_menu_if_needed(HWND hwnd);

static inline BOOL wayland_surface_is_toplevel(struct wayland_surface *surface)
{
    return surface->role == WAYLAND_SURFACE_ROLE_TOPLEVEL && surface->xdg_toplevel;
}

static inline BOOL wayland_surface_is_popup(struct wayland_surface *surface)
{
    return surface->role == WAYLAND_SURFACE_ROLE_POPUP && surface->xdg_popup;
}

static inline BOOL wayland_surface_is_layer(struct wayland_surface *surface)
{
    return surface->role == WAYLAND_SURFACE_ROLE_LAYER && surface->zwlr_layer_surface_v1;
}

/**********************************************************************
 *          Wayland SHM buffer
 */

struct wayland_shm_buffer *wayland_shm_buffer_create(int width, int height,
                                                     enum wl_shm_format format);
struct wayland_shm_buffer *wayland_shm_buffer_from_color_bitmaps(HDC hdc, HBITMAP color,
                                                                 HBITMAP mask, BOOL allow_padding);
void wayland_shm_buffer_ref(struct wayland_shm_buffer *shm_buffer);
void wayland_shm_buffer_unref(struct wayland_shm_buffer *shm_buffer);

/**********************************************************************
 *          Wayland Window
 */

/* private window data */
struct wayland_win_data
{
    struct rb_entry entry;
    /* hwnd that this private data belongs to */
    HWND hwnd;
    /* Win32 state cached before taking win_data_mutex. */
    HWND toplevel;
    HWND owner;
    HWND overlay_owner;
    HWND external_host;
    WCHAR *window_text;
    BOOL visible;
    BOOL explicitly_hidden;
    DWORD style;
    DWORD exstyle;
    RECT client_rect_in_toplevel;
    BOOL client_rect_in_toplevel_valid;
    /* last buffer that was set as window contents */
    struct wayland_shm_buffer *window_contents;
    BOOL content_over_producer;
    /* wayland surface (if any) for this window */
    struct wayland_surface *wayland_surface;
    /* wayland client surface (if any) for this window */
    struct wayland_client_surface *client_surface;
    /* track a stale client surface to ensure DMABUF modifiers stay valid */
    struct wayland_client_surface *stashed_client;
    /* window rects, relative to parent client area */
    struct window_rects rects;
    BOOL is_fullscreen;
    BOOL has_present_rect;
    RECT present_rect;
    BOOL application_fullscreen;
    RECT application_fullscreen_rect;
    BOOL managed;
    BOOL frameless;
    RECT restore_rect;
    BOOL restore_rect_valid;
    BOOL layered_attribs_set;
    BYTE layered_alpha;
    DWORD layered_flags;
    BOOL ime_enabled;
    int num_ime_children;
    UINT32 alpha_multiplier;

    UINT state_update_cmd;
    UINT state_update_swp_flags;
    RECT state_update_rect;
    HWND state_update_foreground;
    uint32_t configure_state_serial;
    char *pending_activation_token;
};

struct wayland_win_data *wayland_win_data_get_nolock(HWND hwnd);
void wayland_win_data_lock(void);
void wayland_win_data_unlock(void);
struct wayland_win_data *wayland_win_data_get(HWND hwnd);
void wayland_win_data_release(struct wayland_win_data *data);
BOOL wayland_win_data_is_fullscreen(const struct wayland_win_data *data);
BOOL wayland_win_data_get_fullscreen_rect(const struct wayland_win_data *data,
                                          BOOL active, RECT *rect);
/* Returns TRUE when rect was filled with host presentation geometry. */
BOOL wayland_win_data_get_presentation_rect(const struct wayland_win_data *data,
                                            BOOL active, RECT *rect);
BOOL wayland_win_data_covers_virtual_screen(const struct wayland_win_data *data);
void wayland_win_data_refresh_fullscreen(struct wayland_win_data *data);

struct wayland_client_surface *get_client_surface(HWND hwnd);
void set_client_surface(HWND hwnd, struct wayland_client_surface *client);
BOOL wayland_toplevel_has_other_client_surface(HWND toplevel,
                                               struct wayland_client_surface *client);
BOOL wayland_toplevel_has_visible_child_surface(HWND toplevel);
void wayland_surface_invalidate_attached_clients(HWND hwnd, struct wl_surface *parent);
BOOL set_window_surface_contents(HWND hwnd, struct wayland_shm_buffer *shm_buffer, HRGN damage_region,
                                 BOOL overlay_content, HRGN clip_region);
struct wayland_shm_buffer *get_window_surface_contents(HWND hwnd);
void ensure_window_surface_contents(HWND hwnd);
void wayland_window_init(void);

/**********************************************************************
 *          Wayland Keyboard
 */

void wayland_keyboard_init(struct wl_keyboard *wl_keyboard);
void wayland_keyboard_deinit(void);
const KBDTABLES *WAYLAND_KbdLayerDescriptor(HKL hkl);
void WAYLAND_ReleaseKbdTables(const KBDTABLES *);

/**********************************************************************
 *          Wayland pointer
 */

void wayland_pointer_init(struct wl_pointer *wl_pointer);
void wayland_pointer_deinit(void);
void wayland_pointer_clear_constraint(void);


/**********************************************************************
 *          Wayland touch
 */
void wayland_touch_init(struct wl_touch *wl_touch);
void wayland_touch_deinit(void);

/**********************************************************************
 *          Wayland text input
 */

void wayland_text_input_init(void);
void wayland_text_input_deinit(void);

/**********************************************************************
 *          Wayland data device
 */

void wayland_data_device_init(void);

/**********************************************************************
 *          Helpers
 */

static inline BOOL intersect_rect(RECT *dst, const RECT *src1, const RECT *src2)
{
    dst->left = max(src1->left, src2->left);
    dst->top = max(src1->top, src2->top);
    dst->right = min(src1->right, src2->right);
    dst->bottom = min(src1->bottom, src2->bottom);
    return !IsRectEmpty(dst);
}

static inline LRESULT send_message(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
{
    return NtUserMessageCall(hwnd, msg, wparam, lparam, NULL, NtUserSendMessage, FALSE);
}

static inline BOOL is_decoration_enabled(DWORD style, DWORD ex_style)
{
    if (ex_style & WS_EX_TOOLWINDOW) return FALSE;
    if (ex_style & WS_EX_LAYERED) return FALSE;

    if ((style & WS_CAPTION) == WS_CAPTION)
        return TRUE;

    return FALSE;
}

RGNDATA *get_region_data(HRGN region);

/**********************************************************************
 *          USER driver functions
 */

void WAYLAND_ActivateWindow(HWND hwnd, HWND previous);
LRESULT WAYLAND_ClipboardWindowProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam);
BOOL WAYLAND_ClipCursor(const RECT *clip, BOOL reset);
LRESULT WAYLAND_DesktopWindowProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
void WAYLAND_DestroyWindow(HWND hwnd);
void WAYLAND_FlashWindowEx(FLASHWINFO *info);
BOOL WAYLAND_SetIMECompositionRect(HWND hwnd, RECT rect);
BOOL WAYLAND_SetIMEEnabled(HWND hwnd, BOOL enable);
void WAYLAND_SetCursor(HWND hwnd, HCURSOR hcursor);
BOOL WAYLAND_SetCursorPos(INT x, INT y);
void WAYLAND_SetLayeredWindowAttributes(HWND hwnd, COLORREF key, BYTE alpha, DWORD flags);
void WAYLAND_SetWindowIcons(HWND hwnd, HICON icon, const ICONINFO *ii, HICON icon_small, const ICONINFO *ii_small);
void WAYLAND_SetWindowStyle(HWND hwnd, INT offset, STYLESTRUCT *style);
void WAYLAND_SetWindowText(HWND hwnd, LPCWSTR text);
UINT WAYLAND_ShowWindow(HWND hwnd, INT cmd, RECT *rect, UINT swp);
LRESULT WAYLAND_SysCommand(HWND hwnd, WPARAM wparam, LPARAM lparam, const POINT *pos);
void WAYLAND_UpdateLayeredWindow(HWND hwnd, BYTE alpha, UINT flags);
UINT WAYLAND_UpdateDisplayDevices(const struct gdi_device_manager *device_manager, void *param);
void WAYLAND_MapNotifyIconPoint(POINT *point);
LRESULT WAYLAND_WindowMessage(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
void WAYLAND_WindowPosChanged(HWND hwnd, HWND insert_after, HWND owner_hint, UINT swp_flags,
                              const struct window_rects *new_rects, struct window_surface *surface);
BOOL WAYLAND_WindowPosChanging(HWND hwnd, UINT swp_flags, BOOL shaped, const struct window_rects *rects);
BOOL WAYLAND_GetWindowStateUpdates(HWND hwnd, UINT *state_cmd, UINT *swp_flags, RECT *rect, HWND *foreground);
BOOL WAYLAND_GetWindowMaxTrackSize(HWND hwnd, SIZE *size);
BOOL WAYLAND_CreateWindowSurface(HWND hwnd, BOOL layered, const RECT *surface_rect, struct window_surface **surface);
BOOL WAYLAND_GetWindowStyleMasks(HWND hwnd,  UINT style, UINT ex_style, UINT *style_mask, UINT *ex_style_mask);
BOOL WAYLAND_HasWindowManager(const char *name);
UINT WAYLAND_VulkanInit(UINT version, void *vulkan_handle, const struct vulkan_driver_funcs **driver_funcs);
UINT WAYLAND_OpenGLInit(UINT version, const struct opengl_funcs *opengl_funcs, const struct opengl_driver_funcs **driver_funcs);

#endif /* __WINE_WAYLANDDRV_H */
