/*
 * Wayland OpenGL functions
 *
 * Copyright 2020 Alexandros Frantzis for Collabora Ltd.
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
#include <dlfcn.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "ntstatus.h"
#define WIN32_NO_STATUS
#include "waylanddrv.h"
#include "wine/debug.h"

#ifdef HAVE_LIBWAYLAND_EGL

WINE_DEFAULT_DEBUG_CHANNEL(waylanddrv);

#include <wayland-egl.h>

#include "wine/opengl_driver.h"

static const struct egl_platform *egl;
static const struct opengl_funcs *funcs;
static const struct opengl_drawable_funcs wayland_drawable_funcs;

/* Original (win32u) driver proc-address resolver, chained from
 * wayland_get_proc_address so that names other than the WINE dmabuf-export
 * extension keep their previous behavior. */
static void *(*prev_get_proc_address)(const char *);

/* dmabuf export leaf (GL texture/renderbuffer -> Linux dmabuf via
 * EGL_MESA_image_dma_buf_export). Resolved lazily and gated behind the
 * runtime extension check below. */
static PFN_eglCreateImageKHR pfn_eglCreateImageKHR;
static PFN_eglDestroyImageKHR pfn_eglDestroyImageKHR;
static PFN_eglExportDMABUFImageMESA pfn_eglExportDMABUFImageMESA;
static PFN_eglExportDMABUFImageQueryMESA pfn_eglExportDMABUFImageQueryMESA;

static pthread_once_t dmabuf_export_init_once = PTHREAD_ONCE_INIT;
static BOOL dmabuf_export_supported;

static BOOL dmabuf_export_type_supported(GLenum type)
{
    if (!egl) return FALSE;
    switch (type)
    {
    case GL_TEXTURE_2D:
    case GL_RENDERBUFFER:
        return TRUE;
    default:
        return FALSE;
    }
}

static void dmabuf_export_init_support(void)
{
    const char *extensions;

    if (!egl || !funcs || !funcs->p_eglGetProcAddress || !funcs->p_eglQueryString ||
        !funcs->p_eglGetCurrentContext)
        return;

    if (!(extensions = funcs->p_eglQueryString(egl->display, EGL_EXTENSIONS))) return;
    if (!strstr(extensions, "EGL_KHR_image_base") && !strstr(extensions, "EGL_KHR_image"))
        return;
    if (!strstr(extensions, "EGL_MESA_image_dma_buf_export"))
        return;
    if (!dmabuf_export_type_supported(GL_TEXTURE_2D))
        return;

    pfn_eglCreateImageKHR = (void *)funcs->p_eglGetProcAddress("eglCreateImageKHR");
    pfn_eglDestroyImageKHR = (void *)funcs->p_eglGetProcAddress("eglDestroyImageKHR");
    pfn_eglExportDMABUFImageMESA = (void *)funcs->p_eglGetProcAddress("eglExportDMABUFImageMESA");
    pfn_eglExportDMABUFImageQueryMESA = (void *)funcs->p_eglGetProcAddress("eglExportDMABUFImageQueryMESA");

    if (!pfn_eglCreateImageKHR || !pfn_eglDestroyImageKHR || !pfn_eglExportDMABUFImageMESA ||
        !pfn_eglExportDMABUFImageQueryMESA)
        return;

    dmabuf_export_supported = TRUE;
}

static BOOL dmabuf_export_bridge_supported(void)
{
    pthread_once(&dmabuf_export_init_once, dmabuf_export_init_support);
    return dmabuf_export_supported;
}

struct wayland_gl_drawable
{
    struct opengl_drawable base;
    struct wl_egl_window *wl_egl_window;
};

static struct wayland_gl_drawable *impl_from_opengl_drawable(struct opengl_drawable *base)
{
    return CONTAINING_RECORD(base, struct wayland_gl_drawable, base);
}

static void wayland_drawable_destroy(struct opengl_drawable *base)
{
    struct wayland_gl_drawable *gl = impl_from_opengl_drawable(base);
    if (gl->wl_egl_window) wl_egl_window_destroy(gl->wl_egl_window);
}

static EGLConfig egl_config_for_format(int format)
{
    return egl->configs[(format - 1) % egl->config_count];
}

static void wayland_gl_drawable_sync_size(struct wayland_gl_drawable *gl)
{
    int client_width, client_height;
    RECT client_rect = {0};

    NtUserGetClientRect(gl->base.client->hwnd, &client_rect, NtUserGetDpiForWindow(gl->base.client->hwnd));
    client_width = client_rect.right - client_rect.left;
    client_height = client_rect.bottom - client_rect.top;
    if (client_width == 0 || client_height == 0) client_width = client_height = 1;

    wl_egl_window_resize(gl->wl_egl_window, client_width, client_height, 0, 0);
}

