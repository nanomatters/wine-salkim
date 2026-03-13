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

#include <stdarg.h>

#include "ntstatus.h"
#define WIN32_NO_STATUS
#define COBJMACROS
#include <winternl.h>
#include "initguid.h"
#include <d2d1_1.h>
#include <d3d10_1.h>
#include "dcomp.h"
#include "wine/test.h"

static HRESULT (WINAPI *pDCompositionCreateDevice)(IDXGIDevice *dxgi_device, REFIID iid, void **device);
static HRESULT (WINAPI *pDCompositionCreateDevice2)(IUnknown *rendering_device, REFIID iid, void **device);

static void *get_stack_pointer(void)
{
    void *stack_pointer = NULL;

#ifdef __i386__
    __asm__ __volatile__("movl %%esp, %0"
                         : "=r"(stack_pointer) /* output */
                         :                     /* no input */
                         :                     /* no clobbered registers */
    );
#elif __x86_64__
    __asm__ __volatile__("movq %%rsp, %0"
                         : "=r"(stack_pointer) /* output */
                         :                     /* no input */
                         :                     /* no clobbered registers */
    );
#else
#error "Unsupported architecture"
#endif
    return stack_pointer;
}

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

static void set_color(D2D1_COLOR_F *color, float r, float g, float b, float a)
{
    color->r = r;
    color->g = g;
    color->b = b;
    color->a = a;
}

/* try to make sure pending X events have been processed before continuing */
static void flush_events(void)
{
    int diff = 200;
    DWORD time;
    MSG msg;

    time = GetTickCount() + diff;
    while (diff > 0)
    {
        if (MsgWaitForMultipleObjects(0, NULL, FALSE, 100, QS_ALLINPUT) == WAIT_TIMEOUT)
            break;
        while (PeekMessageA(&msg, 0, 0, 0, PM_REMOVE))
            DispatchMessageA(&msg);
        diff = time - GetTickCount();
    }
}

static HWND create_window(void)
{
    RECT r = {0, 0, 640, 480};

    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW | WS_VISIBLE, FALSE);
    return CreateWindowW(L"static", L"dcomp_test", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 0, 0,
            r.right - r.left, r.bottom - r.top, NULL, NULL, NULL, NULL);
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

#define get_factory(a, b) get_factory_(__LINE__, a, b)
static void get_factory_(unsigned int line, IDXGIDevice *device, IDXGIFactory **factory)
{
    IDXGIAdapter *adapter;
    HRESULT hr;

    hr = IDXGIDevice_GetAdapter(device, &adapter);
    ok_(__FILE__, line)(hr == S_OK, "Failed to get adapter, hr %#lx.\n", hr);
    hr = IDXGIAdapter_GetParent(adapter, &IID_IDXGIFactory, (void **)factory);
    ok_(__FILE__, line)(hr == S_OK, "Failed to get parent, hr %#lx.\n", hr);
    IDXGIAdapter_Release(adapter);
}

