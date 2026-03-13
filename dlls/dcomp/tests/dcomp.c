/*
 * Unit test for DirectComposition
 *
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

#define COBJMACROS
#include "initguid.h"
#include <d3d10_1.h>
#include "dcomp.h"
#include "wine/test.h"

static HRESULT (WINAPI *pDCompositionCreateDevice)(IDXGIDevice *dxgi_device, REFIID iid, void **device);
static HRESULT (WINAPI *pDCompositionCreateDevice2)(IUnknown *rendering_device, REFIID iid, void **device);

#define check_interface(a, b, c) check_interface_(__LINE__, a, b, c)
static void check_interface_(unsigned int line, void *iface_ptr, REFIID iid, BOOL supported)
{
    IUnknown *iface = iface_ptr;
    HRESULT hr, expected;
    IUnknown *unk;

    expected = supported ? S_OK : E_NOINTERFACE;
    hr = IUnknown_QueryInterface(iface, iid, (void **)&unk);
    ok_(__FILE__, line)(hr == expected, "got hr %#lx, expected %#lx.\n", hr, expected);
    if (SUCCEEDED(hr))
        IUnknown_Release(unk);
}

static IDXGIDevice *create_device(unsigned int flags)
{
    IDXGIDevice *dxgi_device;
    ID3D10Device1 *device;
    HRESULT hr;

    hr = D3D10CreateDevice1(NULL, D3D10_DRIVER_TYPE_HARDWARE, NULL, flags, D3D10_FEATURE_LEVEL_10_0,
            D3D10_1_SDK_VERSION, &device);
    if (SUCCEEDED(hr))
        goto success;
    if (SUCCEEDED(D3D10CreateDevice1(NULL, D3D10_DRIVER_TYPE_WARP, NULL, flags,
            D3D10_FEATURE_LEVEL_10_0, D3D10_1_SDK_VERSION, &device)))
        goto success;
    if (SUCCEEDED(D3D10CreateDevice1(NULL, D3D10_DRIVER_TYPE_REFERENCE, NULL, flags,
            D3D10_FEATURE_LEVEL_10_0, D3D10_1_SDK_VERSION, &device)))
        goto success;

    return NULL;

success:
    hr = ID3D10Device1_QueryInterface(device, &IID_IDXGIDevice, (void **)&dxgi_device);
    ok(SUCCEEDED(hr), "Created device does not implement IDXGIDevice.\n");
    ID3D10Device1_Release(device);
    return dxgi_device;
}

static void test_DCompositionCreateDevice(void)
{
    IDCompositionDevice *dcomp_device;
    IDXGIDevice *dxgi_device;
    ULONG refcount;
    HRESULT hr;

    /* D3D device created without BGRA support */
    if (!(dxgi_device = create_device(0)))
    {
        skip("Failed to create device.\n");
        return;
    }

    hr = pDCompositionCreateDevice(dxgi_device, &IID_IDCompositionDevice, (void **)&dcomp_device);
    ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
    refcount = IDCompositionDevice_Release(dcomp_device);
    ok(!refcount, "Device has %lu references left.\n", refcount);
    refcount = IDXGIDevice_Release(dxgi_device);
    ok(!refcount, "Device has %lu references left.\n", refcount);

    /* D3D device created with BGRA support */
    if (!(dxgi_device = create_device(D3D10_CREATE_DEVICE_BGRA_SUPPORT)))
    {
        skip("Failed to create device.\n");
        return;
    }

    hr = pDCompositionCreateDevice(dxgi_device, &IID_IDCompositionDevice, (void **)&dcomp_device);
    ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);

    check_interface(dcomp_device, &IID_IUnknown, TRUE);
    check_interface(dcomp_device, &IID_IDCompositionDevice, TRUE);
    /* Device created from DCompositionCreateDevice() doesn't support IDCompositionDevice2 */
    check_interface(dcomp_device, &IID_IDCompositionDevice2, FALSE);

    refcount = IDCompositionDevice_Release(dcomp_device);
    ok(!refcount, "Device has %lu references left.\n", refcount);

    /* Parameter checks */
    hr = pDCompositionCreateDevice(NULL, &IID_IDCompositionDevice, (void **)&dcomp_device);
    ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
    refcount = IDCompositionDevice_Release(dcomp_device);
    ok(!refcount, "Device has %lu references left.\n", refcount);

    /* Crash on Windows */
    if (0)
    {
    hr = pDCompositionCreateDevice(dxgi_device, NULL, (void **)&dcomp_device);
    ok(hr == E_INVALIDARG, "Got unexpected hr %#lx.\n", hr);
    }

    hr = pDCompositionCreateDevice(dxgi_device, &IID_IDCompositionDevice2, (void **)&dcomp_device);
    ok(hr == E_NOINTERFACE, "Got unexpected hr %#lx.\n", hr);

    hr = pDCompositionCreateDevice(dxgi_device, &IID_IDCompositionDevice, NULL);
    ok(hr == E_INVALIDARG, "Got unexpected hr %#lx.\n", hr);

    refcount = IDXGIDevice_Release(dxgi_device);
    ok(!refcount, "Device has %lu references left.\n", refcount);
}