static BOOL wayland_opengl_surface_create(HWND hwnd, BOOL raw, int format, struct opengl_drawable **drawable)
{
    EGLConfig config = egl_config_for_format(format);
    struct wayland_client_surface *client;
    EGLint attribs[4], *attrib = attribs;
    struct opengl_drawable *previous;
    struct wayland_gl_drawable *gl;
    RECT rect;

    TRACE("hwnd=%p format=%d\n", hwnd, format);

    if ((previous = *drawable) && previous->format == format) return TRUE;

    NtUserGetClientRect(hwnd, &rect, NtUserGetDpiForWindow(hwnd));
    if (rect.right == rect.left) rect.right = rect.left + 1;
    if (rect.bottom == rect.top) rect.bottom = rect.top + 1;

    if (!egl->has_EGL_EXT_present_opaque)
        WARN("Missing EGL_EXT_present_opaque extension\n");
    else
    {
        *attrib++ = EGL_PRESENT_OPAQUE_EXT;
        *attrib++ = EGL_TRUE;
    }
    *attrib++ = EGL_NONE;

    if (!(client = wayland_client_surface_create(hwnd))) return FALSE;
    gl = opengl_drawable_create(sizeof(*gl), &wayland_drawable_funcs, format, &client->client);
    client_surface_release(&client->client);
    if (!gl) return FALSE;
    gl->base.buffer_map[0] = GL_BACK_LEFT;
    gl->base.buffer_map[1] = GL_BACK_RIGHT;
    gl->base.buffer_map[GL_FRONT - GL_FRONT_LEFT] = GL_BACK;
    gl->base.buffer_map[GL_FRONT_AND_BACK - GL_FRONT_LEFT] = GL_BACK;

    if (!(gl->wl_egl_window = wl_egl_window_create(client->wl_surface, rect.right, rect.bottom))) goto err;
    if (!(gl->base.surface = funcs->p_eglCreateWindowSurface(egl->display, config, gl->wl_egl_window, attribs))) goto err;
    set_client_surface(hwnd, client);

    TRACE("Created drawable %s with egl_surface %p\n", debugstr_opengl_drawable(&gl->base), gl->base.surface);

    if (previous) opengl_drawable_release( previous );
    *drawable = &gl->base;
    return TRUE;

err:
    opengl_drawable_release(&gl->base);
    return FALSE;
}

static void wayland_init_egl_platform(struct egl_platform *platform)
{
    platform->type = EGL_PLATFORM_WAYLAND_KHR;
    platform->native_display = process_wayland.wl_display;
    platform->force_pbuffer_formats = TRUE;
    egl = platform;
}

static int wayland_override_interval(int interval)
{
    const char *env = getenv("WAYLANDDRV_EGL_SWAP_INTERVAL");
    int override = env ? atoi(env) : interval;
    if (override < 0) override = 0;
    if (env) FIXME("HACK: swap interval override %u\n", override);
    return override;
}

static void wayland_drawable_flush(struct opengl_drawable *base, UINT flags)
{
    int interval;
    struct wayland_gl_drawable *gl = impl_from_opengl_drawable(base);

    TRACE("drawable %s, flags %#x\n", debugstr_opengl_drawable(base), flags);

    if (flags & GL_FLUSH_INTERVAL)
    {
        interval = wayland_override_interval(abs(base->interval));
        funcs->p_eglSwapInterval(egl->display, interval);
    }

    /* Since context_flush is called from operations that may latch the native size,
     * perform any pending resizes before calling them. */
    if (flags & GL_FLUSH_UPDATED) wayland_gl_drawable_sync_size(gl);
}

static BOOL wayland_drawable_swap(struct opengl_drawable *base)
{
    struct wayland_gl_drawable *gl = impl_from_opengl_drawable(base);

    /* cannot swap when the window is not visible */
    if (base->interval && !NtUserIsWindowVisible(base->client->hwnd)) return TRUE;

    client_surface_present(base->client);
    funcs->p_eglSwapBuffers(egl->display, gl->base.surface);

    return TRUE;
}

struct wayland_pbuffer
{
    struct opengl_drawable base;
    struct wl_surface *surface;
    struct wl_egl_window *window;
};

static struct wayland_pbuffer *pbuffer_from_opengl_drawable(struct opengl_drawable *base)
{
    return CONTAINING_RECORD(base, struct wayland_pbuffer, base);
}