#define create_swapchain(a, b) create_swapchain_(__LINE__, a, b)
static IDXGISwapChain *create_swapchain_(unsigned int line, IDXGIDevice *device, HWND window)
{
    DXGI_SWAP_CHAIN_DESC desc;
    IDXGISwapChain *swapchain;
    IDXGIFactory *factory;
    HRESULT hr;

    desc.BufferDesc.Width = 640;
    desc.BufferDesc.Height = 480;
    desc.BufferDesc.RefreshRate.Numerator = 60;
    desc.BufferDesc.RefreshRate.Denominator = 1;
    desc.BufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.BufferDesc.ScanlineOrdering = DXGI_MODE_SCANLINE_ORDER_UNSPECIFIED;
    desc.BufferDesc.Scaling = DXGI_MODE_SCALING_UNSPECIFIED;
    desc.SampleDesc.Count = 1;
    desc.SampleDesc.Quality = 0;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = 1;
    desc.OutputWindow = window;
    desc.Windowed = TRUE;
    desc.SwapEffect =  DXGI_SWAP_EFFECT_SEQUENTIAL;
    desc.Flags = 0;

    get_factory(device, &factory);
    hr = IDXGIFactory_CreateSwapChain(factory, (IUnknown *)device, &desc, &swapchain);
    ok_(__FILE__, line)(hr == S_OK, "Failed to create swapchain, hr %#lx.\n", hr);
    IDXGIFactory_Release(factory);

    return swapchain;
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

static void test_DCompositionCreateSharedVisualHandle(void)
{
    HRESULT (WINAPI *pDCompositionCreateSharedVisualHandle)(HANDLE *ret_handle);
    char buffer[1024] = {0};
    OBJECT_TYPE_INFORMATION *type = (OBJECT_TYPE_INFORMATION *)buffer;
    void *stack_pointer, *old_stack_pointer;
    NTSTATUS status;
    HMODULE module;
    HANDLE handle;
    ULONG len = 0;
    HRESULT hr;
    BOOL ret;

    module = GetModuleHandleW(L"dcomp.dll");
    ok(!!module, "GetModuleHandleW failed.\n");

    pDCompositionCreateSharedVisualHandle = (void *)GetProcAddress(module, (LPCSTR)1040);
    ok(!!pDCompositionCreateSharedVisualHandle, "Failed to load function at ordinal 1040.\n");

    hr = pDCompositionCreateSharedVisualHandle(NULL);
    ok(hr == STATUS_INVALID_PARAMETER, "Got unexpected hr %#lx.\n", hr);

    old_stack_pointer = get_stack_pointer();

    handle = NULL;
    hr = pDCompositionCreateSharedVisualHandle(&handle);
    ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);

    stack_pointer = get_stack_pointer();
    ok(stack_pointer == old_stack_pointer, "Got unexpected stack pointer.\n");

    status = NtQueryObject(handle, ObjectTypeInformation, buffer, sizeof(buffer), &len);
    todo_wine
    ok(!status, "Got %#lx.\n", status);
    if (!status)
    {
        ok(!wcscmp(type->TypeName.Buffer, L"Composition"), "Got %s.\n", debugstr_w(type->TypeName.Buffer));

        ret = CloseHandle(handle);
        ok(ret, "CloseHandle failed.\n");
    }
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
    ok(hr == E_INVALIDARG, "Got unexpected hr %#lx.\n", hr);

    hr = IDCompositionDesktopDevice_CreateVisual(desktop_device, &visual2);
    ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
    IDCompositionVisual2_Release(visual2);

    refcount = IDCompositionDesktopDevice_Release(desktop_device);
    ok(!refcount, "Device has %lu references left.\n", refcount);

done:
    refcount = IDXGIDevice_Release(dxgi_device);
    ok(!refcount, "Device has %lu references left.\n", refcount);
}

static void test_visual_SetContent(void)
{
    IDCompositionDevice *dcomp_device;
    IDCompositionVisual *visual;
    IDXGISwapChain *swapchain;
    IDXGIDevice *dxgi_device;
    ULONG refcount;
    HRESULT hr;
    HWND hwnd;

    if (!(dxgi_device = create_device(D3D10_CREATE_DEVICE_BGRA_SUPPORT)))
    {
        skip("Failed to create device.\n");
        return;
    }

    hwnd = create_window();
    swapchain = create_swapchain(dxgi_device, hwnd);
    hr = pDCompositionCreateDevice(dxgi_device, &IID_IDCompositionDevice, (void **)&dcomp_device);
    ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
    hr = IDCompositionDevice_CreateVisual(dcomp_device, &visual);
    ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);

    /* Obviously IDCompositionDevice doesn't have IDXGISwapChain1 */
    hr = IDCompositionVisual_SetContent(visual, (IUnknown *)dcomp_device);
    ok(hr == E_INVALIDARG, "Got unexpected hr %#lx.\n", hr);

    hr = IDCompositionVisual_SetContent(visual, (IUnknown *)swapchain);
    ok(hr == S_OK || broken(hr == DXGI_ERROR_UNSUPPORTED) /* win8 and win10 v1507 TestBot */,
            "Got unexpected hr %#lx.\n", hr);

    hr = IDCompositionVisual_SetContent(visual, NULL);
    ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);

    IDCompositionVisual_Release(visual);
    refcount = IDCompositionDevice_Release(dcomp_device);
    ok(!refcount, "Device has %lu references left.\n", refcount);
    IDXGISwapChain_Release(swapchain);
    DestroyWindow(hwnd);
    refcount = IDXGIDevice_Release(dxgi_device);
    ok(!refcount, "Device has %lu references left.\n", refcount);
}

