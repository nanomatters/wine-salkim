/*
 * Vulkan display driver loading
 *
 * Copyright (c) 2017 Roderick Colenbrander
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
#include <limits.h>
#include <math.h>
#include <dlfcn.h>
#include <pthread.h>
#include <time.h>
#include <unistd.h>
#ifdef HAVE_SYS_EVENTFD_H
#include <sys/eventfd.h>
#endif
#include <sys/socket.h>
#include <poll.h>
#include <errno.h>

#include "ntstatus.h"
#define WIN32_NO_STATUS
#include "win32u_private.h"
#include "ntuser_private.h"
#include "wine/hwnd_dmabuf.h"

#include "fshack_color_spv.h"
#include "fsr_spv.h"

WINE_DEFAULT_DEBUG_CHANNEL(vulkan);

static PFN_vkGetDeviceProcAddr p_vkGetDeviceProcAddr;
static PFN_vkGetInstanceProcAddr p_vkGetInstanceProcAddr;
static PFN_vkCreateInstance p_vkCreateInstance;
static PFN_vkEnumerateInstanceExtensionProperties p_vkEnumerateInstanceExtensionProperties;

static void *vulkan_handle;
static struct vulkan_funcs vulkan_funcs;

WINE_DECLARE_DEBUG_CHANNEL(fps);

static const struct vulkan_driver_funcs *driver_funcs;
static int fshack_enabled = -1;

static void vulkan_driver_load(void);

static const UINT EXTERNAL_MEMORY_WIN32_BITS = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT |
                                               VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_KMT_BIT |
                                               VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_BIT |
                                               VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_KMT_BIT |
                                               VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D12_HEAP_BIT |
                                               VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D12_RESOURCE_BIT;
static const UINT EXTERNAL_MEMORY_FD_BITS = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT |
                                            VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
static const UINT EXTERNAL_SEMAPHORE_WIN32_BITS = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_WIN32_BIT |
                                                  VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_WIN32_KMT_BIT |
                                                  VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_D3D12_FENCE_BIT;
static const UINT EXTERNAL_FENCE_WIN32_BITS = VK_EXTERNAL_FENCE_HANDLE_TYPE_OPAQUE_WIN32_BIT |
                                              VK_EXTERNAL_FENCE_HANDLE_TYPE_OPAQUE_WIN32_KMT_BIT;

/* Query the compositor's importable dmabuf caps. The NtUserGetHwndDmabufCaps shim. */
static UINT hwnd_dmabuf_get_caps( HWND hwnd, void *caps, void *format_modifiers,
                                  UINT max_format_modifiers, UINT *format_modifier_count )
{
    vulkan_driver_load();
    if (!driver_funcs->p_vulkan_get_hwnd_dmabuf_caps)
    {
        if (format_modifier_count) *format_modifier_count = 0;
        return HWND_DMABUF_NOT_FOUND;
    }
    return driver_funcs->p_vulkan_get_hwnd_dmabuf_caps( hwnd, caps, format_modifiers,
                                                        max_format_modifiers, format_modifier_count );
}

UINT WINAPI NtUserHwndDmaBufGetCaps( HWND hwnd, void *caps, void *format_modifiers,
                                     UINT max_format_modifiers, UINT *format_modifier_count )
{
    return hwnd_dmabuf_get_caps( hwnd, caps, format_modifiers,
                                 max_format_modifiers, format_modifier_count );
}

#define ROUND_SIZE(size, mask) ((((SIZE_T)(size) + (mask)) & ~(SIZE_T)(mask)))

static BOOL use_external_memory(void)
{
    return zero_bits != 0;
}

struct mempool
{
    struct mempool *next;
    size_t mem_used;
    char mem[2048];
};

static void mem_free( struct mempool *pool )
{
    struct mempool *iter, *next = pool->next;
    while ((iter = next)) { next = iter->next; free( iter ); }
    pool->mem_used = 0;
    pool->next = NULL;
}

static void *mem_alloc( struct mempool *pool, size_t size )
{
    struct mempool *next = pool;
    if (pool->mem_used + size > sizeof(pool->mem)) next = pool->next;
    if (next && next->mem_used <= sizeof(next->mem) && next->mem_used + size <= sizeof(next->mem))
    {
        void *ret = next->mem + next->mem_used;
        next->mem_used += ROUND_SIZE( size, sizeof(UINT64) - 1 );
        return ret;
    }
    if (!(next = malloc( max( sizeof(*next), offsetof(struct mempool, mem[size]) ) ))) return NULL;
    next->next = pool->next;
    next->mem_used = size;
    pool->next = next;
    return next->mem;
}

struct instance
{
    struct vulkan_instance obj;
    BOOL enable_win32_surface;
    BOOL nvidia_wayland_wsi;

    struct list utils_messengers;
    struct list report_callbacks;

    struct rb_tree objects;
    pthread_rwlock_t objects_lock;
};

struct nvidia_wayland_host_instance
{
    struct list entry;
    PFN_vkDestroyInstance p_vkDestroyInstance;
    VkInstance host_instance;
};

static struct instance *instance_from_handle( VkInstance handle )
{
    struct vulkan_instance *object = vulkan_instance_from_handle( handle );
    return CONTAINING_RECORD( object, struct instance, obj );
}

struct device_memory
{
    struct vulkan_device_memory obj;
    VkDeviceSize size;
    void *vm_map;

    D3DKMT_HANDLE local;
    D3DKMT_HANDLE global;
    HANDLE shared;

    D3DKMT_HANDLE sync;
    D3DKMT_HANDLE mutex;
    VkSemaphore semaphore;
    UINT64 semaphore_value;
};

static inline struct device_memory *device_memory_from_handle( VkDeviceMemory handle )
{
    struct vulkan_device_memory *obj = vulkan_device_memory_from_handle( handle );
    return CONTAINING_RECORD( obj, struct device_memory, obj );
}

struct surface_host
{
    struct list entry;
    VkSurfaceKHR handle;
    UINT swapchain_count;
};

struct surface
{
    struct vulkan_surface obj;
    struct client_surface *client;
    struct swapchain *swapchain;
    HWND hwnd;
    pthread_mutex_t host_lock;
    /* Fullscreen state callbacks use this lock, never host_lock. */
    pthread_mutex_t fullscreen_lock;
    struct list host_surfaces;
    struct surface_host *active_host;
    UINT alpha_swapchain_count;
    LONGLONG DECLSPEC_ALIGN(8) fullscreen_active_owner;
};

static struct surface *surface_from_handle( VkSurfaceKHR handle )
{
    struct vulkan_surface *obj = vulkan_surface_from_handle( handle );
    return CONTAINING_RECORD( obj, struct surface, obj );
}

static VkSurfaceKHR surface_host_handle( struct surface *surface )
{
    return surface->active_host->handle;
}

enum fs_hack_color_mode
{
    FS_HACK_COLOR_SRGB,
    FS_HACK_COLOR_EXTENDED_SRGB,
    FS_HACK_COLOR_RAW,
    FS_HACK_COLOR_PQ,
    FS_HACK_COLOR_HLG,
};

enum fs_hack_transfer
{
    FS_HACK_TRANSFER_NONE,
    FS_HACK_TRANSFER_PQ,
    FS_HACK_TRANSFER_HLG,
    FS_HACK_TRANSFER_SRGB,
};

struct fs_hack_color_constants
{
    float offset[2];
    float extents[2];
    uint32_t transfer;
    uint32_t linear_filter;
};

C_ASSERT(sizeof(struct fs_hack_color_constants) == 24);

struct fs_hack_upscaler
{
    enum fs_hack_color_mode color_mode;
    BOOL is_blit, is_fsr, is_nis;
    BOOL linear_filter;
    union {
        struct {
        } blit;
        struct {
            BOOL fp16;
            BOOL lite;
            float sharpness;
        } fsr;
        struct {
        } nis;
    };
};

/* Return whether integer scaling is on */
static BOOL fs_hack_is_integer(void)
{
    static int is_int = -1;
    if (is_int < 0)
    {
        const char *e = getenv( "WINE_FULLSCREEN_INTEGER_SCALING" );
        is_int = e && strcmp( e, "0" );
        TRACE( "is_integer_scaling: %s\n", is_int ? "TRUE" : "FALSE" );
    }
    return is_int;
}

struct fs_hack_image
{
    uint32_t cmd_queue_idx;
    VkCommandBuffer cmd;
    VkImage swapchain_image;
    VkImage fsr_image;
    VkImage user_image;
    VkSemaphore blit_finished;
    VkImageView user_view, swapchain_view, fsr_view;
    VkDescriptorSet descriptor_set, fsr_set;
};

struct fs_comp_pipeline
{
    VkPipelineLayout pipeline_layout;
    VkPipeline pipeline;
    uint32_t push_size;
};

struct fs_hack_config
{
    BOOL enabled;
    UINT dpi;
    RECT dst;
};

static const char *debugstr_vkextent2d( const VkExtent2D *ext )
{
    if (!ext) return "(null)";
    return wine_dbg_sprintf( "(%d,%d)", (int)ext->width, (int)ext->height );
}

/* Cross-process Vulkan-WSI producer.
 *
 * A windowed swapchain whose toplevel has no on-screen wl_surface is a
 * cross-process child and advertises HWND dmabuf caps. For those we skip the
 * host vkCreateSwapchainKHR and build a managed swapchain of exportable
 * DRM-modifier images. Each presented frame is exported as a dmabuf and
 * published through the per-HWND bridge. On-screen windows get no caps and use
 * the host swapchain path. Works with stock DXVK, native Vulkan and vkd3d. */

#define WINE_VK_DRM_FORMAT_MOD_INVALID 0x00ffffffffffffffull
#define WINE_VK_MANAGED_MAX_IMAGES     8
#define WINE_VK_MANAGED_MAX_MODIFIERS  64
#define WINE_VK_MANAGED_REANNOUNCE_MIN_MS  100
#define WINE_VK_MANAGED_REANNOUNCE_MAX_MS  4000
/* Allow brief pauses beyond the consumer's dmabuf grace period. */
#define WINE_VK_MANAGED_STALL_MS       2000

static pthread_mutex_t nvidia_wayland_instance_lock = PTHREAD_MUTEX_INITIALIZER;
/* NVIDIA tears down process-global Wayland WSI state from vkDestroyInstance()
 * while other instances can still present. Keep affected host instances alive
 * until no NVIDIA Wayland instance remains. Churn against one persistent
 * instance accumulates deferred host instances until process teardown. */
static struct list delayed_nvidia_wayland_host_instances = LIST_INIT( delayed_nvidia_wayland_host_instances );
static unsigned int nvidia_wayland_instance_count;
static LONGLONG managed_next_producer_id;
static LONGLONG fullscreen_next_owner;

enum wine_managed_consumer_state
{
    WINE_MANAGED_CONSUMER_UNKNOWN,
    WINE_MANAGED_CONSUMER_ACTIVE,
    WINE_MANAGED_CONSUMER_SUSPENDED,
};

/* The server reuses one channel while swapchains overlap during recreation. */
struct wine_managed_consumer
{
    struct list entry;
    HWND hwnd;
    unsigned int refcount;
    LONG state;
    LONG reannounce_pending;
    LONG last_reannounce_ms;
    LONG reannounce_delay_ms;
};

static pthread_mutex_t managed_consumers_lock = PTHREAD_MUTEX_INITIALIZER;
static struct list managed_consumers = LIST_INIT( managed_consumers );

struct wine_managed_image
{
    VkImage image;                  /* raw host VkImage handle (no client wrapper) */
    VkDeviceMemory memory;          /* dedicated exportable host VkDeviceMemory */
    int dmabuf_fd;                  /* cached exported dmabuf fd, dup()'d on publish */
    int completion_fd;              /* last explicit render-completion sync_file */
    hwnd_dmabuf_frame_desc_t desc;  /* cached per-image frame descriptor */
    UINT64 release_token;           /* token handed to the compositor for this frame */
    BOOL acquired;                  /* handed to the app, not yet presented */
    BOOL busy;                      /* published, compositor may still read it */
    BOOL valid;                     /* image+memory+fd are all live */
    BOOL consumer_cached;           /* consumer explicitly acked this slot's dmabuf cache */
};

struct wine_managed_swapchain
{
    struct wine_managed_image images[WINE_VK_MANAGED_MAX_IMAGES];
    uint32_t image_count;

    uint64_t realized_modifier;
    unsigned int fourcc;
    unsigned int alpha_mode;        /* DXGI_ALPHA_MODE_* hint for the compositor */
    unsigned int color_space;
    VkFormat format;
    VkExtent2D extents;
    VkImageUsageFlags usage;
    BOOL has_color_space;

    /* ring / publish state */
    uint32_t next_acquire;
    UINT64 next_release_token;
    UINT64 present_id;              /* monotonic frame_seq counter */
    UINT64 producer_unique_id;      /* unique across managed swapchain recreates */
    unsigned int ring_generation;
    DWORD ring_full_since_ms;

    struct vulkan_queue *signal_queue; /* queue used for empty acquire-signal submits */
    int channel_fd;                 /* producer end of the per-hwnd socket or -1 */
    int wake_fd;                    /* local image-state changes */
    pthread_mutex_t lock;
    BOOL lost;                      /* consumer channel died, force swapchain recreate */
    struct wine_managed_consumer *consumer;
    VkFence present_fence;          /* per-frame render-complete fence (export gate) */
    PFN_vkWaitForFences p_vkWaitForFences;
    PFN_vkResetFences p_vkResetFences;
    BOOL explicit_sync;
    HWND hwnd;                      /* server-visible producer HWND */
    BOOL pending_registered;        /* server pending reference is live */
    BOOL channel_registered;        /* server active producer reference is live */
};

struct swapchain
{
    struct vulkan_swapchain obj;
    struct surface *surface;
    struct surface_host *host_surface;
    LONG presentation_generation;
    VkExtent2D extents;
    struct wine_managed_swapchain *managed; /* non-NULL => wine-managed cross-process producer */
    BOOL has_alpha;
    BOOL compositor_scaling;
    VkColorSpaceKHR color_space;
    BOOL uses_color_description;
    VkFullScreenExclusiveEXT fullscreen_policy;
    UINT64 fullscreen_owner;
    LONG fullscreen_acquired;

    /* fs hack data below */
    struct fs_hack_config fshack;
    VkExtent2D host_extents;
    VkCommandPool *cmd_pools; /* VkCommandPool[device->queue_count] */
    VkDeviceMemory user_image_memory, fsr_image_memory;
    uint32_t n_images;
    struct fs_hack_image *fs_hack_images; /* struct fs_hack_image[n_images] */
    VkSampler sampler;
    VkDescriptorPool descriptor_pool;
    VkDescriptorSetLayout descriptor_set_layout;
    VkFormat format;
    struct fs_hack_upscaler upscaler;

    struct fs_comp_pipeline blit_pipeline;
    struct fs_comp_pipeline fsr_easu_pipeline;
    struct fs_comp_pipeline fsr_rcas_pipeline;
};

static struct swapchain *swapchain_from_handle( VkSwapchainKHR handle )
{
    struct vulkan_swapchain *obj = vulkan_swapchain_from_handle( handle );
    return CONTAINING_RECORD( obj, struct swapchain, obj );
}

static BOOL managed_color_space_from_vulkan( VkColorSpaceKHR color_space, unsigned int *wire_color_space )
{
    switch (color_space)
    {
    case VK_COLOR_SPACE_SRGB_NONLINEAR_KHR:
        *wire_color_space = HWND_DMABUF_COLOR_SPACE_SRGB;
        return TRUE;
    case VK_COLOR_SPACE_EXTENDED_SRGB_LINEAR_EXT:
        *wire_color_space = HWND_DMABUF_COLOR_SPACE_SCRGB;
        return TRUE;
    case VK_COLOR_SPACE_HDR10_ST2084_EXT:
        *wire_color_space = HWND_DMABUF_COLOR_SPACE_HDR10_ST2084;
        return TRUE;
    default:
        return FALSE;
    }
}

void win32u_vkDestroySwapchainKHR( VkDevice client_device, VkSwapchainKHR client_swapchain,
                                   const VkAllocationCallbacks *allocator );

static struct surface_host *surface_host_create( VkSurfaceKHR handle )
{
    struct surface_host *host;

    if (!(host = calloc( 1, sizeof(*host) ))) return NULL;
    host->handle = handle;
    return host;
}

static void surface_host_destroy( struct vulkan_instance *instance, struct surface *surface,
                                  struct surface_host *host )
{
    list_remove( &host->entry );
    instance->p_vkDestroySurfaceKHR( instance->host.instance, host->handle, NULL /* allocator */ );
    driver_funcs->p_vulkan_surface_release( surface->client, host->handle );
    free( host );
}

static void surface_host_release_if_unused( struct vulkan_instance *instance, struct surface *surface,
                                            struct surface_host *host )
{
    if (host != surface->active_host && !host->swapchain_count)
        surface_host_destroy( instance, surface, host );
}

static void surface_update_client_alpha( struct surface *surface )
{
    driver_funcs->p_vulkan_surface_set_alpha(
            surface->alpha_swapchain_count ? VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR :
                                             VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
            surface->client );
}

static BOOL swapchain_is_out_of_date( const struct swapchain *swapchain )
{
    LONG generation;

    if (!swapchain->surface) return FALSE;
    generation = ReadAcquire( &swapchain->surface->client->presentation_generation );
    return swapchain->presentation_generation != generation;
}

static void swapchain_apply_color_description( struct swapchain *swapchain )
{
    driver_funcs->p_vulkan_surface_set_color_description(
            swapchain->color_space, swapchain->uses_color_description,
            swapchain->surface->client );
}

static LONG get_swapchain_presentation_generation( struct surface *surface, BOOL topology_updated,
                                                    LONG generation_before_update )
{
    LONG generation = ReadAcquire( &surface->client->presentation_generation );
    ULONG expected = (ULONG)generation_before_update + topology_updated;

    /* A topology update advances the generation once. Any other change raced
     * with swapchain creation, so make the new swapchain stale. */
    if ((ULONG)generation != expected)
    {
        TRACE( "surface %p changed presentation generation %d -> %d while creating a swapchain\n",
               surface, generation_before_update, generation );
        return (LONG)((ULONG)generation_before_update - !topology_updated);
    }

    return generation;
}

struct semaphore
{
    struct vulkan_semaphore obj;
    D3DKMT_HANDLE local;
    D3DKMT_HANDLE global;
    HANDLE shared;
};

static struct semaphore *semaphore_from_handle( VkSemaphore handle )
{
    struct vulkan_semaphore *obj = vulkan_semaphore_from_handle( handle );
    return CONTAINING_RECORD( obj, struct semaphore, obj );
}

struct fence
{
    struct vulkan_fence obj;
    D3DKMT_HANDLE local;
    D3DKMT_HANDLE global;
    HANDLE shared;
};

static struct fence *fence_from_handle( VkFence handle )
{
    struct vulkan_fence *obj = vulkan_fence_from_handle( handle );
    return CONTAINING_RECORD( obj, struct fence, obj );
}

static VkResult allocate_external_host_memory( struct vulkan_device *device, VkMemoryAllocateInfo *alloc_info, uint32_t mem_flags,
                                               VkImportMemoryHostPointerInfoEXT *import_info )
{
    struct vulkan_physical_device *physical_device = device->physical_device;
    VkMemoryHostPointerPropertiesEXT props =
    {
        .sType = VK_STRUCTURE_TYPE_MEMORY_HOST_POINTER_PROPERTIES_EXT,
    };
    uint32_t i, align = physical_device->external_memory_align - 1;
    SIZE_T alloc_size = alloc_info->allocationSize;
    static int once;
    void *mapping = NULL;
    VkResult res;

    if (!once++) FIXME( "Using VK_EXT_external_memory_host\n" );

    if (NtAllocateVirtualMemory( GetCurrentProcess(), &mapping, zero_bits, &alloc_size, MEM_COMMIT, PAGE_READWRITE ))
    {
        ERR( "NtAllocateVirtualMemory failed\n" );
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }

    if ((res = device->p_vkGetMemoryHostPointerPropertiesEXT( device->host.device, VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT,
                                                              mapping, &props )))
    {
        ERR( "vkGetMemoryHostPointerPropertiesEXT failed: %d\n", res );
        return res;
    }

    if (!(props.memoryTypeBits & (1u << alloc_info->memoryTypeIndex)))
    {
        /* If requested memory type is not allowed to use external memory, try to find a supported compatible type. */
        uint32_t mask = mem_flags & ~VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
        for (i = 0; i < physical_device->memory_properties.memoryTypeCount; i++)
        {
            if (!(props.memoryTypeBits & (1u << i))) continue;
            if ((physical_device->memory_properties.memoryTypes[i].propertyFlags & mask) != mask) continue;

            TRACE( "Memory type not compatible with host memory, using %u instead\n", i );
            alloc_info->memoryTypeIndex = i;
            break;
        }
        if (i == physical_device->memory_properties.memoryTypeCount)
        {
            FIXME( "Not found compatible memory type\n" );
            alloc_size = 0;
            NtFreeVirtualMemory( GetCurrentProcess(), &mapping, &alloc_size, MEM_RELEASE );
        }
    }

    if (props.memoryTypeBits & (1u << alloc_info->memoryTypeIndex))
    {
        import_info->sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_HOST_POINTER_INFO_EXT;
        import_info->handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT;
        import_info->pHostPointer = mapping;
        import_info->pNext = alloc_info->pNext;
        alloc_info->pNext = import_info;
        alloc_info->allocationSize = (alloc_info->allocationSize + align) & ~align;
    }

    return VK_SUCCESS;
}

static VkExternalMemoryHandleTypeFlagBits get_host_external_memory_type(void)
{
    struct vulkan_device_extensions extensions = {.has_VK_KHR_external_memory_win32 = 1};
    driver_funcs->p_map_device_extensions( &extensions );
    if (extensions.has_VK_KHR_external_memory_fd) return VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
    if (extensions.has_VK_EXT_external_memory_dma_buf) return VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
    return 0;
}

static UINT map_external_memory_handle_types( UINT handle_types )
{
    UINT host_handle_types = handle_types & EXTERNAL_MEMORY_FD_BITS;

    if (handle_types & EXTERNAL_MEMORY_WIN32_BITS)
        host_handle_types |= get_host_external_memory_type();
    if (handle_types & ~(EXTERNAL_MEMORY_WIN32_BITS | EXTERNAL_MEMORY_FD_BITS))
        FIXME( "Unsupported handle types %#x\n", handle_types );
    return host_handle_types;
}

static VkExternalSemaphoreHandleTypeFlagBits get_host_external_semaphore_type(void)
{
    struct vulkan_device_extensions extensions = {.has_VK_KHR_external_semaphore_win32 = 1};
    driver_funcs->p_map_device_extensions( &extensions );
    if (extensions.has_VK_KHR_external_semaphore_fd) return VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT;
    return 0;
}

static VkExternalFenceHandleTypeFlagBits get_host_external_fence_type(void)
{
    struct vulkan_device_extensions extensions = {.has_VK_KHR_external_fence_win32 = 1};
    driver_funcs->p_map_device_extensions( &extensions );
    if (extensions.has_VK_KHR_external_fence_fd) return VK_EXTERNAL_FENCE_HANDLE_TYPE_OPAQUE_FD_BIT;
    return 0;
}

static void init_shared_resource_path( const WCHAR *name, UNICODE_STRING *str )
{
    UINT len = wcslen( name );
    char buffer[MAX_PATH];

    snprintf( buffer, ARRAY_SIZE(buffer), "\\Sessions\\%u\\BaseNamedObjects\\",
              NtCurrentTeb()->Peb->SessionId );
    str->MaximumLength = asciiz_to_unicode( str->Buffer, buffer );
    str->Length = str->MaximumLength - sizeof(WCHAR);

    memcpy( str->Buffer + str->Length / sizeof(WCHAR), name, (len + 1) * sizeof(WCHAR) );
    str->MaximumLength += len * sizeof(WCHAR);
    str->Length += len * sizeof(WCHAR);
}

static HANDLE create_shared_resource_handle( D3DKMT_HANDLE local, const VkExportMemoryWin32HandleInfoKHR *info )
{
    SECURITY_DESCRIPTOR *security = info->pAttributes ? info->pAttributes->lpSecurityDescriptor : NULL;
    WCHAR bufferW[MAX_PATH * 2];
    UNICODE_STRING name = {.Buffer = bufferW};
    OBJECT_ATTRIBUTES attr;
    NTSTATUS status;
    HANDLE shared;

    if (info->name) init_shared_resource_path( info->name, &name );
    InitializeObjectAttributes( &attr, info->name ? &name : NULL, OBJ_CASE_INSENSITIVE, NULL, security );

    if (!(status = NtGdiDdDDIShareObjects( 1, &local, &attr, info->dwAccess, &shared ))) return shared;
    WARN( "Failed to share resource %#x, status %#x\n", local, status );
    return NULL;
}

HANDLE open_shared_resource_from_name( const WCHAR *name )
{
    D3DKMT_OPENNTHANDLEFROMNAME open_name = {0};
    WCHAR bufferW[MAX_PATH * 2];
    UNICODE_STRING name_str = {.Buffer = bufferW};
    OBJECT_ATTRIBUTES attr;
    NTSTATUS status;

    init_shared_resource_path( name, &name_str );
    InitializeObjectAttributes( &attr, &name_str, OBJ_OPENIF, NULL, NULL );

    open_name.dwDesiredAccess = GENERIC_ALL;
    open_name.pObjAttrib = &attr;
    status = NtGdiDdDDIOpenNtHandleFromName( &open_name );
    if (status) WARN( "Failed to open %s, status %#x\n", debugstr_w( name ), status );
    return open_name.hNtHandle;
}

static const void *find_next_struct( const VkBaseInStructure *header, VkStructureType type )
{
    for (; header; header = header->pNext) if (header->sType == type) return header;
    return NULL;
}

static const void *pop_next_struct( VkBaseOutStructure **next, VkStructureType type )
{
    VkBaseOutStructure *ptr;
    while (*next && (*next)->sType != type) next = &(*next)->pNext;
    if ((ptr = *next)) *next = ptr->pNext;
    return ptr;
}

static int vulkan_object_compare( const void *key, const struct rb_entry *entry )
{
    struct vulkan_object *object = RB_ENTRY_VALUE( entry, struct vulkan_object, entry );
    const uint64_t *host_handle = key;
    if (*host_handle < object->host_handle) return -1;
    if (*host_handle > object->host_handle) return 1;
    return 0;
}

static uint64_t vulkan_instance_client_handle_from_host( struct vulkan_instance *instance, uint64_t host_handle )
{
    struct instance *impl = CONTAINING_RECORD( instance, struct instance, obj );
    struct rb_entry *entry;
    uint64_t result = 0;

    pthread_rwlock_rdlock( &impl->objects_lock );
    if ((entry = rb_get( &impl->objects, &host_handle )))
    {
        struct vulkan_object *object = RB_ENTRY_VALUE( entry, struct vulkan_object, entry );
        result = object->client_handle;
    }
    pthread_rwlock_unlock( &impl->objects_lock );
    return result;
}

static void vulkan_instance_insert_object( struct vulkan_instance *instance, struct vulkan_object *obj )
{
    struct instance *impl = CONTAINING_RECORD( instance, struct instance, obj );
    if (impl->objects.compare)
    {
        pthread_rwlock_wrlock( &impl->objects_lock );
        rb_put( &impl->objects, &obj->host_handle, &obj->entry );
        pthread_rwlock_unlock( &impl->objects_lock );
    }
}

static void vulkan_instance_remove_object( struct vulkan_instance *instance, struct vulkan_object *obj )
{
    struct instance *impl = CONTAINING_RECORD( instance, struct instance, obj );
    if (impl->objects.compare)
    {
        pthread_rwlock_wrlock( &impl->objects_lock );
        rb_remove( &impl->objects, &obj->entry );
        pthread_rwlock_unlock( &impl->objects_lock );
    }
}

static void free_debug_utils_messengers( struct list *messengers )
{
    struct vulkan_debug_utils_messenger *messenger, *next;

    LIST_FOR_EACH_ENTRY_SAFE( messenger, next, messengers, struct vulkan_debug_utils_messenger, entry )
    {
        list_remove( &messenger->entry );
        free( messenger );
    }
}

static void free_debug_report_callbacks( struct list *callbacks )
{
    struct vulkan_debug_report_callback *callback, *next;

    LIST_FOR_EACH_ENTRY_SAFE( callback, next, callbacks, struct vulkan_debug_report_callback, entry )
    {
        list_remove( &callback->entry );
        free( callback );
    }
}

static VkResult convert_instance_create_info( struct mempool *pool, VkInstanceCreateInfo *info, struct instance *instance )
{
    const VkBaseInStructure *header = (const VkBaseInStructure *)info;
    const VkDebugReportCallbackCreateInfoEXT *debug_report_callback;
    const char **extensions;
    uint32_t count = 0;

    while ((header = find_next_struct( header->pNext, VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT )))
    {
        const VkDebugUtilsMessengerCreateInfoEXT *debug_utils_messenger = (const VkDebugUtilsMessengerCreateInfoEXT *)header;
        struct vulkan_debug_utils_messenger *messenger = debug_utils_messenger->pUserData;

        list_remove( &messenger->entry );
        list_add_tail( &instance->utils_messengers, &messenger->entry );
        messenger->instance = &instance->obj;
    }

    if ((debug_report_callback = find_next_struct( info->pNext, VK_STRUCTURE_TYPE_DEBUG_REPORT_CALLBACK_CREATE_INFO_EXT )))
    {
        struct vulkan_debug_report_callback *callback = debug_report_callback->pUserData;

        list_remove( &callback->entry );
        list_add_tail( &instance->report_callbacks, &callback->entry );
        callback->instance = &instance->obj;
    }

    if (info->enabledLayerCount)
    {
        FIXME( "Loading explicit layers is not supported!\n" );
        return VK_ERROR_LAYER_NOT_PRESENT;
    }

    driver_funcs->p_map_instance_extensions( &instance->obj.extensions );
    instance->obj.extensions.has_VK_KHR_win32_surface = 0;

    if (instance->obj.extensions.has_VK_EXT_debug_utils || instance->obj.extensions.has_VK_EXT_debug_report)
    {
        rb_init( &instance->objects, vulkan_object_compare );
        pthread_rwlock_init( &instance->objects_lock, NULL );
    }

    if (instance->obj.extensions.has_VK_KHR_win32_surface && vulkan_funcs.host_extensions.has_VK_EXT_surface_maintenance1)
        instance->obj.extensions.has_VK_EXT_surface_maintenance1 = 1;
    if (vulkan_funcs.host_extensions.has_VK_KHR_get_physical_device_properties2)
        instance->obj.extensions.has_VK_KHR_get_physical_device_properties2 = 1;
    if (use_external_memory())
        instance->obj.extensions.has_VK_KHR_external_memory_capabilities = 1;

    /* VK_KHR_win32_keyed_mutex only requires external memory extensions, but we will use
     * external semaphore fds to implement it, so we enable the instance extensions too */
    instance->obj.extensions.has_VK_KHR_external_semaphore_capabilities = 1;

    if (!(extensions = mem_alloc( pool, sizeof(instance->obj.extensions) * 8 * sizeof(*extensions) ))) return VK_ERROR_OUT_OF_HOST_MEMORY;
#define USE_VK_EXT(x) if (instance->obj.extensions.has_ ## x) extensions[count++] = #x;
    ALL_VK_INSTANCE_EXTS
#undef USE_VK_EXT

    TRACE( "Enabling %u host instance extensions\n", count );
    for (const char **extension = extensions, **end = extension + count; extension < end; extension++)
        TRACE( "  - %s\n", debugstr_a(*extension) );

    info->ppEnabledExtensionNames = extensions;
    info->enabledExtensionCount = count;
    return VK_SUCCESS;
}

static VkResult init_physical_device( struct vulkan_physical_device *physical_device, VkPhysicalDevice host_physical_device,
                                      VkPhysicalDevice client_physical_device, struct vulkan_instance *instance )
{
    struct vulkan_device_extensions extensions = {0};
    VkExtensionProperties *properties;
    uint32_t count;
    VkResult res;

    vulkan_object_init_ptr( &physical_device->obj, (UINT_PTR)host_physical_device, &client_physical_device->obj );
    physical_device->instance = instance;

    instance->p_vkGetPhysicalDeviceMemoryProperties( host_physical_device, &physical_device->memory_properties );

    if ((res = instance->p_vkEnumerateDeviceExtensionProperties( host_physical_device, NULL, &count, NULL ))) return res;
    if (!(properties = calloc( count, sizeof(*properties) ))) return res;
    if ((res = instance->p_vkEnumerateDeviceExtensionProperties( host_physical_device, NULL, &count, properties ))) goto done;

    TRACE( "Host physical device extensions:\n" );
    for (uint32_t i = 0; i < count; i++)
    {
        const char *extension = properties[i].extensionName;
#define USE_VK_EXT(x)                           \
        if (!strcmp( extension, #x ))           \
        {                                       \
            extensions.has_ ## x = 1;           \
            TRACE( "  - %s\n", extension );     \
        } else
        ALL_VK_DEVICE_EXTS
#undef USE_VK_EXT
        WARN( "Extension %s is not supported.\n", debugstr_a(extension) );
    }
    physical_device->extensions = extensions;

    if (zero_bits && physical_device->extensions.has_VK_EXT_map_memory_placed && physical_device->extensions.has_VK_KHR_map_memory2)
    {
        VkPhysicalDeviceMapMemoryPlacedFeaturesEXT map_placed_feature = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAP_MEMORY_PLACED_FEATURES_EXT};
        VkPhysicalDeviceFeatures2 features = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &map_placed_feature};

        instance->p_vkGetPhysicalDeviceFeatures2KHR( host_physical_device, &features );
        if (map_placed_feature.memoryMapPlaced && map_placed_feature.memoryUnmapReserve)
        {
            VkPhysicalDeviceMapMemoryPlacedPropertiesEXT map_placed_props = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAP_MEMORY_PLACED_PROPERTIES_EXT};
            VkPhysicalDeviceProperties2 props = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,.pNext = &map_placed_props};

            instance->p_vkGetPhysicalDeviceProperties2( host_physical_device, &props );
            physical_device->map_placed_align = map_placed_props.minPlacedMemoryMapAlignment;
            TRACE( "Using placed map with alignment %u\n", physical_device->map_placed_align );
        }
    }

    if (zero_bits && physical_device->extensions.has_VK_EXT_external_memory_host && !physical_device->map_placed_align)
    {
        VkPhysicalDeviceExternalMemoryHostPropertiesEXT host_mem_props = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_MEMORY_HOST_PROPERTIES_EXT};
        VkPhysicalDeviceProperties2 props = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &host_mem_props};

        instance->p_vkGetPhysicalDeviceProperties2KHR( host_physical_device, &props );
        physical_device->external_memory_align = host_mem_props.minImportedHostPointerAlignment;
        if (physical_device->external_memory_align) WARN( "Not using VK_EXT_external_memory_host for memory mapping\n" );
        else TRACE( "Using VK_EXT_external_memory_host for memory mapping with alignment: %u\n", physical_device->external_memory_align );
    }

    driver_funcs->p_map_device_extensions( &extensions );
    extensions.has_VK_EXT_full_screen_exclusive =
        driver_funcs->p_vulkan_surface_fullscreen_supported &&
        driver_funcs->p_vulkan_surface_fullscreen;
    if (extensions.has_VK_KHR_external_memory_win32 && zero_bits && !physical_device->map_placed_align)
    {
        WARN( "Cannot export WOW64 memory without VK_EXT_map_memory_placed\n" );
        extensions.has_VK_KHR_external_memory_win32 = 0;
    }
    extensions.has_VK_KHR_win32_keyed_mutex = extensions.has_VK_KHR_timeline_semaphore &&
                                              extensions.has_VK_KHR_external_semaphore_fd;

    /* filter out unsupported client device extensions */
#define USE_VK_EXT(x) client_physical_device->extensions.has_ ## x = extensions.has_ ## x;
    ALL_VK_CLIENT_DEVICE_EXTS
#undef USE_VK_EXT

done:
    free( properties );
    return res;
}

/* Helper function which stores wrapped physical devices in the instance object. */
static VkResult init_physical_devices( struct vulkan_instance *instance, struct vulkan_physical_device *physical_devices )
{
    VkInstance client_instance = instance->client.instance;
    VkPhysicalDevice *host_physical_devices;
    uint32_t physical_device_count;
    unsigned int i;
    VkResult res;

    if ((res = instance->p_vkEnumeratePhysicalDevices( instance->host.instance, &physical_device_count, NULL )))
    {
        ERR( "Failed to enumerate physical devices, res %d\n", res );
        return res;
    }
    if (!physical_device_count) return res;

    if (physical_device_count > client_instance->physical_device_count)
    {
        client_instance->physical_device_count = physical_device_count;
        return VK_ERROR_OUT_OF_POOL_MEMORY;
    }
    client_instance->physical_device_count = physical_device_count;

    if (!(host_physical_devices = calloc( physical_device_count, sizeof(*host_physical_devices) ))) return VK_ERROR_OUT_OF_HOST_MEMORY;
    if ((res = instance->p_vkEnumeratePhysicalDevices( instance->host.instance, &physical_device_count, host_physical_devices ))) goto failed;

    /* Wrap each host physical device handle into a dispatchable object for the ICD loader. */
    for (i = 0; i < physical_device_count; i++)
    {
        VkPhysicalDevice client_physical_device = &client_instance->physical_device[i];
        struct vulkan_physical_device *physical_device = physical_devices + i;
        if ((res = init_physical_device( physical_device, host_physical_devices[i], client_physical_device, instance ))) goto failed;
    }
    instance->physical_device_count = physical_device_count;
    instance->physical_devices = physical_devices;

failed:
    free( host_physical_devices );
    return res;
}

static BOOL instance_has_nvidia_wayland_wsi( struct instance *instance )
{
    static const UINT nvidia_vendor_id = 0x10de;
    VkPhysicalDeviceProperties properties;
    unsigned int i;

    if (!instance->obj.extensions.has_VK_KHR_wayland_surface) return FALSE;

    for (i = 0; i < instance->obj.physical_device_count; i++)
    {
        struct vulkan_physical_device *physical_device = &instance->obj.physical_devices[i];

        instance->obj.p_vkGetPhysicalDeviceProperties( physical_device->host.physical_device, &properties );
        if (properties.vendorID == nvidia_vendor_id) return TRUE;
    }

    return FALSE;
}

static void register_nvidia_wayland_instance( struct instance *instance )
{
    if (!instance_has_nvidia_wayland_wsi( instance )) return;

    pthread_mutex_lock( &nvidia_wayland_instance_lock );
    instance->nvidia_wayland_wsi = TRUE;
    nvidia_wayland_instance_count++;
    pthread_mutex_unlock( &nvidia_wayland_instance_lock );
}

static void destroy_delayed_nvidia_wayland_host_instances( void )
{
    struct nvidia_wayland_host_instance *host, *next;

    LIST_FOR_EACH_ENTRY_SAFE( host, next, &delayed_nvidia_wayland_host_instances,
                              struct nvidia_wayland_host_instance, entry )
    {
        list_remove( &host->entry );
        host->p_vkDestroyInstance( host->host_instance, NULL );
        free( host );
    }
}

static void destroy_host_instance( struct instance *instance )
{
    PFN_vkDestroyInstance p_vkDestroyInstance = instance->obj.p_vkDestroyInstance;
    VkInstance host_instance = instance->obj.host.instance;
    struct nvidia_wayland_host_instance *queued;

    if (!instance->nvidia_wayland_wsi)
    {
        p_vkDestroyInstance( host_instance, NULL );
        return;
    }

    pthread_mutex_lock( &nvidia_wayland_instance_lock );

    if (nvidia_wayland_instance_count) nvidia_wayland_instance_count--;
    if (nvidia_wayland_instance_count)
    {
        if ((queued = malloc( sizeof(*queued) )))
        {
            queued->p_vkDestroyInstance = p_vkDestroyInstance;
            queued->host_instance = host_instance;
            list_add_tail( &delayed_nvidia_wayland_host_instances, &queued->entry );
        }
        else
        {
            ERR( "Failed to allocate delayed NVIDIA Wayland VkInstance destroy, leaking host instance %p.\n",
                 host_instance );
        }
    }
    else
    {
        p_vkDestroyInstance( host_instance, NULL );
        destroy_delayed_nvidia_wayland_host_instances();
    }

    pthread_mutex_unlock( &nvidia_wayland_instance_lock );
}

static BOOL add_instance_extension( const char *extension, size_t len, struct vulkan_instance_extensions *extensions )
{
#define USE_VK_EXT(x) \
    if (len == sizeof(#x) - 1 && !strncmp( #x, extension, len ))    \
    {                                                               \
        if (!extensions->has_ ## x) TRACE( "Adding %s\n", #x );     \
        return extensions->has_ ## x = 1;                           \
    }
    ALL_VK_INSTANCE_EXTS
#undef USE_VK_EXT
    WARN( "Extension %s is not supported.\n", debugstr_a(extension) );
    return FALSE;
}

static void parse_instance_extensions( struct vulkan_instance_extensions *extensions, const char *str )
{
    const char *next;
    for (next = str; *next; next++)
    {
        if (*next != ' ') continue;
        add_instance_extension( str, next - str, extensions );
        str = next + 1;
    }
    if (next > str) add_instance_extension( str, next - str, extensions );
}

static VkResult win32u_vkCreateInstance( const VkInstanceCreateInfo *client_create_info, const VkAllocationCallbacks *allocator,
                                         VkInstance *client_instance_ptr )
{
    VkInstanceCreateInfo *create_info = (VkInstanceCreateInfo *)client_create_info; /* cast away const, chain has been copied in the thunks */
    VkInstance host_instance = VK_NULL_HANDLE, client_instance = *client_instance_ptr;
    const VkCreateInfoWineInstanceCallback *callback_info;
    struct vulkan_physical_device *physical_devices;
    struct mempool pool = {0};
    struct instance *instance;
    unsigned int i;
    VkResult res;

    if (!(instance = calloc( 1, sizeof(*instance) + sizeof(*physical_devices) * client_instance->physical_device_count) ))
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    physical_devices = (struct vulkan_physical_device *)(instance + 1);
    instance->obj.extensions = client_instance->extensions;
    list_init( &instance->utils_messengers );
    list_init( &instance->report_callbacks );

    if (instance->obj.extensions.has_VK_WINE_openxr_instance_extensions)
    {
        parse_instance_extensions( &instance->obj.extensions, getenv( "__WINE_OPENXR_VK_INSTANCE_EXTENSIONS" ) );
        instance->obj.extensions.has_VK_WINE_openxr_instance_extensions = 0;
    }

    pthread_key_create(&instance->obj.transient_object_handle, free);

    if ((res = convert_instance_create_info( &pool, create_info, instance ))) goto failed;
    if ((callback_info = pop_next_struct( (VkBaseOutStructure **)&create_info->pNext, VK_STRUCTURE_TYPE_CREATE_INFO_WINE_INSTANCE_CALLBACK )))
    {
        PFN_vkCreateInstanceCallbackWINE callback = (void *)(UINT_PTR)callback_info->native_create_callback;
        if ((res = callback( create_info, allocator, &host_instance, p_vkGetInstanceProcAddr, (void *)(UINT_PTR)callback_info->context ))) goto failed;
    }
    else if ((res = p_vkCreateInstance( create_info, NULL /* allocator */, &host_instance ))) goto failed;

    vulkan_object_init_ptr( &instance->obj.obj, (UINT_PTR)host_instance, &client_instance->obj );
    instance->obj.p_insert_object = vulkan_instance_insert_object;
    instance->obj.p_remove_object = vulkan_instance_remove_object;
    instance->obj.p_client_handle_from_host = vulkan_instance_client_handle_from_host;

#define USE_VK_FUNC( name )                                                                          \
    instance->obj.p_##name = (void *)p_vkGetInstanceProcAddr( instance->obj.host.instance, #name );  \
    if (!instance->obj.p_##name) TRACE( "Instance proc %s not found.\n", #name );
    ALL_VK_INSTANCE_FUNCS
#undef USE_VK_FUNC

    /* Cache physical devices for vkEnumeratePhysicalDevices within the instance as each vkPhysicalDevice is a dispatchable
     * object, which means we need to wrap the host physical devices and present those to the application.
     */
    if ((res = init_physical_devices( &instance->obj, physical_devices ))) goto failed;
    register_nvidia_wayland_instance( instance );

    TRACE( "Created instance %p, host_instance %p.\n", instance, instance->obj.host.instance );
    for (i = 0; i < instance->obj.physical_device_count; i++)
    {
        struct vulkan_physical_device *physical_device = &instance->obj.physical_devices[i];
        vulkan_instance_insert_object( &instance->obj, &physical_device->obj );
    }
    vulkan_instance_insert_object( &instance->obj, &instance->obj.obj );

failed:
    if (res)
    {
        WARN( "Failed to create vulkan instance, res %d\n", res );
        if (host_instance) instance->obj.p_vkDestroyInstance( host_instance, NULL /* allocator */ );
        free_debug_utils_messengers( &instance->utils_messengers );
        free_debug_report_callbacks( &instance->report_callbacks );
        free( instance );
    }
    mem_free( &pool );
    return res;
}

static void win32u_vkDestroyInstance( VkInstance client_instance, const VkAllocationCallbacks *allocator )
{
    struct instance *instance = instance_from_handle( client_instance );

    if (!instance) return;

    destroy_host_instance( instance );
    for (int i = 0; i < instance->obj.physical_device_count; i++)
        vulkan_instance_remove_object( &instance->obj, &instance->obj.physical_devices[i].obj );
    vulkan_instance_remove_object( &instance->obj, &instance->obj.obj );

    if (instance->objects.compare) pthread_rwlock_destroy( &instance->objects_lock );
    free_debug_utils_messengers( &instance->utils_messengers );
    free_debug_report_callbacks( &instance->report_callbacks );
    pthread_key_delete(instance->obj.transient_object_handle);
    free( instance );
}

static BOOL add_device_extension( const char *extension, size_t len, struct vulkan_device_extensions *extensions )
{
#define USE_VK_EXT(x) \
    if (len == sizeof(#x) - 1 && !strncmp( #x, extension, len ))    \
    {                                                               \
        if (!extensions->has_ ## x) TRACE( "Adding %s\n", #x );     \
        return extensions->has_ ## x = 1;                           \
    }
    ALL_VK_DEVICE_EXTS
#undef USE_VK_EXT
    WARN( "Extension %s is not supported.\n", debugstr_a(extension) );
    return FALSE;
}

static void parse_device_extensions( struct vulkan_device_extensions *extensions, const char *str )
{
    const char *next;

    for (next = str; *next; next++)
    {
        if (*next != ' ') continue;
        add_device_extension( str, next - str, extensions );
        str = next + 1;
    }
    if (next > str) add_device_extension( str, next - str, extensions );
}

static VkResult convert_device_create_info( struct vulkan_physical_device *physical_device, VkDeviceCreateInfo *info,
                                            struct mempool *pool, struct vulkan_device *device )
{
    struct vulkan_instance *instance = physical_device->instance;
    const char **extensions;
    uint32_t count = 0;

    /* Should be filtered out by loader as ICDs don't support layers. */
    info->enabledLayerCount = 0;
    info->ppEnabledLayerNames = NULL;

    if (device->extensions.has_VK_KHR_win32_keyed_mutex)
    {
        device->extensions.has_VK_KHR_timeline_semaphore = 1;
        device->extensions.has_VK_KHR_external_semaphore_fd = 1;
        device->extensions.has_VK_KHR_external_semaphore = 1;
    }

    device->full_screen_exclusive_enabled =
        device->extensions.has_VK_EXT_full_screen_exclusive;
    driver_funcs->p_map_device_extensions( &device->extensions );
    device->extensions.has_VK_EXT_full_screen_exclusive = 0;
    device->extensions.has_VK_KHR_win32_keyed_mutex = 0;
    device->extensions.has_VK_KHR_external_memory_win32 = 0;
    device->extensions.has_VK_KHR_external_fence_win32 = 0;
    device->extensions.has_VK_KHR_external_semaphore_win32 = 0;
    device->extensions.has_VK_WINE_openvr_device_extensions = 0;
    device->extensions.has_VK_WINE_openxr_device_extensions = 0;

    if (device->extensions.has_VK_EXT_external_memory_dma_buf)
        device->extensions.has_VK_KHR_external_memory_fd = 1;

    if (physical_device->map_placed_align)
    {
        VkPhysicalDeviceMapMemoryPlacedFeaturesEXT *map_placed_features;

        if (!(map_placed_features = mem_alloc( pool, sizeof(*map_placed_features) ))) return VK_ERROR_OUT_OF_HOST_MEMORY;
        map_placed_features->sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAP_MEMORY_PLACED_FEATURES_EXT;
        map_placed_features->pNext = (void *)info->pNext;
        map_placed_features->memoryMapPlaced = VK_TRUE;
        map_placed_features->memoryMapRangePlaced = VK_FALSE;
        map_placed_features->memoryUnmapReserve = VK_TRUE;
        info->pNext = map_placed_features;

        device->extensions.has_VK_EXT_map_memory_placed = 1;
        device->extensions.has_VK_KHR_map_memory2 = 1;
    }
    else if (physical_device->external_memory_align)
    {
        device->extensions.has_VK_KHR_external_memory = 1;
        device->extensions.has_VK_EXT_external_memory_host = 1;
    }

    /* win32u uses VkSwapchainPresentScalingCreateInfoEXT if available. */
    if (device->extensions.has_VK_KHR_swapchain && instance->extensions.has_VK_EXT_surface_maintenance1 &&
        physical_device->extensions.has_VK_EXT_swapchain_maintenance1)
        device->extensions.has_VK_EXT_swapchain_maintenance1 = 1;

    /* Force-enable the extensions the cross-process producer needs so we can
     * interpose a managed swapchain even when the app did not request them.
     * Only meaningful for swapchain devices. */
    if (device->extensions.has_VK_KHR_swapchain &&
        physical_device->extensions.has_VK_EXT_image_drm_format_modifier &&
        physical_device->extensions.has_VK_EXT_external_memory_dma_buf)
    {
        device->extensions.has_VK_EXT_image_drm_format_modifier = 1;
        device->extensions.has_VK_EXT_external_memory_dma_buf = 1;
        device->extensions.has_VK_KHR_external_memory_fd = 1;
        device->extensions.has_VK_KHR_external_memory = 1;
        /* VK_EXT_image_drm_format_modifier requires VK_KHR_image_format_list +
         * VK_KHR_bind_memory2 + VK_KHR_sampler_ycbcr_conversion (1.1 core).
         * Enable the KHR aliases defensively when the host advertises them. */
        if (physical_device->extensions.has_VK_KHR_image_format_list)
            device->extensions.has_VK_KHR_image_format_list = 1;
        if (physical_device->extensions.has_VK_KHR_bind_memory2)
            device->extensions.has_VK_KHR_bind_memory2 = 1;
        if (physical_device->extensions.has_VK_KHR_sampler_ycbcr_conversion)
            device->extensions.has_VK_KHR_sampler_ycbcr_conversion = 1;
        if (physical_device->extensions.has_VK_KHR_external_semaphore_fd)
        {
            device->extensions.has_VK_KHR_external_semaphore_fd = 1;
            device->extensions.has_VK_KHR_external_semaphore = 1;
        }
    }

    if (!(extensions = mem_alloc( pool, sizeof(device->extensions) * 8 * sizeof(*extensions) ))) return VK_ERROR_OUT_OF_HOST_MEMORY;
#define USE_VK_EXT(x) if (device->extensions.has_ ## x) extensions[count++] = #x;
    ALL_VK_DEVICE_EXTS
#undef USE_VK_EXT

    TRACE( "Enabling %u host device extensions\n", count );
    for (const char **extension = extensions, **end = extension + count; extension < end; extension++)
        TRACE( "  - %s\n", debugstr_a(*extension) );

    info->ppEnabledExtensionNames = extensions;
    info->enabledExtensionCount = count;
    return VK_SUCCESS;
}

static void init_device_queues( struct vulkan_device *device, const VkDeviceQueueCreateInfo *create_info, VkDevice client_device )
{
    VkDeviceQueueInfo2 info = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_INFO_2};
    VkQueue client_queues = client_device->queues + device->queue_count;
    struct vulkan_queue *queues = device->queues + device->queue_count;

    TRACE( "Queue family index %u, queue count %u.\n", create_info->queueFamilyIndex, create_info->queueCount );

    info.flags = create_info->flags;
    info.queueFamilyIndex = create_info->queueFamilyIndex;
    for (info.queueIndex = 0; info.queueIndex < create_info->queueCount; info.queueIndex++)
    {
        VkQueue host_queue, client_queue = client_queues + info.queueIndex;
        struct vulkan_queue *queue = queues + info.queueIndex;

        if (info.flags && device->p_vkGetDeviceQueue2) device->p_vkGetDeviceQueue2( device->host.device, &info, &host_queue );
        else device->p_vkGetDeviceQueue( device->host.device, info.queueFamilyIndex, info.queueIndex, &host_queue );
        vulkan_object_init_ptr( &queue->obj, (UINT_PTR)host_queue, &client_queue->obj );
        queue->device = device;
        queue->info = info;
        pthread_mutex_init( &queue->mutex, NULL );

        TRACE( "Got device %p queue %p, host_queue %p.\n", device, queue, queue->host.queue );
    }

    device->queue_count += create_info->queueCount;
}

static VkResult win32u_vkCreateDevice( VkPhysicalDevice client_physical_device, const VkDeviceCreateInfo *client_create_info,
                                       const VkAllocationCallbacks *allocator, VkDevice *client_device_ptr )
{
    VkDeviceCreateInfo *create_info = (VkDeviceCreateInfo *)client_create_info; /* cast away const, chain has been copied in the thunks */
    struct vulkan_physical_device *physical_device = vulkan_physical_device_from_handle( client_physical_device );
    struct vulkan_instance *instance = physical_device->instance;
    VkDevice host_device, client_device = *client_device_ptr;
    const VkCreateInfoWineDeviceCallback *callback_info;
    unsigned int queue_count, props_count, i;
    struct vulkan_device *device;
    struct mempool pool = {0};
    VkPhysicalDeviceFeatures features = {0};
    VkResult res;

    if (TRACE_ON(vulkan))
    {
        VkPhysicalDeviceProperties properties = {0};
        instance->p_vkGetPhysicalDeviceProperties( physical_device->host.physical_device, &properties );
        TRACE( "Device name: %s.\n", debugstr_a(properties.deviceName) );
        TRACE( "Vendor ID: %#x, Device ID: %#x.\n", properties.vendorID, properties.deviceID );
        TRACE( "Driver version: %#x.\n", properties.driverVersion );
    }

    /* We need to cache all queues within the device as each requires wrapping since queues are dispatchable objects. */
    for (queue_count = 0, i = 0; i < create_info->queueCreateInfoCount; i++) queue_count += create_info->pQueueCreateInfos[i].queueCount;
    instance->p_vkGetPhysicalDeviceQueueFamilyProperties(physical_device->host.physical_device, &props_count, NULL);

    if (!(device = calloc( 1, sizeof(*device) + queue_count * sizeof(*device->queues) + props_count * sizeof(*device->queue_props) ))) return VK_ERROR_OUT_OF_HOST_MEMORY;
    device->extensions = client_device->extensions;
    device->queues = (void *)(device + 1);
    device->queue_props = (void *)(device->queues + queue_count);

{
        VkPhysicalDeviceFeatures2 *features2;

        /* Enable shaderStorageImageWriteWithoutFormat for fshack
         * This is available on all hardware and driver combinations we care about.
         */
        if (create_info->pEnabledFeatures)
        {
            features = *create_info->pEnabledFeatures;
            features.shaderStorageImageWriteWithoutFormat = VK_TRUE;
            create_info->pEnabledFeatures = &features;
        }
        if ((features2 = (void *)find_next_struct((const void *)create_info, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2)))
        {
            features2->features.shaderStorageImageWriteWithoutFormat = VK_TRUE;
        }
        else if (!create_info->pEnabledFeatures)
        {
            features.shaderStorageImageWriteWithoutFormat = VK_TRUE;
            create_info->pEnabledFeatures = &features;
        }
}

    if (device->extensions.has_VK_WINE_openvr_device_extensions)
    {
        VkPhysicalDeviceProperties properties = {0};
        const char *vr_exts;
        char name[64];
        instance->p_vkGetPhysicalDeviceProperties( physical_device->host.physical_device, &properties );
        sprintf( name, "VK_WINE_OPENVR_DEVICE_EXTS_PCIID_%04x_%04x", properties.vendorID, (uint16_t)properties.deviceID );
        if (!(vr_exts = getenv( name ))) vr_exts = getenv( "VK_WINE_OPENVR_DEVICE_EXTS" );
        if (vr_exts) parse_device_extensions( &device->extensions, vr_exts );
        device->extensions.has_VK_WINE_openvr_device_extensions = 0;
    }
    if (device->extensions.has_VK_WINE_openxr_device_extensions)
    {
        parse_device_extensions( &device->extensions, getenv( "__WINE_OPENXR_VK_DEVICE_EXTENSIONS" ) );
        device->extensions.has_VK_WINE_openxr_device_extensions = 0;
    }

    if ((res = convert_device_create_info( physical_device, create_info, &pool, device ))) goto failed;
    if ((callback_info = pop_next_struct( (VkBaseOutStructure **)&create_info->pNext, VK_STRUCTURE_TYPE_CREATE_INFO_WINE_DEVICE_CALLBACK )))
    {
        PFN_vkCreateDeviceCallbackWINE callback = (void *)(UINT_PTR)callback_info->native_create_callback;
        if ((res = callback( physical_device->host.physical_device, create_info, allocator, &host_device, p_vkGetDeviceProcAddr, (void *)(UINT_PTR)callback_info->context ))) goto failed;
    }
    else if ((res = instance->p_vkCreateDevice( physical_device->host.physical_device, create_info, NULL /* allocator */, &host_device ))) goto failed;

    vulkan_object_init_ptr( &device->obj, (UINT_PTR)host_device, &client_device->obj );
    device->physical_device = physical_device;

#define USE_VK_FUNC( name )                                                          \
    device->p_##name = (void *)p_vkGetDeviceProcAddr( device->host.device, #name );  \
    if (!device->p_##name) TRACE( "Device proc %s not found.\n", #name );
    ALL_VK_DEVICE_FUNCS
#undef USE_VK_FUNC

    for (i = 0; i < create_info->queueCreateInfoCount; i++) init_device_queues( device, create_info->pQueueCreateInfos + i, client_device );
    instance->p_vkGetPhysicalDeviceQueueFamilyProperties( physical_device->host.physical_device, &props_count, device->queue_props );

    TRACE( "Created device %p, host_device %p.\n", device, device->host.device );
    for (struct vulkan_queue *queue = device->queues; queue < device->queues + device->queue_count; queue++)
        instance->p_insert_object( instance, &queue->obj );
    instance->p_insert_object( instance, &device->obj );

failed:
    if (res)
    {
        WARN( "Failed to create device, res %d\n", res );
        free( device );
    }
    mem_free( &pool );
    return res;
}

static void win32u_vkDestroyDevice( VkDevice client_device, const VkAllocationCallbacks *allocator )
{
    struct vulkan_device *device = vulkan_device_from_handle( client_device );
    struct vulkan_instance *instance = device->physical_device->instance;
    unsigned int i;

    if (!device) return;

    for (i = 0; i < device->queue_count; i++)
    {
        if (device->queues[i].managed_host_semaphore)
            device->p_vkDestroySemaphore( device->host.device,
                                          device->queues[i].managed_host_semaphore, NULL );
        if (device->queues[i].managed_present_semaphore)
            device->p_vkDestroySemaphore( device->host.device,
                                          device->queues[i].managed_present_semaphore, NULL );
    }
    device->p_vkDestroyDevice( device->host.device, NULL /* pAllocator */ );
    for (i = 0; i < device->queue_count; i++)
    {
        pthread_mutex_destroy( &device->queues[i].mutex );
        instance->p_remove_object( instance, &device->queues[i].obj );
    }
    instance->p_remove_object( instance, &device->obj );

    free( device );
}

static VkQueue device_find_queue( VkDevice client_device, const VkDeviceQueueInfo2 *info )
{
    struct vulkan_device *device = vulkan_device_from_handle( client_device );

    for (struct vulkan_queue *queue = device->queues; queue < device->queues + device->queue_count; queue++)
        if (!memcmp( &queue->info, info, sizeof(*info) )) return queue->client.queue;

    return VK_NULL_HANDLE;
}

static void win32u_vkGetDeviceQueue( VkDevice client_device, uint32_t family_index, uint32_t queue_index, VkQueue *client_queue )
{
    VkDeviceQueueInfo2 info = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_INFO_2};
    info.queueFamilyIndex = family_index;
    info.queueIndex = queue_index;

    *client_queue = device_find_queue( client_device, &info );
}

static void win32u_vkGetDeviceQueue2( VkDevice client_device, const VkDeviceQueueInfo2 *client_info, VkQueue *client_queue )
{
    VkDeviceQueueInfo2 info = *client_info;
    if (info.pNext) FIXME( "pNext not implemented\n" );
    info.pNext = NULL;

    *client_queue = device_find_queue( client_device, &info );
}

static void set_transient_client_handle(struct vulkan_instance *instance, uint64_t client_handle)
{
    uint64_t *handle = pthread_getspecific(instance->transient_object_handle);
    if (!handle)
    {
        handle = malloc(sizeof(uint64_t));
        pthread_setspecific(instance->transient_object_handle, handle);
    }
    *handle = client_handle;
}

static VkResult win32u_vkAllocateMemory( VkDevice client_device, const VkMemoryAllocateInfo *client_alloc_info,
                                         const VkAllocationCallbacks *allocator, VkDeviceMemory *ret )
{
    VkImportMemoryFdInfoKHR fd_info = {.sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR};
    VkMemoryAllocateInfo *alloc_info = (VkMemoryAllocateInfo *)client_alloc_info; /* cast away const, chain has been copied in the thunks */
    VkBaseOutStructure **next, *prev = (VkBaseOutStructure *)alloc_info;
    struct vulkan_device *device = vulkan_device_from_handle( client_device );
    struct vulkan_physical_device *physical_device = device->physical_device;
    struct vulkan_instance *instance = device->physical_device->instance;
    VkImportMemoryHostPointerInfoEXT host_pointer_info, *pointer_info = NULL;
    VkExportMemoryWin32HandleInfoKHR export_win32 = {.dwAccess = GENERIC_ALL};
    VkImportMemoryWin32HandleInfoKHR *import_win32 = NULL;
    VkDeviceMemory host_device_memory = VK_NULL_HANDLE;
    VkExportMemoryAllocateInfo *export_info = NULL;
    struct device_memory *memory;
    BOOL export_win32_handle = FALSE;
    BOOL nt_shared = FALSE;
    uint32_t mem_flags;
    void *mapping = NULL;
    VkResult res;

    for (next = &prev->pNext; *next; prev = *next, next = &(*next)->pNext)
    {
        switch ((*next)->sType)
        {
        case VK_STRUCTURE_TYPE_DEDICATED_ALLOCATION_MEMORY_ALLOCATE_INFO_NV: break;
        case VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO:
            export_info = (VkExportMemoryAllocateInfo *)*next;
            if (export_info->handleTypes & EXTERNAL_MEMORY_WIN32_BITS)
            {
                export_win32_handle = TRUE;
                nt_shared = !(export_info->handleTypes & (VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_KMT_BIT |
                                                          VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_KMT_BIT));
            }
            export_info->handleTypes = map_external_memory_handle_types( export_info->handleTypes );
            break;
        case VK_STRUCTURE_TYPE_EXPORT_MEMORY_WIN32_HANDLE_INFO_KHR:
            export_win32 = *(VkExportMemoryWin32HandleInfoKHR *)*next;
            *next = (*next)->pNext; next = &prev;
            break;
        case VK_STRUCTURE_TYPE_IMPORT_MEMORY_HOST_POINTER_INFO_EXT:
            pointer_info = (VkImportMemoryHostPointerInfoEXT *)*next;
            break;
        case VK_STRUCTURE_TYPE_IMPORT_MEMORY_WIN32_HANDLE_INFO_KHR:
            import_win32 = (VkImportMemoryWin32HandleInfoKHR *)*next;
            *next = (*next)->pNext; next = &prev;
            break;
        case VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO: break;
        case VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO: break;
        case VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO_TENSOR_ARM: break;
        case VK_STRUCTURE_TYPE_MEMORY_OPAQUE_CAPTURE_ADDRESS_ALLOCATE_INFO: break;
        case VK_STRUCTURE_TYPE_MEMORY_PRIORITY_ALLOCATE_INFO_EXT: break;
        default: FIXME( "Unhandled sType %u.\n", (*next)->sType ); break;
        }
    }

    /* For host visible memory, we try to use VK_EXT_external_memory_host on wow64 to ensure that mapped pointer is 32-bit. */
    mem_flags = physical_device->memory_properties.memoryTypes[alloc_info->memoryTypeIndex].propertyFlags;
    if (physical_device->external_memory_align && (mem_flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) && !pointer_info &&
        (res = allocate_external_host_memory( device, alloc_info, mem_flags, &host_pointer_info )))
        return res;

    if (!(memory = calloc( 1, sizeof(*memory) ))) return VK_ERROR_OUT_OF_HOST_MEMORY;

    if (import_win32)
    {
        switch (import_win32->handleType)
        {
        case VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_KMT_BIT:
        case VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_KMT_BIT:
            memory->global = PtrToUlong( import_win32->handle );
            memory->local = d3dkmt_open_resource( memory->global, NULL, &memory->mutex, &memory->sync );
            break;
        case VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT:
        case VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_BIT:
        case VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D12_HEAP_BIT:
        case VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D12_RESOURCE_BIT:
        {
            HANDLE shared = import_win32->handle;
            if (import_win32->name && !(shared = open_shared_resource_from_name( import_win32->name ))) break;
            memory->local = d3dkmt_open_resource( 0, shared, &memory->mutex, &memory->sync );
            if (shared && shared != import_win32->handle) NtClose( shared );
            break;
        }
        default:
            FIXME( "Unsupported handle type %#x\n", import_win32->handleType );
            break;
        }

        if (device->client.device->extensions.has_VK_KHR_win32_keyed_mutex && memory->sync)
        {
            VkSemaphoreTypeCreateInfo semaphore_type = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};
            VkSemaphoreCreateInfo semaphore_create = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, .pNext = &semaphore_type};
            VkImportSemaphoreFdInfoKHR fd_info = {.sType = VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_FD_INFO_KHR};

            semaphore_type.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
            if ((res = device->p_vkCreateSemaphore( device->host.device, &semaphore_create, NULL, &memory->semaphore ))) goto failed;

            fd_info.handleType = get_host_external_semaphore_type();
            fd_info.semaphore = memory->semaphore;
            if ((fd_info.fd = d3dkmt_object_get_fd( memory->sync )) < 0)
            {
                res = VK_ERROR_INVALID_EXTERNAL_HANDLE;
                goto failed;
            }

            if ((res = device->p_vkImportSemaphoreFdKHR( device->host.device, &fd_info ))) goto failed;
        }

        if ((fd_info.fd = d3dkmt_object_get_fd( memory->local )) < 0)
        {
            res = VK_ERROR_INVALID_EXTERNAL_HANDLE;
            goto failed;
        }

        fd_info.handleType = get_host_external_memory_type();
        fd_info.pNext = alloc_info->pNext;
        alloc_info->pNext = &fd_info;
    }

    set_transient_client_handle(instance, (uintptr_t)&memory->obj.obj);
    if ((res = device->p_vkAllocateMemory( device->host.device, alloc_info, NULL, &host_device_memory ))) goto failed;

    if (export_info && export_win32_handle)
    {
        if (!memory->local)
        {
            VkMemoryGetFdInfoKHR get_fd_info = {.sType = VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR, .memory = host_device_memory};
            int fd = -1;

            switch ((get_fd_info.handleType = get_host_external_memory_type()))
            {
            case VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT:
            case VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT:
                if ((res = device->p_vkGetMemoryFdKHR( device->host.device, &get_fd_info, &fd ))) goto failed;
                break;
            default:
                FIXME( "Unsupported handle type %#x\n", get_fd_info.handleType );
                break;
            }

            memory->local = d3dkmt_create_resource( fd, nt_shared ? NULL : &memory->global );
            close( fd );

            if (!memory->local) goto failed;
        }
        if (nt_shared && !(memory->shared = create_shared_resource_handle( memory->local, &export_win32 ))) goto failed;
    }

    vulkan_object_init( &memory->obj.obj, host_device_memory );
    memory->size = alloc_info->allocationSize;
    memory->vm_map = mapping;
    instance->p_insert_object( instance, &memory->obj.obj );

    *ret = memory->obj.client.device_memory;
    return VK_SUCCESS;

failed:
    WARN( "Failed to allocate memory, res %d\n", res );
    if (host_device_memory) device->p_vkFreeMemory( device->host.device, host_device_memory, NULL );
    if (memory->semaphore) device->p_vkDestroySemaphore( device->host.device, memory->semaphore, NULL );
    d3dkmt_destroy_resource( memory->local );
    d3dkmt_destroy_mutex( memory->mutex );
    d3dkmt_destroy_sync( memory->sync );
    free( memory );
    return VK_ERROR_OUT_OF_HOST_MEMORY;
}

static void win32u_vkFreeMemory( VkDevice client_device, VkDeviceMemory client_memory, const VkAllocationCallbacks *allocator )
{
    struct vulkan_device *device = vulkan_device_from_handle( client_device );
    struct vulkan_physical_device *physical_device = device->physical_device;
    struct vulkan_instance *instance = device->physical_device->instance;
    struct device_memory *memory;

    if (!client_memory) return;
    memory = device_memory_from_handle( client_memory );

    if (memory->vm_map && !physical_device->external_memory_align)
    {
        const VkMemoryUnmapInfoKHR info =
        {
            .sType = VK_STRUCTURE_TYPE_MEMORY_UNMAP_INFO_KHR,
            .memory = memory->obj.host.device_memory,
            .flags = VK_MEMORY_UNMAP_RESERVE_BIT_EXT,
        };
        device->p_vkUnmapMemory2KHR( device->host.device, &info );
    }

    device->p_vkFreeMemory( device->host.device, memory->obj.host.device_memory, NULL );
    instance->p_remove_object( instance, &memory->obj.obj );

    if (memory->vm_map)
    {
        SIZE_T alloc_size = 0;
        NtFreeVirtualMemory( GetCurrentProcess(), &memory->vm_map, &alloc_size, MEM_RELEASE );
    }

    if (memory->semaphore) device->p_vkDestroySemaphore( device->host.device, memory->semaphore, NULL );
    if (memory->shared) NtClose( memory->shared );
    d3dkmt_destroy_resource( memory->local );
    d3dkmt_destroy_mutex( memory->mutex );
    d3dkmt_destroy_sync( memory->sync );
    free( memory );
}

static VkResult win32u_vkGetMemoryWin32HandleKHR( VkDevice client_device, const VkMemoryGetWin32HandleInfoKHR *handle_info, HANDLE *handle )
{
    struct vulkan_device *device = vulkan_device_from_handle( client_device );
    struct device_memory *memory = device_memory_from_handle( handle_info->memory );

    TRACE( "device %p, handle_info %p, handle %p\n", device, handle_info, handle );

    switch (handle_info->handleType)
    {
    case VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_KMT_BIT:
    case VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_KMT_BIT:
        TRACE( "Returning global D3DKMT handle %#x\n", memory->global );
        *handle = UlongToPtr( memory->global );
        return VK_SUCCESS;

    case VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT:
    case VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_BIT:
    case VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D12_HEAP_BIT:
    case VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D12_RESOURCE_BIT:
        NtDuplicateObject( NtCurrentProcess(), memory->shared, NtCurrentProcess(), handle, 0, 0, DUPLICATE_SAME_ATTRIBUTES | DUPLICATE_SAME_ACCESS );
        TRACE( "Returning NT shared handle %p -> %p\n", memory->shared, *handle );
        return VK_SUCCESS;

    default:
        FIXME( "Unsupported handle type %#x\n", handle_info->handleType );
        return VK_ERROR_INCOMPATIBLE_DRIVER;
    }
}

static BOOL is_d3dkmt_global( D3DKMT_HANDLE handle )
{
    return (handle & 0xc0000000) && (handle & 0x3f) == 2;
}

static VkResult win32u_vkGetMemoryWin32HandlePropertiesKHR( VkDevice client_device, VkExternalMemoryHandleTypeFlagBits handle_type, HANDLE handle,
                                                            VkMemoryWin32HandlePropertiesKHR *handle_properties )
{
    static const UINT d3dkmt_type_bits = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_KMT_BIT | VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_KMT_BIT;
    struct vulkan_device *device = vulkan_device_from_handle( client_device );

    TRACE( "device %p, handle_type %#x, handle %p, handle_properties %p\n", device, handle_type, handle, handle_properties );

    if (is_d3dkmt_global( HandleToULong( handle ) )) handle_properties->memoryTypeBits = d3dkmt_type_bits;
    else handle_properties->memoryTypeBits = EXTERNAL_MEMORY_WIN32_BITS & ~d3dkmt_type_bits;

    return VK_SUCCESS;
}

static VkResult win32u_vkMapMemory2KHR( VkDevice client_device, const VkMemoryMapInfoKHR *map_info, void **data )
{
    struct vulkan_device *device = vulkan_device_from_handle( client_device );
    struct vulkan_physical_device *physical_device = device->physical_device;
    struct device_memory *memory = device_memory_from_handle( map_info->memory );
    VkMemoryMapInfoKHR info = *map_info;
    VkMemoryMapPlacedInfoEXT placed_info =
    {
        .sType = VK_STRUCTURE_TYPE_MEMORY_MAP_PLACED_INFO_EXT,
    };
    VkResult res;

    info.memory = memory->obj.host.device_memory;
    if (memory->vm_map)
    {
        *data = (char *)memory->vm_map + info.offset;
        TRACE( "returning %p\n", *data );
        return VK_SUCCESS;
    }

    if (physical_device->map_placed_align)
    {
        SIZE_T alloc_size = memory->size;

        placed_info.pNext = info.pNext;
        info.pNext = &placed_info;
        info.offset = 0;
        info.size = VK_WHOLE_SIZE;
        info.flags |= VK_MEMORY_MAP_PLACED_BIT_EXT;

        if (NtAllocateVirtualMemory( GetCurrentProcess(), &placed_info.pPlacedAddress, zero_bits,
                                     &alloc_size, MEM_COMMIT, PAGE_READWRITE ))
        {
            ERR( "NtAllocateVirtualMemory failed\n" );
            return VK_ERROR_OUT_OF_HOST_MEMORY;
        }
    }

    if (device->p_vkMapMemory2KHR)
        res = device->p_vkMapMemory2KHR( device->host.device, &info, data );
    else
    {
        if (info.pNext) FIXME( "struct extension chain not implemented!\n" );
        res = device->p_vkMapMemory( device->host.device, info.memory, info.offset, info.size, info.flags, data );
    }

    if (placed_info.pPlacedAddress)
    {
        if (res != VK_SUCCESS)
        {
            SIZE_T alloc_size = 0;
            ERR( "vkMapMemory2EXT failed: %d\n", res );
            NtFreeVirtualMemory( GetCurrentProcess(), &placed_info.pPlacedAddress, &alloc_size, MEM_RELEASE );
            return res;
        }
        memory->vm_map = placed_info.pPlacedAddress;
        *data = (char *)memory->vm_map + map_info->offset;
        TRACE( "Using placed mapping %p\n", memory->vm_map );
    }

#ifdef _WIN64
    if (NtCurrentTeb()->WowTebOffset && res == VK_SUCCESS && (UINT_PTR)*data >> 32)
    {
        FIXME( "returned mapping %p does not fit 32-bit pointer\n", *data );
        device->p_vkUnmapMemory( device->host.device, memory->obj.host.device_memory );
        *data = NULL;
        res = VK_ERROR_OUT_OF_HOST_MEMORY;
    }
#endif

    return res;
}

static VkResult win32u_vkMapMemory( VkDevice client_device, VkDeviceMemory client_memory, VkDeviceSize offset,
                                    VkDeviceSize size, VkMemoryMapFlags flags, void **data )
{
    const VkMemoryMapInfoKHR info =
    {
        .sType = VK_STRUCTURE_TYPE_MEMORY_MAP_INFO_KHR,
        .flags = flags,
        .memory = client_memory,
        .offset = offset,
        .size = size,
    };

    return win32u_vkMapMemory2KHR( client_device, &info, data );
}

static VkResult win32u_vkUnmapMemory2KHR( VkDevice client_device, const VkMemoryUnmapInfoKHR *unmap_info )
{
    struct vulkan_device *device = vulkan_device_from_handle( client_device );
    struct vulkan_physical_device *physical_device = device->physical_device;
    struct device_memory *memory = device_memory_from_handle( unmap_info->memory );
    VkMemoryUnmapInfoKHR info;
    VkResult res;

    if (memory->vm_map && physical_device->external_memory_align) return VK_SUCCESS;

    if (!device->p_vkUnmapMemory2KHR)
    {
        if (unmap_info->pNext || memory->vm_map) FIXME( "Not implemented\n" );
        device->p_vkUnmapMemory( device->host.device, memory->obj.host.device_memory );
        return VK_SUCCESS;
    }

    info = *unmap_info;
    info.memory = memory->obj.host.device_memory;
    if (memory->vm_map) info.flags |= VK_MEMORY_UNMAP_RESERVE_BIT_EXT;

    res = device->p_vkUnmapMemory2KHR( device->host.device, &info );

    if (res == VK_SUCCESS && memory->vm_map)
    {
        SIZE_T size = 0;
        NtFreeVirtualMemory( GetCurrentProcess(), &memory->vm_map, &size, MEM_RELEASE );
        memory->vm_map = NULL;
    }
    return res;
}

static void win32u_vkUnmapMemory( VkDevice client_device, VkDeviceMemory client_memory )
{
    const VkMemoryUnmapInfoKHR info =
    {
        .sType = VK_STRUCTURE_TYPE_MEMORY_UNMAP_INFO_KHR,
        .memory = client_memory,
    };

    win32u_vkUnmapMemory2KHR( client_device, &info );
}

static VkResult win32u_vkCreateBuffer( VkDevice client_device, const VkBufferCreateInfo *create_info,
                                       const VkAllocationCallbacks *allocator, VkBuffer *buffer )
{
    VkBaseOutStructure **next, *prev = (VkBaseOutStructure *)create_info; /* cast away const, chain has been copied in the thunks */
    struct vulkan_device *device = vulkan_device_from_handle( client_device );
    struct vulkan_physical_device *physical_device = device->physical_device;
    VkExternalMemoryBufferCreateInfo host_external_info, *external_info = NULL;

    for (next = &prev->pNext; *next; prev = *next, next = &(*next)->pNext)
    {
        switch ((*next)->sType)
        {
        case VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_CREATE_INFO_EXT: break;
        case VK_STRUCTURE_TYPE_BUFFER_OPAQUE_CAPTURE_ADDRESS_CREATE_INFO: break;
        case VK_STRUCTURE_TYPE_BUFFER_USAGE_FLAGS_2_CREATE_INFO: break;
        case VK_STRUCTURE_TYPE_DEDICATED_ALLOCATION_BUFFER_CREATE_INFO_NV: break;
        case VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO:
            external_info = (VkExternalMemoryBufferCreateInfo *)*next;
            external_info->handleTypes = map_external_memory_handle_types( external_info->handleTypes );
            break;
        case VK_STRUCTURE_TYPE_OPAQUE_CAPTURE_DESCRIPTOR_DATA_CREATE_INFO_EXT: break;
        case VK_STRUCTURE_TYPE_VIDEO_PROFILE_LIST_INFO_KHR: break;
        default: FIXME( "Unhandled sType %u.\n", (*next)->sType ); break;
        }
    }

    if (physical_device->external_memory_align && !external_info)
    {
        host_external_info.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO;
        host_external_info.pNext = create_info->pNext;
        host_external_info.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT;
        ((VkBufferCreateInfo *)create_info)->pNext = &host_external_info; /* cast away const, it has been copied in the thunks */
    }

    return device->p_vkCreateBuffer( device->host.device, create_info, NULL, buffer );
}

static void win32u_vkGetDeviceBufferMemoryRequirements( VkDevice client_device, const VkDeviceBufferMemoryRequirements *buffer_requirements,
                                                        VkMemoryRequirements2 *memory_requirements )
{
    VkBaseOutStructure **next, *prev = (VkBaseOutStructure *)buffer_requirements->pCreateInfo; /* cast away const, chain has been copied in the thunks */
    struct vulkan_device *device = vulkan_device_from_handle( client_device );
    VkExternalMemoryBufferCreateInfo *external_info;

    TRACE( "device %p, buffer_requirements %p, memory_requirements %p\n", device, buffer_requirements, memory_requirements );

    for (next = &prev->pNext; *next; prev = *next, next = &(*next)->pNext)
    {
        switch ((*next)->sType)
        {
        case VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_CREATE_INFO_EXT: break;
        case VK_STRUCTURE_TYPE_BUFFER_OPAQUE_CAPTURE_ADDRESS_CREATE_INFO: break;
        case VK_STRUCTURE_TYPE_BUFFER_USAGE_FLAGS_2_CREATE_INFO: break;
        case VK_STRUCTURE_TYPE_DEDICATED_ALLOCATION_BUFFER_CREATE_INFO_NV: break;
        case VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO:
            external_info = (VkExternalMemoryBufferCreateInfo *)*next;
            external_info->handleTypes = map_external_memory_handle_types( external_info->handleTypes );
            break;
        case VK_STRUCTURE_TYPE_OPAQUE_CAPTURE_DESCRIPTOR_DATA_CREATE_INFO_EXT: break;
        case VK_STRUCTURE_TYPE_VIDEO_PROFILE_LIST_INFO_KHR: break;
        default: FIXME( "Unhandled sType %u.\n", (*next)->sType ); break;
        }
    }

    device->p_vkGetDeviceBufferMemoryRequirements( device->host.device, buffer_requirements, memory_requirements );
}

static void get_physical_device_external_buffer_properties( struct vulkan_physical_device *physical_device, const VkPhysicalDeviceExternalBufferInfo *client_buffer_info,
                                                            VkExternalBufferProperties *buffer_properties, PFN_vkGetPhysicalDeviceExternalBufferProperties p_vkGetPhysicalDeviceExternalBufferProperties )
{
    VkPhysicalDeviceExternalBufferInfo *buffer_info = (VkPhysicalDeviceExternalBufferInfo *)client_buffer_info; /* cast away const, it has been copied in the thunks */
    VkExternalMemoryHandleTypeFlagBits handle_type = 0;

    handle_type = buffer_info->handleType;
    buffer_info->handleType = map_external_memory_handle_types( handle_type );

    p_vkGetPhysicalDeviceExternalBufferProperties( physical_device->host.physical_device, buffer_info, buffer_properties );
    buffer_properties->externalMemoryProperties.compatibleHandleTypes = handle_type;
    buffer_properties->externalMemoryProperties.exportFromImportedHandleTypes = handle_type;
}

static void win32u_vkGetPhysicalDeviceExternalBufferProperties( VkPhysicalDevice client_physical_device, const VkPhysicalDeviceExternalBufferInfo *buffer_info,
                                                                VkExternalBufferProperties *buffer_properties )
{
    struct vulkan_physical_device *physical_device = vulkan_physical_device_from_handle( client_physical_device );
    struct vulkan_instance *instance = physical_device->instance;

    TRACE( "physical_device %p, buffer_info %p, buffer_properties %p\n", physical_device, buffer_info, buffer_properties );

    get_physical_device_external_buffer_properties( physical_device, buffer_info, buffer_properties, instance->p_vkGetPhysicalDeviceExternalBufferProperties );
}

static void win32u_vkGetPhysicalDeviceExternalBufferPropertiesKHR( VkPhysicalDevice client_physical_device, const VkPhysicalDeviceExternalBufferInfo *buffer_info,
                                                                   VkExternalBufferProperties *buffer_properties )
{
    struct vulkan_physical_device *physical_device = vulkan_physical_device_from_handle( client_physical_device );
    struct vulkan_instance *instance = physical_device->instance;

    TRACE( "physical_device %p, buffer_info %p, buffer_properties %p\n", physical_device, buffer_info, buffer_properties );

    get_physical_device_external_buffer_properties( physical_device, buffer_info, buffer_properties, instance->p_vkGetPhysicalDeviceExternalBufferPropertiesKHR );
}

static VkResult win32u_vkCreateImage( VkDevice client_device, const VkImageCreateInfo *create_info,
                                      const VkAllocationCallbacks *allocator, VkImage *image )
{
    VkBaseOutStructure **next, *prev = (VkBaseOutStructure *)create_info; /* cast away const, chain has been copied in the thunks */
    struct vulkan_device *device = vulkan_device_from_handle( client_device );
    struct vulkan_physical_device *physical_device = device->physical_device;
    VkExternalMemoryImageCreateInfo host_external_info, *external_info = NULL;

    for (next = &prev->pNext; *next; prev = *next, next = &(*next)->pNext)
    {
        switch ((*next)->sType)
        {
        case VK_STRUCTURE_TYPE_DEDICATED_ALLOCATION_IMAGE_CREATE_INFO_NV: break;
        case VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO:
            external_info = (VkExternalMemoryImageCreateInfo *)*next;
            external_info->handleTypes = map_external_memory_handle_types( external_info->handleTypes );
            break;
        case VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_EXPLICIT_CREATE_INFO_EXT: break;
        case VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_LIST_CREATE_INFO_EXT: break;
        case VK_STRUCTURE_TYPE_IMAGE_ALIGNMENT_CONTROL_CREATE_INFO_MESA: break;
        case VK_STRUCTURE_TYPE_IMAGE_COMPRESSION_CONTROL_EXT: break;
        case VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO: break;
        case VK_STRUCTURE_TYPE_IMAGE_STENCIL_USAGE_CREATE_INFO: break;
        case VK_STRUCTURE_TYPE_IMAGE_SWAPCHAIN_CREATE_INFO_KHR: break;
        case VK_STRUCTURE_TYPE_OPAQUE_CAPTURE_DESCRIPTOR_DATA_CREATE_INFO_EXT: break;
        case VK_STRUCTURE_TYPE_OPTICAL_FLOW_IMAGE_FORMAT_INFO_NV: break;
        case VK_STRUCTURE_TYPE_VIDEO_PROFILE_LIST_INFO_KHR: break;
        default: FIXME( "Unhandled sType %u.\n", (*next)->sType ); break;
        }
    }

    if (physical_device->external_memory_align && !external_info)
    {
        host_external_info.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO;
        host_external_info.pNext = create_info->pNext;
        host_external_info.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT;
        ((VkImageCreateInfo *)create_info)->pNext = &host_external_info; /* cast away const, it has been copied in the thunks */
    }

    return device->p_vkCreateImage( device->host.device, create_info, NULL, image );
}

static void win32u_vkGetDeviceImageMemoryRequirements( VkDevice client_device, const VkDeviceImageMemoryRequirements *image_requirements,
                                                       VkMemoryRequirements2 *memory_requirements )
{
    VkBaseOutStructure **next, *prev = (VkBaseOutStructure *)image_requirements->pCreateInfo; /* cast away const, chain has been copied in the thunks */
    struct vulkan_device *device = vulkan_device_from_handle( client_device );
    VkExternalMemoryImageCreateInfo *external_info;

    TRACE( "device %p, image_requirements %p, memory_requirements %p\n", device, image_requirements, memory_requirements );

    for (next = &prev->pNext; *next; prev = *next, next = &(*next)->pNext)
    {
        switch ((*next)->sType)
        {
        case VK_STRUCTURE_TYPE_DEDICATED_ALLOCATION_IMAGE_CREATE_INFO_NV: break;
        case VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO:
            external_info = (VkExternalMemoryImageCreateInfo *)*next;
            external_info->handleTypes = map_external_memory_handle_types( external_info->handleTypes );
            break;
        case VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_EXPLICIT_CREATE_INFO_EXT: break;
        case VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_LIST_CREATE_INFO_EXT: break;
        case VK_STRUCTURE_TYPE_IMAGE_ALIGNMENT_CONTROL_CREATE_INFO_MESA: break;
        case VK_STRUCTURE_TYPE_IMAGE_COMPRESSION_CONTROL_EXT: break;
        case VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO: break;
        case VK_STRUCTURE_TYPE_IMAGE_STENCIL_USAGE_CREATE_INFO: break;
        case VK_STRUCTURE_TYPE_IMAGE_SWAPCHAIN_CREATE_INFO_KHR: break;
        case VK_STRUCTURE_TYPE_OPAQUE_CAPTURE_DESCRIPTOR_DATA_CREATE_INFO_EXT: break;
        case VK_STRUCTURE_TYPE_OPTICAL_FLOW_IMAGE_FORMAT_INFO_NV: break;
        case VK_STRUCTURE_TYPE_VIDEO_PROFILE_LIST_INFO_KHR: break;
        default: FIXME( "Unhandled sType %u.\n", (*next)->sType ); break;
        }
    }

    device->p_vkGetDeviceImageMemoryRequirements( device->host.device, image_requirements, memory_requirements );
}

static VkResult get_physical_device_image_format_properties( struct vulkan_physical_device *physical_device, const VkPhysicalDeviceImageFormatInfo2 *format_info,
                                                             VkImageFormatProperties2 *format_properties, PFN_vkGetPhysicalDeviceImageFormatProperties2 p_vkGetPhysicalDeviceImageFormatProperties2 )
{
    VkBaseOutStructure **next, *prev = (VkBaseOutStructure *)format_info; /* cast away const, chain has been copied in the thunks */
    VkExternalMemoryHandleTypeFlagBits handle_type = 0;
    VkResult res;

    TRACE( "physical_device %p, format_info %p, format_properties %p\n", physical_device, format_info, format_properties );

    for (next = &prev->pNext; *next; prev = *next, next = &(*next)->pNext)
    {
        switch ((*next)->sType)
        {
        case VK_STRUCTURE_TYPE_IMAGE_COMPRESSION_CONTROL_EXT: break;
        case VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO: break;
        case VK_STRUCTURE_TYPE_IMAGE_STENCIL_USAGE_CREATE_INFO: break;
        case VK_STRUCTURE_TYPE_OPTICAL_FLOW_IMAGE_FORMAT_INFO_NV: break;
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO:
        {
            VkPhysicalDeviceExternalImageFormatInfo *external_info = (VkPhysicalDeviceExternalImageFormatInfo *)*next;
            handle_type = external_info->handleType;
            external_info->handleType = map_external_memory_handle_types( handle_type );
            break;
        }
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_DRM_FORMAT_MODIFIER_INFO_EXT: break;
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_VIEW_IMAGE_FORMAT_INFO_EXT: break;
        case VK_STRUCTURE_TYPE_VIDEO_PROFILE_LIST_INFO_KHR: break;
        default: FIXME( "Unhandled sType %u.\n", (*next)->sType ); break;
        }
    }

    res = p_vkGetPhysicalDeviceImageFormatProperties2( physical_device->host.physical_device, format_info, format_properties );
    for (prev = (VkBaseOutStructure *)format_properties, next = &prev->pNext; *next; prev = *next, next = &(*next)->pNext)
    {
        switch ((*next)->sType)
        {
        case VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES:
        {
            VkExternalImageFormatProperties *props = (VkExternalImageFormatProperties *)*next;
            props->externalMemoryProperties.compatibleHandleTypes = handle_type;
            props->externalMemoryProperties.exportFromImportedHandleTypes = handle_type;
            break;
        }
        case VK_STRUCTURE_TYPE_FILTER_CUBIC_IMAGE_VIEW_IMAGE_FORMAT_PROPERTIES_EXT: break;
        case VK_STRUCTURE_TYPE_HOST_IMAGE_COPY_DEVICE_PERFORMANCE_QUERY: break;
        case VK_STRUCTURE_TYPE_IMAGE_COMPRESSION_PROPERTIES_EXT: break;
        case VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_IMAGE_FORMAT_PROPERTIES: break;
        case VK_STRUCTURE_TYPE_TEXTURE_LOD_GATHER_FORMAT_PROPERTIES_AMD: break;
        default: FIXME( "Unhandled sType %u.\n", (*next)->sType ); break;
        }
    }

    return res;
}

static VkResult win32u_vkGetPhysicalDeviceImageFormatProperties2( VkPhysicalDevice client_physical_device, const VkPhysicalDeviceImageFormatInfo2 *format_info,
                                                                  VkImageFormatProperties2 *format_properties )
{
    struct vulkan_physical_device *physical_device = vulkan_physical_device_from_handle( client_physical_device );
    struct vulkan_instance *instance = physical_device->instance;

    TRACE( "physical_device %p, format_info %p, format_properties %p\n", physical_device, format_info, format_properties );

    return get_physical_device_image_format_properties( physical_device, format_info, format_properties, instance->p_vkGetPhysicalDeviceImageFormatProperties2 );
}

static VkResult win32u_vkGetPhysicalDeviceImageFormatProperties2KHR( VkPhysicalDevice client_physical_device, const VkPhysicalDeviceImageFormatInfo2 *format_info,
                                                                     VkImageFormatProperties2 *format_properties )
{
    struct vulkan_physical_device *physical_device = vulkan_physical_device_from_handle( client_physical_device );
    struct vulkan_instance *instance = physical_device->instance;

    TRACE( "physical_device %p, format_info %p, format_properties %p\n", physical_device, format_info, format_properties );

    return get_physical_device_image_format_properties( physical_device, format_info, format_properties, instance->p_vkGetPhysicalDeviceImageFormatProperties2KHR );
}

static VkResult win32u_vkCreateWin32SurfaceKHR( VkInstance client_instance, const VkWin32SurfaceCreateInfoKHR *create_info,
                                                const VkAllocationCallbacks *allocator, VkSurfaceKHR *ret )
{
    struct vulkan_instance *instance = vulkan_instance_from_handle( client_instance );
    VkSurfaceKHR host_surface;
    struct surface *surface;
    HWND dummy = NULL;
    VkResult res;

    TRACE( "client_instance %p, create_info %p, allocator %p, ret %p\n", client_instance, create_info, allocator, ret );
    if (allocator) FIXME( "Support for allocation callbacks not implemented yet\n" );

    if (!(surface = calloc( 1, sizeof(*surface) ))) return VK_ERROR_OUT_OF_HOST_MEMORY;
    pthread_mutex_init( &surface->host_lock, NULL );
    pthread_mutex_init( &surface->fullscreen_lock, NULL );
    list_init( &surface->host_surfaces );

    /* Windows allows surfaces to be created with no HWND, they return VK_ERROR_SURFACE_LOST_KHR later */
    if (!(surface->hwnd = create_info->hwnd))
    {
        static const WCHAR staticW[] = {'s','t','a','t','i','c',0};
        UNICODE_STRING static_us = RTL_CONSTANT_STRING( staticW );
        dummy = NtUserCreateWindowEx( 0, &static_us, NULL, &static_us, WS_POPUP, 0, 0, 0, 0,
                                      NULL, NULL, NULL, NULL, 0, NULL, NULL, FALSE );
        WARN( "Created dummy window %p for null surface window\n", dummy );
        surface->hwnd = dummy;
    }

    if ((res = driver_funcs->p_vulkan_surface_create( surface->hwnd, fshack_enabled, instance,
                                                      &host_surface, &surface->client )))
    {
        if (dummy) NtUserDestroyWindow( dummy );
        pthread_mutex_destroy( &surface->fullscreen_lock );
        pthread_mutex_destroy( &surface->host_lock );
        free( surface );
        return res;
    }
    if (!(surface->active_host = surface_host_create( host_surface )))
    {
        instance->p_vkDestroySurfaceKHR( instance->host.instance, host_surface, NULL /* allocator */ );
        client_surface_release( surface->client );
        if (dummy) NtUserDestroyWindow( dummy );
        pthread_mutex_destroy( &surface->fullscreen_lock );
        pthread_mutex_destroy( &surface->host_lock );
        free( surface );
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    list_add_tail( &surface->host_surfaces, &surface->active_host->entry );
    add_window_client_surface( surface->hwnd, surface->client );

    vulkan_object_init( &surface->obj.obj, host_surface );
    surface->obj.instance = instance;
    instance->p_insert_object( instance, &surface->obj.obj );

    if (dummy) NtUserDestroyWindow( dummy );

    *ret = surface->obj.client.surface;
    return VK_SUCCESS;
}

static void win32u_vkDestroySurfaceKHR( VkInstance client_instance, VkSurfaceKHR client_surface,
                                        const VkAllocationCallbacks *allocator )
{
    struct vulkan_instance *instance = vulkan_instance_from_handle( client_instance );
    struct surface *surface = surface_from_handle( client_surface );

    if (!surface) return;

    TRACE( "instance %p, handle 0x%s, allocator %p\n", instance, wine_dbgstr_longlong( client_surface ), allocator );
    if (allocator) FIXME( "Support for allocation callbacks not implemented yet\n" );

    pthread_mutex_lock( &surface->host_lock );
    while (!list_empty( &surface->host_surfaces ))
    {
        struct surface_host *host = LIST_ENTRY( list_head( &surface->host_surfaces ),
                                                 struct surface_host, entry );
        surface_host_destroy( instance, surface, host );
    }
    pthread_mutex_unlock( &surface->host_lock );
    client_surface_release( surface->client );

    instance->p_remove_object( instance, &surface->obj.obj );

    pthread_mutex_destroy( &surface->fullscreen_lock );
    pthread_mutex_destroy( &surface->host_lock );
    free( surface );
}

static BOOL get_surface_rect( HWND hwnd, RECT *rect, UINT dpi )
{
    if (!NtUserGetPresentRect( hwnd, rect, dpi ) && !NtUserGetClientRect( hwnd, rect, dpi )) return FALSE;
    OffsetRect( rect, -rect->left, -rect->top );
    return TRUE;
}

static BOOL get_swapchain_surface_rect( HWND hwnd, RECT *rect, UINT dpi )
{
    if (!get_surface_rect( hwnd, rect, dpi )) return FALSE;
    return !IsRectEmpty( rect );
}

static void adjust_surface_capabilities( struct vulkan_instance *instance, struct surface *surface,
                                         VkSurfaceCapabilitiesKHR *capabilities )
{
    RECT client_rect;

    /* Many Windows games, for example Strange Brigade, No Man's Sky, Path of Exile
     * and World War Z, do not expect that maxImageCount can be set to 0.
     * A value of 0 means that there is no limit on the number of images.
     * Nvidia reports 8 on Windows, AMD 16.
     * https://vulkan.gpuinfo.org/displayreport.php?id=9122#surface
     * https://vulkan.gpuinfo.org/displayreport.php?id=9121#surface
     */
    if (!capabilities->maxImageCount) capabilities->maxImageCount = max( capabilities->minImageCount, 16 );

    /* Update the image extents to match what the Win32 WSI would provide. */
    /* FIXME: handle DPI scaling, somehow */
    get_surface_rect( surface->hwnd, &client_rect, NtUserGetDpiForWindow( surface->hwnd ) );
    capabilities->minImageExtent.width = client_rect.right - client_rect.left;
    capabilities->minImageExtent.height = client_rect.bottom - client_rect.top;
    capabilities->maxImageExtent.width = client_rect.right - client_rect.left;
    capabilities->maxImageExtent.height = client_rect.bottom - client_rect.top;
    capabilities->currentExtent.width = client_rect.right - client_rect.left;
    capabilities->currentExtent.height = client_rect.bottom - client_rect.top;
}

static VkResult win32u_vkGetPhysicalDeviceSurfaceCapabilitiesKHR( VkPhysicalDevice client_physical_device, VkSurfaceKHR client_surface,
                                                                  VkSurfaceCapabilitiesKHR *capabilities )
{
    struct vulkan_physical_device *physical_device = vulkan_physical_device_from_handle( client_physical_device );
    struct surface *surface = surface_from_handle( client_surface );
    struct vulkan_instance *instance = physical_device->instance;
    VkResult res;

    if (!NtUserIsWindow( surface->hwnd )) return VK_ERROR_SURFACE_LOST_KHR;
    pthread_mutex_lock( &surface->host_lock );
    res = instance->p_vkGetPhysicalDeviceSurfaceCapabilitiesKHR( physical_device->host.physical_device,
                                                       surface_host_handle( surface ), capabilities );
    pthread_mutex_unlock( &surface->host_lock );
    if (!res) adjust_surface_capabilities( instance, surface, capabilities );
    return res;
}

static void *find_vk_struct( void *s, VkStructureType t );
static BOOL get_fullscreen_monitor_rect( HMONITOR monitor, RECT *rect );

static BOOL has_non_fullscreen_surface_info( const void *chain )
{
    const VkBaseInStructure *header;

    for (header = chain; header; header = header->pNext)
        if (header->sType != VK_STRUCTURE_TYPE_SURFACE_FULL_SCREEN_EXCLUSIVE_INFO_EXT &&
            header->sType != VK_STRUCTURE_TYPE_SURFACE_FULL_SCREEN_EXCLUSIVE_WIN32_INFO_EXT)
            return TRUE;
    return FALSE;
}

static BOOL get_surface_fullscreen_info( struct surface *surface, const void *chain,
                                         struct vulkan_surface_fullscreen_info *driver_info )
{
    const VkSurfaceFullScreenExclusiveWin32InfoEXT *win32_info;
    const VkSurfaceFullScreenExclusiveInfoEXT *fullscreen_info;
    VkFullScreenExclusiveEXT policy;
    HMONITOR monitor;

    fullscreen_info = find_vk_struct( (void *)chain,
                                      VK_STRUCTURE_TYPE_SURFACE_FULL_SCREEN_EXCLUSIVE_INFO_EXT );
    win32_info = find_vk_struct( (void *)chain,
                                VK_STRUCTURE_TYPE_SURFACE_FULL_SCREEN_EXCLUSIVE_WIN32_INFO_EXT );
    policy = fullscreen_info ? fullscreen_info->fullScreenExclusive
                             : VK_FULL_SCREEN_EXCLUSIVE_DEFAULT_EXT;
    switch (policy)
    {
    case VK_FULL_SCREEN_EXCLUSIVE_DEFAULT_EXT:
    case VK_FULL_SCREEN_EXCLUSIVE_ALLOWED_EXT:
        break;
    case VK_FULL_SCREEN_EXCLUSIVE_APPLICATION_CONTROLLED_EXT:
        if (win32_info) break;
        /* fall through */
    default:
        return FALSE;
    }

    monitor = win32_info ? win32_info->hmonitor :
                           NtUserMonitorFromWindow( surface->hwnd, MONITOR_DEFAULTTONEAREST );
    if (!get_fullscreen_monitor_rect( monitor, &driver_info->rect )) return FALSE;
    driver_info->target = win32_info ? VULKAN_SURFACE_FULLSCREEN_TARGET_FIXED
                                     : VULKAN_SURFACE_FULLSCREEN_TARGET_WINDOW;
    return TRUE;
}

static VkResult win32u_vkGetPhysicalDeviceSurfaceCapabilities2KHR( VkPhysicalDevice client_physical_device, const VkPhysicalDeviceSurfaceInfo2KHR *surface_info,
                                                                   VkSurfaceCapabilities2KHR *capabilities )
{
    struct vulkan_physical_device *physical_device = vulkan_physical_device_from_handle( client_physical_device );
    struct surface *surface = surface_from_handle( surface_info->surface );
    VkPhysicalDeviceSurfaceInfo2KHR surface_info_host = *surface_info;
    VkSurfaceCapabilitiesFullScreenExclusiveEXT *fullscreen_capabilities;
    struct vulkan_surface_fullscreen_info fullscreen_info_driver;
    struct vulkan_instance *instance = physical_device->instance;
    VkBool32 fullscreen_supported = VK_FALSE;
    VkResult res;

    fullscreen_capabilities = (VkSurfaceCapabilitiesFullScreenExclusiveEXT *)
        find_vk_struct( capabilities->pNext,
                        VK_STRUCTURE_TYPE_SURFACE_CAPABILITIES_FULL_SCREEN_EXCLUSIVE_EXT );

    if (!surface || !NtUserIsWindow( surface->hwnd )) return VK_ERROR_SURFACE_LOST_KHR;
    if (fullscreen_capabilities && surface->client &&
        driver_funcs->p_vulkan_surface_fullscreen_supported &&
        get_surface_fullscreen_info( surface, surface_info->pNext, &fullscreen_info_driver ))
        fullscreen_supported = driver_funcs->p_vulkan_surface_fullscreen_supported(
            surface->client, &fullscreen_info_driver );

    if (!instance->p_vkGetPhysicalDeviceSurfaceCapabilities2KHR)
    {
        /* Until the loader version exporting this function is common, emulate it using the older non-2 version. */
        VkBaseOutStructure *header;

        for (header = capabilities->pNext; header; header = header->pNext)
            if (header->sType != VK_STRUCTURE_TYPE_SURFACE_CAPABILITIES_FULL_SCREEN_EXCLUSIVE_EXT)
                break;
        if (has_non_fullscreen_surface_info( surface_info_host.pNext ) || header)
            FIXME( "Emulating vkGetPhysicalDeviceSurfaceCapabilities2KHR, ignoring pNext.\n" );
        res = win32u_vkGetPhysicalDeviceSurfaceCapabilitiesKHR( client_physical_device,
                                                                 surface_info->surface,
                                                                 &capabilities->surfaceCapabilities );
        goto done;
    }

    pthread_mutex_lock( &surface->host_lock );
    surface_info_host.surface = surface_host_handle( surface );
    res = instance->p_vkGetPhysicalDeviceSurfaceCapabilities2KHR( physical_device->host.physical_device,
                                                                     &surface_info_host, capabilities );
    pthread_mutex_unlock( &surface->host_lock );
    if (!res) adjust_surface_capabilities( instance, surface, &capabilities->surfaceCapabilities );

done:
    if (!res && fullscreen_capabilities)
        fullscreen_capabilities->fullScreenExclusiveSupported = fullscreen_supported;
    return res;
}

static VkResult win32u_vkGetPhysicalDevicePresentRectanglesKHR( VkPhysicalDevice client_physical_device, VkSurfaceKHR client_surface,
                                                                uint32_t *rect_count, VkRect2D *rects )
{
    struct vulkan_physical_device *physical_device = vulkan_physical_device_from_handle( client_physical_device );
    struct surface *surface = surface_from_handle( client_surface );
    struct vulkan_instance *instance = physical_device->instance;
    VkResult res;

    if (!NtUserIsWindow( surface->hwnd ))
    {
        if (rects && !*rect_count) return VK_INCOMPLETE;
        if (rects) memset( rects, 0, sizeof(VkRect2D) );
        *rect_count = 1;
        return VK_SUCCESS;
    }

    pthread_mutex_lock( &surface->host_lock );
    res = instance->p_vkGetPhysicalDevicePresentRectanglesKHR( physical_device->host.physical_device,
                                                               surface_host_handle( surface ), rect_count, rects );
    pthread_mutex_unlock( &surface->host_lock );
    return res;
}

static void *find_vk_struct( void *s, VkStructureType t )
{
    VkBaseOutStructure *header;

    for (header = s; header; header = header->pNext)
    {
        if (header->sType == t) return header;
    }

    return NULL;
}

static BOOL get_fullscreen_monitor_rect( HMONITOR monitor, RECT *rect )
{
    MONITORINFO info = {.cbSize = sizeof(info)};

    if (!monitor || !NtUserCallTwoParam( HandleToUlong(monitor), (ULONG_PTR)&info,
                                         NtUserCallTwoParam_GetMonitorInfo ))
        return FALSE;

    *rect = map_rect_virt_to_raw( info.rcMonitor, get_thread_dpi() );
    return !IsRectEmpty( rect );
}

static BOOL get_automatic_fullscreen_rect( struct vulkan_device *device,
                                           struct surface *surface,
                                           VkFullScreenExclusiveEXT policy,
                                           const VkSurfaceFullScreenExclusiveWin32InfoEXT *win32_info,
                                           RECT *rect )
{
    DWORD style;
    HMONITOR monitor;

    if (!device->full_screen_exclusive_enabled ||
        (policy != VK_FULL_SCREEN_EXCLUSIVE_DEFAULT_EXT &&
         policy != VK_FULL_SCREEN_EXCLUSIVE_ALLOWED_EXT))
        return FALSE;

    style = NtUserGetWindowLongW( surface->hwnd, GWL_STYLE );
    if (NtUserGetAncestor( surface->hwnd, GA_ROOT ) != surface->hwnd ||
        (style & (WS_POPUP | WS_CHILD | WS_CAPTION | WS_THICKFRAME)) != WS_POPUP)
        return FALSE;

    monitor = win32_info ? win32_info->hmonitor :
                           NtUserMonitorFromWindow( surface->hwnd, MONITOR_DEFAULTTONEAREST );
    return get_fullscreen_monitor_rect( monitor, rect );
}

static void fixup_device_id_vulkan( UINT *vendor_id, UINT *device_id )
{
    struct pci_id id_real;
    const struct pci_id *id = &id_real;

    id_real.vendor = *vendor_id;
    id_real.device = *device_id;
    fixup_device_id( &id );
    *vendor_id = id->vendor;
    *device_id = id->device;
}

static void get_physical_device_properties2( struct vulkan_physical_device *physical_device, VkPhysicalDeviceProperties2 *properties2,
                                             PFN_vkGetPhysicalDeviceProperties2 p_vkGetPhysicalDeviceProperties2 )
{
    VkPhysicalDeviceIDProperties id_host = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES };
    VkPhysicalDeviceProperties2 properties2_host;
    VkPhysicalDeviceVulkan11Properties *vk11;
    VkPhysicalDeviceIDProperties *id;
    VkBool32 device_luid_valid;
    UINT32 node_mask = 0;
    const GUID *uuid;
    LUID luid;

    vk11 = find_vk_struct( properties2, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_PROPERTIES );
    id = find_vk_struct( properties2, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES );

    if (!vk11 && !id)
    {
        properties2_host = *properties2;
        id_host.pNext = properties2->pNext;
        properties2_host.pNext = &id_host;
        p_vkGetPhysicalDeviceProperties2( physical_device->host.physical_device, &properties2_host );
        properties2->properties = properties2_host.properties;
    }
    else p_vkGetPhysicalDeviceProperties2( physical_device->host.physical_device, properties2 );

    if (id)        uuid = (const GUID *)id->deviceUUID;
    else if (vk11) uuid = (const GUID *)vk11->deviceUUID;
    else           uuid = (const GUID *)id_host.deviceUUID;

    device_luid_valid = get_gpu_info_from_uuid( uuid, &luid, &node_mask, properties2->properties.deviceName );
    if (!device_luid_valid) WARN( "luid for %s not found\n", debugstr_guid(uuid) );

    if (id)
    {
        if (device_luid_valid) memcpy( &id->deviceLUID, &luid, sizeof(id->deviceLUID) );
        id->deviceLUIDValid = device_luid_valid;
        id->deviceNodeMask = node_mask;
    }

    if (vk11)
    {
        if (device_luid_valid) memcpy( &vk11->deviceLUID, &luid, sizeof(vk11->deviceLUID) );
        vk11->deviceLUIDValid = device_luid_valid;
        vk11->deviceNodeMask = node_mask;
    }

    fixup_device_id_vulkan( &properties2->properties.vendorID, &properties2->properties.deviceID );

    TRACE( "deviceName:%s deviceLUIDValid:%d LUID:%08x:%08x.\n",
           properties2->properties.deviceName, device_luid_valid, luid.HighPart, luid.LowPart );
}

static void win32u_vkGetPhysicalDeviceProperties( VkPhysicalDevice client_physical_device, VkPhysicalDeviceProperties *properties )
{
    struct vulkan_physical_device *physical_device = vulkan_physical_device_from_handle( client_physical_device );
    VkPhysicalDeviceProperties2 properties2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2 };

    if (!physical_device->instance->extensions.has_VK_KHR_get_physical_device_properties2)
    {
        physical_device->instance->p_vkGetPhysicalDeviceProperties( physical_device->host.physical_device, properties );
        return;
    }
    get_physical_device_properties2( physical_device, &properties2,
                                     physical_device->instance->p_vkGetPhysicalDeviceProperties2KHR );
    *properties = properties2.properties;
}

static void win32u_vkGetPhysicalDeviceProperties2( VkPhysicalDevice client_physical_device, VkPhysicalDeviceProperties2 *properties2 )
{
    struct vulkan_physical_device *physical_device = vulkan_physical_device_from_handle( client_physical_device );

    get_physical_device_properties2( physical_device, properties2, physical_device->instance->p_vkGetPhysicalDeviceProperties2 );
}

static void win32u_vkGetPhysicalDeviceProperties2KHR( VkPhysicalDevice client_physical_device, VkPhysicalDeviceProperties2 *properties2 )
{
    struct vulkan_physical_device *physical_device = vulkan_physical_device_from_handle( client_physical_device );

    get_physical_device_properties2( physical_device, properties2, physical_device->instance->p_vkGetPhysicalDeviceProperties2KHR );
}

static VkResult win32u_vkGetPhysicalDeviceSurfaceFormatsKHR( VkPhysicalDevice client_physical_device, VkSurfaceKHR client_surface,
                                                             uint32_t *format_count, VkSurfaceFormatKHR *formats )
{
    struct vulkan_physical_device *physical_device = vulkan_physical_device_from_handle( client_physical_device );
    struct surface *surface = surface_from_handle( client_surface );
    struct vulkan_instance *instance = physical_device->instance;
    VkResult res;

    pthread_mutex_lock( &surface->host_lock );
    res = instance->p_vkGetPhysicalDeviceSurfaceFormatsKHR( physical_device->host.physical_device,
                                                            surface_host_handle( surface ), format_count, formats );
    pthread_mutex_unlock( &surface->host_lock );
    return res;
}

static VkResult win32u_vkGetPhysicalDeviceSurfaceFormats2KHR( VkPhysicalDevice client_physical_device, const VkPhysicalDeviceSurfaceInfo2KHR *surface_info,
                                                              uint32_t *format_count, VkSurfaceFormat2KHR *formats )
{
    struct vulkan_physical_device *physical_device = vulkan_physical_device_from_handle( client_physical_device );
    struct surface *surface = surface_from_handle( surface_info->surface );
    VkPhysicalDeviceSurfaceInfo2KHR surface_info_host = *surface_info;
    struct vulkan_instance *instance = physical_device->instance;
    VkResult res;

    if (!instance->p_vkGetPhysicalDeviceSurfaceFormats2KHR)
    {
        VkSurfaceFormatKHR *surface_formats;
        UINT i;

        /* Until the loader version exporting this function is common, emulate it using the older non-2 version. */
        if (has_non_fullscreen_surface_info( surface_info_host.pNext ))
            FIXME( "Emulating vkGetPhysicalDeviceSurfaceFormats2KHR, ignoring pNext.\n" );
        if (!formats) return win32u_vkGetPhysicalDeviceSurfaceFormatsKHR( client_physical_device, surface_info->surface, format_count, NULL );

        surface_formats = calloc( *format_count, sizeof(*surface_formats) );
        if (!surface_formats) return VK_ERROR_OUT_OF_HOST_MEMORY;

        res = win32u_vkGetPhysicalDeviceSurfaceFormatsKHR( client_physical_device, surface_info->surface, format_count, surface_formats );
        if (!res || res == VK_INCOMPLETE) for (i = 0; i < *format_count; i++) formats[i].surfaceFormat = surface_formats[i];

        free( surface_formats );
        return res;
    }

    pthread_mutex_lock( &surface->host_lock );
    surface_info_host.surface = surface_host_handle( surface );
    res = instance->p_vkGetPhysicalDeviceSurfaceFormats2KHR( physical_device->host.physical_device,
                                                             &surface_info_host, format_count, formats );
    pthread_mutex_unlock( &surface->host_lock );
    return res;
}

static VkResult win32u_vkGetPhysicalDeviceSurfacePresentModes2EXT(
        VkPhysicalDevice client_physical_device,
        const VkPhysicalDeviceSurfaceInfo2KHR *surface_info,
        uint32_t *present_mode_count, VkPresentModeKHR *present_modes )
{
    struct vulkan_physical_device *physical_device =
        vulkan_physical_device_from_handle( client_physical_device );
    struct surface *surface = surface_from_handle( surface_info->surface );
    struct vulkan_instance *instance = physical_device->instance;
    VkResult res;

    /* Emulated fullscreen does not change host presentation modes. */
    if (!surface || !NtUserIsWindow( surface->hwnd )) return VK_ERROR_SURFACE_LOST_KHR;
    if (has_non_fullscreen_surface_info( surface_info->pNext ))
        FIXME( "Emulating vkGetPhysicalDeviceSurfacePresentModes2EXT, ignoring pNext.\n" );

    pthread_mutex_lock( &surface->host_lock );
    res = instance->p_vkGetPhysicalDeviceSurfacePresentModesKHR(
        physical_device->host.physical_device, surface_host_handle( surface ),
        present_mode_count, present_modes );
    pthread_mutex_unlock( &surface->host_lock );
    return res;
}

static VkResult win32u_vkGetDeviceGroupSurfacePresentModes2EXT(
        VkDevice client_device, const VkPhysicalDeviceSurfaceInfo2KHR *surface_info,
        VkDeviceGroupPresentModeFlagsKHR *modes )
{
    struct vulkan_device *device = vulkan_device_from_handle( client_device );
    struct surface *surface = surface_from_handle( surface_info->surface );
    VkResult res;

    if (!surface || !NtUserIsWindow( surface->hwnd )) return VK_ERROR_SURFACE_LOST_KHR;
    if (has_non_fullscreen_surface_info( surface_info->pNext ))
        FIXME( "Emulating vkGetDeviceGroupSurfacePresentModes2EXT, ignoring pNext.\n" );

    if (!device->p_vkGetDeviceGroupSurfacePresentModesKHR)
    {
        *modes = VK_DEVICE_GROUP_PRESENT_MODE_LOCAL_BIT_KHR;
        return VK_SUCCESS;
    }

    pthread_mutex_lock( &surface->host_lock );
    res = device->p_vkGetDeviceGroupSurfacePresentModesKHR(
        device->host.device, surface_host_handle( surface ), modes );
    pthread_mutex_unlock( &surface->host_lock );
    return res;
}

static VkBool32 win32u_vkGetPhysicalDeviceWin32PresentationSupportKHR( VkPhysicalDevice client_physical_device, uint32_t queue )
{
    struct vulkan_physical_device *physical_device = vulkan_physical_device_from_handle( client_physical_device );
    return driver_funcs->p_get_physical_device_presentation_support( physical_device, queue );
}

static BOOL extents_equals( const VkExtent2D *extents, const RECT *rect )
{
    return extents->width == rect->right - rect->left && extents->height == rect->bottom - rect->top;
}

/*
#version 460

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

layout(binding = 0) uniform sampler2D texSampler;
layout(binding = 1) uniform writeonly image2D outImage;
layout(push_constant) uniform pushConstants {
    //both in real image coords
    vec2 offset;
    vec2 extents;
} constants;

void main()
{
    vec2 texcoord = (vec2(gl_GlobalInvocationID.xy) - constants.offset) / constants.extents;
    vec4 c = texture(texSampler, texcoord);

    // Convert linear -> srgb
    bvec3 isLo = lessThanEqual(c.rgb, vec3(0.0031308f));
    vec3 loPart = c.rgb * 12.92f;
    vec3 hiPart = pow(c.rgb, vec3(5.0f / 12.0f)) * 1.055f - 0.055f;
    c.rgb = mix(hiPart, loPart, isLo);

    imageStore(outImage, ivec2(gl_GlobalInvocationID.xy), c);
}

*/
const uint32_t blit_comp_spv[] =
{
    0x07230203, 0x00010000, 0x0008000a, 0x0000005e, 0x00000000, 0x00020011, 0x00000001, 0x00020011,
    0x00000038, 0x0006000b, 0x00000001, 0x4c534c47, 0x6474732e, 0x3035342e, 0x00000000, 0x0003000e,
    0x00000000, 0x00000001, 0x0006000f, 0x00000005, 0x00000004, 0x6e69616d, 0x00000000, 0x0000000d,
    0x00060010, 0x00000004, 0x00000011, 0x00000008, 0x00000008, 0x00000001, 0x00030003, 0x00000002,
    0x000001cc, 0x00040005, 0x00000004, 0x6e69616d, 0x00000000, 0x00050005, 0x00000009, 0x63786574,
    0x64726f6f, 0x00000000, 0x00080005, 0x0000000d, 0x475f6c67, 0x61626f6c, 0x766e496c, 0x7461636f,
    0x496e6f69, 0x00000044, 0x00060005, 0x00000012, 0x68737570, 0x736e6f43, 0x746e6174, 0x00000073,
    0x00050006, 0x00000012, 0x00000000, 0x7366666f, 0x00007465, 0x00050006, 0x00000012, 0x00000001,
    0x65747865, 0x0073746e, 0x00050005, 0x00000014, 0x736e6f63, 0x746e6174, 0x00000073, 0x00030005,
    0x00000021, 0x00000063, 0x00050005, 0x00000025, 0x53786574, 0x6c706d61, 0x00007265, 0x00040005,
    0x0000002d, 0x6f4c7369, 0x00000000, 0x00040005, 0x00000035, 0x61506f6c, 0x00007472, 0x00040005,
    0x0000003a, 0x61506968, 0x00007472, 0x00050005, 0x00000055, 0x4974756f, 0x6567616d, 0x00000000,
    0x00040047, 0x0000000d, 0x0000000b, 0x0000001c, 0x00050048, 0x00000012, 0x00000000, 0x00000023,
    0x00000000, 0x00050048, 0x00000012, 0x00000001, 0x00000023, 0x00000008, 0x00030047, 0x00000012,
    0x00000002, 0x00040047, 0x00000025, 0x00000022, 0x00000000, 0x00040047, 0x00000025, 0x00000021,
    0x00000000, 0x00040047, 0x00000055, 0x00000022, 0x00000000, 0x00040047, 0x00000055, 0x00000021,
    0x00000001, 0x00030047, 0x00000055, 0x00000019, 0x00040047, 0x0000005d, 0x0000000b, 0x00000019,
    0x00020013, 0x00000002, 0x00030021, 0x00000003, 0x00000002, 0x00030016, 0x00000006, 0x00000020,
    0x00040017, 0x00000007, 0x00000006, 0x00000002, 0x00040020, 0x00000008, 0x00000007, 0x00000007,
    0x00040015, 0x0000000a, 0x00000020, 0x00000000, 0x00040017, 0x0000000b, 0x0000000a, 0x00000003,
    0x00040020, 0x0000000c, 0x00000001, 0x0000000b, 0x0004003b, 0x0000000c, 0x0000000d, 0x00000001,
    0x00040017, 0x0000000e, 0x0000000a, 0x00000002, 0x0004001e, 0x00000012, 0x00000007, 0x00000007,
    0x00040020, 0x00000013, 0x00000009, 0x00000012, 0x0004003b, 0x00000013, 0x00000014, 0x00000009,
    0x00040015, 0x00000015, 0x00000020, 0x00000001, 0x0004002b, 0x00000015, 0x00000016, 0x00000000,
    0x00040020, 0x00000017, 0x00000009, 0x00000007, 0x0004002b, 0x00000015, 0x0000001b, 0x00000001,
    0x00040017, 0x0000001f, 0x00000006, 0x00000004, 0x00040020, 0x00000020, 0x00000007, 0x0000001f,
    0x00090019, 0x00000022, 0x00000006, 0x00000001, 0x00000000, 0x00000000, 0x00000000, 0x00000001,
    0x00000000, 0x0003001b, 0x00000023, 0x00000022, 0x00040020, 0x00000024, 0x00000000, 0x00000023,
    0x0004003b, 0x00000024, 0x00000025, 0x00000000, 0x0004002b, 0x00000006, 0x00000028, 0x00000000,
    0x00020014, 0x0000002a, 0x00040017, 0x0000002b, 0x0000002a, 0x00000003, 0x00040020, 0x0000002c,
    0x00000007, 0x0000002b, 0x00040017, 0x0000002e, 0x00000006, 0x00000003, 0x0004002b, 0x00000006,
    0x00000031, 0x3b4d2e1c, 0x0006002c, 0x0000002e, 0x00000032, 0x00000031, 0x00000031, 0x00000031,
    0x00040020, 0x00000034, 0x00000007, 0x0000002e, 0x0004002b, 0x00000006, 0x00000038, 0x414eb852,
    0x0004002b, 0x00000006, 0x0000003d, 0x3ed55555, 0x0006002c, 0x0000002e, 0x0000003e, 0x0000003d,
    0x0000003d, 0x0000003d, 0x0004002b, 0x00000006, 0x00000040, 0x3f870a3d, 0x0004002b, 0x00000006,
    0x00000042, 0x3d6147ae, 0x0004002b, 0x0000000a, 0x00000049, 0x00000000, 0x00040020, 0x0000004a,
    0x00000007, 0x00000006, 0x0004002b, 0x0000000a, 0x0000004d, 0x00000001, 0x0004002b, 0x0000000a,
    0x00000050, 0x00000002, 0x00090019, 0x00000053, 0x00000006, 0x00000001, 0x00000000, 0x00000000,
    0x00000000, 0x00000002, 0x00000000, 0x00040020, 0x00000054, 0x00000000, 0x00000053, 0x0004003b,
    0x00000054, 0x00000055, 0x00000000, 0x00040017, 0x00000059, 0x00000015, 0x00000002, 0x0004002b,
    0x0000000a, 0x0000005c, 0x00000008, 0x0006002c, 0x0000000b, 0x0000005d, 0x0000005c, 0x0000005c,
    0x0000004d, 0x00050036, 0x00000002, 0x00000004, 0x00000000, 0x00000003, 0x000200f8, 0x00000005,
    0x0004003b, 0x00000008, 0x00000009, 0x00000007, 0x0004003b, 0x00000020, 0x00000021, 0x00000007,
    0x0004003b, 0x0000002c, 0x0000002d, 0x00000007, 0x0004003b, 0x00000034, 0x00000035, 0x00000007,
    0x0004003b, 0x00000034, 0x0000003a, 0x00000007, 0x0004003d, 0x0000000b, 0x0000000f, 0x0000000d,
    0x0007004f, 0x0000000e, 0x00000010, 0x0000000f, 0x0000000f, 0x00000000, 0x00000001, 0x00040070,
    0x00000007, 0x00000011, 0x00000010, 0x00050041, 0x00000017, 0x00000018, 0x00000014, 0x00000016,
    0x0004003d, 0x00000007, 0x00000019, 0x00000018, 0x00050083, 0x00000007, 0x0000001a, 0x00000011,
    0x00000019, 0x00050041, 0x00000017, 0x0000001c, 0x00000014, 0x0000001b, 0x0004003d, 0x00000007,
    0x0000001d, 0x0000001c, 0x00050088, 0x00000007, 0x0000001e, 0x0000001a, 0x0000001d, 0x0003003e,
    0x00000009, 0x0000001e, 0x0004003d, 0x00000023, 0x00000026, 0x00000025, 0x0004003d, 0x00000007,
    0x00000027, 0x00000009, 0x00070058, 0x0000001f, 0x00000029, 0x00000026, 0x00000027, 0x00000002,
    0x00000028, 0x0003003e, 0x00000021, 0x00000029, 0x0004003d, 0x0000001f, 0x0000002f, 0x00000021,
    0x0008004f, 0x0000002e, 0x00000030, 0x0000002f, 0x0000002f, 0x00000000, 0x00000001, 0x00000002,
    0x000500bc, 0x0000002b, 0x00000033, 0x00000030, 0x00000032, 0x0003003e, 0x0000002d, 0x00000033,
    0x0004003d, 0x0000001f, 0x00000036, 0x00000021, 0x0008004f, 0x0000002e, 0x00000037, 0x00000036,
    0x00000036, 0x00000000, 0x00000001, 0x00000002, 0x0005008e, 0x0000002e, 0x00000039, 0x00000037,
    0x00000038, 0x0003003e, 0x00000035, 0x00000039, 0x0004003d, 0x0000001f, 0x0000003b, 0x00000021,
    0x0008004f, 0x0000002e, 0x0000003c, 0x0000003b, 0x0000003b, 0x00000000, 0x00000001, 0x00000002,
    0x0007000c, 0x0000002e, 0x0000003f, 0x00000001, 0x0000001a, 0x0000003c, 0x0000003e, 0x0005008e,
    0x0000002e, 0x00000041, 0x0000003f, 0x00000040, 0x00060050, 0x0000002e, 0x00000043, 0x00000042,
    0x00000042, 0x00000042, 0x00050083, 0x0000002e, 0x00000044, 0x00000041, 0x00000043, 0x0003003e,
    0x0000003a, 0x00000044, 0x0004003d, 0x0000002e, 0x00000045, 0x0000003a, 0x0004003d, 0x0000002e,
    0x00000046, 0x00000035, 0x0004003d, 0x0000002b, 0x00000047, 0x0000002d, 0x000600a9, 0x0000002e,
    0x00000048, 0x00000047, 0x00000046, 0x00000045, 0x00050041, 0x0000004a, 0x0000004b, 0x00000021,
    0x00000049, 0x00050051, 0x00000006, 0x0000004c, 0x00000048, 0x00000000, 0x0003003e, 0x0000004b,
    0x0000004c, 0x00050041, 0x0000004a, 0x0000004e, 0x00000021, 0x0000004d, 0x00050051, 0x00000006,
    0x0000004f, 0x00000048, 0x00000001, 0x0003003e, 0x0000004e, 0x0000004f, 0x00050041, 0x0000004a,
    0x00000051, 0x00000021, 0x00000050, 0x00050051, 0x00000006, 0x00000052, 0x00000048, 0x00000002,
    0x0003003e, 0x00000051, 0x00000052, 0x0004003d, 0x00000053, 0x00000056, 0x00000055, 0x0004003d,
    0x0000000b, 0x00000057, 0x0000000d, 0x0007004f, 0x0000000e, 0x00000058, 0x00000057, 0x00000057,
    0x00000000, 0x00000001, 0x0004007c, 0x00000059, 0x0000005a, 0x00000058, 0x0004003d, 0x0000001f,
    0x0000005b, 0x00000021, 0x00040063, 0x00000056, 0x0000005a, 0x0000005b, 0x000100fd, 0x00010038,
};

static void destroy_pipeline(struct vulkan_device *device, struct fs_comp_pipeline *pipeline)
{
    device->p_vkDestroyPipeline(device->host.device, pipeline->pipeline, NULL);
    pipeline->pipeline = VK_NULL_HANDLE;

    device->p_vkDestroyPipelineLayout(device->host.device, pipeline->pipeline_layout, NULL);
    pipeline->pipeline_layout = VK_NULL_HANDLE;
}

static VkResult create_pipeline( struct vulkan_device *device, struct swapchain *swapchain, const uint32_t *code, uint32_t code_size, uint32_t push_size, struct fs_comp_pipeline *pipeline )
{
    VkComputePipelineCreateInfo pipelineInfo = {0};
    VkPipelineLayoutCreateInfo pipelineLayoutInfo = {0};
    VkShaderModuleCreateInfo shaderInfo = {0};
    VkPushConstantRange pushConstants;
    VkShaderModule shaderModule = 0;
    VkResult res;

    pipeline->push_size = push_size;

    pushConstants.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    pushConstants.offset = 0;
    pushConstants.size = push_size;

    pipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pipelineLayoutInfo.setLayoutCount = 1;
    pipelineLayoutInfo.pSetLayouts = &swapchain->descriptor_set_layout;
    pipelineLayoutInfo.pushConstantRangeCount = 1;
    pipelineLayoutInfo.pPushConstantRanges = &pushConstants;

    if ((res = device->p_vkCreatePipelineLayout(device->host.device, &pipelineLayoutInfo, NULL, &pipeline->pipeline_layout)))
    {
        ERR("vkCreatePipelineLayout: %d\n", res);
        goto fail;
    }

    shaderInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    shaderInfo.codeSize = code_size;
    shaderInfo.pCode = code;

    if ((res = device->p_vkCreateShaderModule(device->host.device, &shaderInfo, NULL, &shaderModule)))
    {
        ERR("vkCreateShaderModule: %d\n", res);
        goto fail;
    }

    pipelineInfo.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    pipelineInfo.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    pipelineInfo.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    pipelineInfo.stage.module = shaderModule;
    pipelineInfo.stage.pName = "main";
    pipelineInfo.layout = pipeline->pipeline_layout;
    pipelineInfo.basePipelineHandle = VK_NULL_HANDLE;
    pipelineInfo.basePipelineIndex = -1;

    if ((res = device->p_vkCreateComputePipelines( device->host.device, VK_NULL_HANDLE, 1,
                                                   &pipelineInfo, NULL, &pipeline->pipeline )))
    {
        ERR( "vkCreateComputePipelines: %d\n", res );
        goto fail;
    }
    else goto out;

fail:
    destroy_pipeline(device, pipeline);

out:
    device->p_vkDestroyShaderModule(device->host.device, shaderModule, NULL);
    return res;
}

static VkResult create_descriptor_set( struct vulkan_device *device, struct swapchain *swapchain, struct fs_hack_image *hack )
{
    VkDescriptorImageInfo userDescriptorImageInfo = {0}, realDescriptorImageInfo = {0};
    VkDescriptorSetAllocateInfo descriptorAllocInfo = {0};
    VkWriteDescriptorSet descriptorWrites[2] = {{0}, {0}};
    VkResult res;

    descriptorAllocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    descriptorAllocInfo.descriptorPool = swapchain->descriptor_pool;
    descriptorAllocInfo.descriptorSetCount = 1;
    descriptorAllocInfo.pSetLayouts = &swapchain->descriptor_set_layout;

    if ((res = device->p_vkAllocateDescriptorSets( device->host.device, &descriptorAllocInfo, &hack->descriptor_set )))
    {
        ERR( "vkAllocateDescriptorSets: %d\n", res );
        return res;
    }

    userDescriptorImageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    userDescriptorImageInfo.imageView = hack->user_view;
    userDescriptorImageInfo.sampler = swapchain->sampler;

    realDescriptorImageInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    realDescriptorImageInfo.imageView = swapchain->upscaler.is_fsr ? hack->fsr_view : hack->swapchain_view;

    descriptorWrites[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    descriptorWrites[0].dstSet = hack->descriptor_set;
    descriptorWrites[0].dstBinding = 0;
    descriptorWrites[0].dstArrayElement = 0;
    descriptorWrites[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    descriptorWrites[0].descriptorCount = 1;
    descriptorWrites[0].pImageInfo = &userDescriptorImageInfo;

    descriptorWrites[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    descriptorWrites[1].dstSet = hack->descriptor_set;
    descriptorWrites[1].dstBinding = 1;
    descriptorWrites[1].dstArrayElement = 0;
    descriptorWrites[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    descriptorWrites[1].descriptorCount = 1;
    descriptorWrites[1].pImageInfo = &realDescriptorImageInfo;

    device->p_vkUpdateDescriptorSets( device->host.device, 2, descriptorWrites, 0, NULL );

    if (swapchain->upscaler.is_fsr)
    {
        if ((res = device->p_vkAllocateDescriptorSets(device->host.device, &descriptorAllocInfo, &hack->fsr_set)))
        {
            ERR("vkAllocateDescriptorSets: %d\n", res);
            return res;
        }

        userDescriptorImageInfo.imageView = hack->fsr_view;

        realDescriptorImageInfo.imageView = hack->swapchain_view;

        descriptorWrites[0].dstSet = hack->fsr_set;
        descriptorWrites[1].dstSet = hack->fsr_set;

        device->p_vkUpdateDescriptorSets(device->host.device, 2, descriptorWrites, 0, NULL);
    }

    return VK_SUCCESS;
}

static VkFormat srgb_to_unorm(VkFormat format)
{
    switch (format)
    {
        case VK_FORMAT_R8G8B8A8_SRGB: return VK_FORMAT_R8G8B8A8_UNORM;
        case VK_FORMAT_B8G8R8A8_SRGB: return VK_FORMAT_B8G8R8A8_UNORM;
        case VK_FORMAT_R8G8B8_SRGB: return VK_FORMAT_R8G8B8_UNORM;
        case VK_FORMAT_B8G8R8_SRGB: return VK_FORMAT_B8G8R8_UNORM;
        case VK_FORMAT_A8B8G8R8_SRGB_PACK32: return VK_FORMAT_A8B8G8R8_UNORM_PACK32;
        default: return format;
    }
}

static VkFormat unorm_to_srgb(VkFormat format)
{
    switch (format)
    {
        case VK_FORMAT_R8G8B8A8_UNORM: return VK_FORMAT_R8G8B8A8_SRGB;
        case VK_FORMAT_B8G8R8A8_UNORM: return VK_FORMAT_B8G8R8A8_SRGB;
        case VK_FORMAT_R8G8B8_UNORM: return VK_FORMAT_R8G8B8_SRGB;
        case VK_FORMAT_B8G8R8_UNORM: return VK_FORMAT_B8G8R8_SRGB;
        case VK_FORMAT_A8B8G8R8_UNORM_PACK32: return VK_FORMAT_A8B8G8R8_SRGB_PACK32;
        default: return format;
    }
}

static enum fs_hack_color_mode fs_hack_color_mode_from_colorspace( VkColorSpaceKHR colorspace )
{
    switch (colorspace)
    {
    case VK_COLOR_SPACE_SRGB_NONLINEAR_KHR:
        return FS_HACK_COLOR_SRGB;
    case VK_COLOR_SPACE_DISPLAY_P3_NONLINEAR_EXT:
    case VK_COLOR_SPACE_EXTENDED_SRGB_NONLINEAR_EXT:
        return FS_HACK_COLOR_EXTENDED_SRGB;
    case VK_COLOR_SPACE_HDR10_ST2084_EXT:
        return FS_HACK_COLOR_PQ;
    case VK_COLOR_SPACE_HDR10_HLG_EXT:
        return FS_HACK_COLOR_HLG;
    default:
        return FS_HACK_COLOR_RAW;
    }
}

static BOOL fs_hack_format_supports( struct vulkan_physical_device *physical_device,
                                     VkFormat format, VkFormatFeatureFlags features )
{
    struct vulkan_instance *instance = physical_device->instance;
    PFN_vkGetPhysicalDeviceFormatProperties p_get_format_props;
    VkFormatProperties props;

    p_get_format_props = (PFN_vkGetPhysicalDeviceFormatProperties)
        p_vkGetInstanceProcAddr( instance->host.instance, "vkGetPhysicalDeviceFormatProperties" );
    if (!p_get_format_props) return FALSE;

    p_get_format_props( physical_device->host.physical_device, format, &props );
    return (props.optimalTilingFeatures & features) == features;
}

static VkResult init_compute_state( struct vulkan_device *device, struct swapchain *swapchain )
{
    VkResult res;
    VkSamplerCreateInfo samplerInfo = {0};
    VkDescriptorPoolSize poolSizes[2] = {{0}, {0}};
    VkDescriptorPoolCreateInfo poolInfo = {0};
    VkDescriptorSetLayoutBinding layoutBindings[2] = {{0}, {0}};
    VkDescriptorSetLayoutCreateInfo descriptorLayoutInfo = {0};
    VkDeviceSize fsrMemTotal = 0, offs;
    VkImageCreateInfo imageInfo = {0};
    VkMemoryRequirements fsrMemReq;
    VkMemoryAllocateInfo allocInfo = {0};
    VkPhysicalDeviceMemoryProperties memProperties;
    VkImageViewCreateInfo viewInfo = {0};
    uint32_t i;
    uint32_t fsr_memory_type = -1;

    samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    samplerInfo.magFilter = samplerInfo.minFilter =
        (swapchain->upscaler.is_fsr ||
         (swapchain->upscaler.linear_filter &&
          (swapchain->upscaler.color_mode == FS_HACK_COLOR_SRGB ||
           swapchain->upscaler.color_mode == FS_HACK_COLOR_RAW))) ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    samplerInfo.addressModeU = swapchain->upscaler.is_fsr ||
        swapchain->upscaler.color_mode != FS_HACK_COLOR_SRGB ?
        VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE : VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    samplerInfo.addressModeV = samplerInfo.addressModeU;
    samplerInfo.addressModeW = samplerInfo.addressModeU;
    samplerInfo.anisotropyEnable = VK_FALSE;
    samplerInfo.maxAnisotropy = 1;
    samplerInfo.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK;
    samplerInfo.unnormalizedCoordinates = VK_FALSE;
    samplerInfo.compareEnable = VK_FALSE;
    samplerInfo.compareOp = VK_COMPARE_OP_ALWAYS;
    samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    samplerInfo.mipLodBias = 0.0f;
    samplerInfo.minLod = 0.0f;
    samplerInfo.maxLod = 0.0f;

    if ((res = device->p_vkCreateSampler( device->host.device, &samplerInfo, NULL, &swapchain->sampler )))
    {
        WARN( "vkCreateSampler failed, res=%d\n", res );
        return res;
    }

    poolSizes[0].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    poolSizes[0].descriptorCount = swapchain->n_images;
    poolSizes[1].type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    poolSizes[1].descriptorCount = swapchain->n_images;

    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.poolSizeCount = 2;
    poolInfo.pPoolSizes = poolSizes;
    poolInfo.maxSets = swapchain->n_images;

    if (swapchain->upscaler.is_fsr)
    {
        poolSizes[0].descriptorCount *= 2;
        poolSizes[1].descriptorCount *= 2;
        poolInfo.maxSets *= 2;
    }

    if ((res = device->p_vkCreateDescriptorPool( device->host.device, &poolInfo, NULL, &swapchain->descriptor_pool )))
    {
        ERR( "vkCreateDescriptorPool: %d\n", res );
        goto fail;
    }

    layoutBindings[0].binding = 0;
    layoutBindings[0].descriptorCount = 1;
    layoutBindings[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    layoutBindings[0].pImmutableSamplers = NULL;
    layoutBindings[0].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

    layoutBindings[1].binding = 1;
    layoutBindings[1].descriptorCount = 1;
    layoutBindings[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    layoutBindings[1].pImmutableSamplers = NULL;
    layoutBindings[1].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

    descriptorLayoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    descriptorLayoutInfo.bindingCount = 2;
    descriptorLayoutInfo.pBindings = layoutBindings;

    if ((res = device->p_vkCreateDescriptorSetLayout( device->host.device, &descriptorLayoutInfo,
                                                      NULL, &swapchain->descriptor_set_layout )))
    {
        ERR( "vkCreateDescriptorSetLayout: %d\n", res );
        goto fail;
    }

    if (swapchain->upscaler.color_mode == FS_HACK_COLOR_SRGB)
        res = create_pipeline( device, swapchain, blit_comp_spv, sizeof(blit_comp_spv),
                               4 * sizeof(float), &swapchain->blit_pipeline );
    else
        res = create_pipeline( device, swapchain, fshack_color_comp_spv, sizeof(fshack_color_comp_spv),
                               4 * sizeof(float) + 2 * sizeof(uint32_t), &swapchain->blit_pipeline );
    if (res)
        goto fail;

    if (swapchain->upscaler.is_fsr)
    {
        if (swapchain->upscaler.fsr.lite) {
            if ((res = create_pipeline( device, swapchain, fsr_easu_lite_comp_spv, sizeof(fsr_easu_lite_comp_spv),
                                        16 * sizeof(uint32_t) /* 4 * uvec4 */, &swapchain->fsr_easu_pipeline )))
                goto fail;
        }
        else
        {
            if ((res = create_pipeline( device, swapchain, fsr_easu_comp_spv, sizeof(fsr_easu_comp_spv),
                                        16 * sizeof(uint32_t) /* 4 * uvec4 */, &swapchain->fsr_easu_pipeline )))
                goto fail;
        }
        if ((res = create_pipeline( device, swapchain, fsr_rcas_comp_spv, sizeof(fsr_rcas_comp_spv),
                                    8 * sizeof(uint32_t) /* uvec4 + ivec4 */, &swapchain->fsr_rcas_pipeline )))
            goto fail;

        /* create intermediate fsr images */
        for (i = 0; i < swapchain->n_images; ++i)
        {
            struct fs_hack_image *hack = &swapchain->fs_hack_images[i];

            imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
            imageInfo.imageType = VK_IMAGE_TYPE_2D;
            imageInfo.extent.width = swapchain->host_extents.width;
            imageInfo.extent.height = swapchain->host_extents.height;
            imageInfo.extent.depth = 1;
            imageInfo.mipLevels = 1;
            imageInfo.arrayLayers = 1;
            imageInfo.format = VK_FORMAT_A2B10G10R10_UNORM_PACK32;
            imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
            imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            imageInfo.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
            imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
            imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
            if ((res = device->p_vkCreateImage( device->host.device, &imageInfo, NULL, &hack->fsr_image )))
            {
                ERR("vkCreateImage failed: %d\n", res);
                goto fail;
            }

            device->p_vkGetImageMemoryRequirements(device->host.device, hack->fsr_image, &fsrMemReq);

            offs = fsrMemTotal % fsrMemReq.alignment;
            if(offs) fsrMemTotal += fsrMemReq.alignment - offs;

            fsrMemTotal += fsrMemReq.size;
        }

        /* allocate backing memory */
        device->physical_device->instance->p_vkGetPhysicalDeviceMemoryProperties(device->physical_device->host.physical_device, &memProperties);

        for (i = 0; i < memProperties.memoryTypeCount; i++)
        {
            if ((memProperties.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) == VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)
            {
                if (fsrMemReq.memoryTypeBits & (1 << i))
                {
                    fsr_memory_type = i;
                    break;
                }
            }
        }

        if (fsr_memory_type == -1)
        {
            ERR("unable to find suitable memory type\n");
            res = VK_ERROR_OUT_OF_HOST_MEMORY;
            goto fail;
        }

        allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocInfo.allocationSize = fsrMemTotal;
        allocInfo.memoryTypeIndex = fsr_memory_type;

        if ((res = device->p_vkAllocateMemory( device->host.device, &allocInfo, NULL, &swapchain->fsr_image_memory )))
        {
            ERR("vkAllocateMemory: %d\n", res);
            goto fail;
        }

        /* bind backing memory and create imageviews */
        fsrMemTotal = 0;
        for (i = 0; i < swapchain->n_images; ++i)
        {
            struct fs_hack_image *hack = &swapchain->fs_hack_images[i];

            device->p_vkGetImageMemoryRequirements(device->host.device, hack->fsr_image, &fsrMemReq);

            offs = fsrMemTotal % fsrMemReq.alignment;
            if(offs) fsrMemTotal += fsrMemReq.alignment - offs;

            if ((res = device->p_vkBindImageMemory( device->host.device, hack->fsr_image, swapchain->fsr_image_memory, fsrMemTotal )))
            {
                ERR("vkBindImageMemory: %d\n", res);
                goto fail;
            }

            fsrMemTotal += fsrMemReq.size;
        }

        /* create imageviews */
        for (i = 0; i < swapchain->n_images; ++i)
        {
            struct fs_hack_image *hack = &swapchain->fs_hack_images[i];

            viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
            viewInfo.image = hack->fsr_image;
            viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
            viewInfo.format = VK_FORMAT_A2B10G10R10_UNORM_PACK32;
            viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            viewInfo.subresourceRange.baseMipLevel = 0;
            viewInfo.subresourceRange.levelCount = 1;
            viewInfo.subresourceRange.baseArrayLayer = 0;
            viewInfo.subresourceRange.layerCount = 1;

            if ((res = device->p_vkCreateImageView( device->host.device, &viewInfo, NULL, &hack->fsr_view )))
            {
                ERR("vkCreateImageView(blit): %d\n", res);
                goto fail;
            }
        }
    }

    for (i = 0; i < swapchain->n_images; ++i)
    {
        struct fs_hack_image *hack = &swapchain->fs_hack_images[i];

        viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image = hack->swapchain_image;
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        if (swapchain->upscaler.color_mode == FS_HACK_COLOR_SRGB)
            viewInfo.format = swapchain->upscaler.is_fsr ?
                              srgb_to_unorm(swapchain->format) : VK_FORMAT_B8G8R8A8_UNORM;
        else
            viewInfo.format = swapchain->format;
        viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        viewInfo.subresourceRange.baseMipLevel = 0;
        viewInfo.subresourceRange.levelCount = 1;
        viewInfo.subresourceRange.baseArrayLayer = 0;
        viewInfo.subresourceRange.layerCount = 1;

        if ((res = device->p_vkCreateImageView( device->host.device, &viewInfo, NULL, &hack->swapchain_view )))
        {
            ERR( "vkCreateImageView(blit): %d\n", res );
            goto fail;
        }

        if ((res = create_descriptor_set( device, swapchain, hack ))) goto fail;
    }

    return VK_SUCCESS;

fail:
    for (i = 0; i < swapchain->n_images; ++i)
    {
        struct fs_hack_image *hack = &swapchain->fs_hack_images[i];

        device->p_vkDestroyImageView(device->host.device, hack->fsr_view, NULL);
        hack->fsr_view = VK_NULL_HANDLE;

        device->p_vkDestroyImageView(device->host.device, hack->swapchain_view, NULL);
        hack->swapchain_view = VK_NULL_HANDLE;

        device->p_vkDestroyImage(device->host.device, hack->fsr_image, NULL);
        hack->fsr_image = VK_NULL_HANDLE;
    }

    destroy_pipeline(device, &swapchain->blit_pipeline);
    destroy_pipeline(device, &swapchain->fsr_easu_pipeline);
    destroy_pipeline(device, &swapchain->fsr_rcas_pipeline);

    device->p_vkDestroyDescriptorSetLayout( device->host.device, swapchain->descriptor_set_layout, NULL );
    swapchain->descriptor_set_layout = VK_NULL_HANDLE;

    device->p_vkDestroyDescriptorPool( device->host.device, swapchain->descriptor_pool, NULL );
    swapchain->descriptor_pool = VK_NULL_HANDLE;

    device->p_vkFreeMemory( device->host.device, swapchain->fsr_image_memory, NULL );
    swapchain->fsr_image_memory = VK_NULL_HANDLE;

    device->p_vkDestroySampler( device->host.device, swapchain->sampler, NULL );
    swapchain->sampler = VK_NULL_HANDLE;

    return res;
}

static void destroy_fs_hack_image( struct vulkan_device *device, struct swapchain *swapchain, struct fs_hack_image *hack )
{
    device->p_vkDestroyImageView( device->host.device, hack->user_view, NULL );
    device->p_vkDestroyImageView(device->host.device, hack->swapchain_view, NULL);
    device->p_vkDestroyImageView(device->host.device, hack->fsr_view, NULL);
    device->p_vkDestroyImage( device->host.device, hack->user_image, NULL );
    device->p_vkDestroyImage(device->host.device, hack->fsr_image, NULL);
    if (hack->cmd) device->p_vkFreeCommandBuffers( device->host.device, swapchain->cmd_pools[hack->cmd_queue_idx], 1, &hack->cmd );
    device->p_vkDestroySemaphore( device->host.device, hack->blit_finished, NULL );
}

static VkResult init_fs_hack_images( struct vulkan_device *device, struct swapchain *swapchain,
                                     const VkSwapchainCreateInfoKHR *createinfo )
{
    struct vulkan_physical_device *physical_device = device->physical_device;
    struct vulkan_instance *instance = physical_device->instance;
    VkFormat user_view_format = swapchain->upscaler.is_fsr ||
                                swapchain->upscaler.color_mode != FS_HACK_COLOR_SRGB
                                ? srgb_to_unorm( createinfo->imageFormat )
                                : unorm_to_srgb( createinfo->imageFormat );
    VkResult res;
    VkImage *real_images = NULL;
    VkDeviceSize userMemTotal = 0, offs;
    VkImageCreateInfo imageInfo = {0};
    VkSemaphoreCreateInfo semaphoreInfo = {0};
    VkMemoryRequirements userMemReq;
    VkMemoryAllocateInfo allocInfo = {0};
    VkPhysicalDeviceMemoryProperties memProperties;
    VkImageViewCreateInfo viewInfo = {0};
    uint32_t count, i = 0, user_memory_type = -1;

    if ((res = device->p_vkGetSwapchainImagesKHR( device->host.device, swapchain->obj.host.swapchain, &count, NULL )))
    {
        WARN( "vkGetSwapchainImagesKHR failed, res=%d\n", res );
        return res;
    }

    real_images = malloc( count * sizeof(VkImage) );
    swapchain->cmd_pools = calloc( device->queue_count, sizeof(VkCommandPool) );
    swapchain->fs_hack_images = calloc( count, sizeof(struct fs_hack_image) );
    if (!real_images || !swapchain->cmd_pools || !swapchain->fs_hack_images) goto fail;

    if ((res = device->p_vkGetSwapchainImagesKHR( device->host.device, swapchain->obj.host.swapchain, &count, real_images )))
    {
        WARN( "vkGetSwapchainImagesKHR failed, res=%d\n", res );
        goto fail;
    }

    /* create user images */
    for (i = 0; i < count; ++i)
    {
        struct fs_hack_image *hack = &swapchain->fs_hack_images[i];

        hack->swapchain_image = real_images[i];

        semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        if ((res = device->p_vkCreateSemaphore( device->host.device, &semaphoreInfo, NULL, &hack->blit_finished )))
        {
            WARN( "vkCreateSemaphore failed, res=%d\n", res );
            goto fail;
        }

        imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        imageInfo.imageType = VK_IMAGE_TYPE_2D;
        imageInfo.extent.width = swapchain->extents.width;
        imageInfo.extent.height = swapchain->extents.height;
        imageInfo.extent.depth = 1;
        imageInfo.mipLevels = 1;
        imageInfo.arrayLayers = createinfo->imageArrayLayers;
        imageInfo.format = createinfo->imageFormat;
        imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
        imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        imageInfo.usage = createinfo->imageUsage | VK_IMAGE_USAGE_SAMPLED_BIT;
        imageInfo.sharingMode = createinfo->imageSharingMode;
        imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
        imageInfo.queueFamilyIndexCount = createinfo->queueFamilyIndexCount;
        imageInfo.pQueueFamilyIndices = createinfo->pQueueFamilyIndices;

        if (createinfo->flags & VK_SWAPCHAIN_CREATE_MUTABLE_FORMAT_BIT_KHR)
            imageInfo.flags |= VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT | VK_IMAGE_CREATE_EXTENDED_USAGE_BIT;
        if (user_view_format != createinfo->imageFormat)
            imageInfo.flags |= VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;

        if ((res = device->p_vkCreateImage( device->host.device, &imageInfo, NULL, &hack->user_image )))
        {
            ERR( "vkCreateImage failed: %d\n", res );
            goto fail;
        }

        device->p_vkGetImageMemoryRequirements( device->host.device, hack->user_image, &userMemReq );

        offs = userMemTotal % userMemReq.alignment;
        if (offs) userMemTotal += userMemReq.alignment - offs;

        userMemTotal += userMemReq.size;

        swapchain->n_images++;
    }

    /* allocate backing memory */
    instance->p_vkGetPhysicalDeviceMemoryProperties( physical_device->host.physical_device, &memProperties );

    for (i = 0; i < memProperties.memoryTypeCount; i++)
    {
        UINT flag = memProperties.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
        if (flag == VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)
        {
            if (userMemReq.memoryTypeBits & (1 << i))
            {
                user_memory_type = i;
                break;
            }
        }
    }

    if (user_memory_type == -1)
    {
        ERR( "unable to find suitable memory type\n" );
        res = VK_ERROR_OUT_OF_HOST_MEMORY;
        goto fail;
    }

    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize = userMemTotal;
    allocInfo.memoryTypeIndex = user_memory_type;

    if ((res = device->p_vkAllocateMemory( device->host.device, &allocInfo, NULL, &swapchain->user_image_memory )))
    {
        ERR( "vkAllocateMemory: %d\n", res );
        goto fail;
    }

    /* bind backing memory and create imageviews */
    userMemTotal = 0;
    for (i = 0; i < count; ++i)
    {
        device->p_vkGetImageMemoryRequirements( device->host.device, swapchain->fs_hack_images[i].user_image, &userMemReq );

        offs = userMemTotal % userMemReq.alignment;
        if (offs) userMemTotal += userMemReq.alignment - offs;

        if ((res = device->p_vkBindImageMemory( device->host.device, swapchain->fs_hack_images[i].user_image,
                                                swapchain->user_image_memory, userMemTotal )))
        {
            ERR( "vkBindImageMemory: %d\n", res );
            goto fail;
        }

        userMemTotal += userMemReq.size;

        viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image = swapchain->fs_hack_images[i].user_image;
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = user_view_format;
        viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        viewInfo.subresourceRange.baseMipLevel = 0;
        viewInfo.subresourceRange.levelCount = 1;
        viewInfo.subresourceRange.baseArrayLayer = 0;
        viewInfo.subresourceRange.layerCount = 1;

        if ((res = device->p_vkCreateImageView( device->host.device, &viewInfo, NULL,
                                                &swapchain->fs_hack_images[i].user_view )))
        {
            ERR( "vkCreateImageView(user): %d\n", res );
            goto fail;
        }
    }

    free( real_images );

    return VK_SUCCESS;

fail:
    for (i = 0; i < swapchain->n_images; ++i) destroy_fs_hack_image( device, swapchain, &swapchain->fs_hack_images[i] );
    free( real_images );
    free( swapchain->cmd_pools );
    free( swapchain->fs_hack_images );
    return res;
}

static VkResult win32u_vkSetLatencySleepModeNV(VkDevice device, VkSwapchainKHR swapchain, const VkLatencySleepModeInfoNV *pSleepModeInfo)
{
    VkLatencySleepModeInfoNV sleep_mode_info_host;

    struct vulkan_device *vk_device = vulkan_device_from_handle(device);
    struct swapchain *vk_swapchain = swapchain_from_handle(swapchain);

    if (!vk_swapchain || swapchain_is_out_of_date( vk_swapchain )) return VK_ERROR_OUT_OF_DATE_KHR;

    vk_device->low_latency_enabled = pSleepModeInfo->lowLatencyMode;

    if (vk_swapchain->managed) return VK_SUCCESS;

    sleep_mode_info_host.sType = VK_STRUCTURE_TYPE_LATENCY_SLEEP_MODE_INFO_NV;
    sleep_mode_info_host.pNext = NULL;
    sleep_mode_info_host.lowLatencyMode = pSleepModeInfo->lowLatencyMode;
    sleep_mode_info_host.lowLatencyBoost = pSleepModeInfo->lowLatencyBoost;
    sleep_mode_info_host.minimumIntervalUs = pSleepModeInfo->minimumIntervalUs;

    return vk_device->p_vkSetLatencySleepModeNV( vk_device->host.device,
                                                 vk_swapchain->obj.host.swapchain, &sleep_mode_info_host );
}

static VkResult win32u_vkLatencySleepNV(VkDevice device, VkSwapchainKHR swapchain, const VkLatencySleepInfoNV *pSleepInfo)
{
    struct vulkan_device *vk_device = vulkan_device_from_handle(device);
    struct swapchain *vk_swapchain = swapchain_from_handle(swapchain);
    VkLatencySleepInfoNV sleep_info_host = *pSleepInfo;
    struct vulkan_semaphore *semaphore;

    semaphore = sleep_info_host.signalSemaphore ? vulkan_semaphore_from_handle(sleep_info_host.signalSemaphore) : NULL;
    sleep_info_host.signalSemaphore = semaphore ? semaphore->host.semaphore : 0;

    if (!vk_swapchain || swapchain_is_out_of_date( vk_swapchain ) || vk_swapchain->managed)
    {
        /* Wake the Reflex sleep immediately: callers wait on the semaphore
         * without checking our result and would hang forever otherwise. */
        if (semaphore)
        {
            VkSemaphoreSignalInfo signal_info =
            {
                .sType = VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO,
                .semaphore = semaphore->host.semaphore,
                .value = sleep_info_host.value,
            };
            UINT64 counter = 0;

            if (!vk_device->p_vkGetSemaphoreCounterValue( vk_device->host.device, semaphore->host.semaphore, &counter ) &&
                counter < sleep_info_host.value)
                vk_device->p_vkSignalSemaphore( vk_device->host.device, &signal_info );
        }
        return (vk_swapchain && vk_swapchain->managed) ? VK_SUCCESS : VK_ERROR_OUT_OF_DATE_KHR;
    }

    return vk_device->p_vkLatencySleepNV(vk_device->host.device, vk_swapchain->obj.host.swapchain, &sleep_info_host);
}

static void win32u_vkSetLatencyMarkerNV(VkDevice device, VkSwapchainKHR swapchain,
                                        const VkSetLatencyMarkerInfoNV *pLatencyMarkerInfo)
{
    struct vulkan_device *vk_device = vulkan_device_from_handle(device);
    struct swapchain *vk_swapchain = swapchain_from_handle(swapchain);

    if (!vk_swapchain || swapchain_is_out_of_date( vk_swapchain )) return;

    if (vk_swapchain->managed) return;

    vk_device->p_vkSetLatencyMarkerNV(vk_device->host.device, vk_swapchain->obj.host.swapchain, pLatencyMarkerInfo);
}

static void win32u_vkGetLatencyTimingsNV(VkDevice device, VkSwapchainKHR swapchain,
                                         VkGetLatencyMarkerInfoNV *pLatencyMarkerInfo)
{
    struct vulkan_device *vk_device = vulkan_device_from_handle(device);
    struct swapchain *vk_swapchain = swapchain_from_handle(swapchain);

    if (!vk_swapchain || swapchain_is_out_of_date( vk_swapchain ))
    {
        pLatencyMarkerInfo->timingCount = 0;
        return;
    }

    if (vk_swapchain->managed)
    {
        pLatencyMarkerInfo->timingCount = 0;
        return;
    }

    vk_device->p_vkGetLatencyTimingsNV(vk_device->host.device, vk_swapchain->obj.host.swapchain, pLatencyMarkerInfo);
}

static BOOL surface_get_fshack_config( struct surface *surface, const VkExtent2D *client_extents,
                                       VkExtent2D *host_extents, struct fs_hack_config *config )
{
    RECT host_rect;
    UINT dpi, raw;

    memset( config, 0, sizeof(*config) );

    /* A direct client must compose into the borrowed root when its content
     * does not cover that surface exactly, regardless of legacy FSHack policy. */
    if (surface->client->funcs->get_presentation_rects &&
        surface->client->funcs->get_presentation_rects( surface->client, &host_rect, &config->dst ))
    {
        host_extents->width = host_rect.right - host_rect.left;
        host_extents->height = host_rect.bottom - host_rect.top;

        if (extents_equals( client_extents, &config->dst ) &&
            config->dst.left == 0 && config->dst.top == 0 &&
            config->dst.right == (LONG)host_extents->width &&
            config->dst.bottom == (LONG)host_extents->height)
        {
            memset( config, 0, sizeof(*config) );
            return FALSE;
        }

        config->enabled = TRUE;
        return TRUE;
    }

    dpi = NtUserGetDpiForWindow( surface->hwnd );
    raw = NtUserGetWinMonitorDpi( surface->hwnd, MDT_RAW_DPI );
    if (!fshack_enabled || !raw || dpi == raw) return FALSE;

    if (!get_swapchain_surface_rect( surface->hwnd, &host_rect, raw )) return FALSE;
    host_extents->width = host_rect.right - host_rect.left;
    host_extents->height = host_rect.bottom - host_rect.top;
    SetRect( &config->dst, 0, 0, host_extents->width, host_extents->height );
    config->enabled = TRUE;
    config->dpi = raw;
    return TRUE;
}

static BOOL surface_is_presentation_scaled( struct surface *surface )
{
    return surface->client->funcs->is_presentation_scaled &&
           surface->client->funcs->is_presentation_scaled( surface->client );
}

static BOOL swapchain_presentation_config_changed( struct swapchain *swapchain )
{
    VkExtent2D host_extents = swapchain->host_extents;
    struct fs_hack_config config;
    BOOL enabled;

    if (swapchain->compositor_scaling) return FALSE;

    if (NtUserGetWindowLongW( swapchain->surface->hwnd, GWL_STYLE ) & WS_MINIMIZE)
        return FALSE;

    enabled = surface_get_fshack_config( swapchain->surface, &swapchain->extents,
                                         &host_extents, &config );

    return enabled != swapchain->fshack.enabled ||
           (enabled && (config.dpi != swapchain->fshack.dpi ||
                        host_extents.width != swapchain->host_extents.width ||
                        host_extents.height != swapchain->host_extents.height ||
                        !EqualRect( &config.dst, &swapchain->fshack.dst )));
}

/* Cross-process producer helpers. */

/* Map a Vulkan swapchain VkFormat to a DRM fourcc. Vulkan B8G8R8A8 in memory
 * reads as a little-endian word ARGB and maps to the DRM *RGB* fourccs. Opaque
 * swapchains use the X-variant (ignore alpha). Returns 0 if unsupported. */
static unsigned int vk_format_to_drm_fourcc( VkFormat format, BOOL opaque )
{
    switch (format)
    {
    /* Vulkan BGRA in memory -> DRM *RGB* (little-endian word ARGB). */
    case VK_FORMAT_B8G8R8A8_UNORM:
    case VK_FORMAT_B8G8R8A8_SRGB:
        return opaque ? 0x34325258 /* XRGB8888 'XR24' */ : 0x34325241 /* ARGB8888 'AR24' */;
    /* Vulkan RGBA in memory -> DRM *BGR* (little-endian word ABGR). */
    case VK_FORMAT_R8G8B8A8_UNORM:
    case VK_FORMAT_R8G8B8A8_SRGB:
        return opaque ? 0x34324258 /* XBGR8888 'XB24' */ : 0x34324241 /* ABGR8888 'AB24' */;
    case VK_FORMAT_A2B10G10R10_UNORM_PACK32:
        return opaque ? 0x30334258 /* XB30 */ : 0x30334241 /* AB30 */;
    case VK_FORMAT_A2R10G10B10_UNORM_PACK32:
        return opaque ? 0x30335258 /* XR30 */ : 0x30335241 /* AR30 */;
    case VK_FORMAT_R16G16B16A16_SFLOAT:
        return 0x48344241 /* ABGR16161616F 'AB4H' */;
    default:
        return 0;
    }
}


/* Memory plane count of (format, modifier) or 0 if unknown. Tiled modifiers
 * may carry an auxiliary plane (e.g. AMD DCC) that must also be published. */
static uint32_t vk_modifier_plane_count( struct vulkan_device *device, VkFormat format,
                                         uint64_t modifier )
{
    struct vulkan_physical_device *physical_device = device->physical_device;
    struct vulkan_instance *instance = physical_device->instance;
    PFN_vkGetPhysicalDeviceFormatProperties2 p_get_format_props;
    VkDrmFormatModifierPropertiesListEXT mod_list =
    {
        .sType = VK_STRUCTURE_TYPE_DRM_FORMAT_MODIFIER_PROPERTIES_LIST_EXT,
    };
    VkFormatProperties2 props = { .sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2, .pNext = &mod_list };
    VkDrmFormatModifierPropertiesEXT *mods;
    uint32_t i, count = 0;

    p_get_format_props = (PFN_vkGetPhysicalDeviceFormatProperties2)
        p_vkGetInstanceProcAddr( instance->host.instance, "vkGetPhysicalDeviceFormatProperties2" );
    if (!p_get_format_props)
        p_get_format_props = (PFN_vkGetPhysicalDeviceFormatProperties2)
            p_vkGetInstanceProcAddr( instance->host.instance, "vkGetPhysicalDeviceFormatProperties2KHR" );
    if (!p_get_format_props) return 0;

    p_get_format_props( physical_device->host.physical_device, format, &props );
    if (!mod_list.drmFormatModifierCount) return 0;
    if (!(mods = calloc( mod_list.drmFormatModifierCount, sizeof(*mods) ))) return 0;
    mod_list.pDrmFormatModifierProperties = mods;
    p_get_format_props( physical_device->host.physical_device, format, &props );

    for (i = 0; i < mod_list.drmFormatModifierCount; i++)
    {
        if (mods[i].drmFormatModifier != modifier) continue;
        count = mods[i].drmFormatModifierPlaneCount;
        break;
    }
    free( mods );
    return count;
}

/* Query whether the host can export an image of (format, modifier) as a
 * DMA_BUF with the requested usage. Multi-plane modifiers are fine. */
static BOOL vk_host_modifier_exportable( struct vulkan_device *device, VkFormat format,
                                         VkImageUsageFlags usage, uint64_t modifier )
{
    struct vulkan_physical_device *physical_device = device->physical_device;
    struct vulkan_instance *instance = physical_device->instance;
    VkPhysicalDeviceImageDrmFormatModifierInfoEXT mod_info =
    {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_DRM_FORMAT_MODIFIER_INFO_EXT,
        .drmFormatModifier = modifier,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };
    VkPhysicalDeviceExternalImageFormatInfo external_info =
    {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO,
        .pNext = &mod_info,
        .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
    };
    VkPhysicalDeviceImageFormatInfo2 format_info =
    {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2,
        .pNext = &external_info,
        .format = format,
        .type = VK_IMAGE_TYPE_2D,
        .tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT,
        .usage = usage,
    };
    VkExternalImageFormatProperties external_props =
    {
        .sType = VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES,
    };
    VkImageFormatProperties2 format_props =
    {
        .sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2,
        .pNext = &external_props,
    };

    if (instance->p_vkGetPhysicalDeviceImageFormatProperties2( physical_device->host.physical_device,
                                                               &format_info, &format_props ))
        return FALSE;
    if (!(external_props.externalMemoryProperties.externalMemoryFeatures &
          VK_EXTERNAL_MEMORY_FEATURE_EXPORTABLE_BIT))
        return FALSE;
    return TRUE;
}

static uint32_t vk_collect_managed_modifiers( struct vulkan_device *device, VkFormat format,
                                              VkImageUsageFlags usage,
                                              const hwnd_dmabuf_format_modifier_t *caps_mods,
                                              UINT caps_count, unsigned int fourcc,
                                              uint64_t *out_mods, uint64_t *out_wire_mods,
                                              uint32_t max_out )
{
    UINT best_tranche = ~0u, i;
    uint32_t out_count = 0;

    for (i = 0; i < caps_count; i++)
    {
        uint64_t wire_modifier = caps_mods[i].modifier;
        uint64_t modifier = wire_modifier;

        if (caps_mods[i].fourcc != fourcc) continue;
        if (modifier == WINE_VK_DRM_FORMAT_MOD_INVALID)
            modifier = 0 /* DRM_FORMAT_MOD_LINEAR */;
        if (!vk_host_modifier_exportable( device, format, usage, modifier )) continue;
        if (caps_mods[i].tranche_index < best_tranche)
            best_tranche = caps_mods[i].tranche_index;
    }

    if (best_tranche == ~0u) return 0;

    for (i = 0; i < caps_count && out_count < max_out; i++)
    {
        uint64_t wire_modifier = caps_mods[i].modifier;
        uint64_t modifier = wire_modifier;

        if (caps_mods[i].fourcc != fourcc || caps_mods[i].tranche_index != best_tranche)
            continue;
        /* MOD_INVALID means "any/implicit". Let the host pick by offering LINEAR. */
        if (modifier == WINE_VK_DRM_FORMAT_MOD_INVALID)
            modifier = 0 /* DRM_FORMAT_MOD_LINEAR */;
        if (!vk_host_modifier_exportable( device, format, usage, modifier )) continue;
        /* dedupe */
        {
            uint32_t j;
            BOOL dup = FALSE;
            for (j = 0; j < out_count; j++) if (out_mods[j] == modifier) { dup = TRUE; break; }
            if (dup) continue;
        }
        out_mods[out_count] = modifier;
        out_wire_mods[out_count] = wire_modifier;
        out_count++;
    }

    return out_count;
}

/* Build the candidate modifier list = caps modifiers (matching fourcc) that the
 * host can also export as a single-plane dmabuf. out_mods is what Vulkan gets,
 * out_wire_mods is what the consumer must see. Returns the count placed in
 * out_mods. 0 means no intersection -> caller falls back to the host swapchain. */
static uint32_t vk_select_managed_modifiers( struct vulkan_device *device, HWND hwnd, VkFormat format,
                                             VkImageUsageFlags usage, BOOL opaque, unsigned int *fourcc_out,
                                             uint64_t *out_mods, uint64_t *out_wire_mods, uint32_t max_out,
                                             unsigned int *caps_flags )
{
    hwnd_dmabuf_format_modifier_t *caps_mods;
    hwnd_dmabuf_host_caps_t caps = {0};
    unsigned int fourcc_first, fourcc_second;
    UINT caps_count = 0;
    uint32_t out_count = 0;

    if (caps_flags) *caps_flags = 0;
    /* The fourcc must match compositeAlpha: opaque swapchains use the X-variant
     * (compositor ignores alpha), blended ones the A-variant. Try the matching
     * variant first, then the other. */
    fourcc_first = vk_format_to_drm_fourcc( format, opaque );
    fourcc_second = vk_format_to_drm_fourcc( format, !opaque );
    if (!fourcc_first && !fourcc_second) return 0;

    /* The compositor can advertise hundreds of (fourcc, modifier) pairs. Query
     * the real count and heap-allocate so a fixed cap cannot drop the fourcc or
     * LINEAR we need. The two-call probe also gates: an on-screen window returns
     * HWND_DMABUF_NOT_FOUND. */
    if (hwnd_dmabuf_get_caps( hwnd, &caps, NULL, 0, &caps_count ) != HWND_DMABUF_OK || !caps_count)
        return 0;
    if (!(caps_mods = calloc( caps_count, sizeof(*caps_mods) ))) return 0;
    memset( &caps, 0, sizeof(caps) );
    if (hwnd_dmabuf_get_caps( hwnd, &caps, caps_mods, caps_count, &caps_count ) != HWND_DMABUF_OK)
    {
        free( caps_mods );
        return 0;
    }
    if (!(caps_count = caps.format_modifier_count))
    {
        free( caps_mods );
        return 0;
    }
    if (caps_flags) *caps_flags = caps.flags;

    *fourcc_out = 0;
    if (fourcc_first)
    {
        out_count = vk_collect_managed_modifiers( device, format, usage, caps_mods, caps_count,
                                                  fourcc_first, out_mods, out_wire_mods, max_out );
        if (out_count) *fourcc_out = fourcc_first;
    }
    if (!out_count && fourcc_second)
    {
        out_count = vk_collect_managed_modifiers( device, format, usage, caps_mods, caps_count,
                                                  fourcc_second, out_mods, out_wire_mods, max_out );
        if (out_count) *fourcc_out = fourcc_second;
    }

    free( caps_mods );
    return out_count;
}

static BOOL wait_sync_file( int fd )
{
    struct pollfd pfd = { .fd = fd, .events = POLLIN };
    int ret;

    do ret = poll( &pfd, 1, -1 );
    while (ret < 0 && errno == EINTR);
    return ret > 0;
}

static void wait_and_close_sync_file( int fd )
{
    if (fd < 0) return;
    wait_sync_file( fd );
    close( fd );
}

static void managed_wait_image_completion( struct wine_managed_image *image )
{
    if (image->completion_fd < 0) return;
    wait_and_close_sync_file( image->completion_fd );
    image->completion_fd = -1;
}

static BOOL managed_image_completion_ready( struct wine_managed_image *image )
{
    struct pollfd pfd = { .fd = image->completion_fd, .events = POLLIN };
    int ret;

    if (image->completion_fd < 0) return TRUE;
    do ret = poll( &pfd, 1, 0 );
    while (ret < 0 && errno == EINTR);
    if (ret <= 0) return FALSE;
    close( image->completion_fd );
    image->completion_fd = -1;
    return TRUE;
}

static struct wine_managed_consumer *managed_consumer_get( HWND hwnd )
{
    struct wine_managed_consumer *consumer;

    pthread_mutex_lock( &managed_consumers_lock );
    LIST_FOR_EACH_ENTRY( consumer, &managed_consumers, struct wine_managed_consumer, entry )
    {
        if (consumer->hwnd != hwnd) continue;
        consumer->refcount++;
        pthread_mutex_unlock( &managed_consumers_lock );
        return consumer;
    }

    if ((consumer = calloc( 1, sizeof(*consumer) )))
    {
        consumer->hwnd = hwnd;
        consumer->refcount = 1;
        list_add_tail( &managed_consumers, &consumer->entry );
    }
    pthread_mutex_unlock( &managed_consumers_lock );
    return consumer;
}

static void managed_consumer_put( struct wine_managed_consumer *consumer )
{
    if (!consumer) return;

    pthread_mutex_lock( &managed_consumers_lock );
    if (!--consumer->refcount)
    {
        list_remove( &consumer->entry );
        free( consumer );
    }
    pthread_mutex_unlock( &managed_consumers_lock );
}

static enum wine_managed_consumer_state managed_consumer_state( struct wine_managed_swapchain *managed )
{
    return ReadAcquire( &managed->consumer->state );
}

static void managed_consumer_set_state( struct wine_managed_swapchain *managed,
                                        enum wine_managed_consumer_state state )
{
    InterlockedExchange( &managed->consumer->state, state );
    InterlockedExchange( &managed->consumer->reannounce_pending, FALSE );
    InterlockedExchange( &managed->consumer->reannounce_delay_ms, 0 );
}

static void managed_consumer_request_state( struct wine_managed_swapchain *managed )
{
    struct wine_managed_consumer *consumer = managed->consumer;
    LONG delay, last, now = NtGetTickCount();

    if (managed_consumer_state( managed ) != WINE_MANAGED_CONSUMER_UNKNOWN) return;
    if (!InterlockedExchange( &consumer->reannounce_pending, TRUE ))
    {
        InterlockedExchange( &consumer->last_reannounce_ms, now );
        InterlockedExchange( &consumer->reannounce_delay_ms,
                             WINE_VK_MANAGED_REANNOUNCE_MIN_MS );
        hwnd_dmabuf_post_wake( managed->hwnd, HWND_DMABUF_WAKE_REANNOUNCE );
        return;
    }

    delay = ReadAcquire( &consumer->reannounce_delay_ms );
    if (delay < WINE_VK_MANAGED_REANNOUNCE_MIN_MS)
        delay = WINE_VK_MANAGED_REANNOUNCE_MIN_MS;
    last = ReadAcquire( &consumer->last_reannounce_ms );
    if ((DWORD)(now - last) < delay) return;
    if (InterlockedCompareExchange( &consumer->last_reannounce_ms, now, last ) == last)
    {
        delay = min( delay * 2, WINE_VK_MANAGED_REANNOUNCE_MAX_MS );
        InterlockedExchange( &consumer->reannounce_delay_ms, delay );
        hwnd_dmabuf_post_wake( managed->hwnd, HWND_DMABUF_WAKE_REANNOUNCE );
    }
}

static void managed_destroy_image( struct vulkan_device *device, struct wine_managed_image *image )
{
    managed_wait_image_completion( image );
    if (image->dmabuf_fd >= 0) { close( image->dmabuf_fd ); image->dmabuf_fd = -1; }
    if (image->image) { device->p_vkDestroyImage( device->host.device, image->image, NULL ); image->image = VK_NULL_HANDLE; }
    if (image->memory) { device->p_vkFreeMemory( device->host.device, image->memory, NULL ); image->memory = VK_NULL_HANDLE; }
    image->valid = FALSE;
    image->acquired = image->busy = FALSE;
}

static void managed_free( struct vulkan_device *device, struct wine_managed_swapchain *managed )
{
    uint32_t i;

    if (!managed) return;
    /* Wait only for writes to these images. The exported dmabufs stay alive
     * for the compositor through their kernel references. */
    for (i = 0; i < managed->image_count; i++)
        managed_destroy_image( device, &managed->images[i] );
    if (managed->present_fence) device->p_vkDestroyFence( device->host.device, managed->present_fence, NULL );
    if (managed->channel_fd >= 0) close( managed->channel_fd );
    if (managed->wake_fd >= 0) close( managed->wake_fd );
    if (managed->channel_registered)
        hwnd_dmabuf_release_channel( managed->hwnd );
    if (managed->pending_registered)
        hwnd_dmabuf_set_pending( managed->hwnd, FALSE );
    managed_consumer_put( managed->consumer );
    pthread_mutex_destroy( &managed->lock );
    free( managed );
}

/* Create one exportable DRM-modifier image + dedicated exportable memory, export
 * its dmabuf fd and cache the realized modifier + per-plane layouts. */
static VkResult managed_create_image( struct vulkan_device *device, struct wine_managed_swapchain *managed,
                                      const uint64_t *modifiers, const uint64_t *wire_modifiers,
                                      uint32_t modifier_count,
                                      struct wine_managed_image *image )
{
    VkImageDrmFormatModifierListCreateInfoEXT mod_list =
    {
        .sType = VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_LIST_CREATE_INFO_EXT,
        .drmFormatModifierCount = modifier_count,
        .pDrmFormatModifiers = modifiers,
    };
    VkExternalMemoryImageCreateInfo external_image =
    {
        .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO,
        .pNext = &mod_list,
        .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
    };
    VkImageCreateInfo image_info =
    {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .pNext = &external_image,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = managed->format,
        .extent = { managed->extents.width, managed->extents.height, 1 },
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT,
        .usage = managed->usage | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    VkMemoryDedicatedAllocateInfo dedicated =
    {
        .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO,
    };
    VkExportMemoryAllocateInfo export_mem =
    {
        .sType = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO,
        .pNext = &dedicated,
        .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
    };
    VkImageMemoryRequirementsInfo2 req_info = { .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_REQUIREMENTS_INFO_2 };
    VkMemoryRequirements2 req = { .sType = VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2 };
    VkMemoryAllocateInfo alloc_info = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .pNext = &export_mem };
    VkImageDrmFormatModifierPropertiesEXT mod_props = { .sType = VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_PROPERTIES_EXT };
    VkMemoryGetFdInfoKHR get_fd = { .sType = VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR };
    VkImageSubresource subresource = { .aspectMask = VK_IMAGE_ASPECT_MEMORY_PLANE_0_BIT_EXT };
    struct vulkan_physical_device *physical_device = device->physical_device;
    VkSubresourceLayout layout = {0};
    uint32_t mem_type_index = ~0u, plane_count, i;
    uint64_t wire_modifier;
    int fd = -1;
    VkResult res;

    image->dmabuf_fd = -1;
    image->completion_fd = -1;

    if ((res = device->p_vkCreateImage( device->host.device, &image_info, NULL, &image->image )))
    {
        WARN( "managed vkCreateImage failed, res %d\n", res );
        return res;
    }

    req_info.image = image->image;
    device->p_vkGetImageMemoryRequirements2( device->host.device, &req_info, &req );

    for (i = 0; i < physical_device->memory_properties.memoryTypeCount; i++)
    {
        if (!(req.memoryRequirements.memoryTypeBits & (1u << i))) continue;
        if (!(physical_device->memory_properties.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) continue;
        mem_type_index = i;
        break;
    }
    if (mem_type_index == ~0u)
    {
        /* fall back to any supported type */
        for (i = 0; i < physical_device->memory_properties.memoryTypeCount; i++)
            if (req.memoryRequirements.memoryTypeBits & (1u << i)) { mem_type_index = i; break; }
    }
    if (mem_type_index == ~0u) { res = VK_ERROR_OUT_OF_DEVICE_MEMORY; goto failed; }

    dedicated.image = image->image;
    alloc_info.allocationSize = req.memoryRequirements.size;
    alloc_info.memoryTypeIndex = mem_type_index;
    if ((res = device->p_vkAllocateMemory( device->host.device, &alloc_info, NULL, &image->memory )))
    {
        WARN( "managed vkAllocateMemory failed, res %d\n", res );
        goto failed;
    }

    if ((res = device->p_vkBindImageMemory( device->host.device, image->image, image->memory, 0 )))
    {
        WARN( "managed vkBindImageMemory failed, res %d\n", res );
        goto failed;
    }

    get_fd.memory = image->memory;
    get_fd.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
    if ((res = device->p_vkGetMemoryFdKHR( device->host.device, &get_fd, &fd )) || fd < 0)
    {
        WARN( "managed vkGetMemoryFdKHR failed, res %d\n", res );
        if (!res) res = VK_ERROR_INVALID_EXTERNAL_HANDLE;
        goto failed;
    }
    image->dmabuf_fd = fd;

    if ((res = device->p_vkGetImageDrmFormatModifierPropertiesEXT( device->host.device, image->image, &mod_props )))
    {
        WARN( "managed vkGetImageDrmFormatModifierPropertiesEXT failed, res %d\n", res );
        goto failed;
    }
    /* Record the swapchain-level realized modifier from the first image. Each
     * image still publishes its own modifier/stride/offset below in case the
     * host picks differently per image. */
    managed->realized_modifier = mod_props.drmFormatModifier;
    wire_modifier = mod_props.drmFormatModifier;
    for (i = 0; i < modifier_count; i++)
    {
        if (modifiers[i] == mod_props.drmFormatModifier)
        {
            wire_modifier = wire_modifiers[i];
            break;
        }
    }

    /* Publish every plane of the realized modifier. A missing auxiliary plane
     * (e.g. AMD DCC) makes the consumer-side dmabuf import fail fatally. */
    plane_count = vk_modifier_plane_count( device, managed->format, mod_props.drmFormatModifier );
    if (!plane_count || plane_count > HWND_DMABUF_MAX_PLANES)
    {
        WARN( "managed image modifier 0x%s has unsupported plane count %u\n",
              wine_dbgstr_longlong(mod_props.drmFormatModifier), plane_count );
        res = VK_ERROR_FORMAT_NOT_SUPPORTED;
        goto failed;
    }

    /* Cache the per-image frame descriptor (filled with the per-frame fields at
     * publish time). */
    memset( &image->desc, 0, sizeof(image->desc) );
    for (i = 0; i < plane_count; i++)
    {
        static const VkImageAspectFlagBits plane_aspects[] =
        {
            VK_IMAGE_ASPECT_MEMORY_PLANE_0_BIT_EXT,
            VK_IMAGE_ASPECT_MEMORY_PLANE_1_BIT_EXT,
            VK_IMAGE_ASPECT_MEMORY_PLANE_2_BIT_EXT,
            VK_IMAGE_ASPECT_MEMORY_PLANE_3_BIT_EXT,
        };
        subresource.aspectMask = plane_aspects[i];
        memset( &layout, 0, sizeof(layout) );
        device->p_vkGetImageSubresourceLayout( device->host.device, image->image, &subresource, &layout );
        image->desc.plane_offsets[i] = (unsigned int)layout.offset;
        image->desc.plane_strides[i] = (unsigned int)layout.rowPitch;
    }
    image->desc.plane_count = plane_count;
    image->desc.version = HWND_DMABUF_DESC_VERSION_V1;
    /* Each ring slot's dmabuf is exported once and dup'd per present. The busy
     * gate blocks re-render until the release token returns. So image_id names a
     * stable dmabuf the consumer may cache and reuse the wl_buffer for. */
    image->desc.flags = HWND_DMABUF_FLAG_STABLE_SLOT;
    image->desc.width = managed->extents.width;
    image->desc.height = managed->extents.height;
    image->desc.fourcc = managed->fourcc;
    image->desc.stride = image->desc.plane_strides[0];
    image->desc.offset = image->desc.plane_offsets[0];
    image->desc.modifier = wire_modifier;
    image->desc.alpha_mode = managed->alpha_mode;
    if (managed->has_color_space)
    {
        image->desc.flags |= HWND_DMABUF_FLAG_COLOR_SPACE;
        image->desc.color_space = managed->color_space;
    }
    image->desc.sync_fd_kind = HWND_DMABUF_SYNC_NONE;
    image->desc.producer_unique_id = 0; /* set from the managed producer id at present */

    image->valid = TRUE;
    image->acquired = image->busy = FALSE;
    return VK_SUCCESS;

failed:
    managed_destroy_image( device, image );
    return res;
}

/* Try to build a wine-managed swapchain. Returns VK_SUCCESS with *out set on
 * success. On any failure returns the error and leaves *out NULL so the caller
 * can fall back to the host swapchain path (we never fail the create call). */
static VkResult managed_swapchain_create( struct vulkan_device *device, struct surface *surface,
                                          const VkSwapchainCreateInfoKHR *create_info,
                                          struct wine_managed_swapchain **out )
{
    uint64_t modifiers[WINE_VK_MANAGED_MAX_MODIFIERS];
    uint64_t wire_modifiers[WINE_VK_MANAGED_MAX_MODIFIERS];
    struct wine_managed_swapchain *managed;
    BOOL opaque_alpha;
    unsigned int fourcc = 0;
    uint32_t modifier_count, count, i;
    unsigned int caps_flags;
    VkImageUsageFlags usage;
    VkResult res;
    unsigned int status;

    *out = NULL;

    opaque_alpha = create_info->compositeAlpha == VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    usage = create_info->imageUsage;

    /* Intersect the compositor's advertised (fourcc, modifier) caps with what the
     * host can export as a single-plane dmabuf, against the realized usage. */
    modifier_count = vk_select_managed_modifiers( device, surface->hwnd, create_info->imageFormat,
                                                  usage | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, opaque_alpha,
                                                  &fourcc, modifiers, wire_modifiers, ARRAY_SIZE(modifiers),
                                                  &caps_flags );
    if (!modifier_count)
    {
        TRACE( "no host-exportable modifier intersection for hwnd %p format %u, falling back to host swapchain\n",
               surface->hwnd, create_info->imageFormat );
        return VK_ERROR_FORMAT_NOT_SUPPORTED;
    }

    if (!(managed = calloc( 1, sizeof(*managed) ))) return VK_ERROR_OUT_OF_HOST_MEMORY;
    pthread_mutex_init( &managed->lock, NULL );
    managed->channel_fd = -1;
    managed->wake_fd = -1;
    managed->hwnd = surface->hwnd;
    if (!(managed->consumer = managed_consumer_get( managed->hwnd )))
    {
        managed_free( device, managed );
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    if (!(caps_flags & HWND_DMABUF_HOST_CAP_CONSUMER_STATE))
        managed_consumer_set_state( managed, WINE_MANAGED_CONSUMER_ACTIVE );
    managed->format = create_info->imageFormat;
    managed->fourcc = fourcc;
    managed->has_color_space =
        managed_color_space_from_vulkan( create_info->imageColorSpace, &managed->color_space );
    /* If the chosen fourcc is the opaque X-variant, tell the compositor to ignore
     * alpha. Otherwise leave alpha as straight. */
    managed->alpha_mode = (fourcc == vk_format_to_drm_fourcc( create_info->imageFormat, TRUE ))
                          ? HWND_DMABUF_ALPHA_MODE_IGNORE : HWND_DMABUF_ALPHA_MODE_UNSPECIFIED;
    managed->extents = create_info->imageExtent;
    managed->usage = usage;
    managed->next_release_token = 0;
    managed->producer_unique_id = InterlockedIncrement64( &managed_next_producer_id );
    if (!managed->producer_unique_id)
        managed->producer_unique_id = InterlockedIncrement64( &managed_next_producer_id );
    managed->ring_generation = 1;

#ifdef HAVE_SYS_EVENTFD_H
    if ((managed->wake_fd = eventfd( 0, EFD_CLOEXEC | EFD_NONBLOCK )) < 0)
    {
        managed_free( device, managed );
        return VK_ERROR_INITIALIZATION_FAILED;
    }
#else
    managed_free( device, managed );
    return VK_ERROR_FEATURE_NOT_PRESENT;
#endif

    status = hwnd_dmabuf_set_pending( managed->hwnd, TRUE );
    if (status != HWND_DMABUF_OK)
    {
        WARN( "failed to mark hwnd %p as pending dmabuf producer, status %u\n",
              managed->hwnd, status );
        managed_free( device, managed );
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    managed->pending_registered = TRUE;

    /* Keep enough images available for asynchronous consumer release. */
    count = max( create_info->minImageCount, 3u );
    if (count > WINE_VK_MANAGED_MAX_IMAGES) count = WINE_VK_MANAGED_MAX_IMAGES;

    /* Pick the first queue for empty acquire-signal submits. */
    if (device->queue_count) managed->signal_queue = device->queues;

    /* Per-frame fence to gate export on just this frame's render (see managed_present). */
    managed->p_vkWaitForFences = (void *)p_vkGetDeviceProcAddr( device->host.device, "vkWaitForFences" );
    managed->p_vkResetFences = (void *)p_vkGetDeviceProcAddr( device->host.device, "vkResetFences" );
    {
        VkFenceCreateInfo fci = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
        if (device->p_vkCreateFence( device->host.device, &fci, NULL, &managed->present_fence ))
            managed->present_fence = VK_NULL_HANDLE;
    }

    managed->explicit_sync = (caps_flags & HWND_DMABUF_HOST_CAP_EXPLICIT_SYNC) &&
                             device->extensions.has_VK_KHR_external_semaphore_fd &&
                             device->p_vkGetSemaphoreFdKHR;

    for (i = 0; i < count; i++)
    {
        if ((res = managed_create_image( device, managed, modifiers, wire_modifiers,
                                         modifier_count, &managed->images[i] )))
        {
            WARN( "failed to create managed image %u, res %d, falling back to host swapchain\n", i, res );
            managed->image_count = i;
            managed_free( device, managed );
            return res;
        }
        managed->images[i].desc.image_id = i;
        managed->image_count = i + 1;
    }

    managed->channel_fd = hwnd_dmabuf_open_channel( surface->hwnd );
    if (managed->channel_fd < 0)
    {
        WARN( "failed to open hwnd %p dmabuf producer channel, falling back to host swapchain\n",
              surface->hwnd );
        managed_free( device, managed );
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    managed->channel_registered = TRUE;
    managed->pending_registered = FALSE;
    managed_consumer_request_state( managed );
    TRACE( "managed swapchain %p hwnd %p socket channel fd %d\n", managed, surface->hwnd, managed->channel_fd );

    *out = managed;
    TRACE( "created managed swapchain %p: %u images %ux%u fourcc %#x modifier 0x%s\n",
           managed, managed->image_count, managed->extents.width, managed->extents.height,
           managed->fourcc, wine_dbgstr_longlong( managed->realized_modifier ) );
    return VK_SUCCESS;
}

/* A managed swapchain has no host acquire operation to signal these objects. */
static VkResult managed_signal_acquire( struct vulkan_device *device, struct wine_managed_swapchain *managed,
                                        VkSemaphore host_semaphore, VkFence host_fence )
{
    VkSubmitInfo submit = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO };
    VkResult res;

    if (!host_semaphore && !host_fence) return VK_SUCCESS;
    if (!managed->signal_queue) return VK_SUCCESS;

    if (host_semaphore)
    {
        submit.signalSemaphoreCount = 1;
        submit.pSignalSemaphores = &host_semaphore;
    }

    vulkan_queue_lock( managed->signal_queue );
    res = device->p_vkQueueSubmit( managed->signal_queue->host.queue, 1, &submit, host_fence );
    vulkan_queue_unlock( managed->signal_queue );
    return res;
}

static void managed_drain_releases( struct wine_managed_swapchain *managed );
static void managed_mark_lost( struct wine_managed_swapchain *managed );

static UINT64 managed_monotonic_time_ns( void )
{
    struct timespec time;

    clock_gettime( CLOCK_MONOTONIC, &time );
    return (UINT64)time.tv_sec * 1000000000 + time.tv_nsec;
}

static void managed_wake_acquire( struct wine_managed_swapchain *managed )
{
    UINT64 value = 1;
    ssize_t ret;

    if (managed->wake_fd < 0) return;
    do ret = write( managed->wake_fd, &value, sizeof(value) );
    while (ret < 0 && errno == EINTR);
}

static void managed_drain_acquire_wake( struct wine_managed_swapchain *managed )
{
    UINT64 value;
    ssize_t ret;

    if (managed->wake_fd < 0) return;
    do ret = read( managed->wake_fd, &value, sizeof(value) );
    while (ret < 0 && errno == EINTR);
}

/* Managed producers are cross-process. Wait here rather than in present. */
static VkResult managed_acquire( struct vulkan_device *device, struct swapchain *swapchain,
                                 UINT64 timeout, VkSemaphore host_semaphore, VkFence host_fence,
                                 uint32_t *image_index )
{
    struct wine_managed_swapchain *managed = swapchain->managed;
    struct surface *surface = swapchain->surface;
    UINT64 start = managed_monotonic_time_ns();
    RECT client_rect;
    VkResult res = VK_SUCCESS;
    uint32_t slot;

    for (;;)
    {
        struct pollfd pfds[WINE_VK_MANAGED_MAX_IMAGES + 2];
        BOOL busy_any = FALSE, valid_any = FALSE, waiting_gpu = FALSE;
        DWORD stall_remaining_ms = INFINITE;
        uint32_t count = 0, i;
        int poll_timeout = -1, ret;

        slot = ~0u;
        pthread_mutex_lock( &managed->lock );
        managed_drain_releases( managed );
        if (managed->lost)
        {
            pthread_mutex_unlock( &managed->lock );
            return VK_ERROR_OUT_OF_DATE_KHR;
        }

        for (i = 0; i < managed->image_count; i++)
        {
            uint32_t idx = (managed->next_acquire + i) % managed->image_count;
            struct wine_managed_image *image = &managed->images[idx];

            if (!image->valid) continue;
            valid_any = TRUE;
            if (image->busy)
            {
                busy_any = TRUE;
                if (!managed_image_completion_ready( image )) waiting_gpu = TRUE;
                continue;
            }
            if (image->acquired) continue;
            if (managed_image_completion_ready( image ))
            {
                slot = idx;
                break;
            }
            waiting_gpu = TRUE;
        }

        if (slot != ~0u)
        {
            managed->ring_full_since_ms = 0;
            managed->images[slot].acquired = TRUE;
            managed->next_acquire = (slot + 1) % managed->image_count;
            *image_index = slot;
            pthread_mutex_unlock( &managed->lock );
            break;
        }
        if (!valid_any)
        {
            pthread_mutex_unlock( &managed->lock );
            return VK_ERROR_OUT_OF_DATE_KHR;
        }

        if (busy_any && !waiting_gpu)
        {
            DWORD now = NtGetTickCount();
            DWORD elapsed;

            if (!managed->ring_full_since_ms) managed->ring_full_since_ms = now;
            elapsed = now - managed->ring_full_since_ms;
            if (elapsed >= WINE_VK_MANAGED_STALL_MS)
            {
                WARN( "managed dmabuf consumer stopped releasing frames\n" );
                managed_mark_lost( managed );
                pthread_mutex_unlock( &managed->lock );
                return VK_ERROR_OUT_OF_DATE_KHR;
            }
            stall_remaining_ms = WINE_VK_MANAGED_STALL_MS - elapsed;
        }
        else
            managed->ring_full_since_ms = 0;

        if (managed->channel_fd >= 0)
            pfds[count++] = (struct pollfd){ .fd = managed->channel_fd, .events = POLLIN };
        if (managed->wake_fd >= 0)
            pfds[count++] = (struct pollfd){ .fd = managed->wake_fd, .events = POLLIN };
        for (i = 0; i < managed->image_count; i++)
            if (managed->images[i].completion_fd >= 0)
                pfds[count++] = (struct pollfd){ .fd = managed->images[i].completion_fd,
                                                 .events = POLLIN };
        pthread_mutex_unlock( &managed->lock );

        if (!timeout) return VK_NOT_READY;
        if (timeout != UINT64_MAX)
        {
            UINT64 elapsed = managed_monotonic_time_ns() - start;
            UINT64 remaining, remaining_ms;

            if (elapsed >= timeout) return VK_TIMEOUT;
            remaining = timeout - elapsed;
            remaining_ms = remaining / 1000000 + !!(remaining % 1000000);
            poll_timeout = remaining_ms > INT_MAX ? INT_MAX : (int)remaining_ms;
        }
        if (stall_remaining_ms != INFINITE &&
            (poll_timeout < 0 || stall_remaining_ms < (DWORD)poll_timeout))
            poll_timeout = stall_remaining_ms;

        do ret = poll( pfds, count, poll_timeout );
        while (ret < 0 && errno == EINTR);
        if (ret < 0)
            return errno == ENOMEM ? VK_ERROR_OUT_OF_HOST_MEMORY : VK_ERROR_OUT_OF_DATE_KHR;
        for (i = 0; i < count; i++)
            if (pfds[i].fd == managed->wake_fd && (pfds[i].revents & POLLIN))
                managed_drain_acquire_wake( managed );
    }

    /* Publish the managed state before submitting the acquire signal. */
    if ((res = managed_signal_acquire( device, managed, host_semaphore, host_fence )))
    {
        pthread_mutex_lock( &managed->lock );
        managed->images[slot].acquired = FALSE;
        managed_wake_acquire( managed );
        pthread_mutex_unlock( &managed->lock );
        return res;
    }

    if (get_swapchain_surface_rect( surface->hwnd, &client_rect, NtUserGetDpiForWindow( surface->hwnd ) ) &&
        !extents_equals( &managed->extents, &client_rect ))
        return VK_SUBOPTIMAL_KHR;

    return VK_SUCCESS;
}

/* Release a busy slot by token. */
static void managed_release_token( struct wine_managed_swapchain *managed, UINT64 release_token, BOOL failed )
{
    uint32_t i;

    if (!release_token) return;
    for (i = 0; i < managed->image_count; i++)
    {
        struct wine_managed_image *image = &managed->images[i];
        if (image->release_token != release_token) continue;
        image->busy = FALSE;
        image->release_token = 0;
        if (failed) image->valid = FALSE;
        managed->ring_full_since_ms = 0;
        managed_wake_acquire( managed );
        return;
    }
}

/* Caller holds managed->lock. */
static void managed_mark_lost( struct wine_managed_swapchain *managed )
{
    uint32_t i;

    managed->lost = TRUE;
    managed->ring_full_since_ms = 0;
    for (i = 0; i < managed->image_count; i++)
    {
        managed->images[i].busy = FALSE;
        managed->images[i].release_token = 0;
    }
    managed_wake_acquire( managed );
}

static BOOL dmabuf_send_error_is_fatal( int err )
{
    return err == EPIPE || err == ECONNRESET || err == ENOTCONN ||
           err == ECONNABORTED || err == ESHUTDOWN || err == EBADF;
}

/* Drain exact release tokens from the consumer. Caller holds managed->lock. */
static void managed_drain_releases( struct wine_managed_swapchain *managed )
{
    hwnd_dmabuf_release_t rel;
    BOOL received = FALSE;
    ssize_t ret;

    if (managed->channel_fd < 0) return;
    for (;;)
    {
        ret = recv( managed->channel_fd, &rel, sizeof(rel), MSG_DONTWAIT );
        if (ret == (ssize_t)sizeof(rel))
        {
            struct wine_managed_image *image;

            received = TRUE;
            /* Zero-token records update the channel's consumer state. */
            if (!rel.release_token)
            {
                if (rel.flags & HWND_DMABUF_RELEASE_CONSUMER_SUSPENDED)
                {
                    TRACE( "hwnd %p consumer suspended\n", managed->hwnd );
                    managed_consumer_set_state( managed, WINE_MANAGED_CONSUMER_SUSPENDED );
                }
                else if (rel.flags & HWND_DMABUF_RELEASE_CONSUMER_ACTIVE)
                {
                    TRACE( "hwnd %p consumer active\n", managed->hwnd );
                    managed_consumer_set_state( managed, WINE_MANAGED_CONSUMER_ACTIVE );
                }
                continue;
            }

            if (rel.producer_unique_id != managed->producer_unique_id) continue;
            /* A release must not resume a suspended consumer. */
            if (managed_consumer_state( managed ) == WINE_MANAGED_CONSUMER_UNKNOWN)
                managed_consumer_set_state( managed, WINE_MANAGED_CONSUMER_ACTIVE );
            if (rel.ring_generation != managed->ring_generation) continue;
            if (rel.image_id >= managed->image_count) continue;

            image = &managed->images[rel.image_id];
            if (!image->release_token || image->release_token != rel.release_token) continue;

            managed_image_completion_ready( image );
            image->busy = FALSE;
            image->release_token = 0;
            image->consumer_cached = !!(rel.flags & HWND_DMABUF_RELEASE_CACHED);
            managed->ring_full_since_ms = 0;
            managed_wake_acquire( managed );
        }
        else if (ret < 0 && errno == EINTR) continue;
        else break;
    }

    if (received && managed_consumer_state( managed ) == WINE_MANAGED_CONSUMER_UNKNOWN)
    {
        InterlockedExchange( &managed->consumer->reannounce_pending, FALSE );
        InterlockedExchange( &managed->consumer->reannounce_delay_ms, 0 );
    }

    if (ret == 0 || (ret < 0 && errno != EAGAIN && errno != EWOULDBLOCK))
    {
        managed_mark_lost( managed );
    }
}

/* Publish one managed image over the dmabuf channel. */
static VkResult managed_present( struct vulkan_device *device, struct swapchain *swapchain, uint32_t image_index,
                                 BOOL present_waits_consumed, int sync_fd )
{
    struct wine_managed_swapchain *managed = swapchain->managed;
    struct surface *surface = swapchain->surface;
    struct wine_managed_image *image;
    hwnd_dmabuf_frame_desc_t desc;
    UINT64 release_token = 0;
    int channel_fd_dup = -1, send_sync_fd = -1;
    enum wine_managed_consumer_state consumer_state;
    BOOL send_frame = FALSE, send_fd = FALSE;
    RECT client_rect;
    VkResult res = VK_SUCCESS;

    if (image_index >= managed->image_count)
    {
        wait_and_close_sync_file( sync_fd );
        return VK_ERROR_OUT_OF_DATE_KHR;
    }

    /* The window changed size, recreate is required. */
    if (!get_surface_rect( surface->hwnd, &client_rect, NtUserGetDpiForWindow( surface->hwnd ) ))
    {
        wait_and_close_sync_file( sync_fd );
        return VK_ERROR_OUT_OF_DATE_KHR;
    }

    if (!present_waits_consumed) vulkan_device_lock_queues( device );
    pthread_mutex_lock( &managed->lock );
    managed_drain_releases( managed );
    if (managed->lost)
    {
        pthread_mutex_unlock( &managed->lock );
        if (!present_waits_consumed) vulkan_device_unlock_queues( device );
        wait_and_close_sync_file( sync_fd );
        return VK_ERROR_OUT_OF_DATE_KHR;
    }
    image = &managed->images[image_index];
    if (!image->valid)
    {
        pthread_mutex_unlock( &managed->lock );
        if (!present_waits_consumed) vulkan_device_unlock_queues( device );
        wait_and_close_sync_file( sync_fd );
        return VK_ERROR_OUT_OF_DATE_KHR;
    }

    /* Explicit synchronization and all-managed fallback presents consume the
     * waits before publishing. Unsupported mixed presents require an idle
     * because the host present owns the wait semaphores. */
    if (!present_waits_consumed)
        res = device->p_vkDeviceWaitIdle( device->host.device );
    if (res < VK_SUCCESS)
    {
        pthread_mutex_unlock( &managed->lock );
        if (!present_waits_consumed) vulkan_device_unlock_queues( device );
        wait_and_close_sync_file( sync_fd );
        return res;
    }

    /* Unsent frames remain producer-owned and need no release token. */
    consumer_state = managed_consumer_state( managed );
    send_frame = consumer_state == WINE_MANAGED_CONSUMER_ACTIVE && managed->channel_fd >= 0;
    send_fd = send_frame && !image->consumer_cached;
    if (send_fd && (channel_fd_dup = dup( image->dmabuf_fd )) < 0)
    {
        image->completion_fd = sync_fd;
        image->acquired = FALSE;
        managed_wake_acquire( managed );
        sync_fd = -1;
        pthread_mutex_unlock( &managed->lock );
        if (!present_waits_consumed) vulkan_device_unlock_queues( device );
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    if (send_frame && sync_fd >= 0 && (send_sync_fd = dup( sync_fd )) < 0)
    {
        if (channel_fd_dup >= 0) close( channel_fd_dup );
        image->completion_fd = sync_fd;
        image->acquired = FALSE;
        managed_wake_acquire( managed );
        sync_fd = -1;
        pthread_mutex_unlock( &managed->lock );
        if (!present_waits_consumed) vulkan_device_unlock_queues( device );
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }

    managed->present_id++;
    if (send_frame)
    {
        /* Assign a fresh release token (never 0: consumer rejects token 0). */
        release_token = ++managed->next_release_token;
        if (!release_token) release_token = ++managed->next_release_token;
        image->release_token = release_token;

        desc = image->desc;
        desc.producer_unique_id = managed->producer_unique_id;
        desc.image_id = image_index;
        desc.ring_generation = managed->ring_generation;
        desc.frame_seq = (unsigned int)managed->present_id;
        desc.release_token = release_token;
        desc.sync_fd_kind = sync_fd >= 0 ? HWND_DMABUF_SYNC_FILE : HWND_DMABUF_SYNC_NONE;
        image->busy = TRUE;
    }
    assert(image->completion_fd < 0);
    image->completion_fd = sync_fd;
    sync_fd = -1;
    image->acquired = FALSE;
    managed_wake_acquire( managed );
    pthread_mutex_unlock( &managed->lock );
    if (!present_waits_consumed) vulkan_device_unlock_queues( device );

    if (send_frame)
    {
        int serr = hwnd_dmabuf_channel_send( managed->channel_fd, &desc,
                                             channel_fd_dup, send_sync_fd );
        if (serr)
        {
            BOOL fatal = dmabuf_send_error_is_fatal( serr );

            pthread_mutex_lock( &managed->lock );
            managed_release_token( managed, release_token, FALSE );
            if (fatal) managed_mark_lost( managed );
            pthread_mutex_unlock( &managed->lock );
            if (fatal) return VK_ERROR_OUT_OF_DATE_KHR;
        }
        else hwnd_dmabuf_post_wake( surface->hwnd, 0 );
    }
    else if (consumer_state == WINE_MANAGED_CONSUMER_UNKNOWN)
        managed_consumer_request_state( managed );

    if (res >= VK_SUCCESS && !IsRectEmpty( &client_rect ) && !extents_equals( &managed->extents, &client_rect ))
        res = VK_SUBOPTIMAL_KHR;

    return res;
}

static void clear_fullscreen_owner( struct surface *surface, UINT64 owner )
{
    if (!owner || !driver_funcs->p_vulkan_surface_fullscreen) return;
    pthread_mutex_lock( &surface->fullscreen_lock );
    InterlockedCompareExchange64( &surface->fullscreen_active_owner, 0,
                                  (LONGLONG)owner );
    driver_funcs->p_vulkan_surface_fullscreen( surface->client, owner,
                                               VULKAN_SURFACE_FULLSCREEN_CLEAR, NULL );
    pthread_mutex_unlock( &surface->fullscreen_lock );
}

static BOOL swapchain_owns_fullscreen( struct swapchain *swapchain )
{
    LONGLONG active_owner;

    if (!swapchain->fullscreen_owner ||
        !ReadAcquire( &swapchain->fullscreen_acquired ))
        return FALSE;

    active_owner = InterlockedCompareExchange64(
        &swapchain->surface->fullscreen_active_owner, 0, 0 );
    return active_owner == (LONGLONG)swapchain->fullscreen_owner;
}

static BOOL swapchain_can_present( struct swapchain *swapchain )
{
    /* An ownership transition may reject one concurrent present. */
    if (!ReadAcquire( &swapchain->fullscreen_acquired )) return TRUE;
    return swapchain_owns_fullscreen( swapchain );
}

static LONGLONG exchange_fullscreen_owner( struct surface *surface, LONGLONG owner )
{
    LONGLONG current, previous;

    /* InterlockedExchange64 is unavailable in 32-bit builds. */
    previous = InterlockedCompareExchange64( &surface->fullscreen_active_owner, 0, 0 );
    while ((current = InterlockedCompareExchange64( &surface->fullscreen_active_owner,
                                                     owner, previous )) != previous)
        previous = current;
    return previous;
}

static VkResult acquire_swapchain_fullscreen( struct swapchain *swapchain )
{
    LONGLONG previous_owner;
    VkResult res;

    if (!swapchain->fullscreen_owner) return VK_ERROR_INITIALIZATION_FAILED;

    pthread_mutex_lock( &swapchain->surface->fullscreen_lock );
    if (swapchain_is_out_of_date( swapchain ))
    {
        pthread_mutex_unlock( &swapchain->surface->fullscreen_lock );
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    if (swapchain_owns_fullscreen( swapchain ))
    {
        pthread_mutex_unlock( &swapchain->surface->fullscreen_lock );
        return VK_SUCCESS;
    }

    previous_owner = exchange_fullscreen_owner( swapchain->surface, 0 );
    res = driver_funcs->p_vulkan_surface_fullscreen(
        swapchain->surface->client, swapchain->fullscreen_owner,
        VULKAN_SURFACE_FULLSCREEN_ACQUIRE, NULL );
    if (!res)
    {
        exchange_fullscreen_owner( swapchain->surface,
                                   (LONGLONG)swapchain->fullscreen_owner );
        InterlockedExchange( &swapchain->fullscreen_acquired, TRUE );
        TRACE( "swapchain %p acquired fullscreen owner %s\n", swapchain,
               wine_dbgstr_longlong( swapchain->fullscreen_owner ) );
    }
    else
        exchange_fullscreen_owner( swapchain->surface, previous_owner );
    pthread_mutex_unlock( &swapchain->surface->fullscreen_lock );
    return res;
}

static VkResult win32u_vkCreateSwapchainKHR( VkDevice client_device, const VkSwapchainCreateInfoKHR *create_info,
                                             const VkAllocationCallbacks *allocator, VkSwapchainKHR *ret )
{
    VkSwapchainPresentScalingCreateInfoEXT scaling = {.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_SCALING_CREATE_INFO_EXT};
    struct swapchain *swapchain, *old_swapchain = swapchain_from_handle( create_info->oldSwapchain );
    struct surface *surface = surface_from_handle( create_info->surface );
    struct vulkan_device *device = vulkan_device_from_handle( client_device );
    struct vulkan_physical_device *physical_device = device->physical_device;
    struct vulkan_instance *instance = physical_device->instance;
    VkSwapchainCreateInfoKHR create_info_host = *create_info;
    const VkSurfaceFullScreenExclusiveWin32InfoEXT *fullscreen_win32_info;
    const VkSurfaceFullScreenExclusiveInfoEXT *fullscreen_info;
    VkFullScreenExclusiveEXT fullscreen_policy;
    struct vulkan_surface_fullscreen_info fullscreen_info_driver;
    struct surface_host *updated_host = NULL;
    LONG generation_before_update;
    VkSurfaceCapabilitiesKHR capabilities;
    VkSwapchainKHR host_swapchain;
    VkColorSpaceKHR mapped_color_space;
    uint32_t format_count = 0;
    VkSurfaceFormatKHR *formats;
    RECT client_rect, fullscreen_rect;
    UINT64 fullscreen_owner = 0;
    VkResult res;
    BOOL topology_updated = FALSE;
    BOOL automatic_fullscreen = FALSE;
    BOOL compositor_scaling;
    BOOL use_fshack;
    BOOL lite;
    float sharpness;

    if (!NtUserIsWindow( surface->hwnd ))
    {
        ERR( "surface %p, hwnd %p is invalid!\n", surface, surface->hwnd );
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    if (surface->client && !(updated_host = surface_host_create( VK_NULL_HANDLE )))
        return VK_ERROR_OUT_OF_HOST_MEMORY;

    fullscreen_info = find_vk_struct( (void *)create_info->pNext,
                                      VK_STRUCTURE_TYPE_SURFACE_FULL_SCREEN_EXCLUSIVE_INFO_EXT );
    fullscreen_win32_info = find_vk_struct(
        (void *)create_info->pNext,
        VK_STRUCTURE_TYPE_SURFACE_FULL_SCREEN_EXCLUSIVE_WIN32_INFO_EXT );
    fullscreen_policy = fullscreen_info ? fullscreen_info->fullScreenExclusive
                                        : VK_FULL_SCREEN_EXCLUSIVE_DEFAULT_EXT;
    TRACE( "surface %p fullscreen policy %u monitor %p\n", surface,
           fullscreen_policy,
           fullscreen_win32_info ? fullscreen_win32_info->hmonitor : NULL );
    if (fullscreen_policy == VK_FULL_SCREEN_EXCLUSIVE_APPLICATION_CONTROLLED_EXT)
    {
        if (!driver_funcs->p_vulkan_surface_fullscreen)
        {
            free( updated_host );
            return VK_ERROR_EXTENSION_NOT_PRESENT;
        }
        if (!fullscreen_win32_info ||
            !get_fullscreen_monitor_rect( fullscreen_win32_info->hmonitor, &fullscreen_rect ))
        {
            free( updated_host );
            return VK_ERROR_INITIALIZATION_FAILED;
        }
    }
    else
    {
        automatic_fullscreen = get_automatic_fullscreen_rect(
            device, surface, fullscreen_policy, fullscreen_win32_info,
            &fullscreen_rect );
    }

    if (fullscreen_policy == VK_FULL_SCREEN_EXCLUSIVE_APPLICATION_CONTROLLED_EXT ||
        automatic_fullscreen)
    {
        fullscreen_info_driver.rect = fullscreen_rect;
        fullscreen_info_driver.target = automatic_fullscreen && !fullscreen_win32_info
                                        ? VULKAN_SURFACE_FULLSCREEN_TARGET_WINDOW
                                        : VULKAN_SURFACE_FULLSCREEN_TARGET_FIXED;
        fullscreen_owner = InterlockedIncrement64( &fullscreen_next_owner );
        if (!fullscreen_owner)
            fullscreen_owner = InterlockedIncrement64( &fullscreen_next_owner );
        pthread_mutex_lock( &surface->fullscreen_lock );
        res = driver_funcs->p_vulkan_surface_fullscreen(
            surface->client, fullscreen_owner, VULKAN_SURFACE_FULLSCREEN_PREPARE,
            &fullscreen_info_driver );
        pthread_mutex_unlock( &surface->fullscreen_lock );
        if (res)
        {
            free( updated_host );
            return res;
        }
        TRACE( "surface %p prepared %s fullscreen owner %s for monitor %s\n",
               surface, automatic_fullscreen ? "automatic" : "application-controlled",
               wine_dbgstr_longlong( fullscreen_owner ), wine_dbgstr_rect( &fullscreen_rect ) );
    }
    else if (old_swapchain && old_swapchain->surface == surface &&
             old_swapchain->fullscreen_owner)
    {
        clear_fullscreen_owner( surface, old_swapchain->fullscreen_owner );
    }

    generation_before_update = ReadAcquire( &surface->client->presentation_generation );

    pthread_mutex_lock( &surface->host_lock );

    /* A window whose direct-presentation eligibility changed after its surface
     * was created (e.g. it went fullscreen, or left fullscreen while promoted)
     * can be re-homed by the driver onto a new host surface. This must happen
     * before the swapchain and per-swapchain surface state (colorspace, alpha)
     * are set up so they apply to the updated surface. The retiring swapchain
     * belongs to the previous host surface, so it cannot be passed as
     * oldSwapchain. */
    if (surface->client)
    {
        for (;;)
        {
            res = driver_funcs->p_vulkan_surface_update( surface->hwnd, instance, surface->client,
                                                         surface_host_handle( surface ), &updated_host->handle,
                                                         &topology_updated );
            if (res != VK_NOT_READY) break;

            /* The driver closed the old presentation generation. Drain host
             * waits outside host_lock, then retry the native topology update. */
            pthread_mutex_unlock( &surface->host_lock );
            client_surface_drain_present_waits( surface->client );
            pthread_mutex_lock( &surface->host_lock );
        }
        if (res)
        {
            pthread_mutex_unlock( &surface->host_lock );
            free( updated_host );
            clear_fullscreen_owner( surface, fullscreen_owner );
            return res;
        }

        if (topology_updated)
        {
            struct surface_host *previous_host = surface->active_host;

            surface->active_host = updated_host;
            TRACE( "surface %p (hwnd %p) topology updated, host surface 0x%s -> 0x%s\n", surface, surface->hwnd,
                  wine_dbgstr_longlong( previous_host->handle ), wine_dbgstr_longlong( updated_host->handle ) );
            list_add_tail( &surface->host_surfaces, &updated_host->entry );
            surface->obj.host.surface = updated_host->handle;
            surface_host_release_if_unused( instance, surface, previous_host );
            old_swapchain = NULL;
        }
        else free( updated_host );
    }

    if (fullscreen_owner)
    {
        RECT host_rect, dst_rect;

        if (!surface->client->funcs->get_presentation_rects ||
            !surface->client->funcs->get_presentation_rects(
                surface->client, &host_rect, &dst_rect ))
        {
            pthread_mutex_unlock( &surface->host_lock );
            clear_fullscreen_owner( surface, fullscreen_owner );
            return VK_ERROR_INITIALIZATION_FAILED;
        }
    }

    if (surface) create_info_host.surface = surface_host_handle( surface );
    create_info_host.oldSwapchain = old_swapchain && old_swapchain->host_surface == surface->active_host
                                    ? old_swapchain->obj.host.swapchain : VK_NULL_HANDLE;

    /* Windows allows client rect to be empty, but host Vulkan often doesn't, adjust extents back to the host capabilities */
    res = instance->p_vkGetPhysicalDeviceSurfaceCapabilitiesKHR( physical_device->host.physical_device, surface_host_handle( surface ), &capabilities );
    if (res)
    {
        pthread_mutex_unlock( &surface->host_lock );
        clear_fullscreen_owner( surface, fullscreen_owner );
        return res;
    }

    mapped_color_space =
        driver_funcs->p_vulkan_map_colorspace( create_info_host.imageColorSpace, surface->client );
    create_info_host.imageColorSpace = mapped_color_space;
    create_info_host.imageExtent.width = max( create_info_host.imageExtent.width, capabilities.minImageExtent.width );
    create_info_host.imageExtent.height = max( create_info_host.imageExtent.height, capabilities.minImageExtent.height );
    compositor_scaling = surface_is_presentation_scaled( surface );

    /* If the swapchain image size is not equal to the presentation size (e.g. because of DPI virtualization or
     * display mode change emulation), MoltenVK's vkQueuePresentKHR returns VK_SUBOPTIMAL_KHR.
     * Create the swapchain with VkSwapchainPresentScalingCreateInfoEXT to avoid this.
     */
    if (!compositor_scaling &&
        get_swapchain_surface_rect( surface->hwnd, &client_rect, NtUserGetWinMonitorDpi( surface->hwnd, MDT_WINE_RAW_DPI ) ) &&
        !extents_equals( &create_info_host.imageExtent, &client_rect ) &&
        instance->extensions.has_VK_EXT_surface_maintenance1 &&
        physical_device->extensions.has_VK_KHR_swapchain_maintenance1)
    {
        scaling.scalingBehavior = VK_PRESENT_SCALING_STRETCH_BIT_EXT;
        scaling.pNext = create_info_host.pNext;
        create_info_host.pNext = &scaling;
    }

    if (!(swapchain = calloc( 1, sizeof(*swapchain) )))
    {
        pthread_mutex_unlock( &surface->host_lock );
        clear_fullscreen_owner( surface, fullscreen_owner );
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }

    swapchain->fullscreen_policy = fullscreen_policy;
    swapchain->fullscreen_owner = fullscreen_owner;
    swapchain->color_space = create_info->imageColorSpace;
    swapchain->uses_color_description =
        mapped_color_space != create_info->imageColorSpace;
    swapchain->host_extents = capabilities.minImageExtent;
    swapchain->compositor_scaling = compositor_scaling;
    if (compositor_scaling)
        TRACE( "Using compositor presentation scaling for hwnd %p swapchain extent %s\n",
               surface->hwnd, debugstr_vkextent2d( &create_info->imageExtent ) );
    use_fshack = !swapchain->compositor_scaling &&
                 surface_get_fshack_config( surface, &create_info->imageExtent,
                                            &swapchain->host_extents, &swapchain->fshack );
    if (use_fshack)
    {
        VkFormat source_view_format;
        BOOL fsr_requested;
        BOOL full_host = swapchain->fshack.dst.left == 0 && swapchain->fshack.dst.top == 0 &&
                         swapchain->fshack.dst.right == (LONG)swapchain->host_extents.width &&
                         swapchain->fshack.dst.bottom == (LONG)swapchain->host_extents.height;

        swapchain->upscaler.color_mode = fs_hack_color_mode_from_colorspace( create_info->imageColorSpace );
        swapchain->upscaler.linear_filter =
            !fs_hack_is_integer() &&
            (create_info->imageExtent.width != (UINT)(swapchain->fshack.dst.right - swapchain->fshack.dst.left) ||
             create_info->imageExtent.height != (UINT)(swapchain->fshack.dst.bottom - swapchain->fshack.dst.top));
        fsr_requested = fs_hack_is_fsr( &lite, &sharpness );
        swapchain->upscaler.is_fsr = fsr_requested &&
                                     swapchain->upscaler.color_mode == FS_HACK_COLOR_SRGB;
        if (fsr_requested && !swapchain->upscaler.is_fsr)
            WARN( "FSR is disabled for colorspace %u\n", create_info->imageColorSpace );
        if (swapchain->upscaler.is_fsr && !full_host)
        {
            WARN( "FSR cannot compose an offset fullscreen destination, using the blit scaler\n" );
            swapchain->upscaler.is_fsr = FALSE;
        }
        swapchain->upscaler.fsr.lite = lite;
        swapchain->upscaler.fsr.sharpness = sharpness;
        create_info_host.imageExtent = swapchain->host_extents;
        if (swapchain->upscaler.color_mode == FS_HACK_COLOR_SRGB)
            create_info_host.imageFormat = swapchain->upscaler.is_fsr ?
                                           VK_FORMAT_B8G8R8A8_SRGB : VK_FORMAT_B8G8R8A8_UNORM;
        else
            create_info_host.imageFormat = srgb_to_unorm( create_info->imageFormat );
        create_info_host.imageUsage = VK_IMAGE_USAGE_STORAGE_BIT;

        swapchain->format = create_info_host.imageFormat;

        if (swapchain->upscaler.is_fsr) {
            swapchain->format = srgb_to_unorm(swapchain->format);
            create_info_host.imageFormat = srgb_to_unorm(create_info_host.imageFormat);
            create_info_host.imageUsage |= VK_IMAGE_USAGE_TRANSFER_DST_BIT; /* XXX: check if supported by surface */
        }

        if (swapchain->upscaler.color_mode == FS_HACK_COLOR_SRGB &&
            unorm_to_srgb( create_info->imageFormat ) == create_info->imageFormat &&
            srgb_to_unorm( create_info->imageFormat ) == create_info->imageFormat)
            FIXME( "Swapchain image format %d has no UNORM/SRGB format pair; colors may be incorrect.\n",
                   create_info->imageFormat );

        if (!(capabilities.supportedUsageFlags & VK_IMAGE_USAGE_STORAGE_BIT))
        {
            if (swapchain->upscaler.color_mode == FS_HACK_COLOR_SRGB)
                FIXME( "Swapchain does not support required VK_IMAGE_USAGE_STORAGE_BIT\n" );
            else
            {
                ERR( "Swapchain does not support storage images for colorspace %u\n",
                     create_info->imageColorSpace );
                pthread_mutex_unlock( &surface->host_lock );
                free( swapchain );
                clear_fullscreen_owner( surface, fullscreen_owner );
                return VK_ERROR_INITIALIZATION_FAILED;
            }
        }

        source_view_format = swapchain->upscaler.is_fsr ||
                             swapchain->upscaler.color_mode != FS_HACK_COLOR_SRGB ?
                             srgb_to_unorm( create_info->imageFormat ) :
                             unorm_to_srgb( create_info->imageFormat );
        if (swapchain->upscaler.color_mode != FS_HACK_COLOR_SRGB &&
            (!fs_hack_format_supports( physical_device, source_view_format,
                                       VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT ) ||
             !fs_hack_format_supports( physical_device, create_info_host.imageFormat,
                                       VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT )))
        {
            ERR( "Cannot preserve format %u for fullscreen scaling\n", create_info->imageFormat );
            pthread_mutex_unlock( &surface->host_lock );
            free( swapchain );
            clear_fullscreen_owner( surface, fullscreen_owner );
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        if (swapchain->upscaler.color_mode == FS_HACK_COLOR_RAW &&
            swapchain->upscaler.linear_filter &&
            !fs_hack_format_supports( physical_device, source_view_format,
                                      VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT ))
        {
            WARN( "Format %u does not support linear filtering, using nearest\n",
                  create_info->imageFormat );
            swapchain->upscaler.linear_filter = FALSE;
        }
    }

    /* check if the new colorspace works with the provided format */
    if (create_info_host.imageColorSpace != create_info->imageColorSpace)
    {
        BOOL found = FALSE;

        res = instance->p_vkGetPhysicalDeviceSurfaceFormatsKHR( physical_device->host.physical_device, surface_host_handle( surface ), &format_count, NULL );
        if (res)
        {
            pthread_mutex_unlock( &surface->host_lock );
            free( swapchain );
            clear_fullscreen_owner( surface, fullscreen_owner );
            return res;
        }

        if (!(formats = calloc( format_count, sizeof(*formats) )))
        {
            pthread_mutex_unlock( &surface->host_lock );
            free( swapchain );
            clear_fullscreen_owner( surface, fullscreen_owner );
            return VK_ERROR_OUT_OF_HOST_MEMORY;
        }

        res = instance->p_vkGetPhysicalDeviceSurfaceFormatsKHR( physical_device->host.physical_device, surface_host_handle( surface ), &format_count, formats );

        if (res)
        {
            pthread_mutex_unlock( &surface->host_lock );
            free( formats );
            free( swapchain );
            clear_fullscreen_owner( surface, fullscreen_owner );
            return res;
        }

    again:
        for (unsigned i = 0; i < format_count; i++)
        {
            if (formats[i].format == create_info_host.imageFormat &&
                formats[i].colorSpace == create_info_host.imageColorSpace)
                found = TRUE;
        }

        /* HACK: try again with VK_COLOR_SPACE_SRGB_NONLINEAR_KHR */
        if (!found && create_info_host.imageColorSpace == VK_COLOR_SPACE_PASS_THROUGH_EXT)
        {
            create_info_host.imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
            goto again;
        }

        if (!found)
        {
            ERR("Colorspace %u is not compatible with format %u\n",
                create_info_host.imageColorSpace, create_info_host.imageFormat);
            create_info_host.imageColorSpace = create_info->imageColorSpace;
        }

        free( formats );
    }

    swapchain->has_alpha = !(create_info_host.compositeAlpha & VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR);
    if (swapchain->has_alpha) surface->alpha_swapchain_count++;
    surface_update_client_alpha( surface );
    InterlockedIncrement( &surface->client->busy_ref );

    /* Interpose a managed cross-process producer only when the window advertises
     * HWND dmabuf caps (an off-screen child whose toplevel has no wl_surface) and
     * the host supports the DRM-modifier and dmabuf external-memory extensions.
     * On-screen windows return no caps and use the host swapchain path below. */
    if (device->extensions.has_VK_EXT_image_drm_format_modifier &&
        device->extensions.has_VK_EXT_external_memory_dma_buf)
    {
        struct wine_managed_swapchain *managed = NULL;
        hwnd_dmabuf_host_caps_t probe_caps = {0};
        UINT probe_count = 0;
        UINT probe_status;

        probe_status = hwnd_dmabuf_get_caps( surface->hwnd, &probe_caps, NULL, 0, &probe_count );
        if (probe_status == HWND_DMABUF_OK && probe_count)
        {
            /* Use the host-clamped extents so the dmabuf size matches the window. */
            VkSwapchainCreateInfoKHR managed_info = *create_info;
            managed_info.imageExtent = create_info_host.imageExtent;

            res = managed_swapchain_create( device, surface, &managed_info, &managed );
            if (res == VK_SUCCESS && managed)
            {
                /* host.swapchain stays VK_NULL_HANDLE for managed swapchains. */
                vulkan_object_init( &swapchain->obj.obj, (UINT_PTR)VK_NULL_HANDLE );
                swapchain->surface = surface;
                swapchain->extents = managed->extents;
                swapchain->managed = managed;
                swapchain->presentation_generation = get_swapchain_presentation_generation(
                        surface, topology_updated, generation_before_update );
                instance->p_insert_object( instance, &swapchain->obj.obj );
                set_window_pixel_format( surface->hwnd, -1, TRUE );
                TRACE( "hwnd %p -> wine-managed swapchain %p (cross-process dmabuf producer)\n",
                       surface->hwnd, swapchain );
                pthread_mutex_unlock( &surface->host_lock );

                if (automatic_fullscreen && (res = acquire_swapchain_fullscreen( swapchain )))
                {
                    win32u_vkDestroySwapchainKHR( client_device,
                                                  swapchain->obj.client.swapchain, NULL );
                    return res;
                }

                *ret = swapchain->obj.client.swapchain;
                return VK_SUCCESS;
            }
            /* Any failure -> fall through to the host swapchain path (never fail). */
            TRACE( "managed swapchain build failed (res %d) for hwnd %p, using host swapchain\n",
                   res, surface->hwnd );
        }
        else
        {
            TRACE( "managed dmabuf unavailable for hwnd %p (status %u, formats %u), using host swapchain\n",
                   surface->hwnd, probe_status, probe_count );
        }
    }
    else
    {
        TRACE( "managed dmabuf unavailable for hwnd %p (modifier %u, external memory %u), using host swapchain\n",
               surface->hwnd, device->extensions.has_VK_EXT_image_drm_format_modifier,
               device->extensions.has_VK_EXT_external_memory_dma_buf );
    }

    if ((res = device->p_vkCreateSwapchainKHR( device->host.device, &create_info_host, NULL, &host_swapchain )))
    {
        if (swapchain->has_alpha)
        {
            if (surface->alpha_swapchain_count) surface->alpha_swapchain_count--;
            else ERR( "surface %p alpha swapchain count underflow\n", surface );
            surface_update_client_alpha( surface );
        }
        pthread_mutex_unlock( &surface->host_lock );
        InterlockedDecrement( &surface->client->busy_ref );
        free( swapchain );
        clear_fullscreen_owner( surface, fullscreen_owner );
        return res;
    }

    vulkan_object_init( &swapchain->obj.obj, host_swapchain );
    swapchain->surface = surface;
    swapchain->host_surface = surface->active_host;
    swapchain->presentation_generation = get_swapchain_presentation_generation(
            surface, topology_updated, generation_before_update );
    surface->swapchain = swapchain;
    swapchain->extents = create_info->imageExtent;
    instance->p_insert_object( instance, &swapchain->obj.obj );
    swapchain->host_surface->swapchain_count++;
    pthread_mutex_unlock( &surface->host_lock );

    if (swapchain->fshack.enabled)
    {
        if ((res = init_fs_hack_images( device, swapchain, create_info )))
        {
            ERR( "creating fs hack images failed: %d\n", res );
            win32u_vkDestroySwapchainKHR( client_device, swapchain->obj.client.swapchain, NULL );
            return res;
        }

        if ((res = init_compute_state(device, swapchain)))
        {
            ERR( "creating blit images failed: %d\n", res );
            win32u_vkDestroySwapchainKHR( client_device, swapchain->obj.client.swapchain, NULL );
            return res;
        }

        WARN( "Enabled fullscreen hack on swapchain %p, scaling from %s to %s in %s\n", swapchain,
              debugstr_vkextent2d(&swapchain->extents), wine_dbgstr_rect(&swapchain->fshack.dst),
              debugstr_vkextent2d(&swapchain->host_extents) );
    }

    if (automatic_fullscreen && (res = acquire_swapchain_fullscreen( swapchain )))
    {
        win32u_vkDestroySwapchainKHR( client_device, swapchain->obj.client.swapchain, NULL );
        return res;
    }

    set_window_pixel_format( surface->hwnd, -1, TRUE );

    *ret = swapchain->obj.client.swapchain;
    return VK_SUCCESS;
}

void win32u_vkDestroySwapchainKHR( VkDevice client_device, VkSwapchainKHR client_swapchain,
                                   const VkAllocationCallbacks *allocator )
{
    struct vulkan_device *device = vulkan_device_from_handle( client_device );
    struct vulkan_instance *instance = device->physical_device->instance;
    struct swapchain *swapchain = swapchain_from_handle( client_swapchain );
    struct surface *surface;

    if (allocator) FIXME( "Support for allocation callbacks not implemented yet\n" );
    if (!swapchain) return;

    clear_fullscreen_owner( swapchain->surface, swapchain->fullscreen_owner );

    if (swapchain->fshack.enabled && !swapchain->managed)
    {
        for (uint32_t i = 0; i < swapchain->n_images; ++i)
        {
            destroy_fs_hack_image( device, swapchain, &swapchain->fs_hack_images[i] );
        }
        for (uint32_t i = 0; i < device->queue_count; ++i)
        {
            if (!swapchain->cmd_pools[i]) continue;
            device->p_vkDestroyCommandPool( device->host.device, swapchain->cmd_pools[i], NULL );
        }

        destroy_pipeline(device, &swapchain->blit_pipeline);
        destroy_pipeline(device, &swapchain->fsr_easu_pipeline);
        destroy_pipeline(device, &swapchain->fsr_rcas_pipeline);
        device->p_vkDestroyDescriptorSetLayout( device->host.device, swapchain->descriptor_set_layout, NULL );
        device->p_vkDestroyDescriptorPool( device->host.device, swapchain->descriptor_pool, NULL );
        device->p_vkDestroySampler( device->host.device, swapchain->sampler, NULL );
        device->p_vkFreeMemory( device->host.device, swapchain->user_image_memory, NULL );
        device->p_vkFreeMemory(device->host.device, swapchain->fsr_image_memory, NULL);
        free( swapchain->cmd_pools );
        free( swapchain->fs_hack_images );
    }

    surface = swapchain->surface;
    if (surface) pthread_mutex_lock( &surface->host_lock );

    if (swapchain->managed)
    {
        /* managed: idle, close fds, destroy images+memory, free. No host swapchain. */
        managed_free( device, swapchain->managed );
        swapchain->managed = NULL;
    }
    else
    {
        device->p_vkDestroySwapchainKHR( device->host.device, swapchain->obj.host.swapchain, NULL );
    }
    if (surface)
    {
        if (surface->swapchain == swapchain) surface->swapchain = NULL;
        if (swapchain->host_surface)
        {
            if (swapchain->host_surface->swapchain_count)
                swapchain->host_surface->swapchain_count--;
            else
                ERR( "surface host 0x%s has no swapchain reference\n",
                     wine_dbgstr_longlong( swapchain->host_surface->handle ) );
            surface_host_release_if_unused( instance, surface, swapchain->host_surface );
        }
        if (swapchain->has_alpha)
        {
            if (surface->alpha_swapchain_count) surface->alpha_swapchain_count--;
            else ERR( "surface %p alpha swapchain count underflow\n", surface );
            surface_update_client_alpha( surface );
        }
        pthread_mutex_unlock( &surface->host_lock );
        InterlockedDecrement( &surface->client->busy_ref );
    }
    instance->p_remove_object( instance, &swapchain->obj.obj );

    free( swapchain );
}

static VkResult win32u_vkAcquireFullScreenExclusiveModeEXT( VkDevice,
                                                             VkSwapchainKHR client_swapchain )
{
    struct swapchain *swapchain = swapchain_from_handle( client_swapchain );
    VkResult res;

    if (!swapchain || swapchain_is_out_of_date( swapchain ) ||
        swapchain->fullscreen_policy != VK_FULL_SCREEN_EXCLUSIVE_APPLICATION_CONTROLLED_EXT ||
        !swapchain->fullscreen_owner)
        return VK_ERROR_INITIALIZATION_FAILED;

    res = acquire_swapchain_fullscreen( swapchain );
    if (res == VK_ERROR_FULL_SCREEN_EXCLUSIVE_MODE_LOST_EXT)
        res = VK_ERROR_INITIALIZATION_FAILED;
    return res;
}

static VkResult win32u_vkReleaseFullScreenExclusiveModeEXT( VkDevice,
                                                             VkSwapchainKHR client_swapchain )
{
    struct swapchain *swapchain = swapchain_from_handle( client_swapchain );
    VkResult res;

    if (!swapchain || swapchain->fullscreen_policy !=
                      VK_FULL_SCREEN_EXCLUSIVE_APPLICATION_CONTROLLED_EXT ||
        !swapchain->fullscreen_owner)
        return VK_ERROR_SURFACE_LOST_KHR;

    pthread_mutex_lock( &swapchain->surface->fullscreen_lock );
    if (!swapchain_owns_fullscreen( swapchain ))
    {
        pthread_mutex_unlock( &swapchain->surface->fullscreen_lock );
        return VK_SUCCESS;
    }

    InterlockedExchange( &swapchain->fullscreen_acquired, FALSE );
    InterlockedCompareExchange64( &swapchain->surface->fullscreen_active_owner,
                                  0, (LONGLONG)swapchain->fullscreen_owner );

    res = driver_funcs->p_vulkan_surface_fullscreen(
        swapchain->surface->client, swapchain->fullscreen_owner,
        VULKAN_SURFACE_FULLSCREEN_RELEASE, NULL );
    if (res == VK_ERROR_FULL_SCREEN_EXCLUSIVE_MODE_LOST_EXT)
        res = VK_ERROR_SURFACE_LOST_KHR;
    if (!res)
        TRACE( "swapchain %p released fullscreen owner %s\n", swapchain,
               wine_dbgstr_longlong( swapchain->fullscreen_owner ) );
    pthread_mutex_unlock( &swapchain->surface->fullscreen_lock );
    return res;
}

static VkResult win32u_vkAcquireNextImage2KHR( VkDevice client_device, const VkAcquireNextImageInfoKHR *acquire_info,
                                               uint32_t *image_index )
{
    struct vulkan_semaphore *semaphore = acquire_info->semaphore ? vulkan_semaphore_from_handle( acquire_info->semaphore ) : NULL;
    struct vulkan_fence *fence = acquire_info->fence ? vulkan_fence_from_handle( acquire_info->fence ) : NULL;
    struct swapchain *swapchain = swapchain_from_handle( acquire_info->swapchain );
    struct vulkan_device *device = vulkan_device_from_handle( client_device );
    VkAcquireNextImageInfoKHR acquire_info_host = *acquire_info;
    struct surface *surface;
    RECT client_rect;
    VkResult res;

    if (!swapchain || swapchain_is_out_of_date( swapchain )) return VK_ERROR_OUT_OF_DATE_KHR;
    if (!swapchain_can_present( swapchain ))
        return VK_ERROR_FULL_SCREEN_EXCLUSIVE_MODE_LOST_EXT;

    surface = swapchain->surface;

    if (swapchain->managed)
        return managed_acquire( device, swapchain, acquire_info->timeout,
                                semaphore ? semaphore->host.semaphore : 0,
                                fence ? fence->host.fence : 0, image_index );

    acquire_info_host.swapchain = swapchain->obj.host.swapchain;
    acquire_info_host.semaphore = semaphore ? semaphore->host.semaphore : 0;
    acquire_info_host.fence = fence ? fence->host.fence : 0;
    res = device->p_vkAcquireNextImage2KHR( device->host.device, &acquire_info_host, image_index );

    if (!res && swapchain_presentation_config_changed( swapchain ))
    {
        WARN( "window %p swapchain %p presentation configuration changed, returning VK_SUBOPTIMAL_KHR\n",
              surface->hwnd, swapchain );
        return VK_SUBOPTIMAL_KHR;
    }

    if (!res && !swapchain->fshack.enabled && !swapchain->compositor_scaling &&
        get_swapchain_surface_rect( surface->hwnd, &client_rect, NtUserGetDpiForWindow( surface->hwnd ) ) &&
        !extents_equals( &swapchain->extents, &client_rect ))
    {
        WARN( "Swapchain size %dx%d does not match client rect %s, returning VK_SUBOPTIMAL_KHR\n",
              swapchain->extents.width, swapchain->extents.height, wine_dbgstr_rect( &client_rect ) );
        return VK_SUBOPTIMAL_KHR;
    }

    return res;
}

static VkResult win32u_vkAcquireNextImageKHR( VkDevice client_device, VkSwapchainKHR client_swapchain, uint64_t timeout,
                                              VkSemaphore client_semaphore, VkFence client_fence, uint32_t *image_index )
{
    struct vulkan_semaphore *semaphore = client_semaphore ? vulkan_semaphore_from_handle( client_semaphore ) : NULL;
    struct vulkan_fence *fence = client_fence ? vulkan_fence_from_handle( client_fence ) : NULL;
    struct swapchain *swapchain = swapchain_from_handle( client_swapchain );
    struct vulkan_device *device = vulkan_device_from_handle( client_device );
    struct surface *surface;
    RECT client_rect;
    VkResult res;

    if (!swapchain || swapchain_is_out_of_date( swapchain )) return VK_ERROR_OUT_OF_DATE_KHR;
    if (!swapchain_can_present( swapchain ))
        return VK_ERROR_FULL_SCREEN_EXCLUSIVE_MODE_LOST_EXT;

    surface = swapchain->surface;

    if (swapchain->managed)
        return managed_acquire( device, swapchain, timeout,
                                semaphore ? semaphore->host.semaphore : 0,
                                fence ? fence->host.fence : 0, image_index );

    res = device->p_vkAcquireNextImageKHR( device->host.device, swapchain->obj.host.swapchain, timeout,
                                              semaphore ? semaphore->host.semaphore : 0, fence ? fence->host.fence : 0,
                                              image_index );

    if (!res && swapchain_presentation_config_changed( swapchain ))
    {
        WARN( "window %p swapchain %p presentation configuration changed, returning VK_SUBOPTIMAL_KHR\n",
              surface->hwnd, swapchain );
        return VK_SUBOPTIMAL_KHR;
    }

    if (!res && !swapchain->fshack.enabled && !swapchain->compositor_scaling &&
        get_swapchain_surface_rect( surface->hwnd, &client_rect, NtUserGetDpiForWindow( surface->hwnd ) ) &&
        !extents_equals( &swapchain->extents, &client_rect ))
    {
        WARN( "Swapchain size %dx%d does not match client rect %s, returning VK_SUBOPTIMAL_KHR\n",
              swapchain->extents.width, swapchain->extents.height, wine_dbgstr_rect( &client_rect ) );
        return VK_SUBOPTIMAL_KHR;
    }

    return res;
}

static BOOL should_skip_wait( HWND hwnd )
{
    if (!NtUserIsWindowVisible( hwnd ))
    {
        WARN( "hwnd=%p not yet visible!\n", hwnd );
        return TRUE;
    }

    return FALSE;
}

/* Keep host waits interruptible across presentation changes. */
#define WINE_VK_PRESENT_WAIT_SLICE_NS (100 * 1000000ull)
/* Recover infinite waits when presentation feedback stalls. */
#define WINE_VK_PRESENT_WAIT_STALL_NS (3000 * 1000000ull)

static VkResult swapchain_wait_for_present( struct vulkan_device *device, struct swapchain *swapchain,
                                            uint64_t present_id, uint64_t timeout,
                                            const VkPresentWait2InfoKHR *info )
{
    struct client_surface *client = swapchain->surface->client;
    BOOL infinite = timeout == UINT64_MAX;
    uint64_t stalled = 0;
    VkResult res;

    for (;;)
    {
        uint64_t slice = min( timeout, WINE_VK_PRESENT_WAIT_SLICE_NS );

        if (swapchain_is_out_of_date( swapchain )) return VK_ERROR_OUT_OF_DATE_KHR;
        if (!client_surface_begin_present_wait( client, swapchain->presentation_generation ))
            return VK_ERROR_OUT_OF_DATE_KHR;

        if (info)
        {
            VkPresentWait2InfoKHR slice_info = *info;
            slice_info.timeout = slice;
            res = device->p_vkWaitForPresent2KHR( device->host.device, swapchain->obj.host.swapchain,
                                                  &slice_info );
        }
        else
        {
            res = device->p_vkWaitForPresentKHR( device->host.device, swapchain->obj.host.swapchain,
                                                 present_id, slice );
        }

        client_surface_end_present_wait( client );

        if (res != VK_TIMEOUT) return res;

        if (should_skip_wait( swapchain->surface->hwnd )) return VK_SUCCESS;

        if (infinite && (stalled += slice) >= WINE_VK_PRESENT_WAIT_STALL_NS)
        {
            client_surface_invalidate_presentation( client );
            WARN( "hwnd %p swapchain %p present wait stalled, returning VK_ERROR_OUT_OF_DATE_KHR\n",
                  swapchain->surface->hwnd, swapchain );
            return VK_ERROR_OUT_OF_DATE_KHR;
        }

        if (infinite) continue;
        if (timeout <= slice) return VK_TIMEOUT;
        timeout -= slice;
    }
}

static VkResult win32u_vkWaitForPresentKHR( VkDevice client_device, VkSwapchainKHR client_swapchain,
                                            uint64_t presentId, uint64_t timeout )
{
    struct vulkan_device *device = vulkan_device_from_handle( client_device );
    struct swapchain *swapchain = swapchain_from_handle( client_swapchain );

    if (!swapchain || swapchain_is_out_of_date( swapchain )) return VK_ERROR_OUT_OF_DATE_KHR;

    /* Managed swapchains have no host swapchain to wait on. Wine paces the present
     * itself. Report presentation as proceeding. */
    if (swapchain->managed) return VK_SUCCESS;

    if (swapchain->surface && should_skip_wait( swapchain->surface->hwnd )) return VK_SUCCESS;

    return swapchain_wait_for_present( device, swapchain, presentId, timeout, NULL );
}

static VkResult win32u_vkWaitForPresent2KHR( VkDevice client_device, VkSwapchainKHR client_swapchain,
                                             const VkPresentWait2InfoKHR *info )
{
    struct vulkan_device *device = vulkan_device_from_handle( client_device );
    struct swapchain *swapchain = swapchain_from_handle( client_swapchain );

    if (!swapchain || swapchain_is_out_of_date( swapchain )) return VK_ERROR_OUT_OF_DATE_KHR;

    if (swapchain->managed) return VK_SUCCESS;

    if (swapchain->surface && should_skip_wait( swapchain->surface->hwnd )) return VK_SUCCESS;

    return swapchain_wait_for_present( device, swapchain, info->presentId, info->timeout, info );
}

static VkResult win32u_vkGetSwapchainImagesKHR( VkDevice client_device, VkSwapchainKHR client_swapchain,
                                                uint32_t *count, VkImage *images )
{
    struct vulkan_device *device = vulkan_device_from_handle( client_device );
    struct swapchain *swapchain = swapchain_from_handle( client_swapchain );
    uint32_t i;

    if (!swapchain || swapchain_is_out_of_date( swapchain )) return VK_ERROR_OUT_OF_DATE_KHR;

    if (swapchain->managed)
    {
        struct wine_managed_swapchain *managed = swapchain->managed;
        uint32_t n;

        if (!images)
        {
            *count = managed->image_count;
            return VK_SUCCESS;
        }
        n = min( *count, managed->image_count );
        for (i = 0; i < n; i++) images[i] = managed->images[i].image; /* raw host VkImage */
        *count = n;
        return n < managed->image_count ? VK_INCOMPLETE : VK_SUCCESS;
    }

    if (images && swapchain->fshack.enabled)
    {
        if (*count > swapchain->n_images) *count = swapchain->n_images;
        for (i = 0; i < *count; ++i) images[i] = swapchain->fs_hack_images[i].user_image;
        return *count == swapchain->n_images ? VK_SUCCESS : VK_INCOMPLETE;
    }

    return device->p_vkGetSwapchainImagesKHR( device->host.device, swapchain->obj.host.swapchain, count, images );
}

static VkResult managed_swapchain_get_status( struct swapchain *swapchain, UINT64 *present_id )
{
    struct wine_managed_swapchain *managed = swapchain->managed;
    VkResult res = VK_SUCCESS;

    pthread_mutex_lock( &managed->lock );
    managed_drain_releases( managed );
    if (present_id) *present_id = managed->present_id;
    if (managed->lost) res = VK_ERROR_OUT_OF_DATE_KHR;
    pthread_mutex_unlock( &managed->lock );
    return res;
}

static VkResult win32u_vkSetSwapchainPresentTimingQueueSizeEXT( VkDevice client_device,
                                                                VkSwapchainKHR client_swapchain, uint32_t size )
{
    struct vulkan_device *device = vulkan_device_from_handle( client_device );
    struct swapchain *swapchain = swapchain_from_handle( client_swapchain );

    if (!swapchain || swapchain_is_out_of_date( swapchain )) return VK_ERROR_OUT_OF_DATE_KHR;

    if (swapchain->managed) return managed_swapchain_get_status( swapchain, NULL );

    return device->p_vkSetSwapchainPresentTimingQueueSizeEXT( device->host.device,
                                                              swapchain->obj.host.swapchain, size );
}

static VkResult win32u_vkGetSwapchainTimingPropertiesEXT( VkDevice client_device, VkSwapchainKHR client_swapchain,
                                                          VkSwapchainTimingPropertiesEXT *properties,
                                                          uint64_t *counter )
{
    struct vulkan_device *device = vulkan_device_from_handle( client_device );
    struct swapchain *swapchain = swapchain_from_handle( client_swapchain );
    VkResult res;

    if (!swapchain || swapchain_is_out_of_date( swapchain )) return VK_ERROR_OUT_OF_DATE_KHR;

    if (swapchain->managed)
    {
        res = managed_swapchain_get_status( swapchain, NULL );
        if (res < VK_SUCCESS) return res;
        properties->refreshDuration = 0;
        properties->refreshInterval = 0;
        if (counter) *counter = 0;
        return VK_SUCCESS;
    }

    return device->p_vkGetSwapchainTimingPropertiesEXT( device->host.device, swapchain->obj.host.swapchain,
                                                        properties, counter );
}

static VkResult win32u_vkGetPastPresentationTimingEXT( VkDevice client_device,
                                                       const VkPastPresentationTimingInfoEXT *info,
                                                       VkPastPresentationTimingPropertiesEXT *properties )
{
    struct vulkan_device *device = vulkan_device_from_handle( client_device );
    struct swapchain *swapchain;
    VkPastPresentationTimingInfoEXT info_host = *info;
    UINT64 present_id = 0;
    VkResult res;

    swapchain = swapchain_from_handle( info->swapchain );
    if (!swapchain || swapchain_is_out_of_date( swapchain )) return VK_ERROR_OUT_OF_DATE_KHR;

    if (swapchain->managed)
    {
        res = managed_swapchain_get_status( swapchain, &present_id );
        if (res < VK_SUCCESS) return res;
        properties->timingPropertiesCounter = present_id;
        properties->timeDomainsCounter = 0;
        properties->presentationTimingCount = 0;
        return VK_SUCCESS;
    }

    info_host.swapchain = swapchain->obj.host.swapchain;
    return device->p_vkGetPastPresentationTimingEXT( device->host.device, &info_host, properties );
}

static VkResult managed_release_swapchain_images( struct swapchain *swapchain,
                                                  const VkReleaseSwapchainImagesInfoKHR *info )
{
    struct wine_managed_swapchain *managed = swapchain->managed;
    VkResult res = VK_SUCCESS;

    pthread_mutex_lock( &managed->lock );
    managed_drain_releases( managed );
    if (managed->lost) res = VK_ERROR_OUT_OF_DATE_KHR;
    else
    {
        for (uint32_t i = 0; i < info->imageIndexCount; i++)
        {
            uint32_t image_index = info->pImageIndices[i];
            if (image_index >= managed->image_count)
            {
                res = VK_ERROR_OUT_OF_DATE_KHR;
                break;
            }
            managed->images[image_index].acquired = FALSE;
            managed->ring_full_since_ms = 0;
        }
        managed_wake_acquire( managed );
    }
    pthread_mutex_unlock( &managed->lock );
    return res;
}

static VkResult win32u_vkReleaseSwapchainImagesKHR( VkDevice client_device,
                                                    const VkReleaseSwapchainImagesInfoKHR *release_info )
{
    struct vulkan_device *device = vulkan_device_from_handle( client_device );
    struct swapchain *swapchain = swapchain_from_handle( release_info->swapchain );
    VkReleaseSwapchainImagesInfoKHR release_info_host = *release_info;

    if (!swapchain || swapchain_is_out_of_date( swapchain )) return VK_ERROR_OUT_OF_DATE_KHR;

    if (swapchain->managed) return managed_release_swapchain_images( swapchain, release_info );

    release_info_host.swapchain = swapchain->obj.host.swapchain;
    return device->p_vkReleaseSwapchainImagesKHR( device->host.device, &release_info_host );
}

static VkResult win32u_vkReleaseSwapchainImagesEXT( VkDevice client_device,
                                                    const VkReleaseSwapchainImagesInfoKHR *release_info )
{
    struct vulkan_device *device = vulkan_device_from_handle( client_device );
    struct swapchain *swapchain = swapchain_from_handle( release_info->swapchain );
    VkReleaseSwapchainImagesInfoKHR release_info_host = *release_info;

    if (!swapchain || swapchain_is_out_of_date( swapchain )) return VK_ERROR_OUT_OF_DATE_KHR;

    if (swapchain->managed) return managed_release_swapchain_images( swapchain, release_info );

    release_info_host.swapchain = swapchain->obj.host.swapchain;
    return device->p_vkReleaseSwapchainImagesEXT( device->host.device, &release_info_host );
}

static void win32u_vkSetHdrMetadataEXT( VkDevice client_device, uint32_t swapchain_count,
                                        const VkSwapchainKHR *client_swapchains,
                                        const VkHdrMetadataEXT *metadata )
{
    VkSwapchainKHR stack_swapchains[16], *host_swapchains = stack_swapchains;
    VkHdrMetadataEXT stack_metadata[16], *host_metadata = stack_metadata;
    struct vulkan_device *device = vulkan_device_from_handle( client_device );
    uint32_t host_count = 0;

    if (swapchain_count > ARRAY_SIZE(stack_swapchains))
    {
        host_swapchains = malloc( swapchain_count * sizeof(*host_swapchains) );
        host_metadata = malloc( swapchain_count * sizeof(*host_metadata) );
        if (!host_swapchains || !host_metadata)
        {
            WARN( "failed to allocate HDR metadata arrays\n" );
            free( host_swapchains );
            free( host_metadata );
            return;
        }
    }

    for (uint32_t i = 0; i < swapchain_count; i++)
    {
        struct swapchain *swapchain = swapchain_from_handle( client_swapchains[i] );
        if (!swapchain || swapchain_is_out_of_date( swapchain )) continue;
        /* HDR metadata does not define the Windows swapchain encoding. */
        if (swapchain->managed) continue;
        host_swapchains[host_count] = swapchain->obj.host.swapchain;
        host_metadata[host_count] = metadata[i];
        host_count++;
    }

    if (host_count)
        device->p_vkSetHdrMetadataEXT( device->host.device, host_count, host_swapchains, host_metadata );

    if (host_swapchains != stack_swapchains)
    {
        free( host_swapchains );
        free( host_metadata );
    }
}

static VkCommandBuffer create_hack_cmd( struct vulkan_queue *queue, struct swapchain *swapchain, uint32_t queue_idx )
{
    VkCommandBufferAllocateInfo allocInfo = {0};
    VkCommandBuffer cmd;
    VkResult res;

    if (!swapchain->cmd_pools[queue_idx])
    {
        VkCommandPoolCreateInfo poolInfo = {0};

        poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        poolInfo.queueFamilyIndex = queue_idx;

        if ((res = queue->device->p_vkCreateCommandPool( queue->device->host.device, &poolInfo, NULL,
                                                         &swapchain->cmd_pools[queue_idx] )))
        {
            ERR( "vkCreateCommandPool failed, res=%d\n", res );
            return NULL;
        }
    }

    allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocInfo.commandPool = swapchain->cmd_pools[queue_idx];
    allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocInfo.commandBufferCount = 1;

    if ((res = queue->device->p_vkAllocateCommandBuffers( queue->device->host.device, &allocInfo, &cmd )))
    {
        ERR( "vkAllocateCommandBuffers failed, res=%d\n", res );
        return NULL;
    }

    return cmd;
}

static void bind_pipeline( struct vulkan_device *device, VkCommandBuffer cmd, struct fs_comp_pipeline *pipeline,
                           VkDescriptorSet set, void *push_data )
{
    device->p_vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline->pipeline);

    device->p_vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                                            pipeline->pipeline_layout, 0, 1, &set, 0, NULL);

    device->p_vkCmdPushConstants(cmd, pipeline->pipeline_layout,
                                       VK_SHADER_STAGE_COMPUTE_BIT, 0, pipeline->push_size, push_data);
}

static void init_barrier(VkImageMemoryBarrier *barrier)
{
    barrier->sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier->pNext = NULL;
    barrier->srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier->dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier->subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier->subresourceRange.baseMipLevel = 0;
    barrier->subresourceRange.levelCount = 1;
    barrier->subresourceRange.baseArrayLayer = 0;
    barrier->subresourceRange.layerCount = 1;
}

static VkResult record_compute_cmd( struct vulkan_device *device, struct swapchain *swapchain, struct fs_hack_image *hack )
{
    struct fs_hack_color_constants color_constants;
    VkResult res;
    VkImageMemoryBarrier barriers[3] = {{0}};
    VkCommandBufferBeginInfo beginInfo = {0};
    float constants[4];

    TRACE( "recording compute command\n" );

    init_barrier(&barriers[0]);
    init_barrier(&barriers[1]);

    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_SIMULTANEOUS_USE_BIT;

    device->p_vkBeginCommandBuffer( hack->cmd, &beginInfo );

    /* for the cs we run... */
    /* transition user image from PRESENT_SRC to SHADER_READ */
    barriers[0].oldLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    barriers[0].newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    barriers[0].image = hack->user_image;
    barriers[0].srcAccessMask = 0;
    barriers[0].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

    /* storage image... */
    /* transition swapchain image from whatever to GENERAL */
    barriers[1].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    barriers[1].newLayout = VK_IMAGE_LAYOUT_GENERAL;
    barriers[1].image = hack->swapchain_image;
    barriers[1].srcAccessMask = 0;
    barriers[1].dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;

    device->p_vkCmdPipelineBarrier( hack->cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                    0, 0, NULL, 0, NULL, 2, barriers );

    if (swapchain->upscaler.color_mode == FS_HACK_COLOR_SRGB)
    {
        constants[0] = swapchain->fshack.dst.left;
        constants[1] = swapchain->fshack.dst.top;
        constants[0] -= 0.5f * (swapchain->fshack.dst.right - swapchain->fshack.dst.left) /
                        swapchain->extents.width;
        constants[1] -= 0.5f * (swapchain->fshack.dst.bottom - swapchain->fshack.dst.top) /
                        swapchain->extents.height;
        constants[2] = swapchain->fshack.dst.right - swapchain->fshack.dst.left;
        constants[3] = swapchain->fshack.dst.bottom - swapchain->fshack.dst.top;
        bind_pipeline(device, hack->cmd, &swapchain->blit_pipeline, hack->descriptor_set, constants);
    }
    else
    {
        color_constants.offset[0] = swapchain->fshack.dst.left;
        color_constants.offset[1] = swapchain->fshack.dst.top;
        color_constants.extents[0] = swapchain->fshack.dst.right - swapchain->fshack.dst.left;
        color_constants.extents[1] = swapchain->fshack.dst.bottom - swapchain->fshack.dst.top;
        color_constants.transfer = swapchain->upscaler.color_mode == FS_HACK_COLOR_PQ ?
                                   FS_HACK_TRANSFER_PQ :
                                   swapchain->upscaler.color_mode == FS_HACK_COLOR_HLG ?
                                   FS_HACK_TRANSFER_HLG :
                                   swapchain->upscaler.color_mode == FS_HACK_COLOR_EXTENDED_SRGB ?
                                   FS_HACK_TRANSFER_SRGB : FS_HACK_TRANSFER_NONE;
        color_constants.linear_filter = swapchain->upscaler.linear_filter;
        bind_pipeline(device, hack->cmd, &swapchain->blit_pipeline, hack->descriptor_set, &color_constants);
    }

    /* local sizes in shader are 8 */
    device->p_vkCmdDispatch( hack->cmd, ceil( swapchain->host_extents.width / 8. ),
                             ceil( swapchain->host_extents.height / 8. ), 1 );

    /* transition user image from SHADER_READ back to PRESENT_SRC */
    barriers[0].oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    barriers[0].newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    barriers[0].image = hack->user_image;
    barriers[0].srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
    barriers[0].dstAccessMask = 0;

    /* transition swapchain image from GENERAL to PRESENT_SRC */
    barriers[1].oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    barriers[1].newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    barriers[1].image = hack->swapchain_image;
    barriers[1].srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    barriers[1].dstAccessMask = 0;

    device->p_vkCmdPipelineBarrier( hack->cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                                    0, 0, NULL, 0, NULL, 2, barriers );

    if ((res = device->p_vkEndCommandBuffer( hack->cmd )))
    {
        ERR( "vkEndCommandBuffer: %d\n", res );
        return res;
    }

    return VK_SUCCESS;
}

static VkResult record_fsr_cmd(struct vulkan_device *device, struct swapchain *swapchain, struct fs_hack_image *hack)
{
    VkImageMemoryBarrier barriers[3] = {{0}};
    VkCommandBufferBeginInfo beginInfo = {0};
    union
    {
        uint32_t uint[16];
        float    fp[16];
    } c;
    VkResult result;

    TRACE("recording compute command\n");

    init_barrier(&barriers[0]);
    init_barrier(&barriers[1]);
    init_barrier(&barriers[2]);

    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_SIMULTANEOUS_USE_BIT;

    device->p_vkBeginCommandBuffer(hack->cmd, &beginInfo);

    /* 1st pass (easu) */
    /* transition user image from PRESENT_SRC to SHADER_READ */
    barriers[0].oldLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    barriers[0].newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    barriers[0].image = hack->user_image;
    barriers[0].srcAccessMask = 0;
    barriers[0].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

    /* storage image... */
    /* transition fsr image from whatever to GENERAL */
    barriers[1].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    barriers[1].newLayout = VK_IMAGE_LAYOUT_GENERAL;
    barriers[1].image = hack->fsr_image;
    barriers[1].srcAccessMask = 0;
    barriers[1].dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;

    device->p_vkCmdPipelineBarrier( hack->cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                                    VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, NULL, 0, NULL, 2, barriers );

    /* perform easu shader */

    c.fp[0] = swapchain->extents.width * (1.0f / swapchain->host_extents.width);
    c.fp[1] = swapchain->extents.height * (1.0f / swapchain->host_extents.height);
    c.fp[2] = 0.5f * c.fp[0] - 0.5f;
    c.fp[3] = 0.5f * c.fp[1] - 0.5f;
    // Viewport pixel position to normalized image space.
    // This is used to get upper-left of 'F' tap.
    c.fp[4] = 1.0f / swapchain->extents.width;
    c.fp[5] = 1.0f / swapchain->extents.height;
    // Centers of gather4, first offset from upper-left of 'F'.
    //      +---+---+
    //      |   |   |
    //      +--(0)--+
    //      | b | c |
    //  +---F---+---+---+
    //  | e | f | g | h |
    //  +--(1)--+--(2)--+
    //  | i | j | k | l |
    //  +---+---+---+---+
    //      | n | o |
    //      +--(3)--+
    //      |   |   |
    //      +---+---+
    c.fp[6] =  1.0f * c.fp[4];
    c.fp[7] = -1.0f * c.fp[5];
    // These are from (0) instead of 'F'.
    c.fp[8] = -1.0f * c.fp[4];
    c.fp[9] =  2.0f * c.fp[5];
    c.fp[10] =  1.0f * c.fp[4];
    c.fp[11] =  2.0f * c.fp[5];
    c.fp[12] =  0.0f * c.fp[4];
    c.fp[13] =  4.0f * c.fp[5];
    c.uint[14] = swapchain->host_extents.width;
    c.uint[15] = swapchain->host_extents.height;

    bind_pipeline(device, hack->cmd, &swapchain->fsr_easu_pipeline, hack->descriptor_set, c.uint);

    /* local sizes in shader are 8 */
    device->p_vkCmdDispatch(hack->cmd, ceil(swapchain->host_extents.width / 8.),
            ceil(swapchain->host_extents.height / 8.), 1);

    /* transition user image from SHADER_READ back to PRESENT_SRC */
    barriers[0].oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    barriers[0].newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    barriers[0].image = hack->user_image;
    barriers[0].srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
    barriers[0].dstAccessMask = 0;

    /* transition fsr image from GENERAL to SHADER_READ */
    barriers[1].oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    barriers[1].newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    barriers[1].image = hack->fsr_image;
    barriers[1].srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    barriers[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

    /* transition swapchain image from whatever to GENERAL */
    barriers[2].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    barriers[2].newLayout = VK_IMAGE_LAYOUT_GENERAL;
    barriers[2].image = hack->swapchain_image;
    barriers[2].srcAccessMask = 0;
    barriers[2].dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;

    device->p_vkCmdPipelineBarrier(hack->cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, NULL, 0, NULL, 3, barriers);

    /* 2nd pass (rcas) */

    c.fp[0] = exp2f(-swapchain->upscaler.fsr.sharpness);
    c.uint[2] = swapchain->host_extents.width;
    c.uint[3] = swapchain->host_extents.height;
    c.uint[4] = 0;
    c.uint[5] = 0;
    c.uint[6] = 0 + swapchain->host_extents.width;
    c.uint[7] = 0 + swapchain->host_extents.height;

    bind_pipeline(device, hack->cmd, &swapchain->fsr_rcas_pipeline, hack->fsr_set, c.uint);

    /* local sizes in shader are 8 */
    device->p_vkCmdDispatch(hack->cmd, ceil(swapchain->host_extents.width / 8.),
            ceil(swapchain->host_extents.height / 8.), 1);

    /* transition swapchain image from GENERAL to PRESENT_SRC */
    barriers[0].oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    barriers[0].newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    barriers[0].image = hack->swapchain_image;
    barriers[0].srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    barriers[0].dstAccessMask = 0;

    device->p_vkCmdPipelineBarrier(hack->cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                         VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, NULL, 0, NULL, 1, barriers);

    result = device->p_vkEndCommandBuffer(hack->cmd);
    if (result != VK_SUCCESS)
    {
        ERR("vkEndCommandBuffer: %d\n", result);
        return result;
    }

    return VK_SUCCESS;
}

#define win32u_vk_find_struct(s, t) win32u_vk_find_struct_((void *)s, VK_STRUCTURE_TYPE_##t)
static void *win32u_vk_find_struct_(void *s, VkStructureType t)
{
    VkBaseOutStructure *header;

    for (header = s; header; header = header->pNext)
    {
        if (header->sType == t)
            return header;
    }

    return NULL;
}

/* Consume present waits and signal a present fence before managed or discarded presents. */
static VkResult present_consume_waits( struct vulkan_device *device, struct vulkan_queue *queue,
                                       struct wine_managed_swapchain *managed, VkFence signal_fence,
                                       const VkSemaphore *semaphores, uint32_t count,
                                       BOOL wait_for_completion )
{
    VkPipelineStageFlags stack_stages[16], *stages = stack_stages;
    VkSubmitInfo submit = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO };
    VkFence fence = signal_fence;
    VkResult res;
    uint32_t i;
    BOOL lock_device;

    if (!fence && managed && managed->present_fence && managed->p_vkWaitForFences && managed->p_vkResetFences)
        fence = managed->present_fence;
    if (!count && !fence) return VK_SUCCESS;
    if (count > ARRAY_SIZE(stack_stages) && !(stages = malloc( count * sizeof(*stages) )))
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    for (i = 0; i < count; i++) stages[i] = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    submit.waitSemaphoreCount = count;
    submit.pWaitSemaphores = semaphores;
    submit.pWaitDstStageMask = stages;
    lock_device = wait_for_completion && !fence;
    if (lock_device) vulkan_device_lock_queues( device );
    else vulkan_queue_lock( queue );
    res = device->p_vkQueueSubmit( queue->host.queue, 1, &submit, fence );
    if (res == VK_SUCCESS && wait_for_completion && !fence)
        res = device->p_vkDeviceWaitIdle( device->host.device );
    if (lock_device) vulkan_device_unlock_queues( device );
    else vulkan_queue_unlock( queue );
    if (res == VK_SUCCESS && wait_for_completion && fence)
    {
        res = managed->p_vkWaitForFences( device->host.device, 1, &fence, VK_TRUE, UINT64_MAX );
        if (res == VK_SUCCESS) res = managed->p_vkResetFences( device->host.device, 1, &fence );
    }
    if (stages != stack_stages) free( stages );
    return res;
}

/* Caller holds queue->mutex. */
static VkResult queue_ensure_managed_present_semaphores( struct vulkan_queue *queue )
{
    struct vulkan_device *device = queue->device;
    VkExportSemaphoreCreateInfo export_info =
    {
        .sType = VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO,
        .handleTypes = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT,
    };
    VkSemaphoreCreateInfo export_semaphore_info =
    {
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
        .pNext = &export_info,
    };
    VkSemaphoreCreateInfo semaphore_info = { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
    VkResult res;

    if (queue->managed_present_sync_unavailable) return VK_ERROR_FEATURE_NOT_PRESENT;
    if (queue->managed_present_semaphore) return VK_SUCCESS;

    if ((res = device->p_vkCreateSemaphore( device->host.device, &export_semaphore_info, NULL,
                                            &queue->managed_present_semaphore )) ||
        (res = device->p_vkCreateSemaphore( device->host.device, &semaphore_info, NULL,
                                            &queue->managed_host_semaphore )))
    {
        if (queue->managed_host_semaphore)
        {
            device->p_vkDestroySemaphore( device->host.device,
                                          queue->managed_host_semaphore, NULL );
            queue->managed_host_semaphore = VK_NULL_HANDLE;
        }
        if (queue->managed_present_semaphore)
        {
            device->p_vkDestroySemaphore( device->host.device,
                                          queue->managed_present_semaphore, NULL );
            queue->managed_present_semaphore = VK_NULL_HANDLE;
        }
        queue->managed_present_sync_unavailable = TRUE;
        return res;
    }

    return VK_SUCCESS;
}

static VkResult present_export_waits( struct vulkan_device *device, struct vulkan_queue *queue,
                                      const VkSemaphore *semaphores, uint32_t count,
                                      BOOL signal_host, int *sync_fd, BOOL *submitted )
{
    VkPipelineStageFlags stack_stages[16], *stages = stack_stages;
    VkSemaphore signal_semaphores[2];
    VkPipelineStageFlags consume_stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    VkSubmitInfo submit =
    {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .waitSemaphoreCount = count,
        .pWaitSemaphores = semaphores,
        .signalSemaphoreCount = signal_host ? 2 : 1,
    };
    VkSemaphoreGetFdInfoKHR fd_info =
    {
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_GET_FD_INFO_KHR,
        .handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT,
    };
    VkResult res, consume_res;
    uint32_t i;

    *sync_fd = -1;
    *submitted = FALSE;
    if (count > ARRAY_SIZE(stack_stages) && !(stages = malloc( count * sizeof(*stages) )))
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    for (i = 0; i < count; i++) stages[i] = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    submit.pWaitDstStageMask = stages;

    vulkan_queue_lock( queue );
    if ((res = queue_ensure_managed_present_semaphores( queue )))
        goto done;
    signal_semaphores[0] = queue->managed_present_semaphore;
    signal_semaphores[1] = queue->managed_host_semaphore;
    submit.pSignalSemaphores = signal_semaphores;
    fd_info.semaphore = queue->managed_present_semaphore;
    res = device->p_vkQueueSubmit( queue->host.queue, 1, &submit, VK_NULL_HANDLE );
    if (res == VK_SUCCESS) *submitted = TRUE;
    if (res == VK_SUCCESS)
        res = device->p_vkGetSemaphoreFdKHR( device->host.device, &fd_info, sync_fd );
    if (*submitted && res < VK_SUCCESS)
    {
        VkSubmitInfo consume =
        {
            .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
            .waitSemaphoreCount = 1,
            .pWaitSemaphores = &queue->managed_present_semaphore,
            .pWaitDstStageMask = &consume_stage,
        };

        if (*sync_fd >= 0)
        {
            close( *sync_fd );
            *sync_fd = -1;
        }
        consume_res = device->p_vkQueueSubmit( queue->host.queue, 1, &consume, VK_NULL_HANDLE );
        if (consume_res < VK_SUCCESS)
        {
            queue->managed_present_sync_unavailable = TRUE;
            res = consume_res;
        }
    }

done:
    vulkan_queue_unlock( queue );
    if (stages != stack_stages) free( stages );
    return res;
}

static void append_present_pnext( const void ***tail, void *entry )
{
    VkBaseInStructure *header = entry;

    header->pNext = NULL;
    **tail = header;
    *tail = (const void **)&header->pNext;
}

static VkFence get_present_fence( const VkSwapchainPresentFenceInfoKHR *info, uint32_t index )
{
    if (!info || index >= info->swapchainCount || !info->pFences || !info->pFences[index])
        return VK_NULL_HANDLE;

    return info->pFences[index];
}

static BOOL present_result_was_enqueued( VkResult result )
{
    /* These extension results are omitted from Wine's Unix Vulkan header. */
    static const VkResult present_timing_queue_full = (VkResult)-1000208000;
    static const VkResult full_screen_exclusive_lost = (VkResult)-1000255000;

    return result == VK_SUCCESS || result == VK_SUBOPTIMAL_KHR ||
           result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_ERROR_SURFACE_LOST_KHR ||
           result == full_screen_exclusive_lost || result == present_timing_queue_full;
}

static VkResult repack_present_pnext( struct mempool *pool, VkPresentInfoKHR *host_info,
                                      const VkPresentInfoKHR *present_info,
                                      const uint32_t *host_indices, uint32_t host_count )
{
    const VkBaseInStructure *header;
    const void **tail = &host_info->pNext;
    uint32_t i;

    host_info->pNext = NULL;
    for (header = present_info->pNext; header; header = header->pNext)
    {
        switch (header->sType)
        {
        case VK_STRUCTURE_TYPE_DEVICE_GROUP_PRESENT_INFO_KHR:
        {
            const VkDeviceGroupPresentInfoKHR *src = (const VkDeviceGroupPresentInfoKHR *)header;
            VkDeviceGroupPresentInfoKHR *dst;

            if (!(dst = mem_alloc( pool, sizeof(*dst) ))) return VK_ERROR_OUT_OF_HOST_MEMORY;
            *dst = *src;
            dst->swapchainCount = host_count;
            if (src->pDeviceMasks)
            {
                uint32_t *masks;

                if (!(masks = mem_alloc( pool, host_count * sizeof(*masks) )))
                    return VK_ERROR_OUT_OF_HOST_MEMORY;
                dst->pDeviceMasks = masks;
                for (i = 0; i < host_count; i++) masks[i] = src->pDeviceMasks[host_indices[i]];
            }
            append_present_pnext( &tail, dst );
            break;
        }
        case VK_STRUCTURE_TYPE_PRESENT_ID_KHR:
        {
            const VkPresentIdKHR *src = (const VkPresentIdKHR *)header;
            VkPresentIdKHR *dst;

            if (!(dst = mem_alloc( pool, sizeof(*dst) ))) return VK_ERROR_OUT_OF_HOST_MEMORY;
            *dst = *src;
            dst->swapchainCount = host_count;
            if (src->pPresentIds)
            {
                uint64_t *ids;

                if (!(ids = mem_alloc( pool, host_count * sizeof(*ids) )))
                    return VK_ERROR_OUT_OF_HOST_MEMORY;
                dst->pPresentIds = ids;
                for (i = 0; i < host_count; i++) ids[i] = src->pPresentIds[host_indices[i]];
            }
            append_present_pnext( &tail, dst );
            break;
        }
        case VK_STRUCTURE_TYPE_PRESENT_ID_2_KHR:
        {
            const VkPresentId2KHR *src = (const VkPresentId2KHR *)header;
            VkPresentId2KHR *dst;

            if (!(dst = mem_alloc( pool, sizeof(*dst) ))) return VK_ERROR_OUT_OF_HOST_MEMORY;
            *dst = *src;
            dst->swapchainCount = host_count;
            if (src->pPresentIds)
            {
                uint64_t *ids;

                if (!(ids = mem_alloc( pool, host_count * sizeof(*ids) )))
                    return VK_ERROR_OUT_OF_HOST_MEMORY;
                dst->pPresentIds = ids;
                for (i = 0; i < host_count; i++) ids[i] = src->pPresentIds[host_indices[i]];
            }
            append_present_pnext( &tail, dst );
            break;
        }
        case VK_STRUCTURE_TYPE_PRESENT_REGIONS_KHR:
        {
            const VkPresentRegionsKHR *src = (const VkPresentRegionsKHR *)header;
            VkPresentRegionsKHR *dst;

            if (!(dst = mem_alloc( pool, sizeof(*dst) ))) return VK_ERROR_OUT_OF_HOST_MEMORY;
            *dst = *src;
            dst->swapchainCount = host_count;
            if (src->pRegions)
            {
                VkPresentRegionKHR *regions;

                if (!(regions = mem_alloc( pool, host_count * sizeof(*regions) )))
                    return VK_ERROR_OUT_OF_HOST_MEMORY;
                dst->pRegions = regions;
                for (i = 0; i < host_count; i++) regions[i] = src->pRegions[host_indices[i]];
            }
            append_present_pnext( &tail, dst );
            break;
        }
        case VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_FENCE_INFO_KHR:
        {
            const VkSwapchainPresentFenceInfoKHR *src = (const VkSwapchainPresentFenceInfoKHR *)header;
            VkSwapchainPresentFenceInfoKHR *dst;

            if (!(dst = mem_alloc( pool, sizeof(*dst) ))) return VK_ERROR_OUT_OF_HOST_MEMORY;
            *dst = *src;
            dst->swapchainCount = host_count;
            if (src->pFences)
            {
                VkFence *fences;

                if (!(fences = mem_alloc( pool, host_count * sizeof(*fences) )))
                    return VK_ERROR_OUT_OF_HOST_MEMORY;
                dst->pFences = fences;
                for (i = 0; i < host_count; i++) fences[i] = get_present_fence( src, host_indices[i] );
            }
            append_present_pnext( &tail, dst );
            break;
        }
        case VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_MODE_INFO_KHR:
        {
            const VkSwapchainPresentModeInfoKHR *src = (const VkSwapchainPresentModeInfoKHR *)header;
            VkSwapchainPresentModeInfoKHR *dst;

            if (!(dst = mem_alloc( pool, sizeof(*dst) ))) return VK_ERROR_OUT_OF_HOST_MEMORY;
            *dst = *src;
            dst->swapchainCount = host_count;
            if (src->pPresentModes)
            {
                VkPresentModeKHR *modes;

                if (!(modes = mem_alloc( pool, host_count * sizeof(*modes) )))
                    return VK_ERROR_OUT_OF_HOST_MEMORY;
                dst->pPresentModes = modes;
                for (i = 0; i < host_count; i++) modes[i] = src->pPresentModes[host_indices[i]];
            }
            append_present_pnext( &tail, dst );
            break;
        }
        case VK_STRUCTURE_TYPE_PRESENT_TIMINGS_INFO_EXT:
        {
            const VkPresentTimingsInfoEXT *src = (const VkPresentTimingsInfoEXT *)header;
            VkPresentTimingsInfoEXT *dst;

            if (!(dst = mem_alloc( pool, sizeof(*dst) ))) return VK_ERROR_OUT_OF_HOST_MEMORY;
            *dst = *src;
            dst->swapchainCount = host_count;
            if (src->pTimingInfos)
            {
                VkPresentTimingInfoEXT *timings;

                if (!(timings = mem_alloc( pool, host_count * sizeof(*timings) )))
                    return VK_ERROR_OUT_OF_HOST_MEMORY;
                dst->pTimingInfos = timings;
                for (i = 0; i < host_count; i++) timings[i] = src->pTimingInfos[host_indices[i]];
            }
            append_present_pnext( &tail, dst );
            break;
        }
        default:
        {
            static int once;

            if (!once++)
                WARN( "dropping unhandled VkPresentInfoKHR pNext sType %u for host-only present\n",
                      header->sType );
            break;
        }
        }
    }

    return VK_SUCCESS;
}

static VkResult win32u_vkQueuePresentKHR( VkQueue client_queue, const VkPresentInfoKHR *client_present_info )
{
    VkPresentInfoKHR present_info_data = *client_present_info;
    VkPresentInfoKHR *present_info = &present_info_data;
    VkResult host_results_buffer[16], *host_results = NULL;
    uint32_t image_indices_buffer[16], *image_indices = NULL;
    VkPresentInfoKHR host_info;
    struct vulkan_queue *queue = vulkan_queue_from_handle( client_queue );
    struct vulkan_device *device = queue->device;
    VkResult res = VK_ERROR_OUT_OF_HOST_MEMORY;
    struct swapchain **present_swapchains;
    VkResult *present_results;
    VkSwapchainKHR *swapchains;
    uint32_t host_indices_buffer[16], *host_indices = host_indices_buffer;
    VkSemaphore *host_wait_semaphores = NULL;
    const VkSwapchainPresentFenceInfoKHR *present_fence_info = NULL;
    struct wine_managed_swapchain *first_managed = NULL;
    uint32_t host_count = 0;
    BOOL skip_managed = FALSE;
    BOOL managed_present_waits_consumed = FALSE;
    BOOL present_waits_submitted = FALSE;
    BOOL managed_sync_complete = FALSE;
    BOOL all_managed_explicit = TRUE;
    BOOL explicit_submit = FALSE;
    VkResult managed_sync_res = VK_SUCCESS;
    VkCommandBuffer *blit_cmds;
    struct mempool pool = {0};
    uint32_t blit_count = 0;
    VkSemaphore blit_sema;
    int managed_sync_fd = -1;
    BOOL fullscreen_lost = FALSE;
    BOOL out_of_date = FALSE;

    TRACE( "queue %p, present_info %p\n", queue, present_info );

    if (!(swapchains = mem_alloc( &pool, present_info->swapchainCount * sizeof(*swapchains) ))) return VK_ERROR_OUT_OF_HOST_MEMORY;
    if (!(present_swapchains = mem_alloc( &pool, present_info->swapchainCount * sizeof(*present_swapchains) ))) goto failed;
    if (!(present_results = mem_alloc( &pool, present_info->swapchainCount * sizeof(*present_results) ))) goto failed;
    if (!(blit_cmds = mem_alloc( &pool, present_info->swapchainCount * sizeof(blit_cmds) ))) goto failed;
    if (present_info->swapchainCount > ARRAY_SIZE(host_indices_buffer) &&
        !(host_indices = mem_alloc( &pool, present_info->swapchainCount * sizeof(*host_indices) ))) goto failed;

    for (uint32_t i = 0; i < present_info->swapchainCount; ++i)
    {
        struct swapchain *swapchain = present_swapchains[i] =
                swapchain_from_handle( present_info->pSwapchains[i] );

        if (swapchain) client_surface_update( swapchain->surface->client );
    }

    for (uint32_t i = 0; i < present_info->swapchainCount; ++i)
    {
        struct swapchain *swapchain = present_swapchains[i];
        struct fs_hack_image *hack;

        present_results[i] = VK_SUCCESS;
        if (!swapchain || swapchain_is_out_of_date( swapchain ))
        {
            present_swapchains[i] = NULL;
            present_results[i] = VK_ERROR_OUT_OF_DATE_KHR;
            out_of_date = TRUE;
            continue;
        }
        if (!swapchain_can_present( swapchain ))
        {
            present_swapchains[i] = NULL;
            present_results[i] = VK_ERROR_FULL_SCREEN_EXCLUSIVE_MODE_LOST_EXT;
            fullscreen_lost = TRUE;
            continue;
        }
        if (swapchain->managed) continue;
        swapchain_apply_color_description( swapchain );
        if (!swapchain->fshack.enabled) continue;
        hack = &swapchain->fs_hack_images[present_info->pImageIndices[i]];
        blit_sema = hack->blit_finished;

        if (!hack->cmd || hack->cmd_queue_idx != queue->info.queueFamilyIndex)
        {
            if (hack->cmd) device->p_vkFreeCommandBuffers( queue->device->host.device, swapchain->cmd_pools[hack->cmd_queue_idx], 1, &hack->cmd );
            if (!(queue->device->queue_props[queue->info.queueFamilyIndex].queueFlags & VK_QUEUE_COMPUTE_BIT)) goto failed; /* TODO */

            if (!(hack->cmd = create_hack_cmd( queue, swapchain, queue->info.queueFamilyIndex )) ||
                (swapchain->upscaler.is_fsr ? record_fsr_cmd(queue->device, swapchain, hack) : record_compute_cmd( queue->device, swapchain, hack )))
            {
                device->p_vkFreeCommandBuffers( queue->device->host.device, swapchain->cmd_pools[hack->cmd_queue_idx], 1, &hack->cmd );
                hack->cmd = NULL;
                goto failed;
            }

            hack->cmd_queue_idx = queue->info.queueFamilyIndex;
        }

        blit_cmds[blit_count++] = hack->cmd;
    }

    if (present_info->waitSemaphoreCount &&
        !(host_wait_semaphores = mem_alloc( &pool, present_info->waitSemaphoreCount *
                                            sizeof(*host_wait_semaphores) )))
        goto failed;
    for (uint32_t i = 0; i < present_info->waitSemaphoreCount; i++)
    {
        struct vulkan_semaphore *semaphore =
            vulkan_semaphore_from_handle( present_info->pWaitSemaphores[i] );
        host_wait_semaphores[i] = semaphore->host.semaphore;
    }
    present_info->pWaitSemaphores = host_wait_semaphores;

    /* Host swapchains feed the real host present. Managed swapchains publish
     * cross-process dmabufs instead. */
    for (uint32_t i = 0; i < present_info->swapchainCount; i++)
    {
        struct swapchain *swapchain = present_swapchains[i];
        if (!swapchain) continue;
        if (swapchain->managed)
        {
            if (!first_managed) first_managed = swapchain->managed;
            if (!swapchain->managed->explicit_sync) all_managed_explicit = FALSE;
            continue;
        }
        swapchains[host_count] = swapchain->obj.host.swapchain;
        host_indices[host_count] = i;
        host_count++;
    }

    /* Finish all host-present allocation and repacking before an internal
     * blit submission consumes the application's wait semaphores. */
    if (host_count)
    {
        host_info = *present_info;
        host_info.swapchainCount = host_count;
        host_info.pSwapchains = swapchains;
        image_indices = host_count <= ARRAY_SIZE(image_indices_buffer) ? image_indices_buffer
                                                                       : mem_alloc( &pool, host_count * sizeof(*image_indices) );
        if (!image_indices) goto failed;
        if (host_count != present_info->swapchainCount &&
            (res = repack_present_pnext( &pool, &host_info, present_info, host_indices, host_count )))
            goto failed;
        host_results = host_count <= ARRAY_SIZE(host_results_buffer) ? host_results_buffer
                                                                     : mem_alloc( &pool, host_count * sizeof(*host_results) );
        if (!host_results) goto failed;
        for (uint32_t i = 0; i < host_count; i++) host_results[i] = VK_SUCCESS;
        host_info.pResults = host_results;
        for (uint32_t i = 0; i < host_count; i++)
            image_indices[i] = present_info->pImageIndices[host_indices[i]];
        host_info.pImageIndices = image_indices;
    }

    if (blit_count)
    {
        VkSubmitInfo submit_info = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO};
        VkPipelineStageFlags *stages;
        VkLatencySubmissionPresentIdNV latencySubmitInfo;
        VkPresentIdKHR *present_id;

        if (!(stages = mem_alloc( &pool, sizeof(VkPipelineStageFlags) * present_info->waitSemaphoreCount ))) goto failed;
        for (uint32_t i = 0; i < present_info->waitSemaphoreCount; ++i) stages[i] = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;

        /* blit user image to real image */
        submit_info.waitSemaphoreCount = present_info->waitSemaphoreCount;
        submit_info.pWaitSemaphores = present_info->pWaitSemaphores;
        submit_info.pWaitDstStageMask = stages;
        submit_info.commandBufferCount = blit_count;
        submit_info.pCommandBuffers = blit_cmds;
        submit_info.signalSemaphoreCount = 1;
        submit_info.pSignalSemaphores = &blit_sema;
        if ((queue->device->low_latency_enabled) &&
            (present_id = win32u_vk_find_struct(present_info, PRESENT_ID_KHR)))
        {
            latencySubmitInfo.sType = VK_STRUCTURE_TYPE_LATENCY_SUBMISSION_PRESENT_ID_NV;
            latencySubmitInfo.pNext = NULL;
            latencySubmitInfo.presentID = *present_id->pPresentIds;
            submit_info.pNext = &latencySubmitInfo;
        }
        vulkan_queue_lock( queue );
        res = device->p_vkQueueSubmit( queue->host.queue, 1, &submit_info, VK_NULL_HANDLE );
        vulkan_queue_unlock( queue );
        if (res < VK_SUCCESS) goto failed;

        present_info->waitSemaphoreCount = 1;
        present_info->pWaitSemaphores = &blit_sema;
    }

    res = fullscreen_lost ? VK_ERROR_FULL_SCREEN_EXCLUSIVE_MODE_LOST_EXT :
          out_of_date ? VK_ERROR_OUT_OF_DATE_KHR : VK_SUCCESS;

    if (first_managed && all_managed_explicit &&
        !queue->managed_present_sync_unavailable)
    {
        managed_sync_res = present_export_waits( device, queue,
                                                 present_info->pWaitSemaphores,
                                                 present_info->waitSemaphoreCount,
                                                 host_count != 0, &managed_sync_fd,
                                                 &explicit_submit );

        if (explicit_submit)
        {
            present_waits_submitted = TRUE;
            managed_present_waits_consumed = TRUE;
            if (host_count)
            {
                host_info.waitSemaphoreCount = 1;
                host_info.pWaitSemaphores = &queue->managed_host_semaphore;
            }
        }
        if (managed_sync_res < VK_SUCCESS && explicit_submit)
        {
            if (res >= VK_SUCCESS) res = managed_sync_res;
            skip_managed = TRUE;
        }
        else if (managed_sync_res < VK_SUCCESS)
            managed_sync_res = VK_SUCCESS;
    }

    /* Host present consumes waits for host swapchains. */
    if (host_count)
    {
        VkResult host_res;

        if (!explicit_submit)
        {
            host_info.waitSemaphoreCount = present_info->waitSemaphoreCount;
            host_info.pWaitSemaphores = present_info->pWaitSemaphores;
        }
        vulkan_queue_lock( queue );
        host_res = device->p_vkQueuePresentKHR( queue->host.queue, &host_info );
        vulkan_queue_unlock( queue );
        if (present_result_was_enqueued( host_res ))
            present_waits_submitted = TRUE;
        else if (explicit_submit)
        {
            VkResult consume_res = present_consume_waits( device, queue, NULL, VK_NULL_HANDLE,
                                                          &queue->managed_host_semaphore, 1, FALSE );
            if (consume_res < VK_SUCCESS && host_res >= VK_SUCCESS) host_res = consume_res;
        }

        for (uint32_t i = 0; i < host_count; i++)
            present_results[host_indices[i]] =
                present_result_was_enqueued( host_res ) ? host_results[i] : host_res;

        if (!present_waits_submitted) skip_managed = TRUE;
        if (host_res < VK_SUCCESS && res >= VK_SUCCESS) res = host_res;
        else if (host_res == VK_SUBOPTIMAL_KHR && res == VK_SUCCESS) res = host_res;
    }

    if (out_of_date && res >= VK_SUCCESS) res = VK_ERROR_OUT_OF_DATE_KHR;

    /* Managed and stale swapchains have no host present to signal these fences. */
    for (const VkBaseInStructure *header = present_info->pNext; header; header = header->pNext)
        if (header->sType == VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_FENCE_INFO_KHR)
        {
            present_fence_info = (const VkSwapchainPresentFenceInfoKHR *)header;
            break;
        }

    /* A discarded present still consumes its waits and signals its present
     * fences. Otherwise a binary semaphore stays signaled and a caller
     * waiting to retire the rejected presentation can block forever. */
    if (!host_count && !first_managed)
    {
        for (uint32_t i = 0; i < present_info->swapchainCount; i++)
        {
            VkFence fence = VK_NULL_HANDLE;
            VkResult cres;

            fence = get_present_fence( present_fence_info, i );
            cres = present_consume_waits( device, queue, NULL, fence,
                                          i ? NULL : present_info->pWaitSemaphores,
                                          i ? 0 : present_info->waitSemaphoreCount, FALSE );

            if (cres < VK_SUCCESS)
            {
                res = cres;
                break;
            }
        }
    }

    /* All-managed presents consume the live wait array once here. */
    if (!managed_present_waits_consumed && !host_count && first_managed &&
        present_info->waitSemaphoreCount)
    {
        VkResult cres = present_consume_waits( device, queue, first_managed, VK_NULL_HANDLE,
                                               present_info->pWaitSemaphores, present_info->waitSemaphoreCount, TRUE );
        if (cres < VK_SUCCESS)
        {
            res = cres;
            skip_managed = TRUE;
        }
        else managed_present_waits_consumed = TRUE;
        present_waits_submitted = managed_present_waits_consumed;
    }
    else if (!managed_present_waits_consumed && !host_count && first_managed)
        present_waits_submitted = TRUE;

    /* Present each managed swapchain after the present waits are consumed. */
    for (uint32_t i = 0; i < present_info->swapchainCount; i++)
    {
        struct swapchain *swapchain = present_swapchains[i];
        VkResult managed_res;
        VkFence fence;
        int sync_fd = -1;

        if (!swapchain) continue;
        if (!swapchain->managed) continue;

        if (skip_managed)
        {
            if (present_waits_submitted &&
                (fence = get_present_fence( present_fence_info, i )))
            {
                VkResult fence_res = present_consume_waits( device, queue, NULL, fence,
                                                            NULL, 0, FALSE );
                if (fence_res < VK_SUCCESS && res >= VK_SUCCESS) res = fence_res;
            }
            present_results[i] = managed_sync_res < VK_SUCCESS ? managed_sync_res : res;
            continue;
        }

        if (managed_sync_fd >= 0 && !managed_sync_complete &&
            (sync_fd = dup( managed_sync_fd )) < 0)
            managed_sync_complete = wait_sync_file( managed_sync_fd );

        if (managed_sync_fd >= 0 && !managed_sync_complete && sync_fd < 0)
            managed_res = VK_ERROR_OUT_OF_HOST_MEMORY;
        else
            managed_res = managed_present( device, swapchain, present_info->pImageIndices[i],
                                           managed_present_waits_consumed, sync_fd );

        /* Managed swapchains have no host present to signal this fence. */
        if ((fence = get_present_fence( present_fence_info, i )))
        {
            VkResult fence_res = present_consume_waits( device, queue, NULL, fence, NULL, 0, FALSE );

            if (fence_res < VK_SUCCESS && managed_res >= VK_SUCCESS) managed_res = fence_res;
        }

        present_results[i] = managed_res;
        if (managed_res < VK_SUCCESS && res >= VK_SUCCESS) res = managed_res;
        else if (managed_res == VK_SUBOPTIMAL_KHR && res == VK_SUCCESS) res = VK_SUBOPTIMAL_KHR;
    }
    if (managed_sync_fd >= 0) close( managed_sync_fd );

    /* Live entries consume the shared wait array, but stale entries are not
     * forwarded to that present and need their fences signaled separately. */
    if (present_waits_submitted)
    {
        for (uint32_t i = 0; i < present_info->swapchainCount; i++)
        {
            VkFence fence;
            VkResult fence_res;

            if (present_swapchains[i] || !(fence = get_present_fence( present_fence_info, i ))) continue;
            fence_res = present_consume_waits( device, queue, NULL, fence, NULL, 0, FALSE );
            if (fence_res < VK_SUCCESS && res >= VK_SUCCESS) res = fence_res;
        }
    }

    for (uint32_t i = 0; i < present_info->swapchainCount; i++)
    {
        struct swapchain *swapchain = present_swapchains[i];
        VkResult swapchain_res = present_results[i];
        struct surface *surface;
        RECT client_rect;

        if (!swapchain) continue;
        if (swapchain_res < VK_SUCCESS) continue;
        surface = swapchain->surface;
        client_surface_present( surface->client );

        if (swapchain->managed) continue; /* managed already set its own result */
        if (!get_surface_rect( surface->hwnd, &client_rect, NtUserGetDpiForWindow( surface->hwnd ) ))
        {
            WARN( "Swapchain window %p is invalid, returning VK_ERROR_OUT_OF_DATE_KHR\n", surface->hwnd );
            present_results[i] = VK_ERROR_OUT_OF_DATE_KHR;
            if (res >= VK_SUCCESS) res = VK_ERROR_OUT_OF_DATE_KHR;
        }
        else if (swapchain_res)
            WARN( "Present returned status %d for swapchain %p\n", swapchain_res, swapchain );
        else if (!swapchain->fshack.enabled && !swapchain->compositor_scaling &&
                 !IsRectEmpty( &client_rect ) &&
                 !extents_equals( &swapchain->extents, &client_rect ))
        {
            WARN( "Swapchain size %dx%d does not match client rect %s, returning VK_SUBOPTIMAL_KHR\n",
                  swapchain->extents.width, swapchain->extents.height, wine_dbgstr_rect( &client_rect ) );
            present_results[i] = VK_SUBOPTIMAL_KHR;
            if (!res) res = VK_SUBOPTIMAL_KHR;
        }
    }

    if (present_info->pResults)
        memcpy( present_info->pResults, present_results,
                present_info->swapchainCount * sizeof(*present_results) );

    if (TRACE_ON( fps ))
    {
        static unsigned long frames, frames_total;
        static long prev_time, start_time;
        DWORD time;

        time = NtGetTickCount();
        frames++;
        frames_total++;

        if (time - prev_time > 1500)
        {
            TRACE_(fps)( "%p @ approx %.2ffps, total %.2ffps\n", queue, 1000.0 * frames / (time - prev_time),
                         1000.0 * frames_total / (time - start_time) );
            prev_time = time;
            frames = 0;

            if (!start_time) start_time = time;
        }
    }

failed:
    mem_free( &pool );
    return res;
}

static LARGE_INTEGER *get_nt_timeout( LARGE_INTEGER *time, DWORD timeout )
{
    if (timeout == INFINITE) return NULL;
    time->QuadPart = (ULONGLONG)timeout * -10000;
    return time;
}

static VkResult acquire_keyed_mutexes( VkWin32KeyedMutexAcquireReleaseInfoKHR *mutex_info, struct mempool *pool,
                                       const VkSemaphoreSubmitInfo **semaphores, UINT *semaphores_count )
{
    UINT i, count = *semaphores_count;
    VkSemaphoreSubmitInfo *submits;
    NTSTATUS status;

    if (!(submits = mem_alloc( pool, (count + mutex_info->acquireCount) * sizeof(*submits) ))) return VK_ERROR_OUT_OF_HOST_MEMORY;
    memcpy( submits, *semaphores, count * sizeof(*submits) );
    memset( submits + count, 0, mutex_info->acquireCount * sizeof(*submits) );

    for (i = 0; i < mutex_info->acquireCount; i++)
    {
        LARGE_INTEGER timeout;
        struct device_memory *memory = device_memory_from_handle( mutex_info->pAcquireSyncs[i] );
        D3DKMT_ACQUIREKEYEDMUTEX acquire =
        {
            .hKeyedMutex = memory->mutex,
            .Key = mutex_info->pAcquireKeys[i],
            .pTimeout = get_nt_timeout( &timeout, mutex_info->pAcquireTimeouts[i] ),
        };
        VkSemaphoreSubmitInfo submit =
        {
            .sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
            .semaphore = memory->semaphore,
            .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
            .deviceIndex = 0,
        };

        if ((status = NtGdiDdDDIAcquireKeyedMutex( &acquire ))) goto error;
        submit.value = memory->semaphore_value = acquire.FenceValue;
        submits[count++] = submit;
    }

    *semaphores = submits;
    *semaphores_count = count;
    return VK_SUCCESS;

error:
    WARN( "Failed to acquire keyed mutex 0x%s key 0x%s, status %#x\n", wine_dbgstr_longlong( mutex_info->pAcquireSyncs[i] ),
          wine_dbgstr_longlong( mutex_info->pAcquireKeys[i] ), status );

    while (i--)
    {
        struct device_memory *memory = device_memory_from_handle( mutex_info->pAcquireSyncs[i] );
        D3DKMT_RELEASEKEYEDMUTEX release =
        {
            .hKeyedMutex = memory->mutex,
            .Key = mutex_info->pAcquireKeys[i],
            .FenceValue = memory->semaphore_value,
        };
        NtGdiDdDDIReleaseKeyedMutex( &release );
    }
    return status == STATUS_TIMEOUT ? VK_TIMEOUT : VK_ERROR_UNKNOWN;
}

static VkResult release_keyed_mutexes( VkWin32KeyedMutexAcquireReleaseInfoKHR *mutex_info, struct mempool *pool,
                                       const VkSemaphoreSubmitInfo **semaphores, UINT *semaphores_count )
{
    UINT i, count = *semaphores_count;
    VkSemaphoreSubmitInfo *submits;
    NTSTATUS status;

    if (!(submits = mem_alloc( pool, (count + mutex_info->releaseCount) * sizeof(*submits) ))) return VK_ERROR_OUT_OF_HOST_MEMORY;
    memcpy( submits, *semaphores, count * sizeof(*submits) );
    memset( submits + count, 0, mutex_info->releaseCount * sizeof(*submits) );

    for (i = 0; i < mutex_info->releaseCount; i++)
    {
        struct device_memory *memory = device_memory_from_handle( mutex_info->pReleaseSyncs[i] );
        D3DKMT_RELEASEKEYEDMUTEX release =
        {
            .hKeyedMutex = memory->mutex,
            .Key = mutex_info->pReleaseKeys[i],
            .FenceValue = memory->semaphore_value + 1,
        };
        VkSemaphoreSubmitInfo submit =
        {
            .sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
            .semaphore = memory->semaphore,
            .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
            .deviceIndex = 0,
        };

        if ((status = NtGdiDdDDIReleaseKeyedMutex( &release ))) goto failed;
        submit.value = memory->semaphore_value + 1;
        submits[count++] = submit;
    }

    *semaphores = submits;
    *semaphores_count = count;
    return VK_SUCCESS;

failed:
    WARN( "Failed to release keyed mutex 0x%s key 0x%s, status %#x\n", wine_dbgstr_longlong( mutex_info->pReleaseSyncs[i] ),
          wine_dbgstr_longlong( mutex_info->pReleaseKeys[i] ), status );
    return VK_ERROR_UNKNOWN;
}

static VkResult win32u_vkQueueSubmit( VkQueue client_queue, uint32_t count, const VkSubmitInfo *submits, VkFence client_fence )
{
    struct vulkan_fence *fence = client_fence ? vulkan_fence_from_handle( client_fence ) : NULL;
    struct vulkan_queue *queue = vulkan_queue_from_handle( client_queue );
    struct vulkan_device *device = queue->device;
    VkResult res = VK_ERROR_OUT_OF_HOST_MEMORY;
    VkTimelineSemaphoreSubmitInfo *timelines;
    struct mempool pool = {0};

    TRACE( "queue %p, count %u, submits %p, fence %p\n", queue, count, submits, fence );

    if (!(timelines = mem_alloc( &pool, count * sizeof(*timelines) ))) return VK_ERROR_OUT_OF_HOST_MEMORY;
    memset( timelines, 0, count * sizeof(*timelines) );

    for (uint32_t i = 0; i < count; i++)
    {
        VkSubmitInfo *submit = (VkSubmitInfo *)submits + i; /* cast away const, chain has been copied in the thunks */
        const VkSemaphoreSubmitInfo *wait_infos = NULL, *signal_infos = NULL;
        VkBaseOutStructure **next, *prev = (VkBaseOutStructure *)submit;
        VkTimelineSemaphoreSubmitInfo *timeline = timelines + i;
        VkSemaphore *wait_semaphores, *signal_semaphores;
        VkDeviceGroupSubmitInfo *device_group = NULL;
        UINT wait_count = 0, signal_count = 0;
        VkPipelineStageFlags *wait_stages;
        uint32_t *indexes;
        uint64_t *values;

        for (uint32_t j = 0; j < submit->commandBufferCount; j++)
        {
            VkCommandBuffer *command_buffers = (VkCommandBuffer *)submit->pCommandBuffers; /* cast away const, chain has been copied in the thunks */
            struct vulkan_command_buffer *command_buffer = vulkan_command_buffer_from_handle( command_buffers[j] );
            command_buffers[j] = command_buffer->host.command_buffer;
        }

        for (uint32_t j = 0; j < submit->waitSemaphoreCount; j++)
        {
            VkSemaphore *semaphores = (VkSemaphore *)submit->pWaitSemaphores; /* cast away const, it has been copied in the thunks */
            struct vulkan_semaphore *semaphore = vulkan_semaphore_from_handle( semaphores[j] );
            semaphores[j] = semaphore->host.semaphore;
        }

        for (uint32_t j = 0; j < submit->signalSemaphoreCount; j++)
        {
            VkSemaphore *semaphores = (VkSemaphore *)submit->pSignalSemaphores; /* cast away const, it has been copied in the thunks */
            struct vulkan_semaphore *semaphore = vulkan_semaphore_from_handle( semaphores[j] );
            semaphores[j] = semaphore->host.semaphore;
        }

        for (next = &prev->pNext; *next; prev = *next, next = &(*next)->pNext)
        {
            switch ((*next)->sType)
            {
            case VK_STRUCTURE_TYPE_D3D12_FENCE_SUBMIT_INFO_KHR:
            {
                VkD3D12FenceSubmitInfoKHR *info = (VkD3D12FenceSubmitInfoKHR *)*next;

                if (timeline->sType == VK_STRUCTURE_TYPE_D3D12_FENCE_SUBMIT_INFO_KHR)
                    ERR( "Duplicated d3d12 fence submit info.\n" );
                else if (timeline->sType)
                    FIXME( "Both d3d12 fence and timeline submit info.\n" );
                timeline->sType = info->sType;
                timeline->waitSemaphoreValueCount = info->waitSemaphoreValuesCount;
                timeline->pWaitSemaphoreValues = info->pWaitSemaphoreValues;
                timeline->signalSemaphoreValueCount = info->signalSemaphoreValuesCount;
                timeline->pSignalSemaphoreValues = info->pSignalSemaphoreValues;
                *next = (*next)->pNext; next = &prev;
                break;
            }
            case VK_STRUCTURE_TYPE_DEVICE_GROUP_SUBMIT_INFO:
                device_group = (VkDeviceGroupSubmitInfo *)*next;
                break;
            case VK_STRUCTURE_TYPE_FRAME_BOUNDARY_EXT: break;
            case VK_STRUCTURE_TYPE_FRAME_BOUNDARY_TENSORS_ARM: break;
            case VK_STRUCTURE_TYPE_LATENCY_SUBMISSION_PRESENT_ID_NV: break;
            case VK_STRUCTURE_TYPE_PERFORMANCE_QUERY_SUBMIT_INFO_KHR: break;
            case VK_STRUCTURE_TYPE_PROTECTED_SUBMIT_INFO: break;
            case VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO:
                if (timeline->sType == VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO)
                    ERR( "Duplicated timeline semaphore submit info.\n" );
                else if (timeline->sType)
                    FIXME( "Both d3d12 fence and timeline submit info.\n" );

                *timeline = *(VkTimelineSemaphoreSubmitInfo *)*next;
                *next = (*next)->pNext; next = &prev; /* remove it from the chain, we'll add it back below */
                break;
            case VK_STRUCTURE_TYPE_WIN32_KEYED_MUTEX_ACQUIRE_RELEASE_INFO_KHR:
            {
                VkWin32KeyedMutexAcquireReleaseInfoKHR *mutex_info = (VkWin32KeyedMutexAcquireReleaseInfoKHR *)*next;
                if ((res = acquire_keyed_mutexes( mutex_info, &pool, &wait_infos, &wait_count ))) goto failed;
                if ((res = release_keyed_mutexes( mutex_info, &pool, &signal_infos, &signal_count ))) goto failed;
                *next = (*next)->pNext; next = &prev;
                break;
            }
            default: FIXME( "Unhandled sType %u.\n", (*next)->sType ); break;
            }
        }

        if (wait_count) /* extra wait semaphores, need to update arrays and counts */
        {
            if (!(wait_semaphores = mem_alloc( &pool, (submit->waitSemaphoreCount + wait_count) * sizeof(*wait_semaphores) ))) goto failed;
            memcpy( wait_semaphores, submit->pWaitSemaphores, submit->waitSemaphoreCount * sizeof(*wait_semaphores) );
            submit->pWaitSemaphores = wait_semaphores;

            if (!(wait_stages = mem_alloc( &pool, (submit->waitSemaphoreCount + wait_count) * sizeof(*wait_stages) ))) goto failed;
            memcpy( wait_stages, submit->pWaitDstStageMask, submit->waitSemaphoreCount * sizeof(*wait_stages) );
            submit->pWaitDstStageMask = wait_stages;

            for (uint32_t j = 0; j < wait_count; j++)
            {
                wait_semaphores[submit->waitSemaphoreCount + j] = wait_infos[j].semaphore;
                wait_stages[submit->waitSemaphoreCount + j] = wait_infos[j].stageMask;
            }
            submit->waitSemaphoreCount += wait_count;

            if (!(values = mem_alloc( &pool, (timeline->waitSemaphoreValueCount + wait_count) * sizeof(*values) ))) goto failed;
            memcpy( values, timeline->pWaitSemaphoreValues, timeline->waitSemaphoreValueCount * sizeof(*values) );
            for (uint32_t j = 0; j < wait_count; j++) values[submit->waitSemaphoreCount + j] = wait_infos[j].value;
            timeline->waitSemaphoreValueCount = submit->waitSemaphoreCount;
            timeline->pWaitSemaphoreValues = values;

            if (device_group)
            {
                if (!(indexes = mem_alloc( &pool, submit->waitSemaphoreCount * sizeof(*indexes) ))) goto failed;
                memcpy( indexes, device_group->pWaitSemaphoreDeviceIndices, device_group->waitSemaphoreCount * sizeof(*indexes) );
                for (uint32_t j = 0; j < wait_count; j++) indexes[device_group->waitSemaphoreCount + j] = wait_infos[j].deviceIndex;
                device_group->waitSemaphoreCount = submit->waitSemaphoreCount;
                device_group->pWaitSemaphoreDeviceIndices = indexes;
            }
        }

        if (signal_count) /* extra signal semaphores, need to update arrays and counts */
        {
            if (!(signal_semaphores = mem_alloc( &pool, (submit->signalSemaphoreCount + signal_count) * sizeof(*signal_semaphores) ))) goto failed;
            memcpy( signal_semaphores, submit->pSignalSemaphores, submit->signalSemaphoreCount * sizeof(*signal_semaphores) );
            for (uint32_t j = 0; j < signal_count; j++) signal_semaphores[submit->signalSemaphoreCount + j] = signal_infos[j].semaphore;
            submit->signalSemaphoreCount += signal_count;
            submit->pSignalSemaphores = signal_semaphores;

            if (!(values = mem_alloc( &pool, submit->signalSemaphoreCount * sizeof(*values) ))) goto failed;
            memcpy( values, timeline->pSignalSemaphoreValues, timeline->signalSemaphoreValueCount * sizeof(*values) );
            for (uint32_t j = 0; j < signal_count; j++) values[submit->signalSemaphoreCount + j] = signal_infos[j].value;
            timeline->signalSemaphoreValueCount = submit->signalSemaphoreCount;
            timeline->pSignalSemaphoreValues = values;

            if (device_group)
            {
                if (!(indexes = mem_alloc( &pool, submit->signalSemaphoreCount * sizeof(*indexes) ))) goto failed;
                memcpy( indexes, device_group->pSignalSemaphoreDeviceIndices, device_group->signalSemaphoreCount * sizeof(*indexes) );
                for (uint32_t j = 0; j < signal_count; j++) indexes[device_group->signalSemaphoreCount + j] = signal_infos[j].deviceIndex;
                device_group->signalSemaphoreCount = submit->signalSemaphoreCount;
                device_group->pSignalSemaphoreDeviceIndices = indexes;
            }
        }

        /* insert the timeline semaphore values in the chain if it was there or has been created */
        if (timeline->sType || wait_count || signal_count)
        {
            timeline->sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO;
            timeline->pNext = submit->pNext;
            submit->pNext = timeline;
        }
    }

    vulkan_queue_lock( queue );
    res = device->p_vkQueueSubmit( queue->host.queue, count, submits, fence ? fence->host.fence : 0 );
    vulkan_queue_unlock( queue );

failed:
    mem_free( &pool );
    return res;
}

static VkResult queue_submit( struct vulkan_queue *queue, uint32_t count, const VkSubmitInfo2 *submits, VkFence client_fence, PFN_vkQueueSubmit2 p_vkQueueSubmit2 )
{
    struct vulkan_fence *fence = client_fence ? vulkan_fence_from_handle( client_fence ) : NULL;
    struct mempool pool = {0};
    VkResult res;

    for (uint32_t i = 0; i < count; i++)
    {
        VkSubmitInfo2 *submit = (VkSubmitInfo2 *)submits + i; /* cast away const, chain has been copied in the thunks */
        VkBaseOutStructure **next, *prev = (VkBaseOutStructure *)submit;

        for (uint32_t j = 0; j < submit->commandBufferInfoCount; j++)
        {
            VkCommandBufferSubmitInfoKHR *command_buffer_infos = (VkCommandBufferSubmitInfoKHR *)submit->pCommandBufferInfos; /* cast away const, chain has been copied in the thunks */
            struct vulkan_command_buffer *command_buffer = vulkan_command_buffer_from_handle( command_buffer_infos[j].commandBuffer );
            command_buffer_infos[j].commandBuffer = command_buffer->host.command_buffer;
            if (command_buffer_infos->pNext) FIXME( "Unhandled struct chain\n" );
        }

        for (uint32_t j = 0; j < submit->waitSemaphoreInfoCount; j++)
        {
            VkSemaphoreSubmitInfo *semaphore_infos = (VkSemaphoreSubmitInfo *)submit->pWaitSemaphoreInfos; /* cast away const, it has been copied in the thunks */
            struct vulkan_semaphore *semaphore = vulkan_semaphore_from_handle( semaphore_infos[j].semaphore );
            semaphore_infos[j].semaphore = semaphore->host.semaphore;
            if (semaphore_infos->pNext) FIXME( "Unhandled struct chain\n" );
        }

        for (uint32_t j = 0; j < submit->signalSemaphoreInfoCount; j++)
        {
            VkSemaphoreSubmitInfo *semaphore_infos = (VkSemaphoreSubmitInfo *)submit->pSignalSemaphoreInfos; /* cast away const, it has been copied in the thunks */
            struct vulkan_semaphore *semaphore = vulkan_semaphore_from_handle( semaphore_infos[j].semaphore );
            semaphore_infos[j].semaphore = semaphore->host.semaphore;
            if (semaphore_infos->pNext) FIXME( "Unhandled struct chain\n" );
        }

        for (next = &prev->pNext; *next; prev = *next, next = &(*next)->pNext)
        {
            switch ((*next)->sType)
            {
            case VK_STRUCTURE_TYPE_FRAME_BOUNDARY_EXT: break;
            case VK_STRUCTURE_TYPE_FRAME_BOUNDARY_TENSORS_ARM: break;
            case VK_STRUCTURE_TYPE_LATENCY_SUBMISSION_PRESENT_ID_NV: break;
            case VK_STRUCTURE_TYPE_PERFORMANCE_QUERY_SUBMIT_INFO_KHR: break;
            case VK_STRUCTURE_TYPE_WIN32_KEYED_MUTEX_ACQUIRE_RELEASE_INFO_KHR:
            {
                VkWin32KeyedMutexAcquireReleaseInfoKHR *mutex_info = (VkWin32KeyedMutexAcquireReleaseInfoKHR *)*next;
                if ((res = acquire_keyed_mutexes( mutex_info, &pool, &submit->pWaitSemaphoreInfos, &submit->waitSemaphoreInfoCount ))) goto failed;
                if ((res = release_keyed_mutexes( mutex_info, &pool, &submit->pSignalSemaphoreInfos, &submit->signalSemaphoreInfoCount ))) goto failed;
                *next = (*next)->pNext; next = &prev;
                break;
            }
            default: FIXME( "Unhandled sType %u.\n", (*next)->sType ); break;
            }
        }
    }

    vulkan_queue_lock( queue );
    res = p_vkQueueSubmit2( queue->host.queue, count, submits, fence ? fence->host.fence : 0 );
    vulkan_queue_unlock( queue );

failed:
    mem_free( &pool );
    return res;
}

static VkResult win32u_vkQueueSubmit2( VkQueue client_queue, uint32_t count, const VkSubmitInfo2 *submits, VkFence client_fence )
{
    struct vulkan_fence *fence = client_fence ? vulkan_fence_from_handle( client_fence ) : NULL;
    struct vulkan_queue *queue = vulkan_queue_from_handle( client_queue );
    struct vulkan_device *device = queue->device;

    TRACE( "queue %p, count %u, submits %p, fence %p\n", queue, count, submits, fence );

    return queue_submit( queue, count, submits, client_fence, device->p_vkQueueSubmit2 );
}

static VkResult win32u_vkQueueSubmit2KHR( VkQueue client_queue, uint32_t count, const VkSubmitInfo2 *submits, VkFence client_fence )
{
    struct vulkan_fence *fence = client_fence ? vulkan_fence_from_handle( client_fence ) : NULL;
    struct vulkan_queue *queue = vulkan_queue_from_handle( client_queue );
    struct vulkan_device *device = queue->device;

    TRACE( "queue %p, count %u, submits %p, fence %p\n", queue, count, submits, fence );

    return queue_submit( queue, count, submits, client_fence, device->p_vkQueueSubmit2KHR );
}

static HANDLE create_shared_semaphore_handle( D3DKMT_HANDLE local, const VkExportSemaphoreWin32HandleInfoKHR *info )
{
    SECURITY_DESCRIPTOR *security = info->pAttributes ? info->pAttributes->lpSecurityDescriptor : NULL;
    WCHAR bufferW[MAX_PATH * 2];
    UNICODE_STRING name = {.Buffer = bufferW};
    OBJECT_ATTRIBUTES attr;
    NTSTATUS status;
    HANDLE shared;

    if (info->name) init_shared_resource_path( info->name, &name );
    InitializeObjectAttributes( &attr, info->name ? &name : NULL, OBJ_CASE_INSENSITIVE, NULL, security );

    if (!(status = NtGdiDdDDIShareObjects( 1, &local, &attr, info->dwAccess, &shared ))) return shared;
    WARN( "Failed to share resource %#x, status %#x\n", local, status );
    return NULL;
}

HANDLE open_shared_semaphore_from_name( const WCHAR *name )
{
    D3DKMT_OPENSYNCOBJECTNTHANDLEFROMNAME open_name = {0};
    WCHAR bufferW[MAX_PATH * 2];
    UNICODE_STRING name_str = {.Buffer = bufferW};
    OBJECT_ATTRIBUTES attr;
    NTSTATUS status;

    init_shared_resource_path( name, &name_str );
    InitializeObjectAttributes( &attr, &name_str, OBJ_OPENIF, NULL, NULL );

    open_name.dwDesiredAccess = GENERIC_ALL;
    open_name.pObjAttrib = &attr;
    status = NtGdiDdDDIOpenSyncObjectNtHandleFromName( &open_name );
    if (status) WARN( "Failed to open %s, status %#x\n", debugstr_w( name ), status );
    return open_name.hNtHandle;
}

static VkResult win32u_vkCreateSemaphore( VkDevice client_device, const VkSemaphoreCreateInfo *client_create_info,
                                          const VkAllocationCallbacks *allocator, VkSemaphore *ret )
{
    VkSemaphoreCreateInfo *create_info = (VkSemaphoreCreateInfo *)client_create_info; /* cast away const, chain has been copied in the thunks */
    VkExportSemaphoreWin32HandleInfoKHR export_win32 = {.dwAccess = GENERIC_ALL};
    struct vulkan_device *device = vulkan_device_from_handle( client_device );
    VkBaseOutStructure **next, *prev = (VkBaseOutStructure *)create_info;
    struct vulkan_instance *instance = device->physical_device->instance;
    VkExportSemaphoreCreateInfoKHR *export_info = NULL;
    struct semaphore *semaphore;
    VkSemaphore host_semaphore;
    BOOL nt_shared = FALSE;
    VkResult res;

    TRACE( "device %p, create_info %p, allocator %p, ret %p\n", device, create_info, allocator, ret );

    for (next = &prev->pNext; *next; prev = *next, next = &(*next)->pNext)
    {
        switch ((*next)->sType)
        {
        case VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO:
            export_info = (VkExportSemaphoreCreateInfoKHR *)*next;
            if (!(export_info->handleTypes & EXTERNAL_SEMAPHORE_WIN32_BITS))
                FIXME( "Unsupported handle types %#x\n", export_info->handleTypes );
            else
            {
                nt_shared = !(export_info->handleTypes & VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_WIN32_KMT_BIT);
                export_info->handleTypes = get_host_external_semaphore_type();
            }
            break;
        case VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_WIN32_HANDLE_INFO_KHR:
            export_win32 = *(VkExportSemaphoreWin32HandleInfoKHR *)*next;
            *next = (*next)->pNext; next = &prev;
            break;
        case VK_STRUCTURE_TYPE_QUERY_LOW_LATENCY_SUPPORT_NV: break;
        case VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO: break;
        default: FIXME( "Unhandled sType %u.\n", (*next)->sType ); break;
        }
    }

    if (!(semaphore = calloc( 1, sizeof(*semaphore) ))) return VK_ERROR_OUT_OF_HOST_MEMORY;

    if ((res = device->p_vkCreateSemaphore( device->host.device, create_info, NULL /* allocator */, &host_semaphore )))
    {
        free( semaphore );
        return res;
    }

    if (export_info)
    {
        VkSemaphoreGetFdInfoKHR fd_info = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_GET_FD_INFO_KHR, .semaphore = host_semaphore};
        int fd = -1;

        switch ((fd_info.handleType = get_host_external_semaphore_type()))
        {
        case VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT:
            if ((res = device->p_vkGetSemaphoreFdKHR( device->host.device, &fd_info, &fd ))) goto failed;
            break;
        default:
            FIXME( "Unsupported handle type %#x\n", fd_info.handleType );
            break;
        }

        semaphore->local = d3dkmt_create_sync( fd, nt_shared ? NULL : &semaphore->global );
        close( fd );

        if (!semaphore->local) goto failed;
        if (nt_shared && !(semaphore->shared = create_shared_semaphore_handle( semaphore->local, &export_win32 ))) goto failed;
    }

    vulkan_object_init( &semaphore->obj.obj, host_semaphore );
    instance->p_insert_object( instance, &semaphore->obj.obj );

    *ret = semaphore->obj.client.semaphore;
    return res;

failed:
    WARN( "Failed to create semaphore, res %d\n", res );
    device->p_vkDestroySemaphore( device->host.device, host_semaphore, NULL );
    d3dkmt_destroy_sync( semaphore->local );
    free( semaphore );
    return VK_ERROR_OUT_OF_HOST_MEMORY;
}

static void win32u_vkDestroySemaphore( VkDevice client_device, VkSemaphore client_semaphore, const VkAllocationCallbacks *allocator )
{
    struct vulkan_device *device = vulkan_device_from_handle( client_device );
    struct semaphore *semaphore = semaphore_from_handle( client_semaphore );
    struct vulkan_instance *instance = device->physical_device->instance;

    TRACE( "device %p, semaphore %p, allocator %p\n", device, semaphore, allocator );

    if (!client_semaphore) return;

    device->p_vkDestroySemaphore( device->host.device, semaphore->obj.host.semaphore, NULL /* allocator */ );
    instance->p_remove_object( instance, &semaphore->obj.obj );

    if (semaphore->shared) NtClose( semaphore->shared );
    d3dkmt_destroy_sync( semaphore->local );
    free( semaphore );
}

static VkResult win32u_vkGetSemaphoreWin32HandleKHR( VkDevice client_device, const VkSemaphoreGetWin32HandleInfoKHR *handle_info, HANDLE *handle )
{
    struct vulkan_device *device = vulkan_device_from_handle( client_device );
    struct semaphore *semaphore = semaphore_from_handle( handle_info->semaphore );

    TRACE( "device %p, handle_info %p, handle %p\n", device, handle_info, handle );

    switch (handle_info->handleType)
    {
    case VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_WIN32_KMT_BIT:
        TRACE( "Returning global D3DKMT handle %#x\n", semaphore->global );
        *handle = UlongToPtr( semaphore->global );
        return VK_SUCCESS;

    case VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_WIN32_BIT:
    case VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_D3D12_FENCE_BIT:
        NtDuplicateObject( NtCurrentProcess(), semaphore->shared, NtCurrentProcess(), handle, 0, 0, DUPLICATE_SAME_ATTRIBUTES | DUPLICATE_SAME_ACCESS );
        TRACE( "Returning NT shared handle %p -> %p\n", semaphore->shared, *handle );
        return VK_SUCCESS;

    default:
        FIXME( "Unsupported handle type %#x\n", handle_info->handleType );
        return VK_ERROR_INCOMPATIBLE_DRIVER;
    }
}

static VkResult win32u_vkImportSemaphoreWin32HandleKHR( VkDevice client_device, const VkImportSemaphoreWin32HandleInfoKHR *handle_info )
{
    VkImportSemaphoreFdInfoKHR fd_info = {.sType = VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_FD_INFO_KHR};
    struct vulkan_device *device = vulkan_device_from_handle( client_device );
    struct semaphore *semaphore = semaphore_from_handle( handle_info->semaphore );
    struct vulkan_instance *instance = device->physical_device->instance;
    D3DKMT_HANDLE local, global = 0;
    VkResult res = VK_SUCCESS;
    HANDLE shared = NULL;

    TRACE( "device %p, handle_info %p\n", device, handle_info );

    switch (handle_info->handleType)
    {
    case VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_WIN32_KMT_BIT:
        global = PtrToUlong( handle_info->handle );
        if (!(local = d3dkmt_open_sync( global, NULL ))) return VK_ERROR_INVALID_EXTERNAL_HANDLE;
        break;
    case VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_WIN32_BIT:
    case VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_D3D12_FENCE_BIT:
        if (handle_info->name && !(shared = open_shared_semaphore_from_name( handle_info->name )))
            return VK_ERROR_INVALID_EXTERNAL_HANDLE;
        else if (!(shared = handle_info->handle) || NtDuplicateObject( NtCurrentProcess(), shared, NtCurrentProcess(), &shared,
                                                                       0, 0, DUPLICATE_SAME_ATTRIBUTES | DUPLICATE_SAME_ACCESS ))
            return VK_ERROR_INVALID_EXTERNAL_HANDLE;

        if (!(local = d3dkmt_open_sync( 0, shared )))
        {
            NtClose( shared );
            return VK_ERROR_INVALID_EXTERNAL_HANDLE;
        }
        break;
    default:
        FIXME( "Unsupported handle type %#x\n", handle_info->handleType );
        return VK_ERROR_INVALID_EXTERNAL_HANDLE;
    }

    if ((fd_info.fd = d3dkmt_object_get_fd( local )) < 0) res = VK_ERROR_INVALID_EXTERNAL_HANDLE;
    if (!res && handle_info->handleType == VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_D3D12_FENCE_BIT)
    {
        /* Recreate semaphore to make sure it has timeline type. */
        VkSemaphoreTypeCreateInfo type_info =
        {
            .sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO,
            .semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE,
        };
        VkSemaphoreCreateInfo create_info =
        {
            .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
            .pNext = &type_info,
        };
        VkSemaphore new_semaphore;

        if ((res = device->p_vkCreateSemaphore( device->host.device, &create_info, NULL, &new_semaphore )))
        {
            ERR( "Failed to create timeline semaphore, vr %d.\n", res );
        }
        else
        {
            instance->p_remove_object( instance, &semaphore->obj.obj );
            device->p_vkDestroySemaphore( device->host.device, semaphore->obj.host.semaphore, NULL );
            semaphore->obj.host.semaphore = new_semaphore;
            instance->p_insert_object( instance, &semaphore->obj.obj );
        }
    }

    if (!res)
    {
        fd_info.handleType = get_host_external_semaphore_type();
        fd_info.semaphore = semaphore->obj.host.semaphore;
        fd_info.flags = handle_info->flags;
        res = device->p_vkImportSemaphoreFdKHR( device->host.device, &fd_info );
    }

    if (res || handle_info->flags & VK_SEMAPHORE_IMPORT_TEMPORARY_BIT)
    {
        /* FIXME: Should we still keep the temporary handles for vkGetSemaphoreWin32HandleKHR? */
        if (shared) NtClose( shared );
        d3dkmt_destroy_sync( local );
    }
    else
    {
        if (semaphore->shared) NtClose( semaphore->shared );
        d3dkmt_destroy_sync( semaphore->local );
        semaphore->shared = shared;
        semaphore->global = global;
        semaphore->local = local;
    }
    return res;
}

static void get_physical_device_external_semaphore_properties( struct vulkan_physical_device *physical_device, const VkPhysicalDeviceExternalSemaphoreInfo *client_semaphore_info,
                                                               VkExternalSemaphoreProperties *semaphore_properties, PFN_vkGetPhysicalDeviceExternalSemaphoreProperties p_vkGetPhysicalDeviceExternalSemaphoreProperties )
{
    VkPhysicalDeviceExternalSemaphoreInfo *semaphore_info = (VkPhysicalDeviceExternalSemaphoreInfo *)client_semaphore_info; /* cast away const, it has been copied in the thunks */
    VkExternalSemaphoreHandleTypeFlagBits handle_type;

    handle_type = semaphore_info->handleType;
    if (semaphore_info->handleType & EXTERNAL_SEMAPHORE_WIN32_BITS) semaphore_info->handleType = get_host_external_semaphore_type();

    p_vkGetPhysicalDeviceExternalSemaphoreProperties( physical_device->host.physical_device, semaphore_info, semaphore_properties );
    semaphore_properties->compatibleHandleTypes = handle_type;
    semaphore_properties->exportFromImportedHandleTypes = handle_type;
}

static void win32u_vkGetPhysicalDeviceExternalSemaphoreProperties( VkPhysicalDevice client_physical_device, const VkPhysicalDeviceExternalSemaphoreInfo *semaphore_info,
                                                                   VkExternalSemaphoreProperties *semaphore_properties )
{
    struct vulkan_physical_device *physical_device = vulkan_physical_device_from_handle( client_physical_device );
    struct vulkan_instance *instance = physical_device->instance;

    TRACE( "physical_device %p, semaphore_info %p, semaphore_properties %p\n", physical_device, semaphore_info, semaphore_properties );

    get_physical_device_external_semaphore_properties( physical_device, semaphore_info, semaphore_properties, instance->p_vkGetPhysicalDeviceExternalSemaphoreProperties );
}

static void win32u_vkGetPhysicalDeviceExternalSemaphorePropertiesKHR( VkPhysicalDevice client_physical_device, const VkPhysicalDeviceExternalSemaphoreInfo *semaphore_info,
                                                                      VkExternalSemaphoreProperties *semaphore_properties )
{
    struct vulkan_physical_device *physical_device = vulkan_physical_device_from_handle( client_physical_device );
    struct vulkan_instance *instance = physical_device->instance;

    TRACE( "physical_device %p, semaphore_info %p, semaphore_properties %p\n", physical_device, semaphore_info, semaphore_properties );

    get_physical_device_external_semaphore_properties( physical_device, semaphore_info, semaphore_properties, instance->p_vkGetPhysicalDeviceExternalSemaphorePropertiesKHR );
}

static VkResult win32u_vkCreateFence( VkDevice client_device, const VkFenceCreateInfo *client_create_info, const VkAllocationCallbacks *allocator, VkFence *ret )
{
    VkFenceCreateInfo *create_info = (VkFenceCreateInfo *)client_create_info; /* cast away const, chain has been copied in the thunks */
    struct vulkan_device *device = vulkan_device_from_handle( client_device );
    VkExportSemaphoreWin32HandleInfoKHR export_win32 = {.dwAccess = GENERIC_ALL};
    VkBaseOutStructure **next, *prev = (VkBaseOutStructure *)create_info;
    struct vulkan_instance *instance = device->physical_device->instance;
    VkExportFenceCreateInfoKHR *export_info = NULL;
    BOOL nt_shared = FALSE;
    struct fence *fence;
    VkFence host_fence;
    VkResult res;

    TRACE( "device %p, create_info %p, allocator %p, ret %p\n", device, create_info, allocator, ret );

    for (next = &prev->pNext; *next; prev = *next, next = &(*next)->pNext)
    {
        switch ((*next)->sType)
        {
        case VK_STRUCTURE_TYPE_EXPORT_FENCE_CREATE_INFO:
            export_info = (VkExportFenceCreateInfoKHR *)*next;
            if (!(export_info->handleTypes & EXTERNAL_FENCE_WIN32_BITS))
                FIXME( "Unsupported handle types %#x\n", export_info->handleTypes );
            else
            {
                nt_shared = !(export_info->handleTypes & VK_EXTERNAL_FENCE_HANDLE_TYPE_OPAQUE_WIN32_KMT_BIT);
                export_info->handleTypes = get_host_external_fence_type();
            }
            break;
        case VK_STRUCTURE_TYPE_EXPORT_FENCE_WIN32_HANDLE_INFO_KHR:
        {
            VkExportFenceWin32HandleInfoKHR *fence_win32 = (VkExportFenceWin32HandleInfoKHR *)*next;
            export_win32.pAttributes = fence_win32->pAttributes;
            export_win32.dwAccess = fence_win32->dwAccess;
            export_win32.name = fence_win32->name;
            *next = (*next)->pNext; next = &prev;
            break;
        }
        default: FIXME( "Unhandled sType %u.\n", (*next)->sType ); break;
        }
    }

    if (!(fence = calloc( 1, sizeof(*fence) ))) return VK_ERROR_OUT_OF_HOST_MEMORY;

    if ((res = device->p_vkCreateFence( device->host.device, create_info, NULL /* allocator */, &host_fence )))
    {
        free( fence );
        return res;
    }

    if (export_info)
    {
        VkFenceGetFdInfoKHR fd_info = {.sType = VK_STRUCTURE_TYPE_FENCE_GET_FD_INFO_KHR, .fence = host_fence};
        int fd = -1;

        switch ((fd_info.handleType = get_host_external_fence_type()))
        {
        case VK_EXTERNAL_FENCE_HANDLE_TYPE_OPAQUE_FD_BIT:
            if ((res = device->p_vkGetFenceFdKHR( device->host.device, &fd_info, &fd ))) goto failed;
            break;
        default:
            FIXME( "Unsupported handle type %#x\n", fd_info.handleType );
            break;
        }

        fence->local = d3dkmt_create_sync( fd, nt_shared ? NULL : &fence->global );
        close( fd );

        if (!fence->local) goto failed;
        if (nt_shared && !(fence->shared = create_shared_semaphore_handle( fence->local, &export_win32 ))) goto failed;
    }

    vulkan_object_init( &fence->obj.obj, host_fence );
    instance->p_insert_object( instance, &fence->obj.obj );

    *ret = fence->obj.client.fence;
    return res;

failed:
    WARN( "Failed to create fence, res %d\n", res );
    device->p_vkDestroyFence( device->host.device, host_fence, NULL );
    d3dkmt_destroy_sync( fence->local );
    free( fence );
    return VK_ERROR_OUT_OF_HOST_MEMORY;
}

static void win32u_vkDestroyFence( VkDevice client_device, VkFence client_fence, const VkAllocationCallbacks *allocator )
{
    struct vulkan_device *device = vulkan_device_from_handle( client_device );
    struct fence *fence = fence_from_handle( client_fence );
    struct vulkan_instance *instance = device->physical_device->instance;

    TRACE( "device %p, fence %p, allocator %p\n", device, fence, allocator );

    if (!client_fence) return;

    device->p_vkDestroyFence( device->host.device, fence->obj.host.fence, NULL /* allocator */ );
    instance->p_remove_object( instance, &fence->obj.obj );

    if (fence->shared) NtClose( fence->shared );
    d3dkmt_destroy_sync( fence->local );
    free( fence );
}

static VkResult win32u_vkGetFenceWin32HandleKHR( VkDevice client_device, const VkFenceGetWin32HandleInfoKHR *handle_info, HANDLE *handle )
{
    struct vulkan_device *device = vulkan_device_from_handle( client_device );
    struct fence *fence = fence_from_handle( handle_info->fence );

    TRACE( "device %p, handle_info %p, handle %p\n", device, handle_info, handle );

    switch (handle_info->handleType)
    {
    case VK_EXTERNAL_FENCE_HANDLE_TYPE_OPAQUE_WIN32_KMT_BIT:
        TRACE( "Returning global D3DKMT handle %#x\n", fence->global );
        *handle = UlongToPtr( fence->global );
        return VK_SUCCESS;

    case VK_EXTERNAL_FENCE_HANDLE_TYPE_OPAQUE_WIN32_BIT:
        NtDuplicateObject( NtCurrentProcess(), fence->shared, NtCurrentProcess(), handle, 0, 0, DUPLICATE_SAME_ATTRIBUTES | DUPLICATE_SAME_ACCESS );
        TRACE( "Returning NT shared handle %p -> %p\n", fence->shared, *handle );
        return VK_SUCCESS;

    default:
        FIXME( "Unsupported handle type %#x\n", handle_info->handleType );
        return VK_ERROR_INCOMPATIBLE_DRIVER;
    }
}

static VkResult win32u_vkImportFenceWin32HandleKHR( VkDevice client_device, const VkImportFenceWin32HandleInfoKHR *handle_info )
{
    VkImportFenceFdInfoKHR fd_info = {.sType = VK_STRUCTURE_TYPE_IMPORT_FENCE_FD_INFO_KHR};
    struct vulkan_device *device = vulkan_device_from_handle( client_device );
    struct fence *fence = fence_from_handle( handle_info->fence );
    D3DKMT_HANDLE local, global = 0;
    HANDLE shared = NULL;
    VkResult res;

    TRACE( "device %p, handle_info %p\n", device, handle_info );

    switch (handle_info->handleType)
    {
    case VK_EXTERNAL_FENCE_HANDLE_TYPE_OPAQUE_WIN32_KMT_BIT:
        global = PtrToUlong( handle_info->handle );
        if (!(local = d3dkmt_open_sync( global, NULL ))) return VK_ERROR_INVALID_EXTERNAL_HANDLE;
        break;
    case VK_EXTERNAL_FENCE_HANDLE_TYPE_OPAQUE_WIN32_BIT:
        if (handle_info->name && !(shared = open_shared_semaphore_from_name( handle_info->name )))
            return VK_ERROR_INVALID_EXTERNAL_HANDLE;
        else if (!(shared = handle_info->handle) || NtDuplicateObject( NtCurrentProcess(), shared, NtCurrentProcess(), &shared,
                                                                       0, 0, DUPLICATE_SAME_ATTRIBUTES | DUPLICATE_SAME_ACCESS ))
            return VK_ERROR_INVALID_EXTERNAL_HANDLE;

        if (!(local = d3dkmt_open_sync( 0, shared )))
        {
            NtClose( shared );
            return VK_ERROR_INVALID_EXTERNAL_HANDLE;
        }
        break;
    default:
        FIXME( "Unsupported handle type %#x\n", handle_info->handleType );
        return VK_ERROR_INVALID_EXTERNAL_HANDLE;
    }

    if ((fd_info.fd = d3dkmt_object_get_fd( local )) < 0) res = VK_ERROR_INVALID_EXTERNAL_HANDLE;
    else
    {
        fd_info.handleType = get_host_external_fence_type();
        fd_info.fence = fence->obj.host.fence;
        fd_info.flags = handle_info->flags;
        res = device->p_vkImportFenceFdKHR( device->host.device, &fd_info );
    }

    if (res || handle_info->flags & VK_FENCE_IMPORT_TEMPORARY_BIT)
    {
        /* FIXME: Should we still keep the temporary handles for vkGetFenceWin32HandleKHR? */
        if (shared) NtClose( shared );
        d3dkmt_destroy_sync( local );
    }
    else
    {
        if (fence->shared) NtClose( fence->shared );
        if (fence->local) d3dkmt_destroy_sync( fence->local );
        fence->shared = shared;
        fence->global = global;
        fence->local = local;
    }
    return VK_SUCCESS;
}

static void get_physical_device_external_fence_properties( struct vulkan_physical_device *physical_device, const VkPhysicalDeviceExternalFenceInfo *client_fence_info,
                                                           VkExternalFenceProperties *fence_properties, PFN_vkGetPhysicalDeviceExternalFenceProperties p_vkGetPhysicalDeviceExternalFenceProperties )
{
    VkPhysicalDeviceExternalFenceInfo *fence_info = (VkPhysicalDeviceExternalFenceInfo *)client_fence_info; /* cast away const, it has been copied in the thunks */
    VkExternalFenceHandleTypeFlagBits handle_type;

    handle_type = fence_info->handleType;
    if (fence_info->handleType & EXTERNAL_FENCE_WIN32_BITS) fence_info->handleType = get_host_external_fence_type();

    p_vkGetPhysicalDeviceExternalFenceProperties( physical_device->host.physical_device, fence_info, fence_properties );
    fence_properties->compatibleHandleTypes = handle_type;
    fence_properties->exportFromImportedHandleTypes = handle_type;
}

static void win32u_vkGetPhysicalDeviceExternalFenceProperties( VkPhysicalDevice client_physical_device, const VkPhysicalDeviceExternalFenceInfo *fence_info,
                                                               VkExternalFenceProperties *fence_properties )
{
    struct vulkan_physical_device *physical_device = vulkan_physical_device_from_handle( client_physical_device );
    struct vulkan_instance *instance = physical_device->instance;

    TRACE( "physical_device %p, fence_info %p, fence_properties %p\n", physical_device, fence_info, fence_properties );

    get_physical_device_external_fence_properties( physical_device, fence_info, fence_properties, instance->p_vkGetPhysicalDeviceExternalFenceProperties );
}

static void win32u_vkGetPhysicalDeviceExternalFencePropertiesKHR( VkPhysicalDevice client_physical_device, const VkPhysicalDeviceExternalFenceInfo *fence_info,
                                                                  VkExternalFenceProperties *fence_properties )
{
    struct vulkan_physical_device *physical_device = vulkan_physical_device_from_handle( client_physical_device );
    struct vulkan_instance *instance = physical_device->instance;

    TRACE( "physical_device %p, fence_info %p, fence_properties %p\n", physical_device, fence_info, fence_properties );

    get_physical_device_external_fence_properties( physical_device, fence_info, fence_properties, instance->p_vkGetPhysicalDeviceExternalFencePropertiesKHR );
}

static struct vulkan_funcs vulkan_funcs =
{
    .p_vkAcquireFullScreenExclusiveModeEXT = win32u_vkAcquireFullScreenExclusiveModeEXT,
    .p_vkAcquireNextImage2KHR = win32u_vkAcquireNextImage2KHR,
    .p_vkAcquireNextImageKHR = win32u_vkAcquireNextImageKHR,
    .p_vkAllocateMemory = win32u_vkAllocateMemory,
    .p_vkCreateBuffer = win32u_vkCreateBuffer,
    .p_vkCreateDevice = win32u_vkCreateDevice,
    .p_vkCreateFence = win32u_vkCreateFence,
    .p_vkCreateImage = win32u_vkCreateImage,
    .p_vkCreateInstance = win32u_vkCreateInstance,
    .p_vkCreateSemaphore = win32u_vkCreateSemaphore,
    .p_vkCreateSwapchainKHR = win32u_vkCreateSwapchainKHR,
    .p_vkCreateWin32SurfaceKHR = win32u_vkCreateWin32SurfaceKHR,
    .p_vkDestroyDevice = win32u_vkDestroyDevice,
    .p_vkDestroyFence = win32u_vkDestroyFence,
    .p_vkDestroyInstance = win32u_vkDestroyInstance,
    .p_vkDestroySemaphore = win32u_vkDestroySemaphore,
    .p_vkDestroySurfaceKHR = win32u_vkDestroySurfaceKHR,
    .p_vkDestroySwapchainKHR = win32u_vkDestroySwapchainKHR,
    .p_vkFreeMemory = win32u_vkFreeMemory,
    .p_vkGetDeviceGroupSurfacePresentModes2EXT = win32u_vkGetDeviceGroupSurfacePresentModes2EXT,
    .p_vkGetDeviceBufferMemoryRequirements = win32u_vkGetDeviceBufferMemoryRequirements,
    .p_vkGetDeviceBufferMemoryRequirementsKHR = win32u_vkGetDeviceBufferMemoryRequirements,
    .p_vkGetDeviceImageMemoryRequirements = win32u_vkGetDeviceImageMemoryRequirements,
    .p_vkGetDeviceQueue = win32u_vkGetDeviceQueue,
    .p_vkGetDeviceQueue2 = win32u_vkGetDeviceQueue2,
    .p_vkGetFenceWin32HandleKHR = win32u_vkGetFenceWin32HandleKHR,
    .p_vkGetMemoryWin32HandleKHR = win32u_vkGetMemoryWin32HandleKHR,
    .p_vkGetMemoryWin32HandlePropertiesKHR = win32u_vkGetMemoryWin32HandlePropertiesKHR,
    .p_vkGetPhysicalDeviceExternalBufferProperties = win32u_vkGetPhysicalDeviceExternalBufferProperties,
    .p_vkGetPhysicalDeviceExternalBufferPropertiesKHR = win32u_vkGetPhysicalDeviceExternalBufferPropertiesKHR,
    .p_vkGetPhysicalDeviceExternalFenceProperties = win32u_vkGetPhysicalDeviceExternalFenceProperties,
    .p_vkGetPhysicalDeviceExternalFencePropertiesKHR = win32u_vkGetPhysicalDeviceExternalFencePropertiesKHR,
    .p_vkGetPhysicalDeviceExternalSemaphoreProperties = win32u_vkGetPhysicalDeviceExternalSemaphoreProperties,
    .p_vkGetPhysicalDeviceExternalSemaphorePropertiesKHR = win32u_vkGetPhysicalDeviceExternalSemaphorePropertiesKHR,
    .p_vkGetPhysicalDeviceImageFormatProperties2 = win32u_vkGetPhysicalDeviceImageFormatProperties2,
    .p_vkGetPhysicalDeviceImageFormatProperties2KHR = win32u_vkGetPhysicalDeviceImageFormatProperties2KHR,
    .p_vkGetPhysicalDevicePresentRectanglesKHR = win32u_vkGetPhysicalDevicePresentRectanglesKHR,
    .p_vkGetPhysicalDeviceProperties = win32u_vkGetPhysicalDeviceProperties,
    .p_vkGetPhysicalDeviceProperties2 = win32u_vkGetPhysicalDeviceProperties2,
    .p_vkGetPhysicalDeviceProperties2KHR = win32u_vkGetPhysicalDeviceProperties2KHR,
    .p_vkGetPhysicalDeviceSurfaceCapabilities2KHR = win32u_vkGetPhysicalDeviceSurfaceCapabilities2KHR,
    .p_vkGetPhysicalDeviceSurfaceCapabilitiesKHR = win32u_vkGetPhysicalDeviceSurfaceCapabilitiesKHR,
    .p_vkGetPhysicalDeviceSurfaceFormats2KHR = win32u_vkGetPhysicalDeviceSurfaceFormats2KHR,
    .p_vkGetPhysicalDeviceSurfaceFormatsKHR = win32u_vkGetPhysicalDeviceSurfaceFormatsKHR,
    .p_vkGetPhysicalDeviceSurfacePresentModes2EXT = win32u_vkGetPhysicalDeviceSurfacePresentModes2EXT,
    .p_vkGetPhysicalDeviceWin32PresentationSupportKHR = win32u_vkGetPhysicalDeviceWin32PresentationSupportKHR,
    .p_vkGetPastPresentationTimingEXT = win32u_vkGetPastPresentationTimingEXT,
    .p_vkGetLatencyTimingsNV = win32u_vkGetLatencyTimingsNV,
    .p_vkGetSemaphoreWin32HandleKHR = win32u_vkGetSemaphoreWin32HandleKHR,
    .p_vkGetSwapchainTimingPropertiesEXT = win32u_vkGetSwapchainTimingPropertiesEXT,
    .p_vkGetSwapchainImagesKHR = win32u_vkGetSwapchainImagesKHR,
    .p_vkImportFenceWin32HandleKHR = win32u_vkImportFenceWin32HandleKHR,
    .p_vkImportSemaphoreWin32HandleKHR = win32u_vkImportSemaphoreWin32HandleKHR,
    .p_vkLatencySleepNV = win32u_vkLatencySleepNV,
    .p_vkMapMemory = win32u_vkMapMemory,
    .p_vkMapMemory2KHR = win32u_vkMapMemory2KHR,
    .p_vkQueuePresentKHR = win32u_vkQueuePresentKHR,
    .p_vkReleaseFullScreenExclusiveModeEXT = win32u_vkReleaseFullScreenExclusiveModeEXT,
    .p_vkReleaseSwapchainImagesEXT = win32u_vkReleaseSwapchainImagesEXT,
    .p_vkReleaseSwapchainImagesKHR = win32u_vkReleaseSwapchainImagesKHR,
    .p_vkSetHdrMetadataEXT = win32u_vkSetHdrMetadataEXT,
    .p_vkSetLatencyMarkerNV = win32u_vkSetLatencyMarkerNV,
    .p_vkSetLatencySleepModeNV = win32u_vkSetLatencySleepModeNV,
    .p_vkSetSwapchainPresentTimingQueueSizeEXT = win32u_vkSetSwapchainPresentTimingQueueSizeEXT,
    .p_vkQueueSubmit = win32u_vkQueueSubmit,
    .p_vkQueueSubmit2 = win32u_vkQueueSubmit2,
    .p_vkQueueSubmit2KHR = win32u_vkQueueSubmit2KHR,
    .p_vkUnmapMemory = win32u_vkUnmapMemory,
    .p_vkUnmapMemory2KHR = win32u_vkUnmapMemory2KHR,
    .p_vkWaitForPresentKHR = win32u_vkWaitForPresentKHR,
    .p_vkWaitForPresent2KHR = win32u_vkWaitForPresent2KHR,
};

static VkResult nulldrv_vulkan_surface_create( HWND hwnd, BOOL raw, const struct vulkan_instance *instance,
                                               VkSurfaceKHR *surface, struct client_surface **client )
{
    VkHeadlessSurfaceCreateInfoEXT create_info = {.sType = VK_STRUCTURE_TYPE_HEADLESS_SURFACE_CREATE_INFO_EXT};
    VkResult res;

    if (!(*client = nulldrv_client_surface_create( hwnd ))) return VK_ERROR_OUT_OF_HOST_MEMORY;
    if ((res = instance->p_vkCreateHeadlessSurfaceEXT( instance->host.instance, &create_info, NULL, surface )))
    {
        client_surface_release(*client);
        *client = NULL;
    }

    return res;
}

static VkResult nulldrv_vulkan_surface_update( HWND hwnd, const struct vulkan_instance *instance,
                                               struct client_surface *client, VkSurfaceKHR old_surface,
                                               VkSurfaceKHR *host_surface, BOOL *updated )
{
    *updated = FALSE;
    return VK_SUCCESS;
}

static void nulldrv_vulkan_surface_release( struct client_surface *client, VkSurfaceKHR host_surface )
{
}

static VkColorSpaceKHR nulldrv_vulkan_map_colorspace( VkColorSpaceKHR colorspace, struct client_surface *client )
{
    return colorspace;
}

static void nulldrv_vulkan_surface_set_color_description(
        VkColorSpaceKHR colorspace, BOOL use_image_description,
        struct client_surface *client )
{
}

static void nulldrv_vulkan_surface_set_alpha( VkCompositeAlphaFlagBitsKHR alpha_bits,
                                              struct client_surface *client )
{
}

static VkBool32 nulldrv_get_physical_device_presentation_support( struct vulkan_physical_device *physical_device, uint32_t queue )
{
    return VK_TRUE;
}

static UINT nulldrv_vulkan_get_hwnd_dmabuf_caps( HWND hwnd, void *caps, void *format_modifiers,
                                                 UINT max_format_modifiers, UINT *format_modifier_count )
{
    if (format_modifier_count) *format_modifier_count = 0;
    return HWND_DMABUF_NOT_FOUND;
}

static void nulldrv_map_instance_extensions( struct vulkan_instance_extensions *extensions )
{
    if (extensions->has_VK_KHR_win32_surface) extensions->has_VK_EXT_headless_surface = 1;
    if (extensions->has_VK_EXT_headless_surface) extensions->has_VK_KHR_win32_surface = 1;
}

static void nulldrv_map_device_extensions( struct vulkan_device_extensions *extensions )
{
    if (extensions->has_VK_KHR_external_memory_win32) extensions->has_VK_KHR_external_memory_fd = 1;
    if (extensions->has_VK_KHR_external_memory_fd) extensions->has_VK_KHR_external_memory_win32 = 1;
    if (extensions->has_VK_KHR_external_semaphore_win32) extensions->has_VK_KHR_external_semaphore_fd = 1;
    if (extensions->has_VK_KHR_external_semaphore_fd) extensions->has_VK_KHR_external_semaphore_win32 = 1;
    if (extensions->has_VK_KHR_external_fence_win32) extensions->has_VK_KHR_external_fence_fd = 1;
    if (extensions->has_VK_KHR_external_fence_fd) extensions->has_VK_KHR_external_fence_win32 = 1;
    extensions->has_VK_WINE_openvr_device_extensions = 1;
    extensions->has_VK_WINE_openxr_device_extensions = 1;
}

static const struct vulkan_driver_funcs nulldrv_funcs =
{
    .p_vulkan_surface_create = nulldrv_vulkan_surface_create,
    .p_vulkan_surface_update = nulldrv_vulkan_surface_update,
    .p_vulkan_surface_release = nulldrv_vulkan_surface_release,
    .p_vulkan_map_colorspace = nulldrv_vulkan_map_colorspace,
    .p_vulkan_surface_set_color_description =
        nulldrv_vulkan_surface_set_color_description,
    .p_vulkan_surface_set_alpha = nulldrv_vulkan_surface_set_alpha,
    .p_get_physical_device_presentation_support = nulldrv_get_physical_device_presentation_support,
    .p_vulkan_get_hwnd_dmabuf_caps = nulldrv_vulkan_get_hwnd_dmabuf_caps,
    .p_map_instance_extensions = nulldrv_map_instance_extensions,
    .p_map_device_extensions = nulldrv_map_device_extensions,
};

static void vulkan_driver_init(void)
{
    UINT status;

    if ((status = user_driver->pVulkanInit( WINE_VULKAN_DRIVER_VERSION, vulkan_handle, &driver_funcs )) &&
        status != STATUS_NOT_IMPLEMENTED)
    {
        ERR( "Failed to initialize the driver vulkan functions, status %#x\n", status );
        return;
    }

    if (status == STATUS_NOT_IMPLEMENTED) driver_funcs = &nulldrv_funcs;
}

static void vulkan_driver_load(void)
{
    static pthread_once_t init_once = PTHREAD_ONCE_INIT;
    pthread_once( &init_once, vulkan_driver_init );
}

static VkResult lazydrv_vulkan_surface_create( HWND hwnd, BOOL raw, const struct vulkan_instance *instance,
                                               VkSurfaceKHR *surface, struct client_surface **client )
{
    vulkan_driver_load();
    return driver_funcs->p_vulkan_surface_create( hwnd, raw, instance, surface, client );
}

static VkResult lazydrv_vulkan_surface_update( HWND hwnd, const struct vulkan_instance *instance,
                                               struct client_surface *client, VkSurfaceKHR old_surface,
                                               VkSurfaceKHR *host_surface, BOOL *updated )
{
    vulkan_driver_load();
    return driver_funcs->p_vulkan_surface_update( hwnd, instance, client, old_surface, host_surface, updated );
}

static void lazydrv_vulkan_surface_release( struct client_surface *client, VkSurfaceKHR host_surface )
{
    vulkan_driver_load();
    driver_funcs->p_vulkan_surface_release( client, host_surface );
}

static VkColorSpaceKHR lazydrv_vulkan_map_colorspace( VkColorSpaceKHR colorspace, struct client_surface *client )
{
    vulkan_driver_load();
    return driver_funcs->p_vulkan_map_colorspace( colorspace, client );
}

static void lazydrv_vulkan_surface_set_color_description(
        VkColorSpaceKHR colorspace, BOOL use_image_description,
        struct client_surface *client )
{
    vulkan_driver_load();
    driver_funcs->p_vulkan_surface_set_color_description(
            colorspace, use_image_description, client );
}

static void lazydrv_vulkan_surface_set_alpha( VkCompositeAlphaFlagBitsKHR alpha_bits,
                                              struct client_surface *client )
{
    vulkan_driver_load();
    driver_funcs->p_vulkan_surface_set_alpha( alpha_bits, client );
}

static VkBool32 lazydrv_get_physical_device_presentation_support( struct vulkan_physical_device *physical_device, uint32_t queue )
{
    vulkan_driver_load();
    return driver_funcs->p_get_physical_device_presentation_support( physical_device, queue );
}

static UINT lazydrv_vulkan_get_hwnd_dmabuf_caps( HWND hwnd, void *caps, void *format_modifiers,
                                                 UINT max_format_modifiers, UINT *format_modifier_count )
{
    vulkan_driver_load();
    if (!driver_funcs->p_vulkan_get_hwnd_dmabuf_caps)
    {
        if (format_modifier_count) *format_modifier_count = 0;
        return HWND_DMABUF_NOT_FOUND;
    }
    return driver_funcs->p_vulkan_get_hwnd_dmabuf_caps( hwnd, caps, format_modifiers,
                                                        max_format_modifiers, format_modifier_count );
}

static void lazydrv_map_instance_extensions( struct vulkan_instance_extensions *extensions )
{
    vulkan_driver_load();
    return driver_funcs->p_map_instance_extensions( extensions );
}

static void lazydrv_map_device_extensions( struct vulkan_device_extensions *extensions )
{
    vulkan_driver_load();
    return driver_funcs->p_map_device_extensions( extensions );
}

static const struct vulkan_driver_funcs lazydrv_funcs =
{
    .p_vulkan_surface_create = lazydrv_vulkan_surface_create,
    .p_vulkan_surface_update = lazydrv_vulkan_surface_update,
    .p_vulkan_surface_release = lazydrv_vulkan_surface_release,
    .p_vulkan_map_colorspace = lazydrv_vulkan_map_colorspace,
    .p_vulkan_surface_set_color_description =
        lazydrv_vulkan_surface_set_color_description,
    .p_vulkan_surface_set_alpha = lazydrv_vulkan_surface_set_alpha,
    .p_get_physical_device_presentation_support = lazydrv_get_physical_device_presentation_support,
    .p_vulkan_get_hwnd_dmabuf_caps = lazydrv_vulkan_get_hwnd_dmabuf_caps,
    .p_map_instance_extensions = lazydrv_map_instance_extensions,
    .p_map_device_extensions = lazydrv_map_device_extensions,
};

static void vulkan_init_once(void)
{
    struct vulkan_instance_extensions extensions = {0};
    VkExtensionProperties *properties = NULL;
    uint32_t count = 0;
    VkResult res;

    const char *env = getenv( "WINE_DISABLE_FULLSCREEN_HACK" );
    fshack_enabled = !env || !atoi( env );

#ifdef SONAME_LIBVULKAN
    vulkan_handle = dlopen( SONAME_LIBVULKAN, RTLD_NOW );
    if (!vulkan_handle) ERR( "Failed to load %s\n", SONAME_LIBVULKAN );
#else
    ERR( "Wine was built without Vulkan support.\n" );
#endif
    if (!vulkan_handle) return;

#define LOAD_FUNCPTR( f )                                                                          \
    if (!(p_##f = dlsym( vulkan_handle, #f )))                                                     \
    {                                                                                              \
        ERR( "Failed to find " #f "\n" );                                                          \
        dlclose( vulkan_handle );                                                                  \
        vulkan_handle = NULL;                                                                      \
        return;                                                                                    \
    }

    LOAD_FUNCPTR( vkGetDeviceProcAddr );
    LOAD_FUNCPTR( vkGetInstanceProcAddr );
#undef LOAD_FUNCPTR

    driver_funcs = &lazydrv_funcs;
    vulkan_funcs.p_vkGetInstanceProcAddr = p_vkGetInstanceProcAddr;
    vulkan_funcs.p_vkGetDeviceProcAddr = p_vkGetDeviceProcAddr;

#define LOAD_FUNCPTR( f ) p_##f = (PFN_##f)p_vkGetInstanceProcAddr( NULL, #f );
    LOAD_FUNCPTR( vkCreateInstance );
    LOAD_FUNCPTR( vkEnumerateInstanceExtensionProperties );
#undef LOAD_FUNCPTR

    do
    {
        free( properties );
        properties = NULL;
        if ((res = p_vkEnumerateInstanceExtensionProperties( NULL, &count, NULL ))) goto failed;
        if (!count || !(properties = malloc( count * sizeof(*properties) ))) goto failed;
    } while ((res = p_vkEnumerateInstanceExtensionProperties( NULL, &count, properties ) == VK_INCOMPLETE));
    if (res) goto failed;

    TRACE( "Host instance extensions:\n" );
    for (uint32_t i = 0; i < count; i++)
    {
        const char *extension = properties[i].extensionName;
#define USE_VK_EXT(x)                           \
        if (!strcmp( extension, #x ))           \
        {                                       \
            extensions.has_ ## x = 1;           \
            TRACE( "  - %s\n", extension );     \
        } else
        ALL_VK_INSTANCE_EXTS
#undef USE_VK_EXT
        WARN( "Extension %s is not supported.\n", debugstr_a(extension) );
    }
    vulkan_funcs.host_extensions = extensions;

    /* map host instance extensions for VK_KHR_win32_surface */
    driver_funcs->p_map_instance_extensions( &extensions );

    /* filter out unsupported client instance extensions */
#define USE_VK_EXT(x) vulkan_funcs.client_extensions.has_ ## x = extensions.has_ ## x;
    ALL_VK_CLIENT_INSTANCE_EXTS
#undef USE_VK_EXT

failed:
    if (res) ERR( "Failed to initialize instance extensions, res %d\n", res );
    free( properties );
}

/***********************************************************************
 *      __wine_get_vulkan_driver  (win32u.so)
 */
const struct vulkan_funcs *__wine_get_vulkan_driver( UINT version )
{
    static pthread_once_t init_once = PTHREAD_ONCE_INIT;

    if (version != WINE_VULKAN_DRIVER_VERSION)
    {
        ERR( "version mismatch, vulkan wants %u but win32u has %u\n", version, WINE_VULKAN_DRIVER_VERSION );
        return NULL;
    }

    pthread_once( &init_once, vulkan_init_once );
    if (!vulkan_handle) return NULL;
    return &vulkan_funcs;
}

/* unix side client-like instance wrapper to fit with the vulkan wrapping infrastructure */
struct instance_wrapper
{
    struct VkInstance_T client;
};

struct vulkan_instance *vulkan_instance_create( const struct vulkan_instance_extensions *extensions )
{
    const struct vulkan_funcs *funcs = __wine_get_vulkan_driver( WINE_VULKAN_DRIVER_VERSION );
    VkInstanceCreateInfo create_info = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    const char *extension_names[sizeof(*extensions) * 8];
    struct instance_wrapper *wrapper;
    UINT device_count = 8;
    VkResult res;

    if (!funcs) return NULL;

    create_info.ppEnabledExtensionNames = extension_names;
#define USE_VK_EXT(x) if (extensions->has_ ## x) extension_names[create_info.enabledExtensionCount++] = #x;
    ALL_VK_INSTANCE_EXTS
#undef USE_VK_EXT

    for (;;)
    {
        VkInstance instance;

        if (!(wrapper = calloc( 1, offsetof(struct instance_wrapper, client.physical_device[device_count]) ))) return NULL;
        wrapper->client.physical_device_count = device_count;
        wrapper->client.extensions = *extensions;
        instance = &wrapper->client;

        if ((res = funcs->p_vkCreateInstance( &create_info, NULL, &instance ))) break;
        if ((wrapper->client.physical_device_count <= device_count)) break;
        device_count = wrapper->client.physical_device_count;
        free( wrapper );
    }

    if (!res) return vulkan_instance_from_handle( &wrapper->client );
    WARN( "Failed to create instance, res %d\n", res );
    free( wrapper );
    return NULL;
}