static void wayland_pbuffer_destroy(struct opengl_drawable *base)
{
    struct wayland_pbuffer *gl = pbuffer_from_opengl_drawable(base);

    TRACE("%s\n", debugstr_opengl_drawable(base));

    if (gl->window)
        wl_egl_window_destroy(gl->window);
    if (gl->surface)
        wl_surface_destroy(gl->surface);
}

static const struct opengl_drawable_funcs wayland_pbuffer_funcs =
{
    .destroy = wayland_pbuffer_destroy,
};

static BOOL wayland_pbuffer_create(HDC hdc, int format, BOOL largest, GLenum texture_format, GLenum texture_target,
                                   GLint max_level, GLsizei *width, GLsizei *height, struct opengl_drawable **surface)
{
    EGLConfig config = egl_config_for_format(format);
    struct wayland_pbuffer *gl;

    TRACE("hdc %p, format %d, largest %u, texture_format %#x, texture_target %#x, max_level %#x, width %d, height %d, private %p\n",
          hdc, format, largest, texture_format, texture_target, max_level, *width, *height, surface);

    if (!(gl = opengl_drawable_create(sizeof(*gl), &wayland_pbuffer_funcs, format, NULL))) return FALSE;
    /* Wayland EGL doesn't support pixmap or pbuffer, create a dummy window surface to act as the target render surface. */
    if (!(gl->surface = wl_compositor_create_surface(process_wayland.wl_compositor))) goto err;
    if (!(gl->window = wl_egl_window_create(gl->surface, *width, *height))) goto err;
    if (!(gl->base.surface = funcs->p_eglCreateWindowSurface(egl->display, config, gl->window, NULL))) goto err;

    TRACE("Created pbuffer %s with egl_surface %p\n", debugstr_opengl_drawable(&gl->base), gl->base.surface);
    *surface = &gl->base;
    return TRUE;

err:
    opengl_drawable_release(&gl->base);
    return FALSE;
}

static BOOL wayland_pbuffer_updated(HDC hdc, struct opengl_drawable *base, GLenum cube_face, GLint mipmap_level)
{
    return GL_TRUE;
}

static UINT wayland_pbuffer_bind(HDC hdc, struct opengl_drawable *base, GLenum buffer)
{
    return -1; /* use default implementation */
}

static BOOL GLAPIENTRY wayland_wglWineExportDmaBufWINE(GLuint texture, GLenum target,
        struct wgl_dmabuf_desc *desc, int *fd)
{
    const EGLint image_attribs[] = {EGL_NONE};
    EGLuint64KHR modifiers[4] = {0};
    EGLint offsets[4] = {0}, strides[4] = {0};
    EGLImageKHR image = EGL_NO_IMAGE_KHR;
    EGLClientBuffer client;
    int export_fd = -1;
    int fourcc = 0, planes = 0;
    EGLenum egl_target;

    TRACE("texture %u, target %#x, desc %p, fd %p.\n", texture, target, desc, fd);

    if (!desc || !fd) return FALSE;
    memset(desc, 0, sizeof(*desc));
    *fd = -1;

    if (!texture || !dmabuf_export_bridge_supported() || !dmabuf_export_type_supported(target))
        return FALSE;

    switch (target)
    {
    case GL_TEXTURE_2D:
        egl_target = EGL_GL_TEXTURE_2D;
        break;
    case GL_RENDERBUFFER:
        egl_target = EGL_GL_RENDERBUFFER;
        break;
    default:
        return FALSE;
    }

    client = (EGLClientBuffer)(uintptr_t)texture;
    image = pfn_eglCreateImageKHR(egl->display, funcs->p_eglGetCurrentContext(),
            egl_target, client, image_attribs);
    if (image == EGL_NO_IMAGE_KHR)
        return FALSE;

    /* The export path currently supports only single-plane RGB buffers. */
    if (!pfn_eglExportDMABUFImageQueryMESA(egl->display, image, &fourcc, &planes, modifiers)
            || planes != 1)
        goto done;

    if (!pfn_eglExportDMABUFImageMESA(egl->display, image, &export_fd, strides, offsets)
            || export_fd < 0)
        goto done;

    desc->fourcc = fourcc;
    desc->stride = strides[0];
    desc->offset = offsets[0];
    desc->modifier = modifiers[0];
    *fd = export_fd;
    export_fd = -1;

done:
    if (image != EGL_NO_IMAGE_KHR)
        pfn_eglDestroyImageKHR(egl->display, image);
    if (export_fd >= 0)
        close(export_fd);
    return *fd >= 0;
}

