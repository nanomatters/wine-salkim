/*
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
 *
 */

/* Shared upscaler loader tests. */

#include <stdarg.h>
#include <stdio.h>
#include "windows.h"
#include "winternl.h"
#include "winver.h"
#include "wine/test.h"

static WCHAR game[MAX_PATH], shared[MAX_PATH], unix_shared[MAX_PATH];
static unsigned char version_data[2048];
static unsigned int version_pos;
static const WCHAR *combined = L"amd_fidelityfx_dx12.dll";
static const WCHAR *split[] = {
    L"amd_fidelityfx_loader_dx12.dll", L"amd_fidelityfx_upscaler_dx12.dll",
    L"amd_fidelityfx_framegeneration_dx12.dll", L"amd_fidelityfx_denoiser_dx12.dll",
    L"amd_fidelityfx_radiancecache_dx12.dll",
};
static const WCHAR *products[] = {
    L"AMD FidelityFX", L"AMD FidelityFX Upscaler", L"AMD FidelityFX Frame Generation",
    L"AMD FidelityFX Ray Regeneration", L"AMD FidelityFX Radiance Cache",
};
static const DWORD split_versions[] = { 0x20003, 0x40001, 0x40000, 0x10002, 0x00009 };

static unsigned int begin_block( const WCHAR *key, WORD length, WORD type )
{
    unsigned int start;
    WORD *words;
    version_pos = (version_pos + 3) & ~3;
    start = version_pos;
    words = (WORD *)(version_data + version_pos);
    words[0] = 0;
    words[1] = length;
    words[2] = type;
    wcscpy( (WCHAR *)(words + 3), key );
    version_pos += 6 + (wcslen(key) + 1) * sizeof(WCHAR);
    version_pos = (version_pos + 3) & ~3;
    return start;
}

static void end_block( unsigned int start )
{
    *(WORD *)(version_data + start) = version_pos - start;
}

static void version_string( const WCHAR *key, const WCHAR *value )
{
    unsigned int start = begin_block( key, wcslen(value) + 1, 1 );
    wcscpy( (WCHAR *)(version_data + version_pos), value );
    version_pos += (wcslen(value) + 1) * sizeof(WCHAR);
    end_block( start );
}

static void make_dll( const WCHAR *directory, const WCHAR *name, DWORD ms, DWORD ls,
                      const WCHAR *company, const WCHAR *product, DWORD flags, BOOL malformed )
{
    WCHAR path[MAX_PATH];
    HRSRC resource = FindResourceA( NULL, "testdll.dll", "TESTDLL" );
    VS_FIXEDFILEINFO fixed = { VS_FFI_SIGNATURE, VS_FFI_STRUCVERSION };
    unsigned int root, strings, table;
    DWORD size, written;
    HANDLE file, update;

    swprintf( path, ARRAY_SIZE(path), L"%s\\%s", directory, name );
    ok(!!resource, "Missing fixture DLL.\n");
    size = SizeofResource( NULL, resource );
    file = CreateFileW( path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL );
    ok(file != INVALID_HANDLE_VALUE, "Cannot create %s: %lu\n", debugstr_w(path), GetLastError());
    if (file == INVALID_HANDLE_VALUE) return;
    ok(WriteFile( file, LockResource(LoadResource(NULL, resource)), size, &written, NULL ) && written == size,
       "Cannot write fixture.\n");
    CloseHandle( file );
    if (!company) return;
    version_pos = 0;
    memset( version_data, 0, sizeof(version_data) );
    root = begin_block( L"VS_VERSION_INFO", sizeof(fixed), 0 );
    fixed.dwFileVersionMS = ms;
    fixed.dwFileVersionLS = ls;
    fixed.dwProductVersionMS = ms;
    fixed.dwProductVersionLS = ls;
    fixed.dwFileFlags = flags;
    fixed.dwFileType = !wcscmp(company, L"NVIDIA") || !wcscmp(company, L"Intel Corporation") ? VFT_UNKNOWN : VFT_DLL;
    memcpy( version_data + version_pos, &fixed, sizeof(fixed) );
    version_pos += sizeof(fixed);
    strings = begin_block( L"StringFileInfo", 0, 1 );
    table = begin_block( !wcscmp(company, L"NVIDIA") ? L"040904E4" : L"040904b0", 0, 1 );
    version_string( L"CompanyName", company );
    version_string( L"ProductName", product );
    end_block( table );
    end_block( strings );
    end_block( root );
    if (malformed) *(WORD *)version_data = 0xffff;
    update = BeginUpdateResourceW( path, FALSE );
    ok(!!update, "BeginUpdateResource failed: %lu\n", GetLastError());
    ok(UpdateResourceW( update, (LPCWSTR)16, (LPCWSTR)1, 0x409, version_data, version_pos ),
       "UpdateResource failed: %lu\n", GetLastError());
    ok(EndUpdateResourceW( update, FALSE ), "EndUpdateResource failed: %lu\n", GetLastError());
}

