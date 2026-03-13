/*
 * Copyright 2020 Nikolay Sivov for CodeWeavers
 * Copyright 2023 Zhiyi Zhang for CodeWeavers
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
#include <stdarg.h>
#include <stdlib.h>

#include "ntstatus.h"
#define WIN32_NO_STATUS
#define COBJMACROS
#include "windef.h"
#include "winbase.h"
#include "wingdi.h"
#include "winternl.h"
#include "initguid.h"
#include "objidl.h"
#include "dcomp_private.h"
#include "dxgi.h"
#include "dwmapi.h"
#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(dcomp);

static HRESULT STDMETHODCALLTYPE device_QueryInterface(IDCompositionDevice *iface,
        REFIID iid, void **out)
{
    struct composition_device *device = impl_from_IDCompositionDevice(iface);

    TRACE("iface %p, iid %s, out %p\n", iface, debugstr_guid(iid), out);

    if (IsEqualGUID(iid, &IID_IUnknown)
            || IsEqualGUID(iid, &IID_IDCompositionDevice))
    {
        IUnknown_AddRef(&device->IDCompositionDevice_iface);
        *out = &device->IDCompositionDevice_iface;
        return S_OK;
    }
    else if (device->version >= 2
            && (IsEqualGUID(iid, &IID_IDCompositionDevice2)
            || IsEqualGUID(iid, &IID_IDCompositionDesktopDevice)))
    {
        IUnknown_AddRef(&device->IDCompositionDesktopDevice_iface);
        *out = &device->IDCompositionDesktopDevice_iface;
        return S_OK;
    }

    FIXME("%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid(iid));
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG STDMETHODCALLTYPE device_AddRef(IDCompositionDevice *iface)
{
    struct composition_device *device = impl_from_IDCompositionDevice(iface);
    ULONG ref = InterlockedIncrement(&device->ref);

    TRACE("iface %p, ref %lu.\n", iface, ref);
    return ref;
}

static ULONG STDMETHODCALLTYPE device_Release(IDCompositionDevice *iface)
{
    struct composition_device *device = impl_from_IDCompositionDevice(iface);
    ULONG ref = InterlockedDecrement(&device->ref);

    TRACE("iface %p, ref %lu.\n", iface, ref);

    if (!ref)
    {
        if (device->thread)
        {
            WaitForSingleObject(device->thread, INFINITE);
            CloseHandle(device->thread);
        }
        DeleteCriticalSection(&device->cs);
        free(device);
    }

    return ref;
}

static void do_composite(const struct composition_target *target, IDXGISwapChain1 *swapchain)
{
    D2D1_BITMAP_PROPERTIES1 bitmap_desc;
    DXGI_SWAP_CHAIN_DESC swapchain_desc;
    ID2D1Bitmap1 *target_bitmap = NULL;
    struct composition_visual *visual;
    ID2D1Bitmap *src_bitmap = NULL;
    IDXGISurface1 *surface = NULL;
    ID2D1Image *old_target = NULL;
    HDC dst_dc, src_dc;
    D2D1_SIZE_U size;
    DWORD style;
    HRESULT hr;

    static const BLENDFUNCTION blend_func =
    {
      AC_SRC_OVER, /* BlendOp */
      0,           /* BlendFlag */
      255,         /* SourceConstantAlpha */
      AC_SRC_ALPHA /* AlphaFormat */
    };

    /* The front buffer is the last back buffer because the composition swapchain must be created
     * with DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL */
    if (FAILED(hr = IDXGISwapChain1_GetDesc(swapchain, &swapchain_desc)))
    {
        ERR("Failed to get the swapchain description, hr %#lx.\n", hr);
        goto done;
    }

    size.width = swapchain_desc.BufferDesc.Width;
    size.height = swapchain_desc.BufferDesc.Height;
    bitmap_desc.pixelFormat.format = DXGI_FORMAT_B8G8R8A8_UNORM;
    bitmap_desc.pixelFormat.alphaMode = D2D1_ALPHA_MODE_PREMULTIPLIED;
    bitmap_desc.dpiX = 0;
    bitmap_desc.dpiY = 0;
    bitmap_desc.bitmapOptions = D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_GDI_COMPATIBLE;
    bitmap_desc.colorContext = NULL;
    visual = impl_from_IDCompositionVisual(target->root);
    if (FAILED(hr = ID2D1DeviceContext_CreateBitmap(visual->device_context, size, NULL, 0,
            &bitmap_desc, &target_bitmap)))
    {
        ERR("Failed to create a bitmap, hr %#lx.\n", hr);
        goto done;
    }

    if (FAILED(hr = IDXGISwapChain1_GetBuffer(swapchain, swapchain_desc.BufferCount - 1,
            &IID_IDXGISurface1, (void **)&surface)))
    {
        ERR("Failed to get the swapchain front buffer, hr %#lx.\n", hr);
        goto done;
    }

    if (FAILED(hr = ID2D1DeviceContext_CreateSharedBitmap(visual->device_context, &IID_IDXGISurface,
            surface, NULL, &src_bitmap)))
    {
        ERR("Failed to create a bitmap from dxgi surface, hr %#lx.\n", hr);
        goto done;
    }

    if (FAILED(hr = ID2D1Bitmap1_CopyFromBitmap(target_bitmap, NULL, src_bitmap, NULL)))
    {
        ERR("Failed to copy bitmap, hr %#lx.\n", hr);
        goto done;
    }

    ID2D1DeviceContext_GetTarget(visual->device_context, &old_target);
    ID2D1DeviceContext_SetTarget(visual->device_context, (ID2D1Image *)target_bitmap);
    if (FAILED(hr = ID2D1GdiInteropRenderTarget_GetDC(visual->interop, D2D1_DC_INITIALIZE_MODE_COPY,
            &src_dc)))
    {
        ERR("GetDC failed, hr %#lx.\n", hr);
        ID2D1DeviceContext_SetTarget(visual->device_context, old_target);
        if (old_target)
            ID2D1Image_Release(old_target);
        goto done;
    }

    style = DCX_USESTYLE | DCX_CACHE;
    if (!target->topmost)
        style |= DCX_CLIPCHILDREN;
    dst_dc = GetDCEx(target->hwnd, 0, style);
    GdiAlphaBlend(dst_dc, 0, 0, size.width, size.height, src_dc, 0, 0, size.width, size.height, blend_func);
    ReleaseDC(target->hwnd, dst_dc);

    ID2D1GdiInteropRenderTarget_ReleaseDC(visual->interop, NULL);
    ID2D1DeviceContext_SetTarget(visual->device_context, old_target);
    if (old_target)
        ID2D1Image_Release(old_target);