static BOOL GLAPIENTRY wayland_wglWineDmaBufExportSupportedWINE(void)
{
    return dmabuf_export_bridge_supported();
}

/**********************************************************************
 *          WGL_NV_DX_interop (in-process)
 *
 * The D3D object is an opaque token. We allocate GL storage for the
 * registered object, export it as a dmabuf when possible, and honour
 * the WGL lock/unlock synchronization contract in this process. The EGL
 * dmabuf-export machinery (pfn_egl*) is shared with the dmabuf export
 * leaf above and resolved once by dmabuf_export_bridge_supported(). */

#ifndef WGL_ACCESS_READ_ONLY_NV
#define WGL_ACCESS_READ_ONLY_NV             0x0000
#define WGL_ACCESS_READ_WRITE_NV            0x0001
#define WGL_ACCESS_WRITE_DISCARD_NV         0x0002
#endif

struct wgl_dx_object
{
    struct wgl_dx_object *next;
    HANDLE      handle;         /* opaque token returned to the app */
    void       *dx_object;      /* the D3D11 resource - opaque to us */
    GLuint      gl_name;        /* GL object name the app gave us */
    GLenum      gl_type;        /* GL_RENDERBUFFER or GL_TEXTURE_2D */
    GLenum      access;         /* WGL_ACCESS_READ_ONLY_NV / READ_WRITE_NV */
    BOOL        locked;
    int         width;
    int         height;
    EGLImageKHR egl_image;
    int         dmabuf_fd;      /* exported fd backing the GL storage */
    uint32_t    fourcc;
    int         num_planes;
    uint64_t    modifier;
    int         offset;
    int         stride;
};

struct wgl_dx_device
{
    struct wgl_dx_device *next;
    HANDLE                handle;
    void                 *dx_device;
    struct wgl_dx_object *objects;
};

static pthread_mutex_t wgl_dx_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_once_t wgl_dx_init_once = PTHREAD_ONCE_INIT;
static struct wgl_dx_device *wgl_dx_devices;
static UINT_PTR wgl_dx_next_handle = 1;
static BOOL wgl_dx_supported;

static HANDLE wgl_dx_alloc_handle(void)
{
    return (HANDLE)(UINT_PTR)(wgl_dx_next_handle++ | 0x80000000ull);
}

static struct wgl_dx_device *wgl_dx_device_find_locked(HANDLE handle)
{
    struct wgl_dx_device *dev;
    for (dev = wgl_dx_devices; dev; dev = dev->next)
        if (dev->handle == handle) return dev;
    return NULL;
}

static struct wgl_dx_object *wgl_dx_object_find_locked(struct wgl_dx_device *dev, HANDLE handle)
{
    struct wgl_dx_object *obj;
    for (obj = dev->objects; obj; obj = obj->next)
        if (obj->handle == handle) return obj;
    return NULL;
}

static struct wgl_dx_object *wgl_dx_object_find_any_locked(HANDLE handle)
{
    struct wgl_dx_device *dev;
    struct wgl_dx_object *obj;

    for (dev = wgl_dx_devices; dev; dev = dev->next)
        if ((obj = wgl_dx_object_find_locked(dev, handle))) return obj;
    return NULL;
}

static BOOL wgl_dx_valid_access(GLenum access)
{
    return access == WGL_ACCESS_READ_ONLY_NV || access == WGL_ACCESS_READ_WRITE_NV ||
           access == WGL_ACCESS_WRITE_DISCARD_NV;
}

static BOOL wgl_dx_valid_object_array(GLint count, HANDLE *objects)
{
    return count >= 0 && (!count || objects);
}

static BOOL wgl_dx_type_supported(GLenum type)
{
    if (!egl) return FALSE;
    switch (type)
    {
    case GL_RENDERBUFFER:
        return egl->has_EGL_KHR_gl_renderbuffer_image;
    case GL_TEXTURE_2D:
        return egl->has_EGL_KHR_gl_texture_2D_image;
    default:
        return FALSE;
    }
}

/* Resolve the complete EGL/GL export path before win32u advertises the extension.
 * pfn_egl* are resolved by dmabuf_export_bridge_supported() which we reuse. */
static void wgl_dx_init_support(void)
{
    if (!dmabuf_export_bridge_supported()) return;
    if (!egl || !funcs) return;

    if (!egl->has_EGL_KHR_image && !egl->has_EGL_KHR_image_base) return;
    if (!wgl_dx_type_supported(GL_RENDERBUFFER) || !wgl_dx_type_supported(GL_TEXTURE_2D)) return;

    if (!funcs->p_eglGetCurrentContext || !funcs->p_glBindRenderbuffer ||
        !funcs->p_glRenderbufferStorage || !funcs->p_glBindTexture ||
        !funcs->p_glTexImage2D || !funcs->p_glGetIntegerv || !funcs->p_glFlush)
        return;

    wgl_dx_supported = TRUE;
}