static void break_import( const WCHAR *directory, const WCHAR *name )
{
    WCHAR path[MAX_PATH];
    unsigned char *data;
    HANDLE file;
    DWORD size, read, i;
    swprintf( path, ARRAY_SIZE(path), L"%s\\%s", directory, name );
    file = CreateFileW( path, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL );
    size = GetFileSize( file, NULL );
    data = malloc(size);
    ReadFile( file, data, size, &read, NULL );
    for (i = 0; i + sizeof("KERNEL32.dll") <= size; ++i)
        if (!memcmp( data + i, "KERNEL32.dll", sizeof("KERNEL32.dll") ))
        {
            memcpy( data + i, "missing_.dll", sizeof("missing_.dll") );
            break;
        }
    ok(i + sizeof("KERNEL32.dll") <= size, "Cannot find fixture import.\n");
    SetFilePointer( file, 0, NULL, FILE_BEGIN );
    WriteFile( file, data, size, &read, NULL );
    CloseHandle( file );
    free(data);
}

static HMODULE load_dll( const WCHAR *directory, const WCHAR *name, const WCHAR *expected, DWORD flags )
{
    WCHAR path[MAX_PATH], actual[MAX_PATH], wanted[MAX_PATH];
    HMODULE module;
    BY_HANDLE_FILE_INFORMATION a, b;
    HANDLE file, other;

    swprintf( path, ARRAY_SIZE(path), L"%s\\%s", directory, name );
    module = LoadLibraryExW( path, NULL, flags );
    ok(!!module, "LoadLibrary(%s) failed: %lu\n", debugstr_w(path), GetLastError());
    if (!module) return NULL;
    GetModuleFileNameW( module, actual, ARRAY_SIZE(actual) );
    swprintf( wanted, ARRAY_SIZE(wanted), L"%s\\%s", expected, name );
    file = CreateFileW( actual, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, 0, NULL );
    other = CreateFileW( wanted, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, 0, NULL );
    ok(GetFileInformationByHandle( file, &a ) && GetFileInformationByHandle( other, &b ), "Cannot inspect modules.\n");
    ok(a.dwVolumeSerialNumber == b.dwVolumeSerialNumber && a.nFileIndexHigh == b.nFileIndexHigh &&
       a.nFileIndexLow == b.nFileIndexLow, "Loaded %s instead of %s\n", debugstr_w(actual), debugstr_w(wanted));
    CloseHandle( file );
    CloseHandle( other );
    return module;
}

