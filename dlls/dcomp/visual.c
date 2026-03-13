/*
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

#define COBJMACROS
#include "windef.h"
#include "winbase.h"
#include "dcomp_private.h"
#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(dcomp);

static HRESULT STDMETHODCALLTYPE visual_QueryInterface(IDCompositionVisual2 *iface, REFIID iid,
        void **out)
{
    struct composition_visual *visual = impl_from_IDCompositionVisual2(iface);

    TRACE("iface %p, iid %s, out %p stub!\n", iface, debugstr_guid(iid), out);

    if (IsEqualGUID(iid, &IID_IUnknown)
            || IsEqualGUID(iid, &IID_IDCompositionVisual)
            || (visual->version >= 2 && IsEqualGUID(iid, &IID_IDCompositionVisual2)))
    {
        IUnknown_AddRef(&visual->IDCompositionVisual2_iface);
        *out = &visual->IDCompositionVisual2_iface;
        return S_OK;
    }

    FIXME("%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid(iid));
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG STDMETHODCALLTYPE visual_AddRef(IDCompositionVisual2 *iface)
{
    struct composition_visual *visual = impl_from_IDCompositionVisual2(iface);
    ULONG ref = InterlockedIncrement(&visual->ref);

    TRACE("iface %p, ref %lu.\n", iface, ref);
    return ref;
}

static ULONG STDMETHODCALLTYPE visual_Release(IDCompositionVisual2 *iface)
{
    struct composition_visual *visual = impl_from_IDCompositionVisual2(iface);
    ULONG ref = InterlockedDecrement(&visual->ref);

    TRACE("iface %p, ref %lu.\n", iface, ref);

    if (!ref)
    {
        if (visual->content)
            IUnknown_Release(visual->content);
        free(visual);
    }

    return ref;
}

static HRESULT STDMETHODCALLTYPE visual_SetOffsetXAnimation(IDCompositionVisual2 *iface,
        IDCompositionAnimation *animation)
{
    FIXME("iface %p, animation %p stub!\n", iface, animation);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE visual_SetOffsetX(IDCompositionVisual2 *iface, float offset_x)
{
    FIXME("iface %p, offset_x %f stub!\n", iface, offset_x);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE visual_SetOffsetYAnimation(IDCompositionVisual2 *iface,
        IDCompositionAnimation *animation)
{
    FIXME("iface %p, animation %p stub!\n", iface, animation);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE visual_SetOffsetY(IDCompositionVisual2 *iface, float offset_y)
{
    FIXME("iface %p, offset_y %f stub!\n", iface, offset_y);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE visual_SetTransformObject(IDCompositionVisual2 *iface,
        IDCompositionTransform *transform)
{
    FIXME("iface %p, transform %p stub!\n", iface, transform);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE visual_SetTransform(IDCompositionVisual2 *iface,
        const D2D_MATRIX_3X2_F *matrix)
{
    FIXME("iface %p, matrix %p stub!\n", iface, matrix);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE visual_SetTransformParent(IDCompositionVisual2 *iface,
        IDCompositionVisual *visual)
{
    FIXME("iface %p, visual %p stub!\n", iface, visual);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE visual_SetEffect(IDCompositionVisual2 *iface,
        IDCompositionEffect *effect)
{
    FIXME("iface %p, effect %p stub!\n", iface, effect);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE visual_SetBitmapInterpolationMode(IDCompositionVisual2 *iface,
        enum DCOMPOSITION_BITMAP_INTERPOLATION_MODE interpolation_mode)
{
    FIXME("iface %p, interpolation_mode %d stub!\n", iface, interpolation_mode);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE visual_SetBorderMode(IDCompositionVisual2 *iface,
        enum DCOMPOSITION_BORDER_MODE border_mode)
{
    FIXME("iface %p, border_mode %d stub!\n", iface, border_mode);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE visual_SetClipObject(IDCompositionVisual2 *iface,
        IDCompositionClip *clip)
{
    FIXME("iface %p, clip %p stub!\n", iface, clip);
    return E_NOTIMPL;
}

/* C++ method: SetClip(THIS_ const D2D_RECT_F &rect).  Use pointer instead of reference */
static HRESULT STDMETHODCALLTYPE visual_SetClip(IDCompositionVisual2 *iface, const D2D_RECT_F *rect)
{
    FIXME("iface %p, rect %p stub!\n", iface, rect);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE visual_SetContent(IDCompositionVisual2 *iface, IUnknown *content)
{
    struct composition_visual *visual = impl_from_IDCompositionVisual2(iface);
    IDXGISwapChain1 *dxgi_swapchain;

    FIXME("iface %p, content %p semi-stub!\n", iface, content);

    if (content && FAILED(IUnknown_QueryInterface(content, &IID_IDXGISwapChain1,
            (void **)&dxgi_swapchain)))
    {
        FIXME("Only IDXGISwapChain1 is supported currently.\n");
        return E_INVALIDARG;
    }

    if (visual->content)
        IUnknown_Release(visual->content);
    visual->content = content;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE visual_AddVisual(IDCompositionVisual2 *iface,
        IDCompositionVisual *visual, BOOL insert_above, IDCompositionVisual *reference_visual)
{
    FIXME("iface %p, visual %p, insert_above %d, reference_visual %p, stub!\n", iface, visual,
            insert_above, reference_visual);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE visual_RemoveVisual(IDCompositionVisual2 *iface,
        IDCompositionVisual *visual)
{
    FIXME("iface %p, visual %p stub!\n", iface, visual);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE visual_RemoveAllVisuals(IDCompositionVisual2 *iface)
{
    FIXME("iface %p stub!\n", iface);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE visual_SetCompositeMode(IDCompositionVisual2 *iface,
        enum DCOMPOSITION_COMPOSITE_MODE composite_mode)
{
    FIXME("iface %p, composite_mode %d stub!\n", iface, composite_mode);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE visual_SetOpacityMode(IDCompositionVisual2 *iface,
        enum DCOMPOSITION_OPACITY_MODE opacity_mode)
{
    FIXME("iface %p, opacity_mode %d stub!\n", iface, opacity_mode);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE visual_SetBackFaceVisibility(IDCompositionVisual2 *iface,
        enum DCOMPOSITION_BACKFACE_VISIBILITY visibility)
{
    FIXME("iface %p, visibility %d stub!\n", iface, visibility);
    return E_NOTIMPL;
}

static const struct IDCompositionVisual2Vtbl visual2_vtbl =
{
    /* IUnknown methods */
    visual_QueryInterface,
    visual_AddRef,
    visual_Release,
    /* IDCompositionVisual methods */
    visual_SetOffsetXAnimation,
    visual_SetOffsetX,
    visual_SetOffsetYAnimation,
    visual_SetOffsetY,
    visual_SetTransformObject,
    visual_SetTransform,
    visual_SetTransformParent,
    visual_SetEffect,
    visual_SetBitmapInterpolationMode,
    visual_SetBorderMode,
    visual_SetClipObject,
    visual_SetClip,
    visual_SetContent,
    visual_AddVisual,
    visual_RemoveVisual,
    visual_RemoveAllVisuals,
    visual_SetCompositeMode,
    /* IDCompositionVisual2 methods */
    visual_SetOpacityMode,
    visual_SetBackFaceVisibility,
};

HRESULT create_visual(int version, REFIID iid, void **new_visual)
{
    struct composition_visual *visual;
    HRESULT hr;

    if (!new_visual)
        return E_INVALIDARG;

    visual = calloc(1, sizeof(*visual));
    if (!visual)
        return E_OUTOFMEMORY;

    visual->IDCompositionVisual2_iface.lpVtbl = &visual2_vtbl;
    visual->version = version;
    visual->ref = 1;
    hr = IUnknown_QueryInterface(&visual->IDCompositionVisual2_iface, iid, new_visual);
    IUnknown_Release(&visual->IDCompositionVisual2_iface);
    return hr;
}