static BOOL wgl_dx_bridge_supported(void)
{
    pthread_once(&wgl_dx_init_once, wgl_dx_init_support);
    return wgl_dx_supported;
}

static void wgl_dx_object_release_dmabuf(struct wgl_dx_object *obj)
{
    if (obj->egl_image && pfn_eglDestroyImageKHR)
        pfn_eglDestroyImageKHR(egl->display, obj->egl_image);
    obj->egl_image = EGL_NO_IMAGE_KHR;
    if (obj->dmabuf_fd >= 0) close(obj->dmabuf_fd);
    obj->dmabuf_fd = -1;
}

static BOOL wgl_dx_object_allocate_gl_storage(struct wgl_dx_object *obj, int width, int height)
{
    GLint previous;

    switch (obj->gl_type)
    {
    case GL_RENDERBUFFER:
        funcs->p_glGetIntegerv(GL_RENDERBUFFER_BINDING, &previous);
        funcs->p_glBindRenderbuffer(GL_RENDERBUFFER, obj->gl_name);
        funcs->p_glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, width, height);
        funcs->p_glBindRenderbuffer(GL_RENDERBUFFER, previous);
        return TRUE;

    case GL_TEXTURE_2D:
        funcs->p_glGetIntegerv(GL_TEXTURE_BINDING_2D, &previous);
        funcs->p_glBindTexture(GL_TEXTURE_2D, obj->gl_name);
        funcs->p_glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
        funcs->p_glBindTexture(GL_TEXTURE_2D, previous);
        return TRUE;
    }

    return FALSE;
}

static EGLenum wgl_dx_object_egl_target(const struct wgl_dx_object *obj)
{
    switch (obj->gl_type)
    {
    case GL_RENDERBUFFER: return EGL_GL_RENDERBUFFER;
    case GL_TEXTURE_2D: return EGL_GL_TEXTURE_2D;
    default: return EGL_NONE;
    }
}

/* Bind the registered GL object, allocate storage of (w,h), wrap it in an
 * EGLImage, and export the dmabuf descriptor. */
static BOOL wgl_dx_object_allocate(struct wgl_dx_object *obj, int width, int height)
{
    EGLClientBuffer client = (EGLClientBuffer)(uintptr_t)obj->gl_name;
    const EGLint image_attribs[1] = { EGL_NONE };
    EGLenum egl_target;
    int fourcc = 0, num_planes = 0;
    EGLuint64KHR modifiers[4] = {0};
    EGLint offsets[4] = {0}, strides[4] = {0};

    if (!wgl_dx_bridge_supported() || !wgl_dx_type_supported(obj->gl_type)) return FALSE;
    if (width <= 0 || height <= 0) return FALSE;

    if (!wgl_dx_object_allocate_gl_storage(obj, width, height)) return FALSE;

    egl_target = wgl_dx_object_egl_target(obj);
    obj->egl_image = pfn_eglCreateImageKHR(egl->display, funcs->p_eglGetCurrentContext(),
                                           egl_target, client, image_attribs);
    if (obj->egl_image == EGL_NO_IMAGE_KHR)
    {
        ERR("eglCreateImageKHR(target %#x) failed for gl_name=%u\n", egl_target, obj->gl_name);
        return FALSE;
    }

    if (!pfn_eglExportDMABUFImageQueryMESA(egl->display, obj->egl_image,
                                            &fourcc, &num_planes, modifiers))
    {
        ERR("eglExportDMABUFImageQueryMESA failed\n");
        wgl_dx_object_release_dmabuf(obj);
        return FALSE;
    }

    if (num_planes != 1)
    {
        WARN("Unsupported multi-plane dmabuf (planes=%d) for GL object type %#x\n", num_planes, obj->gl_type);
        wgl_dx_object_release_dmabuf(obj);
        return FALSE;
    }

    if (!pfn_eglExportDMABUFImageMESA(egl->display, obj->egl_image,
                                        &obj->dmabuf_fd, strides, offsets))
    {
        ERR("eglExportDMABUFImageMESA failed\n");
        wgl_dx_object_release_dmabuf(obj);
        return FALSE;
    }

    obj->width      = width;
    obj->height     = height;
    obj->fourcc     = fourcc;
    obj->num_planes = num_planes;
    obj->modifier   = modifiers[0];
    obj->offset     = offsets[0];
    obj->stride     = strides[0];

    TRACE("allocated WGL DX storage obj=%p type=%#x gl_name=%u %dx%d fourcc=0x%08x modifier=0x%016llx fd=%d stride=%d\n",
            obj->handle, obj->gl_type, obj->gl_name, width, height, fourcc,
            (unsigned long long)obj->modifier, obj->dmabuf_fd, obj->stride);
    return TRUE;
}