static void check_combined(void)
{
    static const struct { DWORD old_ms, old_ls, new_ms, new_ls; BOOL upgrade; } cases[] = {
        {0x10000, 36329, 0x10000, 0x1a162, TRUE},
        {0x10000, 0x1a162, 0x10000, 0x1a162, FALSE},
        {0x10000, 0x2ffff, 0x10000, 0x1a162, FALSE},
        {0x10000, 0x1ffff, 0x10000, 0x20001, TRUE},
        {0x10000, 1, 0x20000, 1, FALSE},
    };
    const WCHAR *company = L"Advanced Micro Devices, Inc.";
    HMODULE module, again;
    WCHAR path[MAX_PATH], old_path[MAX_PATH], legacy[96];
    UNICODE_STRING name;
    NTSTATUS (WINAPI *ldr_load_dll)( const WCHAR *, ULONG *, const UNICODE_STRING *, HMODULE * );
    unsigned int i;

    for (i = 0; i < ARRAY_SIZE(cases); ++i)
    {
        winetest_push_context("case %u", i);
        make_dll( game, combined, cases[i].old_ms, cases[i].old_ls, company, products[0], 0, FALSE );
        make_dll( shared, combined, cases[i].new_ms, cases[i].new_ls, company, products[0], 0, FALSE );
        module = load_dll( game, combined, cases[i].upgrade ? shared : game, 0 );
        if (module) FreeLibrary( module );
        winetest_pop_context();
    }
    make_dll( shared, combined, 0x10000, 2, company, products[0], 0, FALSE );
    for (i = 0; i < 5; ++i)
    {
        make_dll( game, combined, 0x10000, 1, i == 0 ? NULL : i == 1 ? L"Custom build" : company,
                  i == 2 ? L"Custom library" : products[0], i == 3 ? VS_FF_DEBUG : 0, i == 4 );
        module = load_dll( game, combined, game, 0 );
        if (module) FreeLibrary( module );
    }
    make_dll( game, combined, 0x10000, 1, company, products[0], 0, FALSE );
    SetEnvironmentVariableW( L"WINE_OPTISCALER_NAME", L"dxgi.dll" );
    module = load_dll( game, combined, game, 0 );
    if (module) FreeLibrary( module );
    SetEnvironmentVariableW( L"WINE_OPTISCALER_NAME", NULL );
    SetEnvironmentVariableW( L"WINE_FFX_PROVIDER", L"relative" );
    module = load_dll( game, combined, game, 0 );
    if (module) FreeLibrary( module );
    SetEnvironmentVariableW( L"WINE_FFX_PROVIDER", L"/no-such-ffx-provider" );
    module = load_dll( game, combined, game, 0 );
    if (module) FreeLibrary( module );
    SetEnvironmentVariableW( L"WINE_FFX_PROVIDER", NULL );
    SetEnvironmentVariableW( L"WINE_UPSCALER_REPLACE", L"ffx3,ffx4" );
    module = load_dll( game, combined, game, 0 );
    SetEnvironmentVariableW( L"WINE_UPSCALER_REPLACE", NULL );
    SetEnvironmentVariableW( L"WINE_FFX_PROVIDER", unix_shared );
    again = load_dll( game, combined, game, 0 );
    ok(again == module, "Changed an already loaded module.\n");
    if (again) FreeLibrary( again );
    if (module) FreeLibrary( module );
    for (i = 0; i < ARRAY_SIZE(legacy) - 1; ++i) legacy[i] = 'x';
    legacy[ARRAY_SIZE(legacy) - 1] = 0;
    SetEnvironmentVariableW( L"WINE_UPSCALER_REPLACE", legacy );
    module = load_dll( game, combined, shared, 0 );
    if (module) FreeLibrary( module );
    SetEnvironmentVariableW( L"WINE_UPSCALER_REPLACE", NULL );
    module = load_dll( game, combined, game, DONT_RESOLVE_DLL_REFERENCES );
    if (module) FreeLibrary( module );
    module = load_dll( game, combined, shared, 0 );
    again = load_dll( game, combined, shared, 0 );
    ok(again == module, "Created duplicate replacement modules.\n");
    if (again) FreeLibrary( again );
    if (module) FreeLibrary( module );
    ldr_load_dll = (void *)GetProcAddress( GetModuleHandleW(L"ntdll.dll"), "LdrLoadDll" );
    swprintf( path, ARRAY_SIZE(path), L"%s\\%s", game, combined );
    name.Buffer = path;
    name.Length = wcslen(path) * sizeof(WCHAR);
    name.MaximumLength = name.Length + sizeof(WCHAR);
    ok(!ldr_load_dll( NULL, NULL, &name, &module ), "Direct LdrLoadDll failed.\n");
    again = load_dll( game, combined, shared, 0 );
    ok(again == module, "Direct loader used a different DLL.\n");
    if (again) FreeLibrary( again );
    if (module) FreeLibrary( module );
    GetCurrentDirectoryW( ARRAY_SIZE(old_path), old_path );
    SetCurrentDirectoryW( game );
    module = LoadLibraryW( L"AMD_FIDELITYFX_DX12.DLL" );
    again = load_dll( game, combined, shared, 0 );
    ok(module && module == again, "Bare uppercase request was not redirected.\n");
    if (again) FreeLibrary( again );
    if (module) FreeLibrary( module );
    SetCurrentDirectoryW( old_path );
    /* A loaded replacement cannot override a later, newer absolute request. */
    module = load_dll( game, combined, shared, 0 );
    make_dll( game, combined, 0x10000, 3, company, products[0], 0, FALSE );
    again = load_dll( game, combined, game, 0 );
    if (again) FreeLibrary( again );
    if (module) FreeLibrary( module );
    make_dll( game, combined, 0x10000, 1, company, products[0], 0, FALSE );
    break_import( shared, combined );
    module = load_dll( game, combined, game, 0 );
    if (module) FreeLibrary( module );
    swprintf( path, ARRAY_SIZE(path), L"%s\\%s", shared, combined );
    DeleteFileW( path );
    module = load_dll( game, combined, game, 0 );
    if (module) FreeLibrary( module );

}

static void check_split(void)
{
    const WCHAR *company = L"Advanced Micro Devices, Inc.";
    HMODULE loader, effect;
    WCHAR path[MAX_PATH];
    unsigned int i;
    for (i = 0; i < ARRAY_SIZE(split); ++i)
    {
        make_dll( game, split[i], split_versions[i], 1, company, products[i], 0, FALSE );
        make_dll( shared, split[i], split_versions[i], 2, company, products[i], 0, FALSE );
    }
    /* A split backend is never independently upgraded. */
    effect = load_dll( game, split[1], game, 0 );
    loader = load_dll( game, split[0], game, 0 );
    if (loader) FreeLibrary( loader );
    if (effect) FreeLibrary( effect );
    loader = load_dll( game, split[0], shared, 0 );
    effect = load_dll( game, split[1], shared, 0 );
    if (effect) FreeLibrary( effect );
    if (loader) FreeLibrary( loader );
    make_dll( game, split[1], split_versions[1], 2, company, products[1], 0, FALSE );
    loader = load_dll( game, split[0], shared, 0 );
    effect = load_dll( game, split[1], shared, 0 );
    if (effect) FreeLibrary( effect );
    if (loader) FreeLibrary( loader );
    make_dll( game, split[1], split_versions[1], 3, company, products[1], 0, FALSE );
    loader = load_dll( game, split[0], game, 0 );
    effect = load_dll( game, split[1], game, 0 );
    if (effect) FreeLibrary( effect );
    if (loader) FreeLibrary( loader );
    make_dll( game, split[1], split_versions[1], 1, company, products[1], 0, FALSE );
    swprintf( path, ARRAY_SIZE(path), L"%s\\%s", shared, split[2] );
    DeleteFileW( path );
    loader = load_dll( game, split[0], game, 0 );
    if (loader) FreeLibrary( loader );
}

