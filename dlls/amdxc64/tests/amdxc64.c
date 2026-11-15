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
#include "ntstatus.h"
#define WIN32_NO_STATUS
#include "initguid.h"
#include "winternl.h"
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
    LONG interop_queries;
};

static HRESULT WINAPI device_QueryInterface(IUnknown *iface, REFIID iid, void **out)
{
    struct test_device *device = CONTAINING_RECORD(iface, struct test_device, IUnknown_iface);

    if (IsEqualGUID(iid, &IID_ID3D12DXVKInteropDevice)) InterlockedIncrement(&device->interop_queries);
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

static void test_upgrade_probe(void)
{
    LONG queries = device.interop_queries;
    IAmdExtFfxApi *ffx;
    HRESULT hr;

    hr = pAmdExtD3DCreateInterface(&device.IUnknown_iface, &IID_IAmdExtFfxApi, (void **)&ffx);
    ok(hr == S_OK, "Create returned %#lx.\n", hr);
    if (FAILED(hr)) return;
    /* The fake device has no Vulkan interop; only an enabled provider should probe it. */
    if (GetEnvironmentVariableW(L"WINE_FSR4_PROVIDER", NULL, 0) > 1)
        ok(device.interop_queries == queries + 1, "Selected provider did not check the device.\n");
    else
        ok(device.interop_queries == queries, "Disabled provider queried Vulkan interop.\n");
    IAmdExtFfxApi_Release(ffx);
    ok(device.ref == 1, "Leaked device reference %ld.\n", device.ref);
}

static void test_info_stubs(void)
{
    const AmdExtD3DShaderIntrinsicsInfo zero = {0};
    AmdExtD3DShaderIntrinsicsInfo info;
    AmdExtD3DGpuRtVersion version;
    IAmdExtD3DShaderIntrinsics *intrinsics;
    IAmdExtD3DDevice8 *extension;
    IAmdExtD3DFactory *factory;
    HRESULT hr;

    hr = pAmdExtD3DCreateInterface(NULL, &IID_IAmdExtD3DFactory, (void **)&factory);
    ok(hr == S_OK, "Factory returned %#lx.\n", hr);
    if (FAILED(hr)) return;
    hr = IAmdExtD3DFactory_CreateInterface(factory, &device.IUnknown_iface,
            &IID_IAmdExtD3DShaderIntrinsics, (void **)&intrinsics);
    ok(hr == S_OK, "Create returned %#lx.\n", hr);
    if (SUCCEEDED(hr))
    {
        hr = IAmdExtD3DShaderIntrinsics_GetInfo(intrinsics, NULL);
        ok(hr == E_INVALIDARG, "Null info returned %#lx.\n", hr);
        memset(&info, 0xa5, sizeof(info));
        hr = IAmdExtD3DShaderIntrinsics_GetInfo(intrinsics, &info);
        ok(hr == E_NOTIMPL, "Unimplemented info returned %#lx.\n", hr);
        ok(!memcmp(&info, &zero, sizeof(info)), "Info was not cleared.\n");
        IAmdExtD3DShaderIntrinsics_Release(intrinsics);
    }
    hr = IAmdExtD3DFactory_CreateInterface(factory, &device.IUnknown_iface,
            &IID_IAmdExtD3DDevice8, (void **)&extension);
    ok(hr == S_OK, "Create returned %#lx.\n", hr);
    if (SUCCEEDED(hr))
    {
        memset(&version, 0xa5, sizeof(version));
        IAmdExtD3DDevice8_GetGpuRtInterfaceVersion(extension, &version);
        ok(!version.major && !version.minor, "Unimplemented interface version was not cleared.\n");
        memset(&version, 0xa5, sizeof(version));
        IAmdExtD3DDevice8_GetGpuRtBinaryVersion(extension, &version);
        ok(!version.major && !version.minor, "Unimplemented binary version was not cleared.\n");
        IAmdExtD3DDevice8_GetGpuRtInterfaceVersion(extension, NULL);
        IAmdExtD3DDevice8_GetGpuRtBinaryVersion(extension, NULL);
        IAmdExtD3DDevice8_Release(extension);
    }
    IAmdExtD3DFactory_Release(factory);
    ok(device.ref == 1, "Leaked device reference %ld.\n", device.ref);
}

static void test_wave_matrix(void)
{
    AmdExtWaveMatrixProperties properties[2], saved[2];
    IAmdExtD3DFactory *factory;
    IAmdExtD3DDevice8 *extension;
    unsigned int fp8, i;
    SIZE_T count;
    HRESULT hr;

    hr = pAmdExtD3DCreateInterface(NULL, &IID_IAmdExtD3DFactory, (void **)&factory);
    ok(hr == S_OK, "Factory returned %#lx.\n", hr);
    if (FAILED(hr)) return;
    for (fp8 = 0; fp8 < 2; ++fp8)
    {
        device.fp8 = fp8;
        hr = IAmdExtD3DFactory_CreateInterface(factory, &device.IUnknown_iface,
                &IID_IAmdExtD3DDevice8, (void **)&extension);
        ok(hr == S_OK, "Create returned %#lx.\n", hr);
        if (FAILED(hr)) continue;
        device.fp8 = FALSE;
        hr = IAmdExtD3DDevice8_GetWaveMatrixProperties(extension, NULL, properties);
        ok(hr == E_INVALIDARG, "Null count returned %#lx.\n", hr);
        for (i = 0; i < 3; ++i)
        {
            count = i;
            hr = IAmdExtD3DDevice8_GetWaveMatrixProperties(extension, &count, NULL);
            ok(hr == S_OK && count == fp8, "Count query %u returned %#lx, %Iu.\n", i, hr, count);
            memset(properties, 0xa5, sizeof(properties));
            memcpy(saved, properties, sizeof(saved));
            count = i;
            hr = IAmdExtD3DDevice8_GetWaveMatrixProperties(extension, &count, properties);
            ok(hr == (fp8 && !i ? E_NOT_SUFFICIENT_BUFFER : S_OK), "Capacity %u returned %#lx.\n", i, hr);
            ok(count == fp8, "Reported %Iu entries, expected %u.\n", count, fp8);
            if (!fp8 || !i)
                ok(!memcmp(properties, saved, sizeof(saved)), "Unexpected buffer write.\n");
            else
            {
                ok(properties[0].mSize == 16 && properties[0].nSize == 16 && properties[0].kSize == 16,
                        "Unexpected matrix dimensions.\n");
                ok(properties[0].aType == AMD_EXT_WMMA_TYPE_FP8 && properties[0].bType == AMD_EXT_WMMA_TYPE_FP8 &&
                        properties[0].cType == AMD_EXT_WMMA_TYPE_FP32 && properties[0].resultType == AMD_EXT_WMMA_TYPE_FP32 &&
                        !properties[0].saturatingAccumulation, "Unexpected matrix types.\n");
                ok(!memcmp(properties + 1, saved + 1, sizeof(saved[1])), "Wrote past returned entry.\n");
            }
        }
        IAmdExtD3DDevice8_Release(extension);
    }
    IAmdExtD3DFactory_Release(factory);
    ok(device.ref == 1, "Leaked device reference %ld.\n", device.ref);
}

static void test_creation_errors(void)
{
    static const IID *iids[] = {&IID_IAmdExtD3DShaderIntrinsics, &IID_IAmdExtD3DDevice8};
    IAmdExtD3DFactory *factory;
    void *out;
    unsigned int i;
    HRESULT hr;

    hr = pAmdExtD3DCreateInterface(NULL, &IID_IAmdExtD3DFactory, NULL);
    ok(hr == E_INVALIDARG, "Null output returned %#lx.\n", hr);
    out = (void *)0xdeadbeef;
    hr = pAmdExtD3DCreateInterface(NULL, &IID_ID3D12Device, &out);
    ok(hr == E_NOINTERFACE && !out, "Unknown interface returned %#lx, %p.\n", hr, out);
    out = (void *)0xdeadbeef;
    hr = pAmdExtD3DCreateInterface(NULL, &IID_IAmdExtFfxApi, &out);
    ok(hr == E_INVALIDARG && !out, "Null FFX device returned %#lx, %p.\n", hr, out);
    out = (void *)0xdeadbeef;
    hr = pAmdExtD3DCreateInterface(NULL, &IID_IAmdExtAntiLagApi, &out);
    ok(hr == E_INVALIDARG && !out, "Null AntiLag device returned %#lx, %p.\n", hr, out);
    hr = pAmdExtD3DCreateInterface(NULL, &IID_IAmdExtD3DFactory, (void **)&factory);
    ok(hr == S_OK, "Factory with no device returned %#lx.\n", hr);
    if (FAILED(hr)) return;
    for (i = 0; i < ARRAY_SIZE(iids); ++i)
    {
        hr = IAmdExtD3DFactory_CreateInterface(factory, &device.IUnknown_iface, iids[i], NULL);
        ok(hr == E_INVALIDARG, "Null output returned %#lx.\n", hr);
        out = (void *)0xdeadbeef;
        hr = IAmdExtD3DFactory_CreateInterface(factory, NULL, iids[i], &out);
        ok(hr == E_INVALIDARG && !out, "Null device returned %#lx, %p.\n", hr, out);
    }
    out = (void *)0xdeadbeef;
    hr = IAmdExtD3DFactory_CreateInterface(factory, &device.IUnknown_iface, &IID_ID3D12Device, &out);
    ok(hr == E_NOINTERFACE && !out, "Unknown interface returned %#lx, %p.\n", hr, out);
    IAmdExtD3DFactory_Release(factory);
}

static void test_interfaces(void)
{
    static const IID *iids[] = {&IID_IAmdExtD3DFactory, &IID_IAmdExtFfxApi,
            &IID_IAmdExtD3DShaderIntrinsics, &IID_IAmdExtD3DDevice8};
    IAmdExtD3DFactory *factory;
    IUnknown *object, *identity, *self, *out;
    unsigned int i;
    ULONG ref;
    HRESULT hr;

    hr = pAmdExtD3DCreateInterface(NULL, &IID_IAmdExtD3DFactory, (void **)&factory);
    ok(hr == S_OK, "Factory returned %#lx.\n", hr);
    if (FAILED(hr)) return;
    for (i = 0; i < ARRAY_SIZE(iids); ++i)
    {
        if (i < 2)
            hr = pAmdExtD3DCreateInterface(&device.IUnknown_iface, iids[i], (void **)&object);
        else
            hr = IAmdExtD3DFactory_CreateInterface(factory, &device.IUnknown_iface, iids[i], (void **)&object);
        ok(hr == S_OK, "Interface %u returned %#lx.\n", i, hr);
        if (FAILED(hr)) continue;

        hr = IUnknown_QueryInterface(object, &IID_IUnknown, NULL);
        ok(hr == E_POINTER, "Null output returned %#lx.\n", hr);
        out = (void *)0xdeadbeef;
        hr = IUnknown_QueryInterface(object, &IID_ID3D12Device, (void **)&out);
        ok(hr == E_NOINTERFACE && !out, "Unsupported query returned %#lx, %p.\n", hr, out);
        hr = IUnknown_QueryInterface(object, &IID_IUnknown, (void **)&identity);
        ok(hr == S_OK && identity == object, "Identity returned %#lx, %p, expected %p.\n", hr, identity, object);
        if (SUCCEEDED(hr))
        {
            hr = IUnknown_QueryInterface(identity, iids[i], (void **)&self);
            ok(hr == S_OK && self == object, "Self query returned %#lx, %p.\n", hr, self);
            if (SUCCEEDED(hr)) IUnknown_Release(self);
            ref = IUnknown_Release(identity);
            ok(ref == 1, "Unbalanced reference count %lu.\n", ref);
        }
        ref = IUnknown_Release(object);
        ok(ref == (i == 0 ? 1 : 0), "Final reference count %lu.\n", ref);
    }
    IAmdExtD3DFactory_Release(factory);
    ok(device.ref == 1, "Leaked device reference %ld.\n", device.ref);
}

static void test_intrinsics(void)
{
    static const int groups[] =
    {
        -1, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, -1, -1, -1, 5, 5,
        7, 6, 9, 8, 10, 11, 12, 12, -1, -1, -1, -1, -1, -1, -1,
    };
    IAmdExtD3DShaderIntrinsics *intrinsics;
    IAmdExtD3DFactory *factory;
    HRESULT hr, expected;
    unsigned int opcode, caps;
    int group;

    hr = pAmdExtD3DCreateInterface(NULL, &IID_IAmdExtD3DFactory, (void **)&factory);
    ok(hr == S_OK, "Factory returned %#lx.\n", hr);
    if (FAILED(hr)) return;
    for (group = -1; group <= 12; ++group)
    for (caps = 0; caps < 4; ++caps)
    {
        device.intrinsics = group < 0 ? 0 : 1u << group;
        device.wmma = !!(caps & 1);
        device.fp8 = !!(caps & 2);
        hr = IAmdExtD3DFactory_CreateInterface(factory, &device.IUnknown_iface,
                &IID_IAmdExtD3DShaderIntrinsics, (void **)&intrinsics);
        ok(hr == S_OK, "Create returned %#lx.\n", hr);
        if (FAILED(hr)) continue;
        device.intrinsics = 0;
        device.wmma = device.fp8 = FALSE;
        for (opcode = 0; opcode <= 0x23; ++opcode)
        {
            expected = E_NOTIMPL;
            if (opcode < ARRAY_SIZE(groups) && group >= 0 && groups[opcode] == group)
                expected = S_OK;
            if (opcode == AmdExtD3DShaderIntrinsicsSupport_WaveMatrix && (caps & 1)) expected = S_OK;
            if (opcode == AmdExtD3DShaderIntrinsicsSupport_Float8Conversion && (caps & 2)) expected = S_OK;
            hr = IAmdExtD3DShaderIntrinsics_CheckSupport(intrinsics, opcode);
            ok(hr == expected, "Group %d, caps %u, opcode %#x returned %#lx, expected %#lx.\n",
                    group, caps, opcode, hr, expected);
        }
        hr = IAmdExtD3DShaderIntrinsics_CheckSupport(intrinsics, 0xdead);
        ok(hr == E_NOTIMPL, "Unknown opcode returned %#lx.\n", hr);
        IAmdExtD3DShaderIntrinsics_Release(intrinsics);
    }
    IAmdExtD3DFactory_Release(factory);
    ok(device.ref == 1, "Leaked device reference %ld.\n", device.ref);
}

static void test_disabled_provider(void)
{
    IAmdExtFfxApi *ffx;
    HMODULE provider;
    HRESULT hr;
    unsigned int data[32] = {0};

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

static WCHAR *get_environment(const WCHAR *name)
{
    DWORD size = GetEnvironmentVariableW(name, NULL, 0);
    WCHAR *value;

    if (!size && GetLastError() == ERROR_ENVVAR_NOT_FOUND) return NULL;
    value = calloc(size + 1, sizeof(WCHAR));
    GetEnvironmentVariableW(name, value, size);
    return value;
}

static void test_provider_selection(void)
{
    static const WCHAR *paths[] = {NULL, L"", L"/shared/amdxcffx64.dll"};
    static const WCHAR *old_flags[] = {NULL, L"0", L"1"};
    WCHAR *path = get_environment(L"WINE_FSR4_PROVIDER");
    WCHAR *flag = get_environment(L"FSR4_UPGRADE");
    WCHAR *proton_flag = get_environment(L"PROTON_FSR4_UPGRADE");
    unsigned int i, j;

    for (i = 0; i < ARRAY_SIZE(paths); ++i)
    {
        SetEnvironmentVariableW(L"WINE_FSR4_PROVIDER", paths[i]);
        for (j = 0; j < ARRAY_SIZE(old_flags); ++j)
        {
            SetEnvironmentVariableW(L"FSR4_UPGRADE", old_flags[j]);
            SetEnvironmentVariableW(L"PROTON_FSR4_UPGRADE", old_flags[j]);
            test_upgrade_probe();
            test_disabled_provider();
        }
    }
    SetEnvironmentVariableW(L"WINE_FSR4_PROVIDER", path);
    SetEnvironmentVariableW(L"FSR4_UPGRADE", flag);
    SetEnvironmentVariableW(L"PROTON_FSR4_UPGRADE", proton_flag);
    free(path);
    free(flag);
    free(proton_flag);
}

static void write_provider(const WCHAR *path)
{
    HRSRC resource = FindResourceA(NULL, "fsr_provider.dll", "TESTDLL");
    HANDLE file;
    DWORD size, written;

    ok(!!resource, "Provider resource is missing, error %lu.\n", GetLastError());
    if (!resource) return;
    size = SizeofResource(NULL, resource);
    file = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
    ok(file != INVALID_HANDLE_VALUE, "Cannot create %s, error %lu.\n", debugstr_w(path), GetLastError());
    if (file == INVALID_HANDLE_VALUE) return;
    ok(WriteFile(file, LockResource(LoadResource(NULL, resource)), size, &written, NULL) && written == size,
            "Cannot write provider, error %lu.\n", GetLastError());
    CloseHandle(file);
}

static void check_provider_module(HMODULE module, const WCHAR *expected)
{
    BY_HANDLE_FILE_INFORMATION actual_info, expected_info;
    DWORD (WINAPI *marker)(void);
    WCHAR filename[1024], moved[1030];
    HANDLE actual, wanted;
    HMODULE duplicate;
    DWORD length, actual_error, wanted_error;
    BOOL actual_ok, wanted_ok;

    if (!module) return;
    marker = (void *)GetProcAddress(module, "provider_marker");
    ok(!!marker, "Unexpected provider module.\n");
    if (marker) ok(marker() == 0x46535234, "Unexpected provider marker.\n");
    length = GetModuleFileNameW(module, filename, ARRAY_SIZE(filename));
    ok(length && length < ARRAY_SIZE(filename), "Cannot query provider module filename, length %lu, error %lu.\n",
            length, GetLastError());
    ok(GetModuleHandleW(filename) == module, "Cannot find provider by its reported filename %s.\n",
            debugstr_w(filename));
    duplicate = LoadLibraryW(filename);
    ok(duplicate == module, "Loading the provider twice returned different modules %p and %p.\n",
            module, duplicate);
    if (duplicate) FreeLibrary(duplicate);
    actual = CreateFileW(filename, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            NULL, OPEN_EXISTING, 0, NULL);
    actual_error = GetLastError();
    wanted = CreateFileW(expected, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            NULL, OPEN_EXISTING, 0, NULL);
    wanted_error = GetLastError();
    ok(actual != INVALID_HANDLE_VALUE && wanted != INVALID_HANDLE_VALUE,
            "Cannot compare provider files: loaded %s, handle %p, error %lu; selected %s, handle %p, error %lu.\n",
            debugstr_w(filename), actual, actual_error, debugstr_w(expected), wanted, wanted_error);
    if (actual != INVALID_HANDLE_VALUE && wanted != INVALID_HANDLE_VALUE)
    {
        actual_ok = GetFileInformationByHandle(actual, &actual_info);
        wanted_ok = GetFileInformationByHandle(wanted, &expected_info);
        ok(actual_ok, "Cannot query loaded provider.\n");
        ok(wanted_ok, "Cannot query selected provider.\n");
        if (actual_ok && wanted_ok) ok(actual_info.dwVolumeSerialNumber == expected_info.dwVolumeSerialNumber &&
                actual_info.nFileIndexHigh == expected_info.nFileIndexHigh &&
                actual_info.nFileIndexLow == expected_info.nFileIndexLow,
                "Loaded %s instead of %s.\n", debugstr_w(filename), debugstr_w(expected));
    }
    if (actual != INVALID_HANDLE_VALUE) CloseHandle(actual);
    if (wanted != INVALID_HANDLE_VALUE) CloseHandle(wanted);
    swprintf(moved, ARRAY_SIZE(moved), L"%s.moved", filename);
    actual_ok = MoveFileW(filename, moved);
    ok(actual_ok, "Cannot rename loaded provider %s, error %lu.\n", debugstr_w(filename), GetLastError());
    if (actual_ok)
    {
        ok(GetModuleHandleW(filename) == module, "Lost the loaded provider after renaming its file.\n");
        duplicate = LoadLibraryW(filename);
        ok(duplicate == module, "Reloading a renamed provider lost its existing module.\n");
        if (duplicate) FreeLibrary(duplicate);
        ok(MoveFileW(moved, filename), "Cannot restore the provider filename, error %lu.\n", GetLastError());
    }
    FreeLibrary(module);
}

static void test_shared_provider(void)
{
    static const WCHAR *requests[] = {L"amdxcffx64", L"AMDXCFFX64.DLL", L"C:\\missing\\amdxcffx64.dll"};
    char *(CDECL *get_unix_name)(const WCHAR *);
    NTSTATUS (WINAPI *load_dll)(const WCHAR *, DWORD *, const UNICODE_STRING *, HMODULE *);
    WCHAR directory[MAX_PATH], temporary[MAX_PATH], shared[MAX_PATH], fallback[MAX_PATH];
    WCHAR *previous, *unix_name, *long_name, *dos_name;
    char *unix_path;
    UNICODE_STRING name;
    HMODULE module;
    NTSTATUS status;
    unsigned int i;
    DWORD size;
    int len;

    get_unix_name = (void *)GetProcAddress(GetModuleHandleA("kernel32.dll"), "wine_get_unix_file_name");
    if (!get_unix_name)
    {
        win_skip("Shared Unix providers are only available on Wine.\n");
        return;
    }
    load_dll = (void *)GetProcAddress(GetModuleHandleA("ntdll.dll"), "LdrLoadDll");
    size = GetEnvironmentVariableW(L"WINETEST_FSR_SHARED_DIR", temporary, ARRAY_SIZE(temporary));
    ok(size < ARRAY_SIZE(temporary), "Shared test directory is too long.\n");
    if (size >= ARRAY_SIZE(temporary)) return;
    if (!size) GetTempPathW(ARRAY_SIZE(temporary), temporary);
    GetTempFileNameW(temporary, L"fsr", 0, directory);
    DeleteFileW(directory);
    ok(CreateDirectoryW(directory, NULL), "Cannot create provider directory.\n");
    swprintf(shared, ARRAY_SIZE(shared), L"%s\\shared \x4e2d\x6587", directory);
    ok(CreateDirectoryW(shared, NULL), "Cannot create shared directory.\n");
    wcscat(shared, L"\\amdxcffx64.dll");
    swprintf(fallback, ARRAY_SIZE(fallback), L"%s\\amdxcffx64.dll", directory);
    write_provider(shared);
    write_provider(fallback);
    unix_path = get_unix_name(shared);
    ok(!!unix_path, "Cannot resolve provider Unix path.\n");
    if (!unix_path) goto cleanup;
    len = MultiByteToWideChar(CP_UNIXCP, 0, unix_path, -1, NULL, 0);
    unix_name = malloc(len * sizeof(WCHAR));
    MultiByteToWideChar(CP_UNIXCP, 0, unix_path, -1, unix_name, len);
    previous = get_environment(L"WINE_FSR4_PROVIDER");
    SetEnvironmentVariableW(L"WINE_FSR4_PROVIDER", unix_name);
    for (i = 0; i < ARRAY_SIZE(requests); ++i)
    {
        module = LoadLibraryW(requests[i]);
        ok(!!module, "LoadLibrary(%s) failed, error %lu.\n", debugstr_w(requests[i]), GetLastError());
        check_provider_module(module, shared);
        name.Buffer = (WCHAR *)requests[i];
        name.Length = wcslen(name.Buffer) * sizeof(WCHAR);
        name.MaximumLength = name.Length + sizeof(WCHAR);
        module = NULL;
        status = load_dll(NULL, NULL, &name, &module);
        ok(!status, "LdrLoadDll(%s) returned %#lx.\n", debugstr_w(requests[i]), status);
        check_provider_module(module, shared);
    }
    module = LoadLibraryW(L"not_amdxcffx64.dll");
    ok(!module, "Unrelated DLL name was redirected.\n");
    if (module) FreeLibrary(module);
    SetEnvironmentVariableW(L"WINE_FSR4_PROVIDER", L"relative/provider.dll");
    module = LoadLibraryW(fallback);
    ok(!module, "Invalid explicit provider fell back to another DLL.\n");
    if (module) FreeLibrary(module);
    name.Buffer = fallback;
    name.Length = wcslen(fallback) * sizeof(WCHAR);
    name.MaximumLength = name.Length + sizeof(WCHAR);
    module = NULL;
    status = load_dll(NULL, NULL, &name, &module);
    ok(status != STATUS_SUCCESS && !module, "Invalid explicit LdrLoadDll fell back, status %#lx.\n", status);
    if (module) FreeLibrary(module);
    long_name = malloc(1536 * sizeof(WCHAR));
    long_name[0] = '/';
    for (i = 1; i < 1535; ++i) long_name[i] = 'x';
    long_name[i] = 0;
    SetEnvironmentVariableW(L"WINE_FSR4_PROVIDER", long_name);
    free(long_name);
    module = LoadLibraryW(fallback);
    ok(!module, "Long missing provider fell back to another DLL.\n");
    if (module) FreeLibrary(module);
    SetEnvironmentVariableW(L"WINE_FSR4_PROVIDER", L"/nonexistent/proton-fsr-provider.dll");
    module = LoadLibraryW(fallback);
    ok(!module, "Missing explicit provider fell back to another DLL.\n");
    if (module) FreeLibrary(module);
    SetEnvironmentVariableW(L"WINE_FSR4_PROVIDER", L"");
    module = LoadLibraryW(fallback);
    ok(!!module, "Empty setting did not preserve ordinary DLL loading.\n");
    check_provider_module(module, fallback);
    SetEnvironmentVariableW(L"WINE_FSR4_PROVIDER", NULL);
    module = LoadLibraryW(fallback);
    ok(!!module, "Unset setting did not preserve ordinary DLL loading.\n");
    check_provider_module(module, fallback);
    dos_name = malloc((wcslen(unix_name) + ARRAY_SIZE(L"\\\\?\\unix")) * sizeof(WCHAR));
    wcscpy(dos_name, L"\\\\?\\unix");
    wcscat(dos_name, unix_name);
    for (i = 0; dos_name[i]; ++i)
        if (dos_name[i] == '/') dos_name[i] = '\\';
    module = LoadLibraryW(dos_name);
    ok(!!module, "Direct Unix namespace load failed, error %lu.\n", GetLastError());
    check_provider_module(module, shared);
    free(dos_name);
    SetEnvironmentVariableW(L"WINE_FSR4_PROVIDER", previous);
    free(previous);
    free(unix_name);
    HeapFree(GetProcessHeap(), 0, unix_path);
cleanup:
    DeleteFileW(fallback);
    DeleteFileW(shared);
    *wcsrchr(shared, '\\') = 0;
    RemoveDirectoryW(shared);
    RemoveDirectoryW(directory);
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
    test_provider_selection();
    test_creation_errors();
    test_interfaces();
    test_intrinsics();
    test_wave_matrix();
    test_info_stubs();
    test_shared_provider();
    FreeLibrary(module);
}