static HANDLE wayland_wglDXOpenDeviceNV(void *dxDevice)
{
    struct wgl_dx_device *dev;

    if (!wgl_dx_bridge_supported() || !dxDevice) return NULL;
    if (!(dev = calloc(1, sizeof(*dev)))) return NULL;

    pthread_mutex_lock(&wgl_dx_mutex);
    dev->handle = wgl_dx_alloc_handle();
    dev->dx_device = dxDevice;
    dev->next = wgl_dx_devices;
    wgl_dx_devices = dev;
    pthread_mutex_unlock(&wgl_dx_mutex);

    TRACE("dxDevice=%p -> handle=%p\n", dxDevice, dev->handle);
    return dev->handle;
}

static BOOL wayland_wglDXCloseDeviceNV(HANDLE hDevice)
{
    struct wgl_dx_device *dev, **link;
    struct wgl_dx_object *obj, *next;

    TRACE("hDevice=%p\n", hDevice);

    pthread_mutex_lock(&wgl_dx_mutex);
    for (link = &wgl_dx_devices; (dev = *link); link = &(*link)->next)
        if (dev->handle == hDevice) { *link = dev->next; break; }
    pthread_mutex_unlock(&wgl_dx_mutex);

    if (!dev) return FALSE;

    for (obj = dev->objects; obj; obj = next)
    {
        next = obj->next;
        wgl_dx_object_release_dmabuf(obj);
        free(obj);
    }
    free(dev);
    return TRUE;
}

static HANDLE wayland_wglDXRegisterObjectNV(HANDLE hDevice, void *dxObject, GLuint name,
                                             GLenum type, GLenum access)
{
    struct wgl_dx_device *dev;
    struct wgl_dx_object *obj;
    HWND hwnd;
    RECT client = {0};

    TRACE("hDevice=%p dxObject=%p name=%u type=%#x access=%#x\n",
          hDevice, dxObject, name, type, access);

    if (!wgl_dx_bridge_supported() || !dxObject || !name || !wgl_dx_valid_access(access)) return NULL;
    if (!wgl_dx_type_supported(type))
    {
        WARN("unsupported GL type 0x%04x\n", type);
        return NULL;
    }

    pthread_mutex_lock(&wgl_dx_mutex);
    dev = wgl_dx_device_find_locked(hDevice);
    pthread_mutex_unlock(&wgl_dx_mutex);
    if (!dev) return NULL;

    if (!(obj = calloc(1, sizeof(*obj)))) return NULL;
    obj->dmabuf_fd = -1;
    obj->egl_image = EGL_NO_IMAGE_KHR;
    obj->gl_name = name;
    obj->gl_type = type;
    obj->access  = access;
    obj->dx_object = dxObject;

    /* WGL_NV_DX_interop requires a current GL context. The producing
     * HWND is the current draw drawable's client surface HWND - this
     * is the surface size the GL renderbuffer should match. */
    {
        struct wgl_context *ctx = NtCurrentTeb()->glContext;
        struct opengl_drawable *draw = ctx ? ctx->draw : NULL;
        hwnd = (draw && draw->client) ? draw->client->hwnd : NULL;
    }
    if (!hwnd)
    {
        WARN("no current GL context draw surface; wglDXRegisterObjectNV called without makecurrent?\n");
        free(obj);
        return NULL;
    }
    NtUserGetClientRect(hwnd, &client, NtUserGetDpiForWindow(hwnd));
    /* Hidden or new drawables can be 0x0; placeholder storage keeps Register alive. */
    if (client.right == client.left) client.right = client.left + 1;
    if (client.bottom == client.top) client.bottom = client.top + 1;

    if (!wgl_dx_object_allocate(obj, client.right - client.left, client.bottom - client.top))
    {
        free(obj);
        return NULL;
    }

    pthread_mutex_lock(&wgl_dx_mutex);
    if (!(dev = wgl_dx_device_find_locked(hDevice)))
    {
        pthread_mutex_unlock(&wgl_dx_mutex);
        wgl_dx_object_release_dmabuf(obj);
        free(obj);
        return NULL;
    }
    obj->handle = wgl_dx_alloc_handle();
    obj->next = dev->objects;
    dev->objects = obj;
    pthread_mutex_unlock(&wgl_dx_mutex);

    TRACE("  -> handle=%p hwnd=%p %dx%d fourcc=0x%08x\n",
          obj->handle, hwnd, obj->width, obj->height, obj->fourcc);
    return obj->handle;
}