static void check_dlss(void)
{
    static const WCHAR *names[] = { L"nvngx_dlss.dll", L"nvngx_dlssd.dll", L"nvngx_dlssg.dll" };
    static const WCHAR *products[] = { L"NVIDIA Deep Learning SuperSampling",
                                      L"NVIDIA DLSS Ray Reconstruction", L"NVIDIA DLSS-G" };
    static const DWORD versions[] = { 0x20001, 0x30005, 0x10000 };
    const WCHAR *company = L"NVIDIA";
    WCHAR path[MAX_PATH], parent[MAX_PATH], driver[MAX_PATH], unix_driver[MAX_PATH], old_cwd[MAX_PATH];
    char * (CDECL *get_unix_name)(const WCHAR *);
    char *unix_name;
    HMODULE module, again;
    VS_FIXEDFILEINFO *fixed;
    HANDLE update;
    unsigned int i;

    SetEnvironmentVariableW( L"WINE_DLSS_PROVIDER", unix_shared );
    for (i = 0; i < ARRAY_SIZE(names); ++i)
    {
        winetest_push_context("DLSS %u", i);
        make_dll( game, names[i], versions[i], 1, company, products[i], 0, FALSE );
        make_dll( shared, names[i], 310 << 16 | 9, 16 << 16,
                  company, i == 2 ? L"NVIDIA DLSS-G MFGLW" : products[i], 0, FALSE );
        module = load_dll( game, names[i], shared, 0 );
        again = load_dll( game, names[i], shared, 0 );
        ok(module == again, "Created duplicate DLSS replacements.\n");
        if (again) FreeLibrary( again );
        if (module) FreeLibrary( module );
        make_dll( game, names[i], 310 << 16 | 9, 16 << 16, company, products[i], 0, FALSE );
        module = load_dll( game, names[i], game, 0 );
        if (module) FreeLibrary( module );
        make_dll( game, names[i], 310 << 16 | 9, 17 << 16, company, products[i], 0, FALSE );
        module = load_dll( game, names[i], game, 0 );
        if (module) FreeLibrary( module );
        for (unsigned int j = 0; j < 5; ++j)
        {
            make_dll( game, names[i], versions[i], 1,
                      j == 0 ? NULL : j == 1 ? L"OptiScaler" : company,
                      j == 2 ? L"NGX" : products[i], j == 3 ? VS_FF_DEBUG : 0, j == 4 );
            module = load_dll( game, names[i], game, 0 );
            if (module) FreeLibrary( module );
        }
        make_dll( game, names[i], versions[i], 1, company, products[i], 0, FALSE );
        module = load_dll( game, names[i], game, DONT_RESOLVE_DLL_REFERENCES );
        if (module) FreeLibrary( module );
        SetEnvironmentVariableW( L"WINE_OPTISCALER_NAME", L"dxgi.dll" );
        module = load_dll( game, names[i], game, 0 );
        if (module) FreeLibrary( module );
        SetEnvironmentVariableW( L"WINE_OPTISCALER_NAME", NULL );
        SetEnvironmentVariableW( L"WINE_DLSS_PROVIDER", NULL );
        SetEnvironmentVariableW( L"WINE_UPSCALER_REPLACE", L"dlss" );
        module = load_dll( game, names[i], game, 0 );
        SetEnvironmentVariableW( L"WINE_UPSCALER_REPLACE", NULL );
        SetEnvironmentVariableW( L"WINE_DLSS_PROVIDER", unix_shared );
        again = load_dll( game, names[i], game, 0 );
        ok(module == again, "Replaced a loaded original.\n");
        if (again) FreeLibrary( again );
        if (module) FreeLibrary( module );
        winetest_pop_context();
    }
    /* DLSS 1.x SR is a different integration. FG 1.x above is supported. */
    make_dll( game, names[0], 0x10000, 1, company, products[0], 0, FALSE );
    module = load_dll( game, names[0], game, 0 );
    if (module) FreeLibrary( module );
    make_dll( game, names[0], 0x20001, 1, company, products[0], 0, FALSE );
    /* NVIDIA's ProductVersion controls selection even if FileVersion differs. */
    fixed = (void *)(version_data + ((6 + sizeof(L"VS_VERSION_INFO") + 3) & ~3));
    fixed->dwFileVersionMS = 311 << 16;
    swprintf( path, ARRAY_SIZE(path), L"%s\\%s", game, names[0] );
    update = BeginUpdateResourceW( path, FALSE );
    ok(!!update, "Cannot update DLSS file version.\n");
    ok(UpdateResourceW( update, (LPCWSTR)16, (LPCWSTR)1, 0x409, version_data, version_pos ),
       "Cannot write DLSS file version.\n");
    ok(EndUpdateResourceW( update, FALSE ), "Cannot finish DLSS file version.\n");
    module = load_dll( game, names[0], shared, 0 );
    if (module) FreeLibrary( module );
    GetCurrentDirectoryW( ARRAY_SIZE(old_cwd), old_cwd );
    SetCurrentDirectoryW( game );
    module = LoadLibraryW( L"NVNGX_DLSS.DLL" );
    again = load_dll( game, names[0], shared, 0 );
    ok(module && module == again, "Uppercase DLSS request was not redirected.\n");
    if (again) FreeLibrary( again );
    if (module) FreeLibrary( module );
    SetCurrentDirectoryW( old_cwd );
    /* A newer absolute request must not reuse an older shared module. */
    module = load_dll( game, names[0], shared, 0 );
    make_dll( game, names[0], 311 << 16, 0, company, products[0], 0, FALSE );
    again = load_dll( game, names[0], game, 0 );
    if (again) FreeLibrary( again );
    if (module) FreeLibrary( module );
    make_dll( game, names[0], 0x20001, 1, company, products[0], 0, FALSE );
    make_dll( shared, names[0], 310 << 16, 1, company, L"NGX", 0, FALSE );
    module = load_dll( game, names[0], game, 0 );
    if (module) FreeLibrary( module );
    SetEnvironmentVariableW( L"WINE_DLSS_PROVIDER", L"/missing-dlss-provider" );
    module = load_dll( game, names[0], game, 0 );
    if (module) FreeLibrary( module );
    SetEnvironmentVariableW( L"WINE_DLSS_PROVIDER", L"relative" );
    module = load_dll( game, names[1], game, 0 );
    if (module) FreeLibrary( module );
    SetEnvironmentVariableW( L"WINE_DLSS_PROVIDER", unix_shared );
    /* Driver paths must work with Windows and Unix separators and mixed case. */
    swprintf( parent, ARRAY_SIZE(parent), L"%s\\NVIDIA", game );
    swprintf( driver, ARRAY_SIZE(driver), L"%s\\WiNe", parent );
    CreateDirectoryW( parent, NULL );
    CreateDirectoryW( driver, NULL );
    for (i = 0; i < ARRAY_SIZE(names); ++i)
    {
        make_dll( shared, names[i], 310 << 16, 1, company, products[i], 0, FALSE );
        make_dll( driver, names[i], versions[i], 1, company, products[i], 0, FALSE );
        module = load_dll( driver, names[i], driver, 0 );
        if (module) FreeLibrary( module );
        swprintf( path, ARRAY_SIZE(path), L"%s/%s", driver, names[i] );
        for (WCHAR *p = path; *p; ++p) if (*p == '\\') *p = '/';
        module = LoadLibraryW( path );
        again = load_dll( driver, names[i], driver, 0 );
        ok(module && module == again, "Slash-separated driver path was redirected.\n");
        if (again) FreeLibrary( again );
        if (module) FreeLibrary( module );
        swprintf( path, ARRAY_SIZE(path), L"%s\\%s", driver, names[i] );
        DeleteFileW( path );
    }
    RemoveDirectoryW( driver );
    RemoveDirectoryW( parent );
    /* Also honour an explicit NVIDIA_WINE_DLL_DIR outside the usual suffix. */
    swprintf( driver, ARRAY_SIZE(driver), L"%s\\custom-driver", game );
    CreateDirectoryW( driver, NULL );
    get_unix_name = (void *)GetProcAddress( GetModuleHandleW(L"kernel32.dll"), "wine_get_unix_file_name" );
    unix_name = get_unix_name( driver );
    MultiByteToWideChar( CP_UNIXCP, 0, unix_name, -1, unix_driver, ARRAY_SIZE(unix_driver) );
    HeapFree( GetProcessHeap(), 0, unix_name );
    SetEnvironmentVariableW( L"NVIDIA_WINE_DLL_DIR", unix_driver );
    make_dll( driver, names[2], 0x10000, 1, company, products[2], 0, FALSE );
    module = load_dll( driver, names[2], driver, 0 );
    if (module) FreeLibrary( module );
    SetEnvironmentVariableW( L"NVIDIA_WINE_DLL_DIR", NULL );
    swprintf( path, ARRAY_SIZE(path), L"%s\\%s", driver, names[2] );
    DeleteFileW( path );
    RemoveDirectoryW( driver );
    /* Wine retains failed import mappings, so run this after the reuse checks. */
    make_dll( shared, names[0], 310 << 16, 1, company, products[0], 0, FALSE );
    break_import( shared, names[0] );
    module = load_dll( game, names[0], game, 0 );
    if (module) FreeLibrary( module );
    SetEnvironmentVariableW( L"WINE_DLSS_PROVIDER", NULL );
    for (i = 0; i < ARRAY_SIZE(names); ++i)
    {
        swprintf( path, ARRAY_SIZE(path), L"%s\\%s", game, names[i] );
        DeleteFileW( path );
        swprintf( path, ARRAY_SIZE(path), L"%s\\%s", shared, names[i] );
        DeleteFileW( path );
    }
}

