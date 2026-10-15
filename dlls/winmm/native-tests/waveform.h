/* Native tests of the production waveform implementation.
 *
 * Copyright 2026 Erhan Bilgili
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
#include <wchar.h>

#define COBJMACROS
#include "initguid.h"
#include "windef.h"
#include "winbase.h"
#include "winternl.h"
#include "ole2.h"
#include "mmdeviceapi.h"
#include "audioclient.h"
#include "audiopolicy.h"
#include "mmsystem.h"
#include "msacm.h"
#include "wine/debug.h"

static void *test_malloc(size_t size);
static void *test_calloc(size_t count, size_t size);
static void test_free(void *ptr);
static int test_wcscmp(const WCHAR *a, const WCHAR *b);

/* Only platform services and allocation are replaced. The initialization,
 * enumeration, callbacks, cleanup, object layouts, and vtables are real. */
#define malloc test_malloc
#define calloc test_calloc
#define free test_free
#define wcscmp test_wcscmp
#ifndef WAVEFORM_TEST_SOURCE
#define WAVEFORM_TEST_SOURCE "../waveform.c"
#endif
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wimplicit-fallthrough"
#include WAVEFORM_TEST_SOURCE
#pragma GCC diagnostic pop
#undef malloc
#undef calloc
#undef free
#undef wcscmp

static unsigned int creates, registers, unregisters, releases, com_inits, com_uninits;
static unsigned int endpoint_queries[2], endpoint_count[2];
static unsigned int allocations, allocated, live_locks, deleted_locks;
static void *allocation_ptrs[256];
static HRESULT create_result = S_OK, register_result = S_OK, com_result = S_OK;
static HRESULT endpoint_result[2] = {S_OK, S_OK};
static IMMNotificationClient *registered_client;
static BOOL require_unregistered_during_cleanup;
static pthread_mutex_t once_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t once_cond = PTHREAD_COND_INITIALIZER;
static pthread_mutex_t callback_mutex = PTHREAD_MUTEX_INITIALIZER;
static IMMDeviceEnumerator mock_enumerator;
static IMMDeviceCollection mock_collection[2];
static IMMDevice mock_device;
static IPropertyStore mock_property_store;

static void *test_malloc(size_t size)
{
    void *ptr = malloc(size);
    unsigned int i;

    assert(ptr);
    for (i = 0; i < ARRAY_SIZE(allocation_ptrs); ++i)
        if (!allocation_ptrs[i]) break;
    assert(i < ARRAY_SIZE(allocation_ptrs));
    allocation_ptrs[i] = ptr;
    ++allocations;
    ++allocated;
    return ptr;
}

static void *test_calloc(size_t count, size_t size)
{
    void *ptr = test_malloc(count * size);
    memset(ptr, 0, count * size);
    return ptr;
}

static void test_free(void *ptr)
{
    unsigned int i;

    if (!ptr) return;
    for (i = 0; i < ARRAY_SIZE(allocation_ptrs); ++i)
        if (allocation_ptrs[i] == ptr) break;
    assert(i < ARRAY_SIZE(allocation_ptrs));
    allocation_ptrs[i] = NULL;
    assert(allocated);
    --allocated;
    free(ptr);
}

static int test_wcscmp(const WCHAR *a, const WCHAR *b)
{
    assert(a && b);
    while (*a && *a == *b) ++a, ++b;
    return *a - *b;
}

unsigned char __wine_dbg_get_channel_flags(struct __wine_debug_channel *channel) { return 0; }
int __wine_dbg_header(enum __wine_debug_class cls, struct __wine_debug_channel *channel,
                      const char *function) { return -1; }
int __wine_dbg_output(const char *str) { return 0; }
const char *__wine_dbg_strdup(const char *str) { return "<debug>"; }

HRESULT WINAPI CoInitialize(void *reserved)
{
    ++com_inits;
    return com_result;
}

void WINAPI CoUninitialize(void) { ++com_uninits; }