static BOOL wayland_wglDXUnregisterObjectNV(HANDLE hDevice, HANDLE hObject)
{
    struct wgl_dx_device *dev;
    struct wgl_dx_object *obj, **link;

    TRACE("hDevice=%p hObject=%p\n", hDevice, hObject);

    pthread_mutex_lock(&wgl_dx_mutex);
    dev = wgl_dx_device_find_locked(hDevice);
    obj = NULL;
    if (dev)
    {
        for (link = &dev->objects; (obj = *link); link = &(*link)->next)
            if (obj->handle == hObject) { *link = obj->next; break; }
    }
    if (obj && obj->locked)
    {
        *link = obj;
        obj = NULL;
    }
    pthread_mutex_unlock(&wgl_dx_mutex);

    if (!obj) return FALSE;
    wgl_dx_object_release_dmabuf(obj);
    free(obj);
    return TRUE;
}

static BOOL wayland_wglDXObjectAccessNV(HANDLE hObject, GLenum access)
{
    struct wgl_dx_object *obj;

    TRACE("hObject=%p access=%#x\n", hObject, access);

    if (!wgl_dx_bridge_supported() || !wgl_dx_valid_access(access)) return FALSE;

    pthread_mutex_lock(&wgl_dx_mutex);
    obj = wgl_dx_object_find_any_locked(hObject);
    if (obj && !obj->locked) obj->access = access;
    else obj = NULL;
    pthread_mutex_unlock(&wgl_dx_mutex);

    return !!obj;
}

static BOOL wayland_wglDXSetResourceShareHandleNV(void *dxObject, HANDLE shareHandle)
{
    /* No Windows handle sharing is performed here; D3D shared NT handles stay
     * on the D3D path. Accept the association so callers using the normal NV
     * setup sequence can continue to RegisterObject. */
    TRACE("dxObject=%p shareHandle=%p (no-op)\n", dxObject, shareHandle);
    return wgl_dx_bridge_supported() && dxObject && shareHandle;
}

static BOOL wayland_wglDXLockObjectsNV(HANDLE hDevice, GLint count, HANDLE *hObjects)
{
    struct wgl_dx_device *dev;
    GLint i;

    TRACE("hDevice=%p count=%d hObjects=%p\n", hDevice, count, hObjects);

    if (!wgl_dx_bridge_supported() || !NtCurrentTeb()->glContext ||
        !wgl_dx_valid_object_array(count, hObjects))
        return FALSE;

    pthread_mutex_lock(&wgl_dx_mutex);
    if (!(dev = wgl_dx_device_find_locked(hDevice)))
    {
        pthread_mutex_unlock(&wgl_dx_mutex);
        return FALSE;
    }
    for (i = 0; i < count; i++)
    {
        struct wgl_dx_object *obj = wgl_dx_object_find_locked(dev, hObjects[i]);
        if (!obj || obj->locked)
        {
            pthread_mutex_unlock(&wgl_dx_mutex);
            return FALSE;
        }
    }
    for (i = 0; i < count; i++)
    {
        struct wgl_dx_object *obj = wgl_dx_object_find_locked(dev, hObjects[i]);
        obj->locked = TRUE;
    }
    pthread_mutex_unlock(&wgl_dx_mutex);
    return TRUE;
}

static BOOL wayland_wglDXUnlockObjectsNV(HANDLE hDevice, GLint count, HANDLE *hObjects)
{
    struct wgl_dx_device *dev;
    GLint i;

    TRACE("hDevice=%p count=%d hObjects=%p\n", hDevice, count, hObjects);

    if (!wgl_dx_bridge_supported() || !NtCurrentTeb()->glContext ||
        !wgl_dx_valid_object_array(count, hObjects))
        return FALSE;

    pthread_mutex_lock(&wgl_dx_mutex);
    dev = wgl_dx_device_find_locked(hDevice);
    if (!dev)
    {
        pthread_mutex_unlock(&wgl_dx_mutex);
        return FALSE;
    }
    for (i = 0; i < count; i++)
    {
        struct wgl_dx_object *obj = wgl_dx_object_find_locked(dev, hObjects[i]);
        if (!obj || !obj->locked)
        {
            pthread_mutex_unlock(&wgl_dx_mutex);
            return FALSE;
        }
    }
    /* Pure in-process WGL_NV_DX_interop: Unlock is a GPU pipeline
     * synchronisation barrier ("GL writes finished, hand control of
     * the D3D resource back to the app"). We flush the GL command
     * stream and clear the locked flag. The app is responsible for
     * presenting the underlying D3D texture via its own swapchain. */
    funcs->p_glFlush();

    for (i = 0; i < count; i++)
    {
        struct wgl_dx_object *obj = wgl_dx_object_find_locked(dev, hObjects[i]);
        obj->locked = FALSE;
    }
    pthread_mutex_unlock(&wgl_dx_mutex);

    return TRUE;
}