static void test_DCompositionCreateDevice2(void)
{
    IDCompositionDesktopDevice *desktop_device;
    IDCompositionDevice2 *dcomp_device2;
    IDCompositionDevice *dcomp_device;
    IDXGIDevice *dxgi_device;
    ULONG refcount;
    HRESULT hr;

    /* D3D device created without BGRA support */
    if (!(dxgi_device = create_device(0)))
    {
        skip("Failed to create device.\n");
        return;
    }

    hr = pDCompositionCreateDevice2((IUnknown *)dxgi_device, &IID_IDCompositionDevice,
            (void **)&dcomp_device);
    ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
    refcount = IDCompositionDevice_Release(dcomp_device);
    ok(!refcount, "Device has %lu references left.\n", refcount);
    refcount = IDXGIDevice_Release(dxgi_device);
    ok(!refcount, "Device has %lu references left.\n", refcount);

    /* D3D device created with BGRA support */
    if (!(dxgi_device = create_device(D3D10_CREATE_DEVICE_BGRA_SUPPORT)))
    {
        skip("Failed to create device.\n");
        return;
    }

    hr = pDCompositionCreateDevice2((IUnknown *)dxgi_device, &IID_IDCompositionDevice,
            (void **)&dcomp_device);
    ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);

    /* Interface checks */
    check_interface(dcomp_device, &IID_IUnknown, TRUE);
    check_interface(dcomp_device, &IID_IDCompositionDevice, TRUE);
    check_interface(dcomp_device, &IID_IDCompositionDevice2, TRUE);
    check_interface(dcomp_device, &IID_IDCompositionDesktopDevice, TRUE);

    refcount = IDCompositionDevice_Release(dcomp_device);
    ok(!refcount, "Device has %lu references left.\n", refcount);

    /* Parameter checks */
    hr = pDCompositionCreateDevice2(NULL, &IID_IDCompositionDevice, (void **)&dcomp_device);
    ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
    refcount = IDCompositionDevice_Release(dcomp_device);
    ok(!refcount, "Device has %lu references left.\n", refcount);

    /* Crash on Windows */
    if (0)
    {
    hr = pDCompositionCreateDevice2((IUnknown *)dxgi_device, NULL, (void **)&dcomp_device);
    ok(hr == E_INVALIDARG, "Got unexpected hr %#lx.\n", hr);
    }

    /* IDCompositionDevice2 needs to be queried from the device instance */
    hr = pDCompositionCreateDevice2((IUnknown *)dxgi_device, &IID_IDCompositionDevice2,
            (void **)&dcomp_device2);
    ok(hr == E_NOINTERFACE, "Got unexpected hr %#lx.\n", hr);

    hr = pDCompositionCreateDevice2((IUnknown *)dxgi_device, &IID_IDCompositionDesktopDevice,
            (void **)&desktop_device);
    ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
    refcount = IDCompositionDesktopDevice_Release(desktop_device);
    ok(!refcount, "Device has %lu references left.\n", refcount);

    hr = pDCompositionCreateDevice2((IUnknown *)dxgi_device, &IID_IDCompositionDevice, NULL);
    ok(hr == E_INVALIDARG, "Got unexpected hr %#lx.\n", hr);

    refcount = IDXGIDevice_Release(dxgi_device);
    ok(!refcount, "Device has %lu references left.\n", refcount);
}