static const struct
{
    const WCHAR *name, *product;
    DWORD old_ms, new_ms, new_ls;
} xess[] =
{
    { L"libxess.dll",      L"XeSS SDK",                  0x10003, 0x20000, 0x20044 },
    { L"libxess_dx11.dll", L"XeSS SDK",                  0x20000, 0x20000, 0x20044 },
    { L"libxess_fg.dll",   L"XeSS Frame Generation SDK", 0x10002, 0x10003, 0x1004e },
    { L"libxell.dll",      L"XeLL SDK",                  0x10002, 0x10003, 0x2000a },
};

static void make_xess_set(void)
{
    unsigned int i;
    for (i = 0; i < ARRAY_SIZE(xess); ++i)
    {
        make_dll( game, xess[i].name, xess[i].old_ms, 1, L"Intel Corporation", xess[i].product, 0, FALSE );
        make_dll( shared, xess[i].name, xess[i].new_ms, xess[i].new_ls,
                  L"Intel Corporation", xess[i].product, 0, FALSE );
    }
}

static void check_xess(void)
{
    const WCHAR *company = L"Intel Corporation";
    HMODULE module, other;
    WCHAR path[MAX_PATH], foreign[MAX_PATH];
    unsigned int i;

    SetEnvironmentVariableW( L"WINE_XESS_PROVIDER", unix_shared );
    make_xess_set();
    for (i = 0; i < ARRAY_SIZE(xess); ++i)
    {
        winetest_push_context("XeSS %u", i);
        module = load_dll( game, xess[i].name, shared, 0 );
        other = load_dll( game, xess[i].name, shared, 0 );
        ok(module == other, "Created a duplicate XeSS replacement.\n");
        if (other) FreeLibrary( other );
        if (module) FreeLibrary( module );
        make_dll( game, xess[i].name, xess[i].new_ms, xess[i].new_ls, company, xess[i].product, 0, FALSE );
        module = load_dll( game, xess[i].name, game, 0 );
        if (module) FreeLibrary( module );
        make_dll( game, xess[i].name, xess[i].new_ms, xess[i].new_ls + 1, company, xess[i].product, 0, FALSE );
        module = load_dll( game, xess[i].name, game, 0 );
        if (module) FreeLibrary( module );
        make_dll( game, xess[i].name, xess[i].old_ms, 1, company, xess[i].product, 0, FALSE );
        module = load_dll( game, xess[i].name, game, DONT_RESOLVE_DLL_REFERENCES );
        if (module) FreeLibrary( module );
        SetEnvironmentVariableW( L"WINE_OPTISCALER_NAME", L"dxgi.dll" );
        module = load_dll( game, xess[i].name, game, 0 );
        if (module) FreeLibrary( module );
        SetEnvironmentVariableW( L"WINE_OPTISCALER_NAME", NULL );
        winetest_pop_context();
    }
    /* A pair can be loaded in either order, but never with an original already loaded. */
    for (i = 0; i < ARRAY_SIZE(xess); ++i)
    {
        unsigned int sibling = i ^ 1;
        module = load_dll( game, xess[i].name, shared, 0 );
        other = load_dll( game, xess[sibling].name, shared, 0 );
        if (other) FreeLibrary( other );
        if (module) FreeLibrary( module );
        SetEnvironmentVariableW( L"WINE_XESS_PROVIDER", NULL );
        module = load_dll( game, xess[i].name, game, 0 );
        SetEnvironmentVariableW( L"WINE_XESS_PROVIDER", unix_shared );
        other = load_dll( game, xess[sibling].name, game, 0 );
        if (other) FreeLibrary( other );
        if (module) FreeLibrary( module );
    }
    /* An equal companion follows a selected bundle, preserving FG/XeLL coherence. */
    make_dll( game, xess[3].name, xess[3].new_ms, xess[3].new_ls, company, xess[3].product, 0, FALSE );
    module = load_dll( game, xess[2].name, shared, 0 );
    other = load_dll( game, xess[3].name, shared, 0 );
    if (other) FreeLibrary( other );
    if (module) FreeLibrary( module );
    module = load_dll( game, xess[3].name, game, 0 );
    other = load_dll( game, xess[2].name, game, 0 );
    if (other) FreeLibrary( other );
    if (module) FreeLibrary( module );
    make_dll( game, xess[3].name, xess[3].new_ms, xess[3].new_ls + 1, company, xess[3].product, 0, FALSE );
    module = load_dll( game, xess[2].name, game, 0 );
    if (module) FreeLibrary( module );
    make_dll( game, xess[3].name, xess[3].old_ms, 1, company, L"Custom XeLL", 0, FALSE );
    module = load_dll( game, xess[2].name, game, 0 );
    if (module) FreeLibrary( module );
    make_dll( game, xess[3].name, xess[3].old_ms, 1, company, xess[3].product, 0, FALSE );
    /* XeLL can be used alone, but an FG library in another directory still matters. */
    swprintf( path, ARRAY_SIZE(path), L"%s\\%s", game, xess[2].name );
    ok(DeleteFileW(path), "Cannot remove original FG fixture.\n");
    module = load_dll( game, xess[3].name, shared, 0 );
    if (module) FreeLibrary( module );
    swprintf( foreign, ARRAY_SIZE(foreign), L"%s\\foreign", game );
    ok(CreateDirectoryW(foreign, NULL), "Cannot create foreign FG directory.\n");
    make_dll( foreign, xess[2].name, xess[2].old_ms, 1, company, xess[2].product, 0, FALSE );
    SetEnvironmentVariableW( L"WINE_XESS_PROVIDER", NULL );
    module = load_dll( foreign, xess[2].name, foreign, 0 );
    SetEnvironmentVariableW( L"WINE_XESS_PROVIDER", unix_shared );
    other = load_dll( game, xess[3].name, game, 0 );
    if (other) FreeLibrary( other );
    if (module) FreeLibrary( module );
    swprintf( path, ARRAY_SIZE(path), L"%s\\%s", foreign, xess[2].name );
    DeleteFileW( path );
    RemoveDirectoryW( foreign );
    make_dll( game, xess[2].name, xess[2].new_ms, xess[2].new_ls + 1, company, xess[2].product, 0, FALSE );
    module = load_dll( game, xess[3].name, game, 0 );
    if (module) FreeLibrary( module );
    make_dll( game, xess[2].name, xess[2].old_ms, 1, company, xess[2].product, 0, FALSE );
    swprintf( path, ARRAY_SIZE(path), L"%s\\%s", shared, xess[3].name );
    ok(DeleteFileW(path), "Cannot remove shared XeLL fixture.\n");
    module = load_dll( game, xess[2].name, game, 0 );
    if (module) FreeLibrary( module );
    make_dll( shared, xess[3].name, xess[3].new_ms, xess[3].new_ls, company, xess[3].product, 0, TRUE );
    module = load_dll( game, xess[2].name, game, 0 );
    if (module) FreeLibrary( module );
    make_dll( shared, xess[3].name, xess[3].new_ms, xess[3].new_ls, company, xess[3].product, 0, FALSE );
    /* Validate the provider pair too, including manually supplied directories. */
    make_dll( shared, xess[3].name, xess[3].old_ms, 1, company, xess[3].product, 0, FALSE );
    module = load_dll( game, xess[2].name, game, 0 );
    if (module) FreeLibrary( module );
    make_dll( shared, xess[3].name, xess[3].new_ms, xess[3].new_ls, company, xess[3].product, 0, FALSE );
    make_dll( shared, xess[1].name, xess[1].new_ms, xess[1].new_ls + 1, company, xess[1].product, 0, FALSE );
    module = load_dll( game, xess[0].name, game, 0 );
    if (module) FreeLibrary( module );
    make_dll( shared, xess[1].name, xess[1].new_ms, xess[1].new_ls, company, xess[1].product, 0, FALSE );
    swprintf( path, ARRAY_SIZE(path), L"%s\\%s", game, xess[3].name );
    ok(DeleteFileW(path), "Cannot remove original XeLL fixture.\n");
    module = load_dll( game, xess[2].name, game, 0 );
    if (module) FreeLibrary( module );
    make_dll( game, xess[3].name, xess[3].old_ms, 1, company, xess[3].product, 0, FALSE );
    /* Older SR scaling semantics cannot be adapted by a DLL loader. */
    make_dll( game, xess[0].name, 0x10002, 1, company, xess[0].product, 0, FALSE );
    module = load_dll( game, xess[0].name, game, 0 );
    if (module) FreeLibrary( module );
    make_dll( game, xess[0].name, xess[0].old_ms, 1, company, xess[0].product, 0, FALSE );
    for (i = 0; i < 5; ++i)
    {
        make_dll( game, xess[0].name, xess[0].old_ms, 1,
                  i == 0 ? NULL : i == 1 ? L"OptiScaler" : company,
                  i == 2 ? L"Custom library" : xess[0].product, i == 3 ? VS_FF_DEBUG : 0, i == 4 );
        module = load_dll( game, xess[0].name, game, 0 );
        if (module) FreeLibrary( module );
    }
    make_dll( game, xess[0].name, xess[0].old_ms, 1, company, xess[0].product, 0, FALSE );
    SetEnvironmentVariableW( L"WINE_XESS_PROVIDER", NULL );
    SetEnvironmentVariableW( L"WINE_UPSCALER_REPLACE", L"xess" );
    module = load_dll( game, xess[0].name, game, 0 );
    if (module) FreeLibrary( module );
    SetEnvironmentVariableW( L"WINE_UPSCALER_REPLACE", NULL );
    SetEnvironmentVariableW( L"WINE_XESS_PROVIDER", L"relative" );
    module = load_dll( game, xess[0].name, game, 0 );
    if (module) FreeLibrary( module );
    SetEnvironmentVariableW( L"WINE_XESS_PROVIDER", L"/missing-xess-provider" );
    module = load_dll( game, xess[0].name, game, 0 );
    if (module) FreeLibrary( module );
    SetEnvironmentVariableW( L"WINE_XESS_PROVIDER", unix_shared );
    /* Intel's driver implementation DLLs have different names and stay untouched. */
    make_dll( game, L"igxess2.dll", xess[0].old_ms, 1, company, xess[0].product, 0, FALSE );
    module = load_dll( game, L"igxess2.dll", game, 0 );
    if (module) FreeLibrary( module );
    swprintf( path, ARRAY_SIZE(path), L"%s\\igxess2.dll", game );
    DeleteFileW( path );
    /* Failed imports retain an image in Wine, so test this last for the bundle. */
    break_import( shared, xess[0].name );
    module = load_dll( game, xess[0].name, game, 0 );
    if (module) FreeLibrary( module );
    /* Once FG is selected, a failed XeLL load must not fall back to an older SDK. */
    module = load_dll( game, xess[2].name, shared, 0 );
    make_dll( game, xess[3].name, xess[3].old_ms, 1, company, L"Custom XeLL", 0, FALSE );
    swprintf( path, ARRAY_SIZE(path), L"%s\\%s", game, xess[3].name );
    other = LoadLibraryW( path );
    ok(!other, "Mixed a selected FG library with custom XeLL.\n");
    if (other) FreeLibrary( other );
    make_dll( game, xess[3].name, xess[3].old_ms, 1, company, xess[3].product, 0, FALSE );
    break_import( shared, xess[3].name );
    other = LoadLibraryW( path );
    ok(!other, "Fell back to older XeLL after selecting shared FG.\n");
    if (other) FreeLibrary( other );
    if (module) FreeLibrary( module );
    SetEnvironmentVariableW( L"WINE_XESS_PROVIDER", NULL );
    for (i = 0; i < ARRAY_SIZE(xess); ++i)
    {
        swprintf( path, ARRAY_SIZE(path), L"%s\\%s", game, xess[i].name );
        DeleteFileW( path );
        swprintf( path, ARRAY_SIZE(path), L"%s\\%s", shared, xess[i].name );
        DeleteFileW( path );
    }
}

