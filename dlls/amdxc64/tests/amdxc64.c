/*
 * amdxc64 tests
 *
 * Copyright 2026 Erhan Bilgili
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * Lesser General Public License for more details.
 */

#define COBJMACROS
#include "initguid.h"
#include "../amdxc_interfaces.h"
#include "../vkd3d_device_vkd3d_ext.h"
#include "wine/test.h"

static HRESULT (CDECL *pAmdExtD3DCreateInterface)(IUnknown *, REFIID, void **);

struct test_device
{
    IUnknown IUnknown_iface;
    ID3D12DeviceExt3 ID3D12DeviceExt3_iface;
    LONG ref;
    ULONG intrinsics;
    BOOL wmma, fp8;
};

static HRESULT WINAPI device_QueryInterface(IUnknown *iface, REFIID iid, void **out)
{
    struct test_device *device = CONTAINING_RECORD(iface, struct test_device, IUnknown_iface);

    *out = NULL;
    if (IsEqualGUID(iid, &IID_IUnknown)) *out = iface;
    else if (IsEqualGUID(iid, &IID_ID3D12DeviceExt3)) *out = &device->ID3D12DeviceExt3_iface;
    else return E_NOINTERFACE;
    InterlockedIncrement(&device->ref);
    return S_OK;
}

static ULONG WINAPI device_AddRef(IUnknown *iface)
{
    struct test_device *device = CONTAINING_RECORD(iface, struct test_device, IUnknown_iface);
    return InterlockedIncrement(&device->ref);
}

static ULONG WINAPI device_Release(IUnknown *iface)
{
    struct test_device *device = CONTAINING_RECORD(iface, struct test_device, IUnknown_iface);
    return InterlockedDecrement(&device->ref);
}

static const IUnknownVtbl device_vtbl = {device_QueryInterface, device_AddRef, device_Release};

static HRESULT WINAPI ext_QueryInterface(ID3D12DeviceExt3 *iface, REFIID iid, void **out)
{
    struct test_device *device = CONTAINING_RECORD(iface, struct test_device, ID3D12DeviceExt3_iface);
    return device_QueryInterface(&device->IUnknown_iface, iid, out);
}

static ULONG WINAPI ext_AddRef(ID3D12DeviceExt3 *iface)
{
    struct test_device *device = CONTAINING_RECORD(iface, struct test_device, ID3D12DeviceExt3_iface);
    return device_AddRef(&device->IUnknown_iface);
}

static ULONG WINAPI ext_Release(ID3D12DeviceExt3 *iface)
{
    struct test_device *device = CONTAINING_RECORD(iface, struct test_device, ID3D12DeviceExt3_iface);
    return device_Release(&device->IUnknown_iface);
}

static BOOL WINAPI ext_SupportsAGSExtension(ID3D12DeviceExt3 *iface, D3D12_AGS_EXTENSION extension)
{
    struct test_device *device = CONTAINING_RECORD(iface, struct test_device, ID3D12DeviceExt3_iface);

    if (extension == D3D12_AGS_EXTENSION_WMMA_FP8) return device->wmma;
    if (extension == D3D12_AGS_EXTENSION_WMMA_FP8_NATIVE) return device->fp8;
    return extension < 32 && (device->intrinsics & (1u << extension));
}

static const ID3D12DeviceExt3Vtbl ext_vtbl =
{
    .QueryInterface = ext_QueryInterface,
    .AddRef = ext_AddRef,
    .Release = ext_Release,
    .SupportsAGSExtension = ext_SupportsAGSExtension,
};

static struct test_device device = {{&device_vtbl}, {&ext_vtbl}, 1};

static void test_disabled_provider(void)
{
    IAmdExtFfxApi *ffx;
    HMODULE provider;
    char env[16];
    DWORD len;
    HRESULT hr;
    unsigned int data[32] = {0};

    SetLastError(ERROR_SUCCESS);
    len = GetEnvironmentVariableA("FSR4_UPGRADE", env, sizeof(env));
    if ((!len && GetLastError() == ERROR_ENVVAR_NOT_FOUND) || (len == 1 && env[0] == '1'))
        return;

    provider = GetModuleHandleA("amdxcffx64.dll");
    hr = pAmdExtD3DCreateInterface(&device.IUnknown_iface, &IID_IAmdExtFfxApi, (void **)&ffx);
    ok(hr == S_OK, "Create returned %#lx.\n", hr);
    if (FAILED(hr)) return;
    hr = IAmdExtFfxApi_UpdateFfxApiProvider(ffx, NULL, 0);
    ok(hr == E_INVALIDARG, "Null data returned %#lx.\n", hr);
    hr = IAmdExtFfxApi_UpdateFfxApiProvider(ffx, data, sizeof(data));
    ok(hr == E_NOTIMPL, "Disabled upgrade returned %#lx.\n", hr);
    ok(GetModuleHandleA("amdxcffx64.dll") == provider, "Disabled upgrade loaded a provider.\n");
    IAmdExtFfxApi_Release(ffx);
    ok(device.ref == 1, "Leaked device reference %ld.\n", device.ref);
}

START_TEST(amdxc64)
{
    HMODULE module = LoadLibraryA("amdxc64.dll");

    if (!module)
    {
        win_skip("amdxc64.dll is unavailable.\n");
        return;
    }
    pAmdExtD3DCreateInterface = (void *)GetProcAddress(module, "AmdExtD3DCreateInterface");
    if (!pAmdExtD3DCreateInterface)
    {
        win_skip("AmdExtD3DCreateInterface is unavailable.\n");
        FreeLibrary(module);
        return;
    }
    test_disabled_provider();
    FreeLibrary(module);
}