static void *wayland_get_proc_address(const char *name)
{
    if (!strcmp(name, "wglWineDmaBufExportSupportedWINE"))
        return wayland_wglWineDmaBufExportSupportedWINE;
    if (!strcmp(name, "wglWineExportDmaBufWINE"))
        return dmabuf_export_bridge_supported() ? (void *)wayland_wglWineExportDmaBufWINE : NULL;

    if (!strncmp(name, "wglDX", 5) && !wgl_dx_bridge_supported()) return NULL;
    if (!strcmp(name, "wglDXOpenDeviceNV"))             return wayland_wglDXOpenDeviceNV;
    if (!strcmp(name, "wglDXCloseDeviceNV"))            return wayland_wglDXCloseDeviceNV;
    if (!strcmp(name, "wglDXRegisterObjectNV"))         return wayland_wglDXRegisterObjectNV;
    if (!strcmp(name, "wglDXUnregisterObjectNV"))       return wayland_wglDXUnregisterObjectNV;
    if (!strcmp(name, "wglDXObjectAccessNV"))           return wayland_wglDXObjectAccessNV;
    if (!strcmp(name, "wglDXSetResourceShareHandleNV")) return wayland_wglDXSetResourceShareHandleNV;
    if (!strcmp(name, "wglDXLockObjectsNV"))            return wayland_wglDXLockObjectsNV;
    if (!strcmp(name, "wglDXUnlockObjectsNV"))          return wayland_wglDXUnlockObjectsNV;

    return prev_get_proc_address ? prev_get_proc_address(name) : NULL;
}

static struct opengl_driver_funcs wayland_driver_funcs =
{
    .p_init_egl_platform = wayland_init_egl_platform,
    .p_surface_create = wayland_opengl_surface_create,
    .p_pbuffer_create = wayland_pbuffer_create,
    .p_pbuffer_updated = wayland_pbuffer_updated,
    .p_pbuffer_bind = wayland_pbuffer_bind,
};

static const struct opengl_drawable_funcs wayland_drawable_funcs =
{
    .destroy = wayland_drawable_destroy,
    .flush = wayland_drawable_flush,
    .swap = wayland_drawable_swap,
};

/**********************************************************************
 *           WAYLAND_OpenGLInit
 */
UINT WAYLAND_OpenGLInit(UINT version, const struct opengl_funcs *opengl_funcs, const struct opengl_driver_funcs **driver_funcs)
{
    if (version != WINE_OPENGL_DRIVER_VERSION)
    {
        ERR("Version mismatch, opengl32 wants %u but driver has %u\n",
            version, WINE_OPENGL_DRIVER_VERSION);
        return STATUS_INVALID_PARAMETER;
    }

    if (!opengl_funcs->egl_handle) return STATUS_NOT_SUPPORTED;
    funcs = opengl_funcs;

    prev_get_proc_address = (*driver_funcs)->p_get_proc_address;
    wayland_driver_funcs.p_get_proc_address = wayland_get_proc_address;
    wayland_driver_funcs.p_init_pixel_formats = (*driver_funcs)->p_init_pixel_formats;
    wayland_driver_funcs.p_describe_pixel_format = (*driver_funcs)->p_describe_pixel_format;
    wayland_driver_funcs.p_init_wgl_extensions = (*driver_funcs)->p_init_wgl_extensions;
    wayland_driver_funcs.p_context_create = (*driver_funcs)->p_context_create;
    wayland_driver_funcs.p_context_destroy = (*driver_funcs)->p_context_destroy;
    wayland_driver_funcs.p_make_current = (*driver_funcs)->p_make_current;

    *driver_funcs = &wayland_driver_funcs;
    return STATUS_SUCCESS;
}

#else /* No GL */

UINT WAYLAND_OpenGLInit(UINT version, const struct opengl_funcs *opengl_funcs, const struct opengl_driver_funcs **driver_funcs)
{
    return STATUS_NOT_IMPLEMENTED;
}

#endif