HRESULT WINAPI CoCreateInstance(REFCLSID clsid, IUnknown *outer, DWORD context,
                               REFIID iid, void **out)
{
    ++creates;
    *out = FAILED(create_result) ? NULL : &mock_enumerator;
    return create_result;
}

void WINAPI CoTaskMemFree(void *ptr) { test_free(ptr); }
HRESULT WINAPI CoCreateGuid(GUID *guid) { memset(guid, 0, sizeof(*guid)); return S_OK; }

BOOL WINAPI InitOnceExecuteOnce(INIT_ONCE *once, PINIT_ONCE_FN fn, void *param, void **context)
{
    BOOL ret;

    pthread_mutex_lock(&once_mutex);
    while (once->Ptr == (void *)1) pthread_cond_wait(&once_cond, &once_mutex);
    if (once->Ptr == (void *)2)
    {
        pthread_mutex_unlock(&once_mutex);
        return TRUE;
    }
    once->Ptr = (void *)1;
    pthread_mutex_unlock(&once_mutex);
    ret = fn(once, param, context);
    pthread_mutex_lock(&once_mutex);
    once->Ptr = ret ? (void *)2 : NULL;
    pthread_cond_broadcast(&once_cond);
    pthread_mutex_unlock(&once_mutex);
    return ret;
}

BOOL WINAPI InitializeCriticalSectionEx(CRITICAL_SECTION *cs, DWORD spin, DWORD flags)
{
    cs->DebugInfo = calloc(1, sizeof(*cs->DebugInfo));
    assert(cs->DebugInfo);
    ++live_locks;
    return TRUE;
}

void WINAPI DeleteCriticalSection(CRITICAL_SECTION *cs)
{
    if (cs == &g_devthread_lock) return;
    if (require_unregistered_during_cleanup) assert(!registered_client);
    assert(live_locks);
    --live_locks;
    ++deleted_locks;
    free(cs->DebugInfo);
    cs->DebugInfo = NULL;
}

void WINAPI EnterCriticalSection(CRITICAL_SECTION *cs)
{
    assert(cs == &g_devthread_lock);
    pthread_mutex_lock(&callback_mutex);
}

void WINAPI LeaveCriticalSection(CRITICAL_SECTION *cs)
{
    assert(cs == &g_devthread_lock);
    pthread_mutex_unlock(&callback_mutex);
}

BOOL WINAPI CloseHandle(HANDLE handle) { assert(!handle); return FALSE; }
HANDLE WINAPI CreateEventW(SECURITY_ATTRIBUTES *attr, BOOL manual, BOOL state, const WCHAR *name)
{ abort(); }
INT WINAPI MulDiv(INT number, INT numerator, INT denominator) { abort(); }
MMRESULT WINAPI acmStreamOpen(PHACMSTREAM stream, HACMDRIVER driver, PWAVEFORMATEX source,
                             PWAVEFORMATEX dest, PWAVEFILTER filter, DWORD_PTR callback,
                             DWORD_PTR instance, DWORD flags) { abort(); }
MMRESULT WINAPI acmStreamClose(HACMSTREAM stream, DWORD flags) { abort(); }
BOOL WINAPI DriverCallback(DWORD_PTR callback, DWORD flags, HDRVR device, DWORD message,
                           DWORD_PTR user, DWORD_PTR first, DWORD_PTR second) { abort(); }
HRESULT WINAPI PropVariantClear(PROPVARIANT *value)
{
    assert(value->vt == VT_LPWSTR);
    test_free(value->pwszVal);
    PropVariantInit(value);
    return S_OK;
}

static ULONG WINAPI enum_release(IMMDeviceEnumerator *iface) { return ++releases; }

static HRESULT WINAPI enum_endpoints(IMMDeviceEnumerator *iface, EDataFlow flow,
                                      DWORD mask, IMMDeviceCollection **out)
{
    assert(flow == eRender || flow == eCapture);
    ++endpoint_queries[flow];
    *out = FAILED(endpoint_result[flow]) ? NULL : &mock_collection[flow];
    return endpoint_result[flow];
}