static void test_device_CreateTargetForHwnd(void)
{
    IDCompositionTarget *target, *target2, *target3;
    IDCompositionDesktopDevice *desktop_device;
    IDCompositionDevice *dcomp_device;
    IDXGIDevice *dxgi_device;
    HRESULT hr, hr2, hr3;
    ULONG refcount;
    HWND hwnd;

    if (!(dxgi_device = create_device(D3D10_CREATE_DEVICE_BGRA_SUPPORT)))
    {
        skip("Failed to create device.\n");
        return;
    }

    hwnd = CreateWindowW(L"static", L"test", WS_POPUP, 0, 0, 1, 1, 0, 0, 0, 0);
    ok(!!hwnd, "Failed to create a test window.\n");

    /* Test CreateTargetForHwnd() with IDCompositionDevice */
    hr = pDCompositionCreateDevice(dxgi_device, &IID_IDCompositionDevice, (void **)&dcomp_device);
    ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);

    /* Parameter checks */
    hr = IDCompositionDevice_CreateTargetForHwnd(dcomp_device, NULL, FALSE, &target);
    ok(hr == E_INVALIDARG, "Got unexpected hr %#lx.\n", hr);

    hr = IDCompositionDevice_CreateTargetForHwnd(dcomp_device, GetDesktopWindow(), FALSE, &target);
    ok(hr == E_INVALIDARG, "Got unexpected hr %#lx.\n", hr);

    hr = IDCompositionDevice_CreateTargetForHwnd(dcomp_device, hwnd, FALSE, &target);
    ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);

    hr2 = IDCompositionDevice_CreateTargetForHwnd(dcomp_device, hwnd, TRUE, &target2);
    ok(hr2 == S_OK, "Got unexpected hr %#lx.\n", hr);

    hr3 = IDCompositionDevice_CreateTargetForHwnd(dcomp_device, hwnd, FALSE, &target3);
    ok(hr3 == DCOMPOSITION_ERROR_WINDOW_ALREADY_COMPOSED, "Got unexpected hr %#lx.\n", hr);

    hr3 = IDCompositionDevice_CreateTargetForHwnd(dcomp_device, hwnd, TRUE, &target3);
    ok(hr3 == DCOMPOSITION_ERROR_WINDOW_ALREADY_COMPOSED, "Got unexpected hr %#lx.\n", hr);

    if (SUCCEEDED(hr))
        IDCompositionTarget_Release(target);
    if (SUCCEEDED(hr2))
        IDCompositionTarget_Release(target2);

    hr = IDCompositionDevice_CreateTargetForHwnd(dcomp_device, hwnd, FALSE, NULL);
    ok(hr == E_INVALIDARG, "Got unexpected hr %#lx.\n", hr);

    refcount = IDCompositionDevice_Release(dcomp_device);
    ok(!refcount, "Device has %lu references left.\n", refcount);

    /* Test CreateTargetForHwnd() with IDCompositionDesktopDevice */
    if (!pDCompositionCreateDevice2)
    {
        win_skip("DCompositionCreateDevice2() is unavailable.\n");
        goto done;
    }

    hr = pDCompositionCreateDevice2((IUnknown *)dxgi_device, &IID_IDCompositionDesktopDevice,
            (void **)&desktop_device);
    ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);

    /* Parameter checks */
    hr = IDCompositionDesktopDevice_CreateTargetForHwnd(desktop_device, NULL, FALSE, &target);
    ok(hr == E_INVALIDARG, "Got unexpected hr %#lx.\n", hr);

    hr = IDCompositionDesktopDevice_CreateTargetForHwnd(desktop_device, hwnd, FALSE, &target);
    ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
    IDCompositionTarget_Release(target);

    hr = IDCompositionDesktopDevice_CreateTargetForHwnd(desktop_device, hwnd, TRUE, &target);
    ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
    IDCompositionTarget_Release(target);

    hr = IDCompositionDesktopDevice_CreateTargetForHwnd(desktop_device, hwnd, FALSE, NULL);
    ok(hr == E_INVALIDARG, "Got unexpected hr %#lx.\n", hr);

    refcount = IDCompositionDesktopDevice_Release(desktop_device);
    ok(!refcount, "Device has %lu references left.\n", refcount);