done:
    if (src_bitmap)
        ID2D1Bitmap_Release(src_bitmap);
    if (surface)
        IDXGISurface1_Release(surface);
    if (target_bitmap)
        ID2D1Bitmap1_Release(target_bitmap);
}

static DWORD WINAPI composite_thread_proc(void *iface)
{
    struct composition_device *device = impl_from_IDCompositionDevice(iface);
    unsigned int count, frequency, refresh_period;
    IDXGISwapChain1 *swapchain = NULL;
    struct composition_target *target;
    struct composition_visual *visual;
    HDC hdc;

    /* TODO: Implement and use D3DKMTWaitForVerticalBlankEvent() */
    hdc = GetDC(0);
    frequency = GetDeviceCaps(hdc, VREFRESH);
    refresh_period = 1000 / frequency;
    ReleaseDC(0, hdc);

    while (TRUE)
    {
        EnterCriticalSection(&device->cs);

        count = 0;
        LIST_FOR_EACH_ENTRY(target, &device->targets, struct composition_target, entry)
        {
            if (!target->root)
                continue;

            visual = impl_from_IDCompositionVisual(target->root);
            if (!visual->content)
                continue;

            if (FAILED(IUnknown_QueryInterface(visual->content, &IID_IDXGISwapChain1, (void **)&swapchain)))
            {
                FIXME("Only IDXGISwapChain1 is currently supported.\n");
                continue;
            }

            do_composite(target, swapchain);
            count++;
            IDXGISwapChain1_Release(swapchain);
        }

        if (!count)
        {
            device->thread_exited = TRUE;
            LeaveCriticalSection(&device->cs);
            break;
        }

        LeaveCriticalSection(&device->cs);
        Sleep(refresh_period);
    }

    return 0;
}