static void test_target_SetRoot(void)
{
    IDCompositionTarget *target, *target2;
    IDCompositionDevice *dcomp_device;
    IDCompositionVisual *visual;
    IDXGISwapChain *swapchain;
    IDXGIDevice *dxgi_device;
    HWND hwnd, hwnd2, hwnd3;
    ULONG refcount;
    HRESULT hr;

    if (!(dxgi_device = create_device(D3D10_CREATE_DEVICE_BGRA_SUPPORT)))
    {
        skip("Failed to create device.\n");
        return;
    }

    hwnd = create_window();
    swapchain = create_swapchain(dxgi_device, hwnd);
    hr = pDCompositionCreateDevice(dxgi_device, &IID_IDCompositionDevice, (void **)&dcomp_device);
    ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
    hr = IDCompositionDevice_CreateVisual(dcomp_device, &visual);
    ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
    hr = IDCompositionVisual_SetContent(visual, (IUnknown *)swapchain);
    ok(hr == S_OK || broken(hr == DXGI_ERROR_UNSUPPORTED) /* win8 and win10 v1507 TestBot */,
            "Got unexpected hr %#lx.\n", hr);
    hwnd2 = create_window();
    hr = IDCompositionDevice_CreateTargetForHwnd(dcomp_device, hwnd2, TRUE, &target);
    ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
    hwnd3 = create_window();
    hr = IDCompositionDevice_CreateTargetForHwnd(dcomp_device, hwnd3, TRUE, &target2);
    ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);

    hr = IDCompositionTarget_SetRoot(target, visual);
    ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);

    /* SetRoot with a visual already bound to a target */
    hr = IDCompositionTarget_SetRoot(target2, visual);
    ok(hr == E_INVALIDARG, "Got unexpected hr %#lx.\n", hr);

    hr = IDCompositionTarget_SetRoot(target, NULL);
    ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);

    hr = IDCompositionTarget_SetRoot(target2, visual);
    ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);

    DestroyWindow(hwnd3);
    DestroyWindow(hwnd2);
    IDCompositionTarget_Release(target2);
    IDCompositionTarget_Release(target);
    IDCompositionVisual_Release(visual);
    refcount = IDCompositionDevice_Release(dcomp_device);
    ok(!refcount, "Device has %lu references left.\n", refcount);
    IDXGISwapChain_Release(swapchain);
    DestroyWindow(hwnd);
    refcount = IDXGIDevice_Release(dxgi_device);
    ok(!refcount, "Device has %lu references left.\n", refcount);
}

static void render_color_surface(IDXGISurface *surface, D2D1_COLOR_F *color)
{
    ID2D1RenderTarget *render_target;
    ID2D1Factory *d2d_factory;
    HRESULT hr;

    static const D2D1_RENDER_TARGET_PROPERTIES rt_desc =
    {
        .type = D2D1_RENDER_TARGET_TYPE_DEFAULT,
        .pixelFormat.format = DXGI_FORMAT_B8G8R8A8_UNORM,
        .pixelFormat.alphaMode = D2D1_ALPHA_MODE_PREMULTIPLIED,
        .dpiX = 0.0f,
        .dpiY = 0.0f,
        .usage = D2D1_RENDER_TARGET_USAGE_NONE,
        .minLevel = D2D1_FEATURE_LEVEL_DEFAULT,
    };

    hr = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &IID_ID2D1Factory, NULL, (void **)&d2d_factory);
    ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);

    hr = ID2D1Factory_CreateDxgiSurfaceRenderTarget(d2d_factory, (IDXGISurface *)surface,
            &rt_desc, &render_target);
    ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);

    ID2D1RenderTarget_BeginDraw(render_target);
    ID2D1RenderTarget_Clear(render_target, color);
    hr = ID2D1RenderTarget_EndDraw(render_target, NULL, NULL);
    ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);

    ID2D1RenderTarget_Release(render_target);
    ID2D1Factory_Release(d2d_factory);
}

static void render_color_swapchain(IDXGISwapChain1 *swapchain, D2D1_COLOR_F *color)
{
    IDXGISurface *surface;
    HRESULT hr;

    hr = IDXGISwapChain1_GetBuffer(swapchain, 0, &IID_IDXGISurface, (void **)&surface);
    ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
    render_color_surface(surface, color);
    IDXGISwapChain1_Present(swapchain, 0, 0);
    IDXGISurface_Release(surface);
}

#define expect_rendered_color(a, b) _expect_rendered_color(__LINE__, a, b)
static void _expect_rendered_color(int line, HWND hwnd, COLORREF expected_color)
{
    COLORREF color = CLR_INVALID;
    int time = 0;
    POINT pt;
    HDC hdc;

    pt.x = 100;
    pt.y = 100;
    ClientToScreen(hwnd, &pt);

    hdc = GetDC(0);
    while (time < 500)
    {
        color = GetPixel(hdc, pt.x, pt.y);
        if (color == expected_color)
            break;

        /* Wait for DWM to finish composition */
        Sleep(100);
        time += 100;
        continue;
    }
    ReleaseDC(0, hdc);
    ok_(__FILE__, line)(color == expected_color, "Expected color %#06lx, got %#06lx.\n",
            expected_color, color);
}