START_TEST(upscaler)
{
    WCHAR temp[MAX_PATH], root[MAX_PATH], path[MAX_PATH];
    char * (CDECL *get_unix_name)(const WCHAR *);
    char *unix_name;
    unsigned int i;

    if (strcmp(winetest_platform, "wine") || sizeof(void *) != 8)
    {
        win_skip("Upscaler replacement requires 64-bit Wine.\n");
        return;
    }
    GetTempPathW( ARRAY_SIZE(temp), temp );
    GetTempFileNameW( temp, L"ffx", 0, root );
    DeleteFileW( root );
    CreateDirectoryW( root, NULL );
    swprintf( game, ARRAY_SIZE(game), L"%s\\game", root );
    swprintf( shared, ARRAY_SIZE(shared), L"%s\\shared", root );
    CreateDirectoryW( game, NULL );
    CreateDirectoryW( shared, NULL );
    get_unix_name = (void *)GetProcAddress( GetModuleHandleW(L"kernel32.dll"), "wine_get_unix_file_name" );
    if (!get_unix_name) return;
    unix_name = get_unix_name( shared );
    ok(!!unix_name, "Cannot get shared Unix path.\n");
    if (!unix_name) return;
    MultiByteToWideChar( CP_UNIXCP, 0, unix_name, -1, unix_shared, ARRAY_SIZE(unix_shared) );
    HeapFree( GetProcessHeap(), 0, unix_name );
    SetEnvironmentVariableW( L"WINE_FFX_PROVIDER", unix_shared );
    check_combined();
    check_split();
    check_dlss();
    check_xess();
    SetEnvironmentVariableW( L"WINE_FFX_PROVIDER", NULL );
    for (i = 0; i <= ARRAY_SIZE(split); ++i)
    {
        const WCHAR *name = i == ARRAY_SIZE(split) ? combined : split[i];
        swprintf( path, ARRAY_SIZE(path), L"%s\\%s", game, name );
        DeleteFileW( path );
        swprintf( path, ARRAY_SIZE(path), L"%s\\%s", shared, name );
        DeleteFileW( path );
    }
    RemoveDirectoryW( game );
    RemoveDirectoryW( shared );
    RemoveDirectoryW( root );
}