static HRESULT STDMETHODCALLTYPE device_Commit(IDCompositionDevice *iface)
{
    struct composition_device *device = impl_from_IDCompositionDevice(iface);
    HRESULT hr = S_OK;

    FIXME("iface %p semi-stub!\n", iface);

    EnterCriticalSection(&device->cs);

    if (!device->thread || device->thread_exited)
    {
        if (device->thread)
        {
            WaitForSingleObject(device->thread, INFINITE);
            CloseHandle(device->thread);
        }
        device->thread_exited = FALSE;
        device->thread = CreateThread(NULL, 0, composite_thread_proc, iface, 0, NULL);
    }

    LeaveCriticalSection(&device->cs);

    return hr;
}

static HRESULT STDMETHODCALLTYPE device_WaitForCommitCompletion(IDCompositionDevice *iface)
{
    FIXME("iface %p stub!\n", iface);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE device_GetFrameStatistics(IDCompositionDevice *iface,
        DCOMPOSITION_FRAME_STATISTICS *statistics)
{
    FIXME("iface %p, statistics %p stub!\n", iface, statistics);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE device_CreateTargetForHwnd(IDCompositionDevice *iface,
        HWND hwnd, BOOL topmost, IDCompositionTarget **target)
{
    struct composition_device *device = impl_from_IDCompositionDevice(iface);

    TRACE("iface %p, hwnd %p, topmost %d, target %p\n", iface, hwnd, topmost, target);

    return create_target(device, hwnd, topmost, target);
}

static HRESULT STDMETHODCALLTYPE device_CreateVisual(IDCompositionDevice *iface,
        IDCompositionVisual **visual)
{
    TRACE("iface %p, visual %p\n", iface, visual);

    return create_visual(1, &IID_IDCompositionVisual, (void **)visual);
}

static HRESULT STDMETHODCALLTYPE device_CreateSurface(IDCompositionDevice *iface,
        UINT width, UINT height, DXGI_FORMAT pixel_format, DXGI_ALPHA_MODE alpha_mode,
        IDCompositionSurface **surface)
{
    FIXME("iface %p, width %u, height %u, format %#x, alpha_mode %#x, surface %p stub!\n", iface,
            width, height, pixel_format, alpha_mode, surface);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE device_CreateVirtualSurface(IDCompositionDevice *iface,
        UINT width, UINT height, DXGI_FORMAT pixel_format, DXGI_ALPHA_MODE alpha_mode,
        IDCompositionVirtualSurface **surface)
{
    FIXME("iface %p, width %u, height %u, format %#x, alpha_mode %#x, surface %p stub!\n", iface,
            width, height, pixel_format, alpha_mode, surface);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE device_CreateSurfaceFromHandle(IDCompositionDevice *iface,
        HANDLE handle, IUnknown **surface)
{
    FIXME("iface %p, handle %p, surface %p stub!\n", iface, handle, surface);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE device_CreateSurfaceFromHwnd(IDCompositionDevice *iface,
        HWND hwnd, IUnknown **surface)
{
    FIXME("iface %p, hwnd %p, surface %p stub!\n", iface, hwnd, surface);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE device_CreateTranslateTransform(IDCompositionDevice *iface,
        IDCompositionTranslateTransform **transform)
{
    FIXME("iface %p, transform %p stub!\n", iface, transform);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE device_CreateScaleTransform(IDCompositionDevice *iface,
        IDCompositionScaleTransform **transform)
{
    FIXME("iface %p, transform %p stub!\n", iface, transform);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE device_CreateRotateTransform(IDCompositionDevice *iface,
        IDCompositionRotateTransform **transform)
{
    FIXME("iface %p, transform %p stub!\n", iface, transform);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE device_CreateSkewTransform(IDCompositionDevice *iface,
        IDCompositionSkewTransform **transform)
{
    FIXME("iface %p, transform %p stub!\n", iface, transform);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE device_CreateMatrixTransform(IDCompositionDevice *iface,
        IDCompositionMatrixTransform **transform)
{
    FIXME("iface %p, transform %p stub!\n", iface, transform);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE device_CreateTransformGroup(IDCompositionDevice *iface,
        IDCompositionTransform **transforms, UINT elements,
        IDCompositionTransform **transform_group)
{
    FIXME("iface %p, transforms %p, elements %u, transform_group %p stub!\n", iface, transforms,
            elements, transform_group);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE device_CreateTranslateTransform3D(IDCompositionDevice *iface,
        IDCompositionTranslateTransform3D **transform_3d)
{
    FIXME("iface %p, translate_transform_3d %p stub!\n", iface, transform_3d);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE device_CreateScaleTransform3D(IDCompositionDevice *iface,
        IDCompositionScaleTransform3D **transform_3d)
{
    FIXME("iface %p, transform_3d %p stub!\n", iface, transform_3d);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE device_CreateRotateTransform3D(IDCompositionDevice *iface,
        IDCompositionRotateTransform3D **transform_3d)
{
    FIXME("iface %p, transform_3d %p stub!\n", iface, transform_3d);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE device_CreateMatrixTransform3D(IDCompositionDevice *iface,
        IDCompositionMatrixTransform3D **transform_3d)
{
    FIXME("iface %p, transform_3d %p stub!\n", iface, transform_3d);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE device_CreateTransform3DGroup(IDCompositionDevice *iface,
        IDCompositionTransform3D **transforms_3d, UINT elements,
        IDCompositionTransform3D **transform_3d_group)
{
    FIXME("iface %p, transforms_3d %p, elements %u, transform_3d_group %p stub!\n", iface,
            transforms_3d, elements, transform_3d_group);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE device_CreateEffectGroup(IDCompositionDevice *iface,
        IDCompositionEffectGroup **effect_group)
{
    FIXME("iface %p, effect_group %p stub!\n", iface, effect_group);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE device_CreateRectangleClip(IDCompositionDevice *iface,
        IDCompositionRectangleClip **clip)
{
    FIXME("iface %p, clip %p stub!\n", iface, clip);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE device_CreateAnimation(IDCompositionDevice *iface,
        IDCompositionAnimation **animation)
{
    FIXME("iface %p, animation %p stub!\n", iface, animation);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE device_CheckDeviceState(IDCompositionDevice *iface,
        BOOL *valid)
{
    FIXME("iface %p, valid %p stub!\n", iface, valid);
    return E_NOTIMPL;
}

static const struct IDCompositionDeviceVtbl device_vtbl =
{
    /* IUnknown methods */
    device_QueryInterface,
    device_AddRef,
    device_Release,
    /* IDCompositionDevice methods */
    device_Commit,
    device_WaitForCommitCompletion,
    device_GetFrameStatistics,
    device_CreateTargetForHwnd,
    device_CreateVisual,
    device_CreateSurface,
    device_CreateVirtualSurface,
    device_CreateSurfaceFromHandle,
    device_CreateSurfaceFromHwnd,
    device_CreateTranslateTransform,
    device_CreateScaleTransform,
    device_CreateRotateTransform,
    device_CreateSkewTransform,
    device_CreateMatrixTransform,
    device_CreateTransformGroup,
    device_CreateTranslateTransform3D,
    device_CreateScaleTransform3D,
    device_CreateRotateTransform3D,
    device_CreateMatrixTransform3D,
    device_CreateTransform3DGroup,
    device_CreateEffectGroup,
    device_CreateRectangleClip,
    device_CreateAnimation,
    device_CheckDeviceState,
};

static HRESULT STDMETHODCALLTYPE desktop_device_QueryInterface(IDCompositionDesktopDevice *iface,
        REFIID iid, void **out)
{
    struct composition_device *device = impl_from_IDCompositionDesktopDevice(iface);

    TRACE("iface %p.\n", iface);

    return IDCompositionDevice_QueryInterface(&device->IDCompositionDevice_iface, iid, out);
}

static ULONG STDMETHODCALLTYPE desktop_device_AddRef(IDCompositionDesktopDevice *iface)
{
    struct composition_device *device = impl_from_IDCompositionDesktopDevice(iface);

    TRACE("iface %p.\n", iface);

    return IDCompositionDevice_AddRef(&device->IDCompositionDevice_iface);
}

static ULONG STDMETHODCALLTYPE desktop_device_Release(IDCompositionDesktopDevice *iface)
{
    struct composition_device *device = impl_from_IDCompositionDesktopDevice(iface);

    TRACE("iface %p.\n", iface);

    return IDCompositionDevice_Release(&device->IDCompositionDevice_iface);
}

static HRESULT STDMETHODCALLTYPE desktop_device_Commit(IDCompositionDesktopDevice *iface)
{
    struct composition_device *device = impl_from_IDCompositionDesktopDevice(iface);

    TRACE("iface %p.\n", iface);

    return IDCompositionDevice_Commit(&device->IDCompositionDevice_iface);
}

static HRESULT STDMETHODCALLTYPE desktop_device_WaitForCommitCompletion(IDCompositionDesktopDevice *iface)
{
    FIXME("iface %p stub!\n", iface);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE desktop_device_GetFrameStatistics(IDCompositionDesktopDevice *iface,
        DCOMPOSITION_FRAME_STATISTICS *statistics)
{
    FIXME("iface %p, statistics %p stub!\n", iface, statistics);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE desktop_device_CreateVisual(IDCompositionDesktopDevice *iface,
        IDCompositionVisual2 **visual)
{
    TRACE("iface %p, visual %p\n", iface, visual);

    return create_visual(2, &IID_IDCompositionVisual2, (void **)visual);
}

static HRESULT STDMETHODCALLTYPE desktop_device_CreateSurfaceFactory(IDCompositionDesktopDevice *iface,
        IUnknown *rendering_device, IDCompositionSurfaceFactory **surface_factory)
{
    FIXME("iface %p, rendering_device %p, surface_factory %p stub!\n", iface, rendering_device,
            surface_factory);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE desktop_device_CreateSurface(IDCompositionDesktopDevice *iface,
        UINT width, UINT height, DXGI_FORMAT pixel_format, DXGI_ALPHA_MODE alpha_mode,
        IDCompositionSurface **surface)
{
    FIXME("iface %p, width %u, height %u, format %#x, alpha_mode %#x, surface %p stub!\n", iface,
            width, height, pixel_format, alpha_mode, surface);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE desktop_device_CreateVirtualSurface(IDCompositionDesktopDevice *iface,
        UINT width, UINT height, DXGI_FORMAT pixel_format, DXGI_ALPHA_MODE alpha_mode,
        IDCompositionVirtualSurface **surface)
{
    FIXME("iface %p, width %u, height %u, format %#x, alpha_mode %#x, surface %p stub!\n", iface,
            width, height, pixel_format, alpha_mode, surface);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE desktop_device_CreateTranslateTransform(IDCompositionDesktopDevice *iface,
        IDCompositionTranslateTransform **transform)
{
    FIXME("iface %p, transform %p stub!\n", iface, transform);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE desktop_device_CreateScaleTransform(IDCompositionDesktopDevice *iface,
        IDCompositionScaleTransform **transform)
{
    FIXME("iface %p, transform %p stub!\n", iface, transform);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE desktop_device_CreateRotateTransform(IDCompositionDesktopDevice *iface,
        IDCompositionRotateTransform **transform)
{
    FIXME("iface %p, transform %p stub!\n", iface, transform);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE desktop_device_CreateSkewTransform(IDCompositionDesktopDevice *iface,
        IDCompositionSkewTransform **transform)
{
    FIXME("iface %p, transform %p stub!\n", iface, transform);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE desktop_device_CreateMatrixTransform(IDCompositionDesktopDevice *iface,
        IDCompositionMatrixTransform **transform)
{
    FIXME("iface %p, transform %p stub!\n", iface, transform);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE desktop_device_CreateTransformGroup(IDCompositionDesktopDevice *iface,
        IDCompositionTransform **transforms, UINT elements, IDCompositionTransform **transform_group)
{
    FIXME("iface %p, transforms %p, elements %u, transform_group %p stub!\n", iface, transforms,
            elements, transform_group);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE desktop_device_CreateTranslateTransform3D(IDCompositionDesktopDevice *iface,
        IDCompositionTranslateTransform3D **transform_3d)
{
    FIXME("iface %p, translate_transform_3d %p stub!\n", iface, transform_3d);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE desktop_device_CreateScaleTransform3D(IDCompositionDesktopDevice *iface,
        IDCompositionScaleTransform3D **transform_3d)
{
    FIXME("iface %p, transform_3d %p stub!\n", iface, transform_3d);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE desktop_device_CreateRotateTransform3D(IDCompositionDesktopDevice *iface,
        IDCompositionRotateTransform3D **transform_3d)
{
    FIXME("iface %p, transform_3d %p stub!\n", iface, transform_3d);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE desktop_device_CreateMatrixTransform3D(IDCompositionDesktopDevice *iface,
        IDCompositionMatrixTransform3D **transform_3d)
{
    FIXME("iface %p, transform_3d %p stub!\n", iface, transform_3d);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE desktop_device_CreateTransform3DGroup(IDCompositionDesktopDevice *iface,
        IDCompositionTransform3D **transforms_3d, UINT elements,
        IDCompositionTransform3D **transform_3d_group)
{
    FIXME("iface %p, transforms_3d %p, elements %u, transform_3d_group %p stub!\n", iface,
            transforms_3d, elements, transform_3d_group);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE desktop_device_CreateEffectGroup(IDCompositionDesktopDevice *iface,
        IDCompositionEffectGroup **effect_group)
{
    FIXME("iface %p, effect_group %p stub!\n", iface, effect_group);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE desktop_device_CreateRectangleClip(IDCompositionDesktopDevice *iface,
        IDCompositionRectangleClip **clip)
{
    FIXME("iface %p, clip %p stub!\n", iface, clip);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE desktop_device_CreateAnimation(IDCompositionDesktopDevice *iface,
        IDCompositionAnimation **animation)
{
    FIXME("iface %p, animation %p stub!\n", iface, animation);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE desktop_device_CreateTargetForHwnd(IDCompositionDesktopDevice *iface,
        HWND hwnd, BOOL topmost, IDCompositionTarget **target)
{
    struct composition_device *device = impl_from_IDCompositionDesktopDevice(iface);

    TRACE("iface %p, hwnd %p, topmost %d, target %p\n", iface, hwnd, topmost, target);

    return IDCompositionDevice_CreateTargetForHwnd(&device->IDCompositionDevice_iface, hwnd,
            topmost, target);
}

static HRESULT STDMETHODCALLTYPE desktop_device_CreateSurfaceFromHandle(IDCompositionDesktopDevice *iface,
        HANDLE handle, IUnknown **surface)
{
    FIXME("iface %p, handle %p, surface %p stub!\n", iface, handle, surface);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE desktop_device_CreateSurfaceFromHwnd(IDCompositionDesktopDevice *iface,
        HWND hwnd, IUnknown **surface)
{
    FIXME("iface %p, hwnd %p, surface %p stub!\n", iface, hwnd, surface);
    return E_NOTIMPL;
}

static const struct IDCompositionDesktopDeviceVtbl desktop_device_vtbl =
{
    /* IUnknown methods */
    desktop_device_QueryInterface,
    desktop_device_AddRef,
    desktop_device_Release,
    /* IDCompositionDevice2 methods */
    desktop_device_Commit,
    desktop_device_WaitForCommitCompletion,
    desktop_device_GetFrameStatistics,
    desktop_device_CreateVisual,
    desktop_device_CreateSurfaceFactory,
    desktop_device_CreateSurface,
    desktop_device_CreateVirtualSurface,
    desktop_device_CreateTranslateTransform,
    desktop_device_CreateScaleTransform,
    desktop_device_CreateRotateTransform,
    desktop_device_CreateSkewTransform,
    desktop_device_CreateMatrixTransform,
    desktop_device_CreateTransformGroup,
    desktop_device_CreateTranslateTransform3D,
    desktop_device_CreateScaleTransform3D,
    desktop_device_CreateRotateTransform3D,
    desktop_device_CreateMatrixTransform3D,
    desktop_device_CreateTransform3DGroup,
    desktop_device_CreateEffectGroup,
    desktop_device_CreateRectangleClip,
    desktop_device_CreateAnimation,
    /* IDCompositionDesktopDevice methods */
    desktop_device_CreateTargetForHwnd,
    desktop_device_CreateSurfaceFromHandle,
    desktop_device_CreateSurfaceFromHwnd,
};

static HRESULT create_device(int version, REFIID iid, void **out)
{
    struct composition_device *device;
    HRESULT hr;

    if (!out)
        return E_INVALIDARG;

    device = calloc(1, sizeof(*device));
    if (!device)
        return E_OUTOFMEMORY;

    InitializeCriticalSection(&device->cs);
    list_init(&device->targets);
    device->IDCompositionDevice_iface.lpVtbl = &device_vtbl;
    device->IDCompositionDesktopDevice_iface.lpVtbl = &desktop_device_vtbl;
    device->version = version;
    device->ref = 1;
    hr = IDCompositionDevice_QueryInterface(&device->IDCompositionDevice_iface, iid, out);
    IDCompositionDevice_Release(&device->IDCompositionDevice_iface);
    return hr;
}

HRESULT WINAPI DCompositionCreateDevice(IDXGIDevice *dxgi_device, REFIID iid, void **device)
{
    TRACE("%p, %s, %p\n", dxgi_device, debugstr_guid(iid), device);

    if (!IsEqualIID(iid, &IID_IDCompositionDevice))
        return E_NOINTERFACE;

    return create_device(1, iid, device);
}

HRESULT WINAPI DCompositionCreateDevice2(IUnknown *rendering_device, REFIID iid, void **device)
{
    TRACE("%p, %s, %p\n", rendering_device, debugstr_guid(iid), device);

    if (!IsEqualIID(iid, &IID_IDCompositionDevice)
            && !IsEqualIID(iid, &IID_IDCompositionDesktopDevice))
        return E_NOINTERFACE;

    return create_device(2, iid, device);
}

HRESULT WINAPI DCompositionCreateDevice3(IUnknown *rendering_device, REFIID iid, void **device)
{
    FIXME("%p, %s, %p.\n", rendering_device, debugstr_guid(iid), device);

    return E_NOTIMPL;
}

HRESULT WINAPI DCompositionCreateSharedVisualHandle(void **handle)
{
    FIXME("handle %p stub!\n", handle);

    if (!handle)
        return STATUS_INVALID_PARAMETER;

    *handle = (void*)0xdeadbee1;
    FIXME("--- returning a fake handle %p.\n", *handle);
    return S_OK;
}