done:
    DestroyWindow(hwnd);
    refcount = IDXGIDevice_Release(dxgi_device);
    ok(!refcount, "Device has %lu references left.\n", refcount);
}

static void test_device_CreateVisual(void)
{
    IDCompositionDesktopDevice *desktop_device;
    IDCompositionDevice *dcomp_device;
    IDCompositionVisual2 *visual2;
    IDCompositionVisual *visual;
    IDXGIDevice *dxgi_device;
    ULONG refcount;
    HRESULT hr;

    if (!(dxgi_device = create_device(D3D10_CREATE_DEVICE_BGRA_SUPPORT)))
    {
        skip("Failed to create device.\n");
        return;
    }

    /* Test CreateVisual() with IDCompositionDevice */
    hr = pDCompositionCreateDevice(dxgi_device, &IID_IDCompositionDevice, (void **)&dcomp_device);
    ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);

    /* Parameter checks */
    hr = IDCompositionDevice_CreateVisual(dcomp_device, NULL);
    ok(hr == E_INVALIDARG, "Got unexpected hr %#lx.\n", hr);

    hr = IDCompositionDevice_CreateVisual(dcomp_device, &visual);
    ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
    /* IDCompositionVisual objects created from a device from DCompositionCreateDevice() doesn't
     * support IDCompositionVisual2 */
    hr = IDCompositionVisual_QueryInterface(visual, &IID_IDCompositionVisual2, (void *)&visual2);
    ok(hr == E_NOINTERFACE, "Got unexpected hr %#lx.\n", hr);
    IDCompositionVisual_Release(visual);

    refcount = IDCompositionDevice_Release(dcomp_device);
    ok(!refcount, "Device has %lu references left.\n", refcount);

    /* Test CreateVisual() with IDCompositionDesktopDevice */
    if (!pDCompositionCreateDevice2)
    {
        win_skip("DCompositionCreateDevice2() is unavailable.\n");
        goto done;
    }

    hr = pDCompositionCreateDevice2((IUnknown *)dxgi_device, &IID_IDCompositionDesktopDevice,
            (void **)&desktop_device);
    ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);

    /* Parameter checks */
    hr = IDCompositionDesktopDevice_CreateVisual(desktop_device, NULL);
    todo_wine
    ok(hr == E_INVALIDARG, "Got unexpected hr %#lx.\n", hr);

    hr = IDCompositionDesktopDevice_CreateVisual(desktop_device, &visual2);
    todo_wine
    ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
    if (SUCCEEDED(hr))
        IDCompositionVisual2_Release(visual2);

    refcount = IDCompositionDesktopDevice_Release(desktop_device);
    ok(!refcount, "Device has %lu references left.\n", refcount);

done:
    refcount = IDXGIDevice_Release(dxgi_device);
    ok(!refcount, "Device has %lu references left.\n", refcount);
}

START_TEST(dcomp)
{
    HMODULE module;

    module = LoadLibraryW(L"dcomp.dll");
    if (!module)
    {
        win_skip("dcomp.dll not found.\n");
        return;
    }

    pDCompositionCreateDevice = (void *)GetProcAddress(module, "DCompositionCreateDevice");
    pDCompositionCreateDevice2 = (void *)GetProcAddress(module, "DCompositionCreateDevice2");

    if (!pDCompositionCreateDevice)
    {
        win_skip("DCompositionCreateDevice() is unavailable.\n");
        FreeLibrary(module);
        return;
    }

    test_DCompositionCreateDevice();
    test_DCompositionCreateDevice2();
    test_device_CreateTargetForHwnd();
    test_device_CreateVisual();

    FreeLibrary(module);
}