static HRESULT WINAPI enum_default(IMMDeviceEnumerator *iface, EDataFlow flow,
                                    ERole role, IMMDevice **out)
{
    *out = &mock_device;
    return S_OK;
}

static HRESULT WINAPI enum_register(IMMDeviceEnumerator *iface, IMMNotificationClient *client)
{
    ++registers;
    assert(!registered_client);
    if (SUCCEEDED(register_result)) registered_client = client;
    return register_result;
}

static HRESULT WINAPI enum_unregister(IMMDeviceEnumerator *iface, IMMNotificationClient *client)
{
    ++unregisters;
    assert(!registered_client || registered_client == client);
    registered_client = NULL;
    return S_OK;
}

static IMMDeviceEnumeratorVtbl enum_vtbl = {
    .Release = enum_release,
    .EnumAudioEndpoints = enum_endpoints,
    .GetDefaultAudioEndpoint = enum_default,
    .RegisterEndpointNotificationCallback = enum_register,
    .UnregisterEndpointNotificationCallback = enum_unregister,
};
static IMMDeviceEnumerator mock_enumerator = {&enum_vtbl};

static ULONG WINAPI collection_release(IMMDeviceCollection *iface) { return 1; }
static HRESULT WINAPI collection_count(IMMDeviceCollection *iface, UINT *out)
{
    *out = endpoint_count[iface == &mock_collection[eCapture]];
    return S_OK;
}
static HRESULT WINAPI collection_item(IMMDeviceCollection *iface, UINT index, IMMDevice **out)
{
    assert(!index);
    *out = &mock_device;
    return S_OK;
}
static IMMDeviceCollectionVtbl collection_vtbl = {
    .Release = collection_release,
    .GetCount = collection_count,
    .Item = collection_item,
};
static IMMDeviceCollection mock_collection[2] = {{&collection_vtbl}, {&collection_vtbl}};

static ULONG WINAPI device_release(IMMDevice *iface) { return 1; }
static HRESULT WINAPI device_activate(IMMDevice *iface, REFIID iid, DWORD context,
                                      PROPVARIANT *params, void **out) { return E_NOINTERFACE; }
static HRESULT WINAPI device_props(IMMDevice *iface, DWORD access, IPropertyStore **out)
{
    *out = &mock_property_store;
    return S_OK;
}
static HRESULT WINAPI device_id(IMMDevice *iface, WCHAR **out)
{
    *out = test_malloc(sizeof(L"endpoint"));
    memcpy(*out, L"endpoint", sizeof(L"endpoint"));
    return S_OK;
}
static IMMDeviceVtbl device_vtbl = {
    .Release = device_release,
    .Activate = device_activate,
    .OpenPropertyStore = device_props,
    .GetId = device_id,
};
static IMMDevice mock_device = {&device_vtbl};

static ULONG WINAPI property_release(IPropertyStore *iface) { return 1; }
static HRESULT WINAPI property_value(IPropertyStore *iface, REFPROPERTYKEY key, PROPVARIANT *out)
{
    out->vt = VT_LPWSTR;
    out->pwszVal = test_malloc(sizeof(L"mock"));
    memcpy(out->pwszVal, L"mock", sizeof(L"mock"));
    return S_OK;
}
static IPropertyStoreVtbl property_vtbl = {
    .Release = property_release,
    .GetValue = property_value,
};
static IPropertyStore mock_property_store = {&property_vtbl};

static void run_test(void (*test)(void), const char *name)
{
    pid_t child = fork();
    int status;

    assert(child >= 0);
    if (!child)
    {
        test();
        _exit(0);
    }
    assert(waitpid(child, &status, 0) == child);
    if (!WIFEXITED(status) || WEXITSTATUS(status))
    {
        fprintf(stderr, "%s failed, status %#x\n", name, status);
        exit(1);
    }
    printf("%s passed\n", name);
}
