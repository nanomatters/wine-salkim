/*
 * Copyright 2025 Zhiyi Zhang for CodeWeavers
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
#include <assert.h>

#define COBJMACROS
#include "windef.h"
#include "winbase.h"
#include "dcomp_private.h"
#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(dcomp);

static HRESULT STDMETHODCALLTYPE surface_QueryInterface(IDCompositionSurface *iface, REFIID iid, void **out)
{
    TRACE("iface %p, iid %s, out %p!\n", iface, debugstr_guid(iid), out);

    if (IsEqualGUID(iid, &IID_IUnknown)
            || IsEqualGUID(iid, &IID_IDCompositionSurface))
    {
        IUnknown_AddRef(iface);
        *out = iface;
        return S_OK;
    }

    FIXME("%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid(iid));
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG STDMETHODCALLTYPE surface_AddRef(IDCompositionSurface *iface)
{
    struct composition_surface *surface = impl_from_IDCompositionSurface(iface);
    ULONG ref = InterlockedIncrement(&surface->ref);

    TRACE("iface %p, ref %lu.\n", iface, ref);
    return ref;
}

static ULONG STDMETHODCALLTYPE surface_Release(IDCompositionSurface *iface)
{
    struct composition_surface *surface = impl_from_IDCompositionSurface(iface);
    ULONG ref = InterlockedDecrement(&surface->ref);

    TRACE("iface %p, ref %lu.\n", iface, ref);

    if (!ref)
    {
        IUnknown_Release(surface->physical_surface);
        IDCompositionSurfaceFactory_Release(surface->factory);
        if (surface->draw_surface)
            ID3D11Texture2D_Release(surface->draw_surface);
        free(surface);
    }

    return ref;
}

static HRESULT STDMETHODCALLTYPE surface_BeginDraw(IDCompositionSurface *iface,
        const RECT *rect, REFIID iid, void **object, POINT *offset)
{
    struct composition_surface *surface = impl_from_IDCompositionSurface(iface);
    struct composition_surface_factory *factory = impl_from_IDCompositionSurfaceFactory(surface->factory);
    struct composition_device *device = impl_from_IDCompositionDevice(factory->device);
    D3D11_TEXTURE2D_DESC texture_desc;
    ID3D11Texture2D *draw_surface;
    ID3D11Device *d3d11_device;
    RECT whole_rect;
    HRESULT hr;

    TRACE("iface %p, rect %s, iid %s, object %p, offset %p.\n", iface, wine_dbgstr_rect(rect),
            debugstr_guid(iid), object, offset);

    if (IsEqualGUID(iid, &IID_ID2D1DeviceContext))
    {
        FIXME("ID2D1DeviceContext draw surface is currently unsupported.\n");
        return E_NOTIMPL;
    }

    if (!rect)
    {
        SetRect(&whole_rect, 0, 0, surface->width, surface->height);
        rect = &whole_rect;
    }

    /* TODO: Check if IDCompositionSurface is virtual when virtual IDCompositionSurface is implemented */

    /* The first BeginDraw must use the whole surface for non-virtual IDCompositionSurface */
    if (!surface->draw_surface && !(rect->left == 0 && rect->top == 0
            && rect->right == surface->width && rect->bottom == surface->height))
        return E_INVALIDARG;

    if (rect->left < 0 || rect->top < 0 || rect->right > surface->width
            || rect->bottom > surface->height)
        return E_INVALIDARG;

    if (!object || !offset)
        return E_INVALIDARG;

    if (!surface->draw_surface)
    {
        if (!IsEqualGUID(&factory->rendering_device_iid, &IID_IDXGIDevice))
        {
            ERR("Only IDCompositionSurfaceFactory created with an IDXGIDevice rendering device is "
                    "currently supported, rendering device guid %s.\n",
                    wine_dbgstr_guid(&factory->rendering_device_iid));
            return E_NOTIMPL;
        }

        hr = IUnknown_QueryInterface(factory->rendering_device, &IID_ID3D11Device, (void **)&d3d11_device);
        if (FAILED(hr))
        {
            FIXME("Failed to get a d3d11 device, should a new one be created?\n");
            return hr;
        }

        texture_desc.Width = surface->width;
        texture_desc.Height = surface->height;
        texture_desc.MipLevels = 1;
        texture_desc.ArraySize = 1;
        texture_desc.Format = surface->pixel_format;
        texture_desc.SampleDesc.Count = 1;
        texture_desc.SampleDesc.Quality = 0;
        texture_desc.Usage = D3D11_USAGE_DEFAULT;
        texture_desc.BindFlags = D3D11_BIND_RENDER_TARGET;
        texture_desc.CPUAccessFlags = 0;
        texture_desc.MiscFlags = D3D11_RESOURCE_MISC_GUARDED;
        if (surface->pixel_format == DXGI_FORMAT_B8G8R8A8_UNORM)
            texture_desc.MiscFlags |= D3D11_RESOURCE_MISC_GDI_COMPATIBLE;
        hr = ID3D11Device_CreateTexture2D(d3d11_device, &texture_desc, NULL, &draw_surface);
        ID3D11Device_Release(d3d11_device);
        if (FAILED(hr))
        {
            ERR("Failed to create a draw surface.\n");
            return hr;
        }

        if (InterlockedCompareExchangePointer((void **)&surface->draw_surface, draw_surface, 0))
            ID3D11Texture2D_Release(draw_surface);
    }

    dcomp_lock();
    if (device->drawing_surface && device->drawing_surface != &surface->IDCompositionSurface_iface)
    {
        dcomp_unlock();
        return DCOMPOSITION_ERROR_SURFACE_BEING_RENDERED;
    }
    device->drawing_surface = &surface->IDCompositionSurface_iface;
    dcomp_unlock();

    hr = IUnknown_QueryInterface(surface->draw_surface, iid, object);
    if (FAILED(hr))
        return hr;

    offset->x = rect->left;
    offset->y = rect->top;
    surface->draw_rect = *rect;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE surface_EndDraw(IDCompositionSurface *iface)
{
    struct composition_surface *surface = impl_from_IDCompositionSurface(iface);
    struct composition_surface_factory *factory = impl_from_IDCompositionSurfaceFactory(surface->factory);
    struct composition_device *device = impl_from_IDCompositionDevice(factory->device);
    ID3D11Resource *dst_resource, *src_resource;
    ID3D11DeviceContext *d3d11_device_context;
    ID3D11Device *d3d11_device;
    D3D11_BOX box;
    HRESULT hr;

    TRACE("iface %p.\n", iface);

    dcomp_lock();
    if (!(device->drawing_surface && device->drawing_surface == &surface->IDCompositionSurface_iface))
    {
        dcomp_unlock();
        return DCOMPOSITION_ERROR_SURFACE_NOT_BEING_RENDERED;
    }
    device->drawing_surface = NULL;
    dcomp_unlock();

    /* TODO: Copy data after Commit() is called instead of doing it immediately here */

    if (!IsEqualGUID(&factory->rendering_device_iid, &IID_IDXGIDevice))
    {
        ERR("Only IDCompositionSurfaceFactory created with an IDXGIDevice rendering device is "
                "currently supported, rendering device guid %s.\n",
                wine_dbgstr_guid(&factory->rendering_device_iid));
        return E_NOTIMPL;
    }

    hr = IUnknown_QueryInterface(factory->rendering_device, &IID_ID3D11Device, (void **)&d3d11_device);
    if (FAILED(hr))
    {
        FIXME("Failed to get a d3d11 device, should a new one be created?\n");
        return hr;
    }

    hr = ID3D11Texture2D_QueryInterface(surface->draw_surface, &IID_ID3D11Resource, (void **)&src_resource);
    if (FAILED(hr))
    {
        FIXME("Failed to get the source ID3D11Resource.\n");
        ID3D11Device_Release(d3d11_device);
        return hr;
    }

    hr = IUnknown_QueryInterface(surface->physical_surface, &IID_ID3D11Resource, (void **)&dst_resource);
    if (FAILED(hr))
    {
        FIXME("Failed to get the destination ID3D11Resource.\n");
        ID3D11Resource_Release(src_resource);
        ID3D11Device_Release(d3d11_device);
        return hr;
    }

    box.left = surface->draw_rect.left;
    box.top = surface->draw_rect.top;
    box.front = 0;
    box.right = surface->draw_rect.right;
    box.bottom = surface->draw_rect.bottom;
    box.back = 1;

    ID3D11Device_GetImmediateContext(d3d11_device, &d3d11_device_context);
    ID3D11DeviceContext_CopySubresourceRegion(d3d11_device_context, dst_resource, 0,
            surface->draw_rect.left, surface->draw_rect.top, 0, src_resource, 0, &box);
    ID3D11DeviceContext_Release(d3d11_device_context);
    ID3D11Resource_Release(dst_resource);
    ID3D11Resource_Release(src_resource);
    ID3D11Device_Release(d3d11_device);

    SetRectEmpty(&surface->draw_rect);
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE surface_SuspendDraw(IDCompositionSurface *iface)
{
    FIXME("iface %p stub!\n", iface);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE surface_ResumeDraw(IDCompositionSurface *iface)
{
        FIXME("iface %p stub!\n", iface);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE surface_Scroll(IDCompositionSurface *iface, const RECT *scroll,
        const RECT *clip, int offset_x, int offset_y)
{
    FIXME("iface %p, scroll %s, clip %s, offset_x %d, offset_y %d stub!\n", iface,
            wine_dbgstr_rect(scroll), wine_dbgstr_rect(clip), offset_x, offset_y);
    return E_NOTIMPL;
}

static const struct IDCompositionSurfaceVtbl surface_vtbl =
{
    /* IUnknown methods */
    surface_QueryInterface,
    surface_AddRef,
    surface_Release,
    /* IDCompositionSurface methods */
    surface_BeginDraw,
    surface_EndDraw,
    surface_SuspendDraw,
    surface_ResumeDraw,
    surface_Scroll,
};

struct composition_surface *unsafe_impl_from_IDCompositionSurface(IDCompositionSurface *iface)
{
    if (!iface)
        return NULL;
    assert(iface->lpVtbl == &surface_vtbl);
    return CONTAINING_RECORD(iface, struct composition_surface, IDCompositionSurface_iface);
}

HRESULT create_surface(struct composition_surface_factory *factory, UINT width, UINT height,
        DXGI_FORMAT pixel_format, DXGI_ALPHA_MODE alpha_mode, IDCompositionSurface **dcomp_surface)
{
    struct composition_surface *surface;
    const GUID *physical_surface_iid;
    IUnknown *physical_surface;
    HRESULT hr;

    if (!width || !height)
        return E_INVALIDARG;

    if (pixel_format != DXGI_FORMAT_B8G8R8A8_UNORM && pixel_format != DXGI_FORMAT_R8G8B8A8_UNORM
            && pixel_format != DXGI_FORMAT_R16G16B16A16_FLOAT)
        return E_INVALIDARG;

    if (alpha_mode == DXGI_ALPHA_MODE_UNSPECIFIED)
        alpha_mode = DXGI_ALPHA_MODE_IGNORE;

    if (alpha_mode != DXGI_ALPHA_MODE_PREMULTIPLIED && alpha_mode != DXGI_ALPHA_MODE_IGNORE)
        return E_INVALIDARG;

    if (IsEqualGUID(&factory->rendering_device_iid, &IID_IDXGIDevice))
    {
        IDXGISurface *dxgi_surface;
        DXGI_SURFACE_DESC desc;

        desc.Width = width;
        desc.Height = height;
        desc.Format = pixel_format;
        desc.SampleDesc.Count = 1;
        desc.SampleDesc.Quality = 0;
        /* TODO: What about alpha_mode ? */

        hr = IDXGIDevice_CreateSurface((IDXGIDevice *)factory->rendering_device, &desc, 1,
                DXGI_USAGE_BACK_BUFFER | DXGI_USAGE_SHADER_INPUT, NULL, &dxgi_surface);
        if (FAILED(hr))
        {
            ERR("Failed to create a IDXGISurface.\n");
            return hr;
        }

        physical_surface = (IUnknown *)dxgi_surface;
        physical_surface_iid = &IID_IDXGISurface;
    }
    else
    {
        FIXME("Only IDCompositionSurfaceFactory created with an IDXGIDevice rendering device is "
              "currently supported, rendering device guid %s.\n",
              wine_dbgstr_guid(&factory->rendering_device_iid));
        return E_NOTIMPL;
    }

    surface = calloc(1, sizeof(*surface));
    if (!surface)
        return E_OUTOFMEMORY;

    surface->IDCompositionSurface_iface.lpVtbl = &surface_vtbl;
    surface->factory = &factory->IDCompositionSurfaceFactory_iface;
    IDCompositionSurfaceFactory_AddRef(surface->factory);
    surface->physical_surface = physical_surface;
    surface->physical_surface_iid = *physical_surface_iid;
    surface->width = width;
    surface->height = height;
    surface->pixel_format = pixel_format;
    surface->alpha_mode = alpha_mode;
    surface->ref = 1;

    *dcomp_surface = &surface->IDCompositionSurface_iface;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE factory_QueryInterface(IDCompositionSurfaceFactory *iface, REFIID iid, void **out)
{
    TRACE("iface %p, iid %s, out %p!\n", iface, debugstr_guid(iid), out);

    if (IsEqualGUID(iid, &IID_IUnknown)
            || IsEqualGUID(iid, &IID_IDCompositionSurfaceFactory))
    {
        IUnknown_AddRef(iface);
        *out = iface;
        return S_OK;
    }

    FIXME("%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid(iid));
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG STDMETHODCALLTYPE factory_AddRef(IDCompositionSurfaceFactory *iface)
{
    struct composition_surface_factory *factory = impl_from_IDCompositionSurfaceFactory(iface);
    ULONG ref = InterlockedIncrement(&factory->ref);

    TRACE("iface %p, ref %lu.\n", iface, ref);
    return ref;
}

static ULONG STDMETHODCALLTYPE factory_Release(IDCompositionSurfaceFactory *iface)
{
    struct composition_surface_factory *factory = impl_from_IDCompositionSurfaceFactory(iface);
    ULONG ref = InterlockedDecrement(&factory->ref);

    TRACE("iface %p, ref %lu.\n", iface, ref);

    if (!ref)
    {
        IDCompositionDevice_Release(factory->device);
        IUnknown_Release(factory->rendering_device);
        free(factory);
    }

    return ref;
}

static HRESULT STDMETHODCALLTYPE factory_CreateSurface(IDCompositionSurfaceFactory *iface,
        UINT width, UINT height, DXGI_FORMAT pixel_format, DXGI_ALPHA_MODE alpha_mode,
        IDCompositionSurface **surface)
{
    struct composition_surface_factory *factory = impl_from_IDCompositionSurfaceFactory(iface);

    FIXME("iface %p, width %u, height %u, format %#x, alpha_mode %#x, surface %p semi-stub!\n", iface,
            width, height, pixel_format, alpha_mode, surface);

    return create_surface(factory, width, height, pixel_format, alpha_mode, surface);
}

static HRESULT STDMETHODCALLTYPE factory_CreateVirtualSurface(IDCompositionSurfaceFactory *iface,
        UINT width, UINT height, DXGI_FORMAT pixel_format, DXGI_ALPHA_MODE alpha_mode,
        IDCompositionVirtualSurface **surface)
{
    FIXME("iface %p, width %u, height %u, format %#x, alpha_mode %#x, surface %p stub!\n", iface,
            width, height, pixel_format, alpha_mode, surface);
    return E_NOTIMPL;
}

static const struct IDCompositionSurfaceFactoryVtbl factory_vtbl =
{
    /* IUnknown methods */
    factory_QueryInterface,
    factory_AddRef,
    factory_Release,
    /* IDCompositionSurfaceFactory methods */
    factory_CreateSurface,
    factory_CreateVirtualSurface,
};

HRESULT create_surface_factory(struct composition_device *device, IUnknown *rendering_device,
        IDCompositionSurfaceFactory **obj)
{
    struct composition_surface_factory *factory;
    IDXGIDevice *dxgi_device;
    ID2D1Device *d2d_device;
    const GUID *iid;

    if (!rendering_device)
        return E_INVALIDARG;

    if (SUCCEEDED(IUnknown_QueryInterface(rendering_device, &IID_IDXGIDevice, (void *)&dxgi_device)))
    {
        TRACE("Creating a surface factory with an IDXGIDevice rendering device.\n");
        iid = &IID_IDXGIDevice;
    }
    else if (SUCCEEDED(IUnknown_QueryInterface(rendering_device, &IID_ID2D1Device, (void *)&d2d_device)))
    {
        TRACE("Creating a surface factory with an IID_ID2D1Device rendering device.\n");
        iid = &IID_ID2D1Device;
    }
    else
    {
        ERR("Unknown rendering device.\n");
        return E_NOINTERFACE;
    }

    factory = calloc(1, sizeof(*factory));
    if (!factory)
        return E_OUTOFMEMORY;

    factory->IDCompositionSurfaceFactory_iface.lpVtbl = &factory_vtbl;
    factory->ref = 1;
    factory->rendering_device = IsEqualGUID(iid, &IID_IDXGIDevice) ? (IUnknown *)dxgi_device : (IUnknown *)d2d_device;
    factory->rendering_device_iid = *iid;
    factory->device = &device->IDCompositionDevice_iface;
    IDCompositionDevice_AddRef(&device->IDCompositionDevice_iface);

    *obj = &factory->IDCompositionSurfaceFactory_iface;
    return S_OK;
}