static void test_device_Commit(void)
{
    DXGI_SWAP_CHAIN_DESC1 swapchain_desc;
    IDCompositionDevice *dcomp_device;
    ID3D10Device1 *d3d10_device;
    IDCompositionTarget *target;
    IDCompositionVisual *visual;
    IDXGISwapChain1 *swapchain;
    IDXGIDevice *dxgi_device;
    IDXGIFactory2 *factory2;
    IDXGIFactory *factory;
    D2D1_COLOR_F color;
    ULONG refcount;
    HBRUSH brush;
    HRESULT hr;
    HWND hwnd;
    RECT rect;
    HDC hdc;

    swapchain_desc.Width = 640;
    swapchain_desc.Height = 480;
    swapchain_desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    swapchain_desc.Stereo = FALSE;
    swapchain_desc.SampleDesc.Count = 1;
    swapchain_desc.SampleDesc.Quality = 0;
    swapchain_desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    swapchain_desc.BufferCount = 2;
    swapchain_desc.Scaling = DXGI_SCALING_STRETCH;
    swapchain_desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
    swapchain_desc.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;
    swapchain_desc.Flags = 0;

    hr = D3D10CreateDevice1(NULL, D3D10_DRIVER_TYPE_HARDWARE, NULL,
            D3D10_CREATE_DEVICE_BGRA_SUPPORT, D3D10_FEATURE_LEVEL_10_0, D3D10_1_SDK_VERSION, &d3d10_device);
    ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);

    hr = ID3D10Device1_QueryInterface(d3d10_device, &IID_IDXGIDevice, (void **)&dxgi_device);
    ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);

    get_factory(dxgi_device, &factory);
    IDXGIDevice_Release(dxgi_device);
    hr = IDXGIFactory_QueryInterface(factory, &IID_IDXGIFactory2, (void **)&factory2);
    IDXGIFactory_Release(factory);
    if (FAILED(hr))
    {
        win_skip("IDXGIFactory2 not available.\n");
        refcount = ID3D10Device1_Release(d3d10_device);
        ok(!refcount, "Device has %lu references left.\n", refcount);
        return;
    }

    hr = IDXGIFactory2_CreateSwapChainForComposition(factory2, (IUnknown *)d3d10_device,
            &swapchain_desc, NULL, &swapchain);
    ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);

    hr = pDCompositionCreateDevice(NULL, &IID_IDCompositionDevice, (void **)&dcomp_device);
    ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);

    hr = IDCompositionDevice_CreateVisual(dcomp_device, &visual);
    ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);

    hr = IDCompositionVisual_SetContent(visual, (IUnknown *)swapchain);
    ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);

    hwnd = create_window();
    flush_events();

    hr = IDCompositionDevice_CreateTargetForHwnd(dcomp_device, hwnd, TRUE, &target);
    ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
    hr = IDCompositionTarget_SetRoot(target, visual);
    ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);

    /* Commit() is not called, swapchain presentation shouldn't affect window content */
    /* Render window green */
    hdc = GetDC(hwnd);
    brush = CreateSolidBrush(RGB(0, 0xff, 0));
    GetClientRect(hwnd, &rect);
    FillRect(hdc, &rect, brush);
    DeleteObject(brush);
    ReleaseDC(hwnd, hdc);
    flush_events();

    set_color(&color, 1.0f, 0.0f, 1.0f, 1.0f);
    render_color_swapchain(swapchain, &color);
    expect_rendered_color(hwnd, RGB(0, 0xff, 0));

    /* Call Commit() */
    hr = IDCompositionDevice_Commit(dcomp_device);
    ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);

    /* Render swapchain red */
    set_color(&color, 1.0f, 0.0f, 0.0f, 1.0f);
    render_color_swapchain(swapchain, &color);
    expect_rendered_color(hwnd, RGB(0xff, 0, 0));

    /* Render swapchain blue */
    set_color(&color, 0.0f, 0.0f, 1.0f, 1.0f);
    render_color_swapchain(swapchain, &color);
    expect_rendered_color(hwnd, RGB(0, 0, 0xff));

    DestroyWindow(hwnd);
    IDCompositionTarget_Release(target);
    IDCompositionVisual_Release(visual);
    refcount = IDCompositionDevice_Release(dcomp_device);
    ok(!refcount, "Device has %lu references left.\n", refcount);
    IDXGISwapChain1_Release(swapchain);
    IDXGIFactory2_Release(factory2);
    refcount = ID3D10Device1_Release(d3d10_device);
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
    test_DCompositionCreateSharedVisualHandle();
    test_device_Commit();
    test_device_CreateTargetForHwnd();
    test_device_CreateVisual();
    test_target_SetRoot();
    test_visual_SetContent();

    FreeLibrary(module);
}
