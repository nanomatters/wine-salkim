/*
 * Tests for Direct3D 12 Media Foundation buffers.
 *
 * Copyright 2026 Erhan Bilgili
 *
 * This library is free software. You can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation. Either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY. Without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 */

#define COBJMACROS
#include <stdlib.h>
#include <string.h>
#include "mfapi.h"
#include "mfidl.h"
#include "mftransform.h"
#include "mferror.h"
#include "wine/test.h"
#include "initguid.h"
#include "d3d12.h"
#undef EXTERN_GUID
#define EXTERN_GUID DEFINE_GUID
#include "mfd3d12.h"

static HRESULT (WINAPI *pD3D12CreateDevice)(IUnknown *, D3D_FEATURE_LEVEL, REFIID, void **);
static HRESULT (WINAPI *pMFCreateD3D12SynchronizationObject)(ID3D12Device *, REFIID, void **);
static HRESULT (WINAPI *pMFCreateDXGIDeviceManager)(UINT *, IMFDXGIDeviceManager **);
static HRESULT (WINAPI *pMFCreateDXGISurfaceBuffer)(REFIID, IUnknown *, UINT, BOOL, IMFMediaBuffer **);
static HRESULT (WINAPI *pMFCreateVideoSampleAllocatorEx)(REFIID, void **);
static HRESULT (WINAPI *pMFCreateSampleCopierMFT)(IMFTransform **);

DEFINE_GUID(resource_lifetime_key, 0x3f9fc996, 0xb914, 0x4fb9, 0xa3, 0x17, 0xde, 0x24, 0x65, 0x9d, 0xd4, 0xc2);

struct resource_lifetime
{
    IUnknown IUnknown_iface;
    LONG refcount;
    HANDLE event;
};

static HRESULT WINAPI resource_lifetime_QueryInterface(IUnknown *iface, REFIID iid, void **out)
{
    *out = NULL;
    if (!IsEqualGUID(iid, &IID_IUnknown)) return E_NOINTERFACE;
    *out = iface;
    IUnknown_AddRef(iface);
    return S_OK;
}

static ULONG WINAPI resource_lifetime_AddRef(IUnknown *iface)
{
    struct resource_lifetime *lifetime = CONTAINING_RECORD(iface, struct resource_lifetime, IUnknown_iface);
    return InterlockedIncrement(&lifetime->refcount);
}

static ULONG WINAPI resource_lifetime_Release(IUnknown *iface)
{
    struct resource_lifetime *lifetime = CONTAINING_RECORD(iface, struct resource_lifetime, IUnknown_iface);
    ULONG refcount = InterlockedDecrement(&lifetime->refcount);

    if (!refcount)
    {
        SetEvent(lifetime->event);
        CloseHandle(lifetime->event);
        free(lifetime);
    }
    return refcount;
}

static const IUnknownVtbl resource_lifetime_vtbl =
{
    resource_lifetime_QueryInterface,
    resource_lifetime_AddRef,
    resource_lifetime_Release,
};

static HANDLE track_resource_destruction(ID3D12Resource *resource)
{
    struct resource_lifetime *lifetime;
    HANDLE event;
    HRESULT hr;

    if (!(event = CreateEventW(NULL, TRUE, FALSE, NULL))) return NULL;
    if (!(lifetime = malloc(sizeof(*lifetime))))
    {
        CloseHandle(event);
        return NULL;
    }
    lifetime->IUnknown_iface.lpVtbl = &resource_lifetime_vtbl;
    lifetime->refcount = 1;
    if (!DuplicateHandle(GetCurrentProcess(), event, GetCurrentProcess(), &lifetime->event,
            0, FALSE, DUPLICATE_SAME_ACCESS))
    {
        CloseHandle(event);
        free(lifetime);
        return NULL;
    }
    hr = ID3D12Resource_SetPrivateDataInterface(resource, &resource_lifetime_key, &lifetime->IUnknown_iface);
    ok(hr == S_OK, "Failed to track texture destruction, hr %#lx.\n", hr);
    IUnknown_Release(&lifetime->IUnknown_iface);
    if (FAILED(hr))
    {
        CloseHandle(event);
        return NULL;
    }
    return event;
}

static ID3D12CommandQueue *create_queue(ID3D12Device *device)
{
    D3D12_COMMAND_QUEUE_DESC desc = {0};
    ID3D12CommandQueue *queue = NULL;
    HRESULT hr;

    desc.Type = D3D12_COMMAND_LIST_TYPE_COPY;
    hr = ID3D12Device_CreateCommandQueue(device, &desc, &IID_ID3D12CommandQueue, (void **)&queue);
    ok(hr == S_OK, "Failed to create queue, hr %#lx.\n", hr);
    return queue;
}

static HRESULT WINAPI failing_queue_Signal(ID3D12CommandQueue *iface, ID3D12Fence *fence, UINT64 value)
{
    return E_FAIL;
}

static const ID3D12CommandQueueVtbl failing_queue_vtbl =
{
    .Signal = failing_queue_Signal,
};

static ID3D12CommandQueue failing_queue = { &failing_queue_vtbl };

struct recording_queue
{
    ID3D12CommandQueue ID3D12CommandQueue_iface;
    ID3D12CommandQueue *queue;
    ID3D12Fence *fences[2];
    UINT64 values[2];
    unsigned int count;
    BOOL fail_completion;
};

static HRESULT WINAPI recording_queue_Signal(ID3D12CommandQueue *iface, ID3D12Fence *fence, UINT64 value)
{
    struct recording_queue *queue = CONTAINING_RECORD(iface, struct recording_queue, ID3D12CommandQueue_iface);
    unsigned int index = queue->count++;

    ok(index < ARRAY_SIZE(queue->fences), "Unexpected signal %u.\n", index);
    if (index < ARRAY_SIZE(queue->fences))
    {
        queue->fences[index] = fence;
        queue->values[index] = value;
        ID3D12Fence_AddRef(fence);
    }
    if (index == 1 && queue->fail_completion) return E_FAIL;
    return ID3D12CommandQueue_Signal(queue->queue, fence, value);
}

static const ID3D12CommandQueueVtbl recording_queue_vtbl =
{
    .Signal = recording_queue_Signal,
};

static void clear_recorded_signals(struct recording_queue *queue)
{
    unsigned int i;

    for (i = 0; i < ARRAY_SIZE(queue->fences); ++i)
    {
        if (queue->fences[i]) ID3D12Fence_Release(queue->fences[i]);
        queue->fences[i] = NULL;
    }
    queue->count = 0;
}

static void test_producer_completion(ID3D12Device *device)
{
    struct recording_queue queue = {{&recording_queue_vtbl}};
    IMFD3D12SynchronizationObjectCommands *commands = NULL;
    D3D12_RESOURCE_DESC desc = {0};
    D3D12_HEAP_PROPERTIES heap = {0};
    ID3D12Fence *gate = NULL, *completion = NULL, *first_private_fence = NULL;
    ID3D12Resource *resource = NULL;
    IMFMediaBuffer *buffer = NULL;
    IMFDXGIBuffer *dxgi = NULL;
    HANDLE event = NULL, destroyed_event = NULL;
    UINT64 previous_value = 0, public_value = 0;
    ULONG initial_refcount, refcount = 0;
    unsigned int i, attempt;
    DWORD result;
    BOOL drained = FALSE;
    HRESULT hr;

    /* Observe Wine's private completion signals without relying on global
     * handle counts. Windows may call additional methods on the queue. */
    if (strcmp(winetest_platform, "wine")) return;
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = desc.Height = 16;
    desc.DepthOrArraySize = desc.MipLevels = desc.SampleDesc.Count = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    hr = ID3D12Device_CreateCommittedResource(device, &heap, D3D12_HEAP_FLAG_NONE, &desc,
            D3D12_RESOURCE_STATE_COMMON, NULL, &IID_ID3D12Resource, (void **)&resource);
    ok(hr == S_OK, "Failed to create completion texture, hr %#lx.\n", hr);
    if (FAILED(hr)) goto done;
    destroyed_event = track_resource_destruction(resource);
    ok(!!destroyed_event, "Failed to track completion texture.\n");
    if (!destroyed_event) goto done;
    hr = pMFCreateDXGISurfaceBuffer(&IID_ID3D12Resource, (IUnknown *)resource, 0, FALSE, &buffer);
    ok(hr == S_OK, "Failed to wrap completion texture, hr %#lx.\n", hr);
    if (FAILED(hr)) goto done;
    hr = IMFMediaBuffer_QueryInterface(buffer, &IID_IMFDXGIBuffer, (void **)&dxgi);
    ok(hr == S_OK, "Failed to get completion DXGI buffer, hr %#lx.\n", hr);
    if (FAILED(hr)) goto done;
    hr = IMFDXGIBuffer_GetUnknown(dxgi, &MF_D3D12_SYNCHRONIZATION_OBJECT,
            &IID_IMFD3D12SynchronizationObjectCommands, (void **)&commands);
    ok(hr == S_OK, "Failed to get completion synchronization, hr %#lx.\n", hr);
    if (FAILED(hr)) goto done;
    if (!(queue.queue = create_queue(device))) goto done;
    hr = ID3D12Device_CreateFence(device, 0, D3D12_FENCE_FLAG_NONE, &IID_ID3D12Fence, (void **)&gate);
    ok(hr == S_OK, "Failed to create completion gate, hr %#lx.\n", hr);
    if (FAILED(hr)) goto done;
    hr = ID3D12Device_CreateFence(device, 0, D3D12_FENCE_FLAG_NONE, &IID_ID3D12Fence, (void **)&completion);
    ok(hr == S_OK, "Failed to create queue completion fence, hr %#lx.\n", hr);
    if (FAILED(hr)) goto done;
    event = CreateEventW(NULL, FALSE, FALSE, NULL);
    ok(!!event, "Failed to create queue completion event.\n");
    if (!event) goto done;
    IMFD3D12SynchronizationObjectCommands_AddRef(commands);
    initial_refcount = IMFD3D12SynchronizationObjectCommands_Release(commands);

    for (i = 0; i < 16; ++i)
    {
        hr = IMFD3D12SynchronizationObjectCommands_EnqueueResourceReady(commands, &queue.ID3D12CommandQueue_iface);
        ok(hr == S_OK, "Failed to enqueue producer %u, hr %#lx.\n", i, hr);
        ok(queue.count == 2, "Unexpected producer signal count %u.\n", queue.count);
        if (FAILED(hr) || queue.count != 2) goto done;
        if (!i)
        {
            first_private_fence = queue.fences[1];
            ID3D12Fence_AddRef(first_private_fence);
            public_value = queue.values[0];
        }
        ok(queue.fences[1] == first_private_fence, "Completed producer fence was not reused at %u.\n", i);
        ok(queue.values[1] == previous_value + 1, "Unexpected private value %s after %s.\n",
                wine_dbgstr_longlong(queue.values[1]), wine_dbgstr_longlong(previous_value));
        ok(queue.values[0] == public_value, "Producer changed public generation without Reset.\n");
        previous_value = queue.values[1];
        hr = ID3D12CommandQueue_Signal(queue.queue, completion, i + 1);
        ok(hr == S_OK, "Failed to signal queue completion, hr %#lx.\n", hr);
        hr = ID3D12Fence_SetEventOnCompletion(completion, i + 1, event);
        ok(hr == S_OK, "Failed to register queue completion, hr %#lx.\n", hr);
        result = WaitForSingleObject(event, 5000);
        ok(result == WAIT_OBJECT_0, "Producer queue did not complete, result %#lx.\n", result);
        /* Fence completion can precede the RTWorkQ callback. Its reference
         * must also be released before the slot is expected to be reusable. */
        for (attempt = 0; attempt < 500; ++attempt)
        {
            IMFD3D12SynchronizationObjectCommands_AddRef(commands);
            refcount = IMFD3D12SynchronizationObjectCommands_Release(commands);
            if (refcount == initial_refcount) break;
            Sleep(10);
        }
        ok(refcount == initial_refcount, "Producer callback retained %lu references, expected %lu.\n",
                refcount, initial_refcount);
        clear_recorded_signals(&queue);
    }

    /* A public Ready signal is accepted, then the trailing completion signal
     * fails. Retain the texture until completion can actually be established. */
    hr = ID3D12CommandQueue_Wait(queue.queue, gate, 1);
    ok(hr == S_OK, "Failed to gate failed-completion producer, hr %#lx.\n", hr);
    queue.fail_completion = TRUE;
    hr = IMFD3D12SynchronizationObjectCommands_EnqueueResourceReady(commands, &queue.ID3D12CommandQueue_iface);
    ok(hr == E_FAIL && queue.count == 2, "Unexpected failed completion, count %u, hr %#lx.\n", queue.count, hr);
    IMFD3D12SynchronizationObjectCommands_Release(commands);
    commands = NULL;
    IMFDXGIBuffer_Release(dxgi);
    dxgi = NULL;
    IMFMediaBuffer_Release(buffer);
    buffer = NULL;
    ID3D12Resource_Release(resource);
    resource = NULL;
    result = WaitForSingleObject(destroyed_event, 100);
    ok(result == WAIT_TIMEOUT, "Failed completion destroyed pending texture, result %#lx.\n", result);
done:
    if (gate) ID3D12Fence_Signal(gate, 1);
    if (queue.queue && completion && event)
    {
        ID3D12CommandQueue_Signal(queue.queue, completion, 100);
        ID3D12Fence_SetEventOnCompletion(completion, 100, event);
        result = WaitForSingleObject(event, 5000);
        ok(result == WAIT_OBJECT_0, "Completion queue did not drain, result %#lx.\n", result);
        drained = result == WAIT_OBJECT_0;
    }
    if (queue.fail_completion && queue.fences[1] && drained)
    {
        result = WaitForSingleObject(destroyed_event, 100);
        ok(result == WAIT_TIMEOUT, "Texture was released without private completion, result %#lx.\n", result);
        /* The real queue is now drained. Complete the deliberately failed
         * signal so the fault-injection test does not leave retained objects. */
        hr = ID3D12Fence_Signal(queue.fences[1], queue.values[1]);
        ok(hr == S_OK, "Failed to complete injected signal, hr %#lx.\n", hr);
        result = WaitForSingleObject(destroyed_event, 5000);
        ok(result == WAIT_OBJECT_0, "Completed texture was not destroyed, result %#lx.\n", result);
    }
    clear_recorded_signals(&queue);
    if (commands) IMFD3D12SynchronizationObjectCommands_Release(commands);
    if (dxgi) IMFDXGIBuffer_Release(dxgi);
    if (buffer) IMFMediaBuffer_Release(buffer);
    if (resource) ID3D12Resource_Release(resource);
    if (first_private_fence) ID3D12Fence_Release(first_private_fence);
    if (queue.queue) ID3D12CommandQueue_Release(queue.queue);
    if (gate) ID3D12Fence_Release(gate);
    if (completion) ID3D12Fence_Release(completion);
    if (event) CloseHandle(event);
    if (destroyed_event) CloseHandle(destroyed_event);
}

static void test_failed_release_signal(ID3D12Device *device)
{
    IMFD3D12SynchronizationObjectCommands *commands = NULL;
    IMFD3D12SynchronizationObject *object = NULL;
    ID3D12CommandQueue *queue = NULL;
    ID3D12Fence *gate = NULL;
    HANDLE event = NULL;
    DWORD result;
    HRESULT hr;

    /* Fault injection only. Windows may use other methods on this queue. */
    if (strcmp(winetest_platform, "wine")) return;
    hr = pMFCreateD3D12SynchronizationObject(device, &IID_IMFD3D12SynchronizationObject, (void **)&object);
    ok(hr == S_OK, "Failed to create synchronization object, hr %#lx.\n", hr);
    if (FAILED(hr)) return;
    hr = IMFD3D12SynchronizationObject_QueryInterface(object, &IID_IMFD3D12SynchronizationObjectCommands,
            (void **)&commands);
    ok(hr == S_OK, "Failed to get synchronization commands, hr %#lx.\n", hr);
    if (FAILED(hr)) goto done;
    if (!(queue = create_queue(device))) goto done;
    hr = ID3D12Device_CreateFence(device, 0, D3D12_FENCE_FLAG_NONE, &IID_ID3D12Fence, (void **)&gate);
    ok(hr == S_OK, "Failed to create gate fence, hr %#lx.\n", hr);
    if (FAILED(hr)) goto done;
    event = CreateEventW(NULL, FALSE, FALSE, NULL);
    ok(!!event, "Failed to create final-release event.\n");
    if (!event) goto done;

    hr = ID3D12CommandQueue_Wait(queue, gate, 1);
    ok(hr == S_OK, "Failed to gate consumer, hr %#lx.\n", hr);
    hr = IMFD3D12SynchronizationObjectCommands_EnqueueResourceRelease(commands, queue);
    ok(hr == S_OK, "Failed to enqueue real consumer release, hr %#lx.\n", hr);
    hr = IMFD3D12SynchronizationObjectCommands_EnqueueResourceRelease(commands, &failing_queue);
    ok(hr == E_FAIL, "Unexpected injected queue failure %#lx.\n", hr);
    hr = IMFD3D12SynchronizationObject_SignalEventOnFinalResourceRelease(object, event);
    ok(hr == S_OK, "Failed to register final release, hr %#lx.\n", hr);
    result = WaitForSingleObject(event, 100);
    ok(result == WAIT_TIMEOUT, "Cancelled work completed another consumer's release, result %#lx.\n", result);
    hr = IMFD3D12SynchronizationObject_Reset(object);
    ok(hr == MF_E_UNEXPECTED, "Cancelled work changed pending release count, hr %#lx.\n", hr);
    hr = ID3D12Fence_Signal(gate, 1);
    ok(hr == S_OK, "Failed to release consumer, hr %#lx.\n", hr);
    result = WaitForSingleObject(event, 5000);
    ok(result == WAIT_OBJECT_0, "Consumer release did not finish, result %#lx.\n", result);
    hr = IMFD3D12SynchronizationObject_Reset(object);
    ok(hr == S_OK, "Cancelled work prevented reset after real release, hr %#lx.\n", hr);
done:
    if (gate) ID3D12Fence_Signal(gate, 1);
    if (commands) IMFD3D12SynchronizationObjectCommands_Release(commands);
    if (object) IMFD3D12SynchronizationObject_Release(object);
    if (queue) ID3D12CommandQueue_Release(queue);
    if (gate) ID3D12Fence_Release(gate);
    if (event) CloseHandle(event);
}

/* Use an independent GPU readback, not the media buffer's CPU mapping, to
 * verify that ContiguousCopyFrom actually uploads the decoded frame. */
static void check_texture_data(ID3D12Device *device, ID3D12Resource *texture,
        IMFD3D12SynchronizationObjectCommands *commands, UINT subresource,
        const BYTE *expected, DWORD expected_size, unsigned int plane_count)
{
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprints[2];
    D3D12_COMMAND_QUEUE_DESC queue_desc = {0};
    D3D12_HEAP_PROPERTIES heap = {0};
    D3D12_TEXTURE_COPY_LOCATION dst = {0}, src = {0};
    ID3D12CommandAllocator *allocator = NULL;
    ID3D12GraphicsCommandList *list = NULL;
    ID3D12CommandQueue *queue = NULL;
    ID3D12Resource *readback = NULL;
    D3D12_RESOURCE_DESC desc, buffer_desc = {0};
    ID3D12Fence *fence = NULL;
    UINT64 row_sizes[2], total_size = 0, image_size = 0;
    UINT rows[2], plane_stride, expected_stride;
    D3D12_RANGE read_range;
    HANDLE event = NULL;
    UINT i, row, result;
    BYTE *data;
    BOOL pending = FALSE;
    HRESULT hr;

    texture->lpVtbl->GetDesc(texture, &desc);
    plane_stride = desc.MipLevels * desc.DepthOrArraySize;
    for (i = 0; i < plane_count; ++i)
    {
        total_size = (total_size + D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT - 1)
                & ~(UINT64)(D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT - 1);
        ID3D12Device_GetCopyableFootprints(device, &desc, subresource + i * plane_stride, 1, total_size,
                &footprints[i], &rows[i], &row_sizes[i], NULL);
        total_size = footprints[i].Offset + (UINT64)rows[i] * footprints[i].Footprint.RowPitch;
        image_size += rows[i];
    }
    ok(image_size && !(expected_size % image_size), "Unexpected image size %s, expected %lu.\n",
            wine_dbgstr_longlong(image_size), expected_size);
    if (!image_size || expected_size % image_size) goto done;
    expected_stride = expected_size / image_size;
    for (i = 0; i < plane_count; ++i)
    {
        ok(expected_stride >= row_sizes[i], "Unexpected stride %u for plane %u, row size %s.\n",
                expected_stride, i, wine_dbgstr_longlong(row_sizes[i]));
        if (expected_stride < row_sizes[i]) goto done;
    }

    heap.Type = D3D12_HEAP_TYPE_READBACK;
    buffer_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    buffer_desc.Width = total_size;
    buffer_desc.Height = 1;
    buffer_desc.DepthOrArraySize = 1;
    buffer_desc.MipLevels = 1;
    buffer_desc.SampleDesc.Count = 1;
    buffer_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    hr = ID3D12Device_CreateCommittedResource(device, &heap, D3D12_HEAP_FLAG_NONE, &buffer_desc,
            D3D12_RESOURCE_STATE_COPY_DEST, NULL, &IID_ID3D12Resource, (void **)&readback);
    ok(hr == S_OK, "Failed to create readback buffer, hr %#lx.\n", hr);
    if (FAILED(hr)) goto done;

    queue_desc.Type = D3D12_COMMAND_LIST_TYPE_COPY;
    hr = ID3D12Device_CreateCommandQueue(device, &queue_desc, &IID_ID3D12CommandQueue, (void **)&queue);
    ok(hr == S_OK, "Failed to create copy queue, hr %#lx.\n", hr);
    if (FAILED(hr)) goto done;
    hr = ID3D12Device_CreateCommandAllocator(device, queue_desc.Type, &IID_ID3D12CommandAllocator,
            (void **)&allocator);
    ok(hr == S_OK, "Failed to create command allocator, hr %#lx.\n", hr);
    if (FAILED(hr)) goto done;
    hr = ID3D12Device_CreateCommandList(device, 0, queue_desc.Type, allocator, NULL,
            &IID_ID3D12GraphicsCommandList, (void **)&list);
    ok(hr == S_OK, "Failed to create command list, hr %#lx.\n", hr);
    if (FAILED(hr)) goto done;
    hr = ID3D12Device_CreateFence(device, 0, D3D12_FENCE_FLAG_NONE, &IID_ID3D12Fence, (void **)&fence);
    ok(hr == S_OK, "Failed to create fence, hr %#lx.\n", hr);
    if (FAILED(hr)) goto done;

    src.pResource = texture;
    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dst.pResource = readback;
    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    for (i = 0; i < plane_count; ++i)
    {
        src.SubresourceIndex = subresource + i * plane_stride;
        dst.PlacedFootprint = footprints[i];
        ID3D12GraphicsCommandList_CopyTextureRegion(list, &dst, 0, 0, 0, &src, NULL);
    }
    hr = ID3D12GraphicsCommandList_Close(list);
    ok(hr == S_OK, "Failed to close command list, hr %#lx.\n", hr);
    if (FAILED(hr)) goto done;
    event = CreateEventW(NULL, FALSE, FALSE, NULL);
    ok(!!event, "Failed to create event.\n");
    if (!event) goto done;
    hr = ID3D12Fence_SetEventOnCompletion(fence, 1, event);
    ok(hr == S_OK, "Failed to register copy fence, hr %#lx.\n", hr);
    if (FAILED(hr)) goto done;
    hr = IMFD3D12SynchronizationObjectCommands_EnqueueResourceReadyWait(commands, queue);
    ok(hr == S_OK, "Failed to wait for decoded frame, hr %#lx.\n", hr);
    if (FAILED(hr)) goto done;
    ID3D12CommandQueue_ExecuteCommandLists(queue, 1, (ID3D12CommandList **)&list);
    pending = TRUE;
    hr = IMFD3D12SynchronizationObjectCommands_EnqueueResourceRelease(commands, queue);
    ok(hr == S_OK, "Failed to release decoded frame, hr %#lx.\n", hr);
    hr = ID3D12CommandQueue_Signal(queue, fence, 1);
    ok(hr == S_OK, "Failed to signal copy fence, hr %#lx.\n", hr);
    if (FAILED(hr)) goto done;
    result = WaitForSingleObject(event, 5000);
    ok(result == WAIT_OBJECT_0, "Copy did not complete, result %#x.\n", result);
    if (result != WAIT_OBJECT_0) goto done;
    pending = FALSE;

    read_range.Begin = 0;
    read_range.End = total_size;
    hr = ID3D12Resource_Map(readback, 0, &read_range, (void **)&data);
    ok(hr == S_OK, "Failed to map readback, hr %#lx.\n", hr);
    if (FAILED(hr)) goto done;
    for (i = 0; i < plane_count; ++i)
    {
        for (row = 0; row < rows[i]; ++row)
        {
            ok(!memcmp(data + footprints[i].Offset + row * footprints[i].Footprint.RowPitch,
                    expected, row_sizes[i]), "Unexpected data in plane %u, row %u.\n", i, row);
            expected += expected_stride;
        }
    }
    ID3D12Resource_Unmap(readback, 0, NULL);

done:
    if (pending && ID3D12Fence_GetCompletedValue(fence) < 1
            && SUCCEEDED(ID3D12Device_GetDeviceRemovedReason(device)))
    {
        /* Do not turn a timed-out test into destruction of in-flight resources. */
        ID3D12Resource_AddRef(texture);
        return;
    }
    if (event) CloseHandle(event);
    if (fence) ID3D12Fence_Release(fence);
    if (list) ID3D12GraphicsCommandList_Release(list);
    if (allocator) ID3D12CommandAllocator_Release(allocator);
    if (queue) ID3D12CommandQueue_Release(queue);
    if (readback) ID3D12Resource_Release(readback);
}

static void wait_for_buffer_release(IMFD3D12SynchronizationObject *object, BOOL reset)
{
    HANDLE event;
    DWORD result;
    HRESULT hr;

    event = CreateEventW(NULL, FALSE, FALSE, NULL);
    ok(!!event, "Failed to create final-release event.\n");
    if (!event) return;
    hr = IMFD3D12SynchronizationObject_SignalEventOnFinalResourceRelease(object, event);
    ok(hr == S_OK, "Failed to register final release, hr %#lx.\n", hr);
    if (SUCCEEDED(hr))
    {
        result = WaitForSingleObject(event, 5000);
        ok(result == WAIT_OBJECT_0, "Consumers did not release frame, result %#lx.\n", result);
        if (result == WAIT_OBJECT_0 && reset)
        {
            hr = IMFD3D12SynchronizationObject_Reset(object);
            ok(hr == S_OK, "Failed to reset released frame, hr %#lx.\n", hr);
        }
    }
    CloseHandle(event);
}

static void check_contiguous_data(const BYTE *data, const BYTE *expected, DWORD length, UINT stride, UINT row_size)
{
    unsigned int row;

    for (row = 0; row < length / stride; ++row)
        ok(!memcmp(data + row * stride, expected + row * stride, row_size), "Unexpected data in row %u.\n", row);
}

static void check_buffer_data(ID3D12Device *device, IMFMediaBuffer *buffer, unsigned int plane_count,
        unsigned int subresource)
{
    IMF2DBuffer2 *buffer2d;
    IMFD3D12SynchronizationObjectCommands *sync_cmd = NULL;
    IMFD3D12SynchronizationObject *sync = NULL;
    IMFDXGIBuffer *dxgi;
    ID3D12Resource *resource;
    D3D12_RESOURCE_DESC desc;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint;
    UINT64 footprint_size = 0, row_size;
    DWORD length, current, maximum;
    BYTE *data = NULL, *expected = NULL, *locked_data;
    unsigned int i, pass, index, plane, rows, primary_rows = 0, stride, plane_stride;
    HRESULT hr;

    hr = IMFMediaBuffer_QueryInterface(buffer, &IID_IMF2DBuffer2, (void **)&buffer2d);
    ok(hr == S_OK, "Failed to get 2D buffer, hr %#lx.\n", hr);
    if (FAILED(hr)) return;
    hr = IMF2DBuffer2_GetContiguousLength(buffer2d, &length);
    ok(hr == S_OK, "Failed to get buffer length, hr %#lx.\n", hr);
    if (FAILED(hr)) goto done;
    expected = malloc(length);
    data = malloc(length);
    if (!expected || !data) goto done;

    hr = IMFMediaBuffer_QueryInterface(buffer, &IID_IMFDXGIBuffer, (void **)&dxgi);
    ok(hr == S_OK, "Failed to get DXGI buffer, hr %#lx.\n", hr);
    if (FAILED(hr)) goto done;
    hr = IMFDXGIBuffer_GetResource(dxgi, &IID_ID3D12Resource, (void **)&resource);
    ok(hr == S_OK, "Failed to get D3D12 resource, hr %#lx.\n", hr);
    if (FAILED(hr)) goto done_dxgi;
    hr = IMFDXGIBuffer_GetSubresourceIndex(dxgi, &index);
    ok(hr == S_OK, "Failed to get subresource index, hr %#lx.\n", hr);
    if (FAILED(hr)) goto done_resource;
    ok(index == subresource, "Unexpected subresource index %u, expected %u.\n", index, subresource);
    hr = IMFDXGIBuffer_GetUnknown(dxgi, &MF_D3D12_SYNCHRONIZATION_OBJECT,
            &IID_IMFD3D12SynchronizationObject, (void **)&sync);
    ok(hr == S_OK, "Failed to get synchronization object, hr %#lx.\n", hr);
    if (FAILED(hr)) goto done_resource;
    hr = IMFDXGIBuffer_GetUnknown(dxgi, &MF_D3D12_SYNCHRONIZATION_OBJECT,
            &IID_IMFD3D12SynchronizationObjectCommands, (void **)&sync_cmd);
    ok(hr == S_OK, "Failed to get synchronization commands, hr %#lx.\n", hr);
    if (FAILED(hr)) goto done_resource;

    resource->lpVtbl->GetDesc(resource, &desc);
    plane_stride = desc.MipLevels * desc.DepthOrArraySize;
    for (plane = 0; plane < plane_count; ++plane)
    {
        footprint_size = (footprint_size + D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT - 1)
                & ~(UINT64)(D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT - 1);
        ID3D12Device_GetCopyableFootprints(device, &desc, subresource + plane * plane_stride,
                1, footprint_size, &footprint, &rows, &row_size, NULL);
        if (!plane) primary_rows = rows;
        footprint_size = footprint.Offset + (UINT64)(rows - 1) * footprint.Footprint.RowPitch + row_size;
    }
    hr = IMFMediaBuffer_GetMaxLength(buffer, &maximum);
    ok(hr == S_OK && maximum == footprint_size, "Unexpected maximum length %lu, expected %s, hr %#lx.\n",
            maximum, wine_dbgstr_longlong(footprint_size), hr);
    stride = length / (primary_rows + (plane_count == 2 ? primary_rows / 2 : 0));

    for (pass = 0; pass < 2; ++pass)
    {
        for (i = 0; i < length; ++i)
            expected[i] = i % stride < row_size ? i * 13 + pass * 29 : 0;
        hr = IMF2DBuffer2_ContiguousCopyFrom(buffer2d, expected, length);
        ok(hr == S_OK, "Failed to upload pass %u, hr %#lx.\n", pass, hr);
        if (FAILED(hr)) goto done_resource;
        check_texture_data(device, resource, sync_cmd, subresource, expected, length, plane_count);
        hr = IMF2DBuffer2_ContiguousCopyTo(buffer2d, data, length);
        ok(hr == S_OK, "Failed to read pass %u, hr %#lx.\n", pass, hr);
        if (SUCCEEDED(hr)) check_contiguous_data(data, expected, length, stride, row_size);
        if (!pass) wait_for_buffer_release(sync, TRUE);
    }

    hr = IMFMediaBuffer_Lock(buffer, &locked_data, NULL, &current);
    ok(hr == S_OK, "Failed to lock contiguous buffer, hr %#lx.\n", hr);
    if (SUCCEEDED(hr))
    {
        ok(current == length, "Unexpected locked length %lu.\n", current);
        check_contiguous_data(locked_data, expected, length, stride, row_size);
        memset(locked_data, 0x7f, length);
        hr = IMFMediaBuffer_Unlock(buffer);
        ok(hr == S_OK, "Failed to unlock contiguous buffer, hr %#lx.\n", hr);
        check_texture_data(device, resource, sync_cmd, subresource, expected, length, plane_count);
    }

    hr = IMFMediaBuffer_SetCurrentLength(buffer, length);
    ok(hr == S_OK, "Failed to set length, hr %#lx.\n", hr);
    hr = IMFMediaBuffer_GetCurrentLength(buffer, &current);
    ok(hr == S_OK && !current, "Unexpected length %lu, hr %#lx.\n", current, hr);

    hr = IMFMediaBuffer_Lock(buffer, &locked_data, NULL, &current);
    ok(hr == S_OK, "Failed to relock contiguous buffer, hr %#lx.\n", hr);
    if (SUCCEEDED(hr))
    {
        check_contiguous_data(locked_data, expected, length, stride, row_size);
        hr = IMFMediaBuffer_Unlock(buffer);
        ok(hr == S_OK, "Failed to unlock contiguous buffer, hr %#lx.\n", hr);
    }
    wait_for_buffer_release(sync, TRUE);
done_resource:
    if (sync_cmd) IMFD3D12SynchronizationObjectCommands_Release(sync_cmd);
    if (sync) IMFD3D12SynchronizationObject_Release(sync);
    ID3D12Resource_Release(resource);
done_dxgi:
    IMFDXGIBuffer_Release(dxgi);
done:
    free(data);
    free(expected);
    IMF2DBuffer2_Release(buffer2d);
}

static void test_surface_buffers(ID3D12Device *device)
{
    static const struct
    {
        DXGI_FORMAT format;
        UINT width, planes, mips, subresource;
    }
    formats[] =
    {
        {DXGI_FORMAT_B8G8R8A8_UNORM, 66, 1, 1, 1},
        {DXGI_FORMAT_B8G8R8A8_UNORM, 66, 1, 2, 3},
        {DXGI_FORMAT_NV12, 66, 2, 1, 1},
        {DXGI_FORMAT_P010, 66, 2, 1, 1},
        {DXGI_FORMAT_P016, 66, 2, 1, 1},
        {DXGI_FORMAT_R8_UNORM, 5, 1, 1, 1},
        {DXGI_FORMAT_R16_UNORM, 3, 1, 1, 1},
    };
    D3D12_HEAP_PROPERTIES heap = {0};
    D3D12_RESOURCE_DESC desc = {0};
    ID3D12Resource *resource;
    IMFMediaBuffer *buffer;
    unsigned int i;
    HRESULT hr;

    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = 66;
    desc.Height = 18;
    desc.DepthOrArraySize = 2;
    desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;

    for (i = 0; i < ARRAY_SIZE(formats); ++i)
    {
        winetest_push_context("format %u", formats[i].format);
        desc.Format = formats[i].format;
        desc.Width = formats[i].width;
        desc.MipLevels = formats[i].mips;
        hr = ID3D12Device_CreateCommittedResource(device, &heap, D3D12_HEAP_FLAG_NONE, &desc,
                D3D12_RESOURCE_STATE_COMMON, NULL, &IID_ID3D12Resource, (void **)&resource);
        ok(hr == S_OK, "Failed to create texture, hr %#lx.\n", hr);
        if (FAILED(hr)) goto next;
        hr = pMFCreateDXGISurfaceBuffer(&IID_ID3D12Resource, (IUnknown *)resource,
                formats[i].subresource, FALSE, &buffer);
        ok(hr == S_OK, "Failed to wrap resource, hr %#lx.\n", hr);
        if (SUCCEEDED(hr))
        {
            check_buffer_data(device, buffer, formats[i].planes, formats[i].subresource);
            IMFMediaBuffer_Release(buffer);
        }
        ID3D12Resource_Release(resource);
next:
        winetest_pop_context();
    }
}

static void check_sample_copy(ID3D12Device *device, IMFMediaType *type, IMFSample *sample)
{
    IMFMediaBuffer *input_buffer = NULL, *output_buffer = NULL;
    IMFTransform *copier = NULL;
    IMFSample *input = NULL;
    IMFDXGIBuffer *dxgi = NULL;
    IMFD3D12SynchronizationObjectCommands *commands = NULL;
    IMFD3D12SynchronizationObject *sync = NULL;
    ID3D12Resource *resource = NULL;
    MFT_OUTPUT_DATA_BUFFER output = {0};
    BYTE *expected = NULL, *data;
    DWORD length, status, i;
    LONGLONG time;
    UINT64 size;
    HRESULT hr;

    if (!pMFCreateSampleCopierMFT) return;
    hr = IMFMediaType_GetUINT64(type, &MF_MT_FRAME_SIZE, &size);
    ok(hr == S_OK, "Failed to get frame size, hr %#lx.\n", hr);
    if (FAILED(hr)) return;
    length = (size >> 32) * (UINT32)size * 3 / 2;
    if (!(expected = malloc(length))) return;
    for (i = 0; i < length; ++i) expected[i] = i * 23 + 19;

    hr = pMFCreateSampleCopierMFT(&copier);
    ok(hr == S_OK, "Failed to create sample copier, hr %#lx.\n", hr);
    if (FAILED(hr)) goto done;
    hr = IMFTransform_SetInputType(copier, 0, type, 0);
    ok(hr == S_OK, "Failed to set copier input type, hr %#lx.\n", hr);
    if (FAILED(hr)) goto done;
    hr = IMFTransform_SetOutputType(copier, 0, type, 0);
    ok(hr == S_OK, "Failed to set copier output type, hr %#lx.\n", hr);
    if (FAILED(hr)) goto done;
    hr = MFCreateSample(&input);
    ok(hr == S_OK, "Failed to create input sample, hr %#lx.\n", hr);
    if (FAILED(hr)) goto done;
    hr = MFCreateMemoryBuffer(length, &input_buffer);
    ok(hr == S_OK, "Failed to create input buffer, hr %#lx.\n", hr);
    if (FAILED(hr)) goto done;
    hr = IMFMediaBuffer_Lock(input_buffer, &data, NULL, NULL);
    ok(hr == S_OK, "Failed to lock input buffer, hr %#lx.\n", hr);
    if (FAILED(hr)) goto done;
    memcpy(data, expected, length);
    IMFMediaBuffer_Unlock(input_buffer);
    IMFMediaBuffer_SetCurrentLength(input_buffer, length);
    hr = IMFSample_AddBuffer(input, input_buffer);
    ok(hr == S_OK, "Failed to add input buffer, hr %#lx.\n", hr);
    if (FAILED(hr)) goto done;
    IMFSample_SetSampleTime(input, 123456);
    IMFSample_SetSampleDuration(input, 333333);

    hr = IMFTransform_ProcessInput(copier, 0, input, 0);
    ok(hr == S_OK, "Failed to submit input sample, hr %#lx.\n", hr);
    if (FAILED(hr)) goto done;
    output.pSample = sample;
    hr = IMFTransform_ProcessOutput(copier, 0, 1, &output, &status);
    ok(hr == S_OK, "Failed to copy decoded sample to D3D12, hr %#lx.\n", hr);
    if (output.pEvents) IMFCollection_Release(output.pEvents);
    if (FAILED(hr)) goto done;
    hr = IMFSample_GetSampleTime(sample, &time);
    ok(hr == S_OK && time == 123456, "Unexpected sample time %s, hr %#lx.\n", wine_dbgstr_longlong(time), hr);
    hr = IMFSample_GetBufferByIndex(sample, 0, &output_buffer);
    ok(hr == S_OK, "Failed to get output buffer, hr %#lx.\n", hr);
    if (FAILED(hr)) goto done;
    hr = IMFMediaBuffer_QueryInterface(output_buffer, &IID_IMFDXGIBuffer, (void **)&dxgi);
    ok(hr == S_OK, "Failed to get output DXGI buffer, hr %#lx.\n", hr);
    if (FAILED(hr)) goto done;
    hr = IMFDXGIBuffer_GetResource(dxgi, &IID_ID3D12Resource, (void **)&resource);
    ok(hr == S_OK, "Failed to get output texture, hr %#lx.\n", hr);
    if (FAILED(hr)) goto done;
    hr = IMFDXGIBuffer_GetUnknown(dxgi, &MF_D3D12_SYNCHRONIZATION_OBJECT,
            &IID_IMFD3D12SynchronizationObjectCommands, (void **)&commands);
    ok(hr == S_OK, "Failed to get decoded frame synchronization, hr %#lx.\n", hr);
    if (FAILED(hr)) goto done;
    hr = IMFDXGIBuffer_GetUnknown(dxgi, &MF_D3D12_SYNCHRONIZATION_OBJECT,
            &IID_IMFD3D12SynchronizationObject, (void **)&sync);
    ok(hr == S_OK, "Failed to get decoded frame release interface, hr %#lx.\n", hr);
    check_texture_data(device, resource, commands, 0, expected, length, 2);
    if (sync) wait_for_buffer_release(sync, FALSE);

done:
    if (commands) IMFD3D12SynchronizationObjectCommands_Release(commands);
    if (sync) IMFD3D12SynchronizationObject_Release(sync);
    if (resource) ID3D12Resource_Release(resource);
    if (dxgi) IMFDXGIBuffer_Release(dxgi);
    if (output_buffer) IMFMediaBuffer_Release(output_buffer);
    if (copier) IMFTransform_ProcessMessage(copier, MFT_MESSAGE_COMMAND_FLUSH, 0);
    if (input_buffer) IMFMediaBuffer_Release(input_buffer);
    if (input) IMFSample_Release(input);
    if (copier) IMFTransform_Release(copier);
    free(expected);
}

static void wait_for_free_samples(IMFVideoSampleAllocatorCallback *callback, LONG expected)
{
    LONG count = -1;
    unsigned int attempt;
    HRESULT hr;

    for (attempt = 0; attempt < 500; ++attempt)
    {
        hr = IMFVideoSampleAllocatorCallback_GetFreeSampleCount(callback, &count);
        ok(hr == S_OK, "Failed to get free sample count, hr %#lx.\n", hr);
        if (FAILED(hr) || count == expected) break;
        Sleep(10);
    }
    ok(count == expected, "Unexpected free sample count %ld, expected %ld.\n", count, expected);
}

static void check_sample_reuse(ID3D12Device *device, IMFVideoSampleAllocatorEx *allocator,
        IMFMediaType *type, unsigned int buffer_count)
{
    IMFD3D12SynchronizationObjectCommands *commands[2] = {0};
    ID3D12CommandQueue *queues[2] = {0};
    ID3D12Fence *gates[2] = {0}, *completions[2] = {0};
    ID3D12Resource *resources[2] = {0}, *new_resource = NULL;
    IMFVideoSampleAllocatorCallback *callback = NULL;
    IMFAttributes *attributes = NULL;
    IMFSample *sample = NULL, *new_sample = NULL;
    IMFMediaBuffer *buffer;
    IMFDXGIBuffer *dxgi;
    HANDLE event = NULL, ready_events[2] = {0}, release_events[2] = {0}, destroyed_events[2] = {0};
    ULONG_PTR old_resource;
    unsigned int i, pass;
    DWORD result, before;
    LONG count;
    HRESULT hr;

    hr = IMFVideoSampleAllocatorEx_QueryInterface(allocator, &IID_IMFVideoSampleAllocatorCallback,
            (void **)&callback);
    ok(hr == S_OK, "Failed to get allocator callback, hr %#lx.\n", hr);
    if (FAILED(hr)) return;
    hr = MFCreateAttributes(&attributes, 1);
    ok(hr == S_OK, "Failed to create allocator attributes, hr %#lx.\n", hr);
    if (FAILED(hr)) goto done;
    IMFAttributes_SetUINT32(attributes, &MF_SA_BUFFERS_PER_SAMPLE, buffer_count);
    for (i = 0; i < buffer_count; ++i)
    {
        ready_events[i] = CreateEventW(NULL, FALSE, FALSE, NULL);
        release_events[i] = CreateEventW(NULL, FALSE, FALSE, NULL);
        ok(ready_events[i] && release_events[i], "Failed to create buffer events.\n");
        if (!ready_events[i] || !release_events[i]) goto done;
        if (!(queues[i] = create_queue(device))) goto done;
        hr = ID3D12Device_CreateFence(device, 0, D3D12_FENCE_FLAG_NONE, &IID_ID3D12Fence,
                (void **)&gates[i]);
        ok(hr == S_OK, "Failed to create gate %u, hr %#lx.\n", i, hr);
        if (FAILED(hr)) goto done;
        hr = ID3D12Device_CreateFence(device, 0, D3D12_FENCE_FLAG_NONE, &IID_ID3D12Fence,
                (void **)&completions[i]);
        ok(hr == S_OK, "Failed to create completion %u, hr %#lx.\n", i, hr);
        if (FAILED(hr)) goto done;
    }
    event = CreateEventW(NULL, FALSE, FALSE, NULL);
    ok(!!event, "Failed to create allocator event.\n");
    if (!event) goto done;
    hr = IMFVideoSampleAllocatorEx_InitializeSampleAllocatorEx(allocator, 1, 1, attributes, type);
    ok(hr == S_OK, "Failed to initialize one-sample pool, hr %#lx.\n", hr);
    if (FAILED(hr)) goto done;

    for (pass = 0; pass < 2; ++pass)
    {
        winetest_push_context("pool with %u buffers, pass %u", buffer_count, pass);
        hr = IMFVideoSampleAllocatorEx_AllocateSample(allocator, &sample);
        ok(hr == S_OK, "Failed to allocate sample, hr %#lx.\n", hr);
        if (FAILED(hr)) goto next;
        for (i = 0; i < buffer_count; ++i)
        {
            hr = IMFSample_GetBufferByIndex(sample, i, &buffer);
            ok(hr == S_OK, "Failed to get buffer %u, hr %#lx.\n", i, hr);
            if (FAILED(hr)) goto next;
            hr = IMFMediaBuffer_QueryInterface(buffer, &IID_IMFDXGIBuffer, (void **)&dxgi);
            IMFMediaBuffer_Release(buffer);
            ok(hr == S_OK, "Failed to get DXGI buffer, hr %#lx.\n", hr);
            if (FAILED(hr)) goto next;
            hr = IMFDXGIBuffer_GetUnknown(dxgi, &MF_D3D12_SYNCHRONIZATION_OBJECT,
                    &IID_IMFD3D12SynchronizationObjectCommands, (void **)&commands[i]);
            ok(hr == S_OK, "Failed to get buffer synchronization, hr %#lx.\n", hr);
            hr = IMFDXGIBuffer_GetResource(dxgi, &IID_ID3D12Resource, (void **)&resources[i]);
            ok(hr == S_OK, "Failed to get buffer resource, hr %#lx.\n", hr);
            IMFDXGIBuffer_Release(dxgi);
            if (!commands[i] || !resources[i]) goto next;
            if (pass)
            {
                destroyed_events[i] = track_resource_destruction(resources[i]);
                ok(!!destroyed_events[i], "Failed to track resource %u.\n", i);
                if (!destroyed_events[i]) goto next;
            }
            hr = IMFD3D12SynchronizationObjectCommands_EnqueueResourceReady(commands[i], queues[i]);
            ok(hr == S_OK, "Failed to mark buffer ready, hr %#lx.\n", hr);
            hr = IMFD3D12SynchronizationObjectCommands_EnqueueResourceReadyWait(commands[i], queues[i]);
            ok(hr == S_OK, "Failed to enqueue buffer-ready wait, hr %#lx.\n", hr);
            if (pass)
            {
                result = WaitForSingleObject(ready_events[i], 5000);
                ok(result == WAIT_OBJECT_0, "Reused buffer did not become ready, result %#lx.\n", result);
            }
            hr = ID3D12CommandQueue_Wait(queues[i], gates[i], pass + 1);
            ok(hr == S_OK, "Failed to gate consumer, hr %#lx.\n", hr);
            hr = IMFD3D12SynchronizationObjectCommands_EnqueueResourceRelease(commands[i], queues[i]);
            ok(hr == S_OK, "Failed to enqueue consumer release, hr %#lx.\n", hr);
            {
                IMFD3D12SynchronizationObject *object;

                hr = IMFD3D12SynchronizationObjectCommands_QueryInterface(commands[i],
                        &IID_IMFD3D12SynchronizationObject, (void **)&object);
                ok(hr == S_OK, "Failed to get final-release interface, hr %#lx.\n", hr);
                if (SUCCEEDED(hr))
                {
                    /* Allocator retirement must not overwrite the public event. */
                    hr = IMFD3D12SynchronizationObject_SignalEventOnFinalResourceRelease(object, release_events[i]);
                    ok(hr == S_OK, "Failed to register application's final-release event, hr %#lx.\n", hr);
                    IMFD3D12SynchronizationObject_Release(object);
                }
            }
            hr = ID3D12CommandQueue_Signal(queues[i], completions[i], pass + 1);
            ok(hr == S_OK, "Failed to enqueue consumer completion, hr %#lx.\n", hr);
        }
        IMFSample_Release(sample);
        sample = NULL;
        hr = IMFVideoSampleAllocatorCallback_GetFreeSampleCount(callback, &count);
        ok(hr == S_OK && !count, "Pending sample was recycled, count %ld, hr %#lx.\n", count, hr);
        hr = IMFVideoSampleAllocatorEx_AllocateSample(allocator, &new_sample);
        ok(hr == MF_E_SAMPLEALLOCATOR_EMPTY, "Pending pool was not empty, hr %#lx.\n", hr);
        if (SUCCEEDED(hr))
        {
            IMFSample_Release(new_sample);
            new_sample = NULL;
        }

        if (pass)
        {
            old_resource = (ULONG_PTR)resources[0];
            for (i = 0; i < buffer_count; ++i)
            {
                IMFD3D12SynchronizationObjectCommands_Release(commands[i]);
                commands[i] = NULL;
                ID3D12Resource_Release(resources[i]);
                resources[i] = NULL;
            }
            /* Tear down without waiting on an application's indefinitely
             * blocked consumer. Old callbacks must not enter the new pool. */
            before = GetTickCount();
            hr = IMFVideoSampleAllocatorEx_UninitializeSampleAllocator(allocator);
            ok(hr == S_OK, "Failed to uninitialize pending pool, hr %#lx.\n", hr);
            ok(GetTickCount() - before < 1000, "Uninitialization blocked on GPU consumers.\n");
            for (i = 0; i < buffer_count; ++i)
                ok(WaitForSingleObject(destroyed_events[i], 0) == WAIT_TIMEOUT,
                        "Pending consumer resource %u was destroyed.\n", i);
            hr = IMFVideoSampleAllocatorEx_InitializeSampleAllocatorEx(allocator, 1, 1, attributes, type);
            ok(hr == S_OK, "Failed to reinitialize pending pool, hr %#lx.\n", hr);
            if (FAILED(hr)) goto next;
            hr = IMFVideoSampleAllocatorEx_AllocateSample(allocator, &new_sample);
            ok(hr == S_OK, "Failed to allocate new-generation sample, hr %#lx.\n", hr);
            if (FAILED(hr)) goto next;
            hr = IMFSample_GetBufferByIndex(new_sample, 0, &buffer);
            ok(hr == S_OK, "Failed to get new-generation buffer, hr %#lx.\n", hr);
            if (SUCCEEDED(hr))
            {
                hr = IMFMediaBuffer_QueryInterface(buffer, &IID_IMFDXGIBuffer, (void **)&dxgi);
                ok(hr == S_OK, "Failed to get new-generation DXGI buffer, hr %#lx.\n", hr);
                if (SUCCEEDED(hr))
                {
                    hr = IMFDXGIBuffer_GetResource(dxgi, &IID_ID3D12Resource, (void **)&new_resource);
                    ok(hr == S_OK, "Failed to get new-generation resource, hr %#lx.\n", hr);
                    ok((ULONG_PTR)new_resource != old_resource, "Reinitialized pool reused a pending resource.\n");
                    if (new_resource) ID3D12Resource_Release(new_resource);
                    new_resource = NULL;
                    IMFDXGIBuffer_Release(dxgi);
                }
                IMFMediaBuffer_Release(buffer);
            }
        }
        for (i = 0; i < buffer_count; ++i)
        {
            hr = ID3D12Fence_Signal(gates[i], pass + 1);
            ok(hr == S_OK, "Failed to release consumer %u, hr %#lx.\n", i, hr);
            hr = ID3D12Fence_SetEventOnCompletion(completions[i], pass + 1, event);
            ok(hr == S_OK, "Failed to register consumer completion, hr %#lx.\n", hr);
            result = WaitForSingleObject(event, 5000);
            ok(result == WAIT_OBJECT_0, "Consumer did not complete, result %#lx.\n", result);
            result = WaitForSingleObject(release_events[i], 5000);
            ok(result == WAIT_OBJECT_0, "Allocator replaced application's release event, result %#lx.\n", result);
            if (i + 1 < buffer_count)
            {
                Sleep(50);
                hr = IMFVideoSampleAllocatorCallback_GetFreeSampleCount(callback, &count);
                ok(hr == S_OK && !count, "Partly released sample was recycled, count %ld, hr %#lx.\n", count, hr);
            }
        }
        if (pass)
        {
            for (i = 0; i < buffer_count; ++i)
            {
                result = WaitForSingleObject(destroyed_events[i], 5000);
                ok(result == WAIT_OBJECT_0, "Old resource %u was not destroyed, result %#lx.\n", i, result);
            }
            hr = IMFVideoSampleAllocatorCallback_GetFreeSampleCount(callback, &count);
            ok(hr == S_OK && !count, "Old generation entered new pool, count %ld, hr %#lx.\n", count, hr);
            IMFSample_Release(new_sample);
            new_sample = NULL;
        }
        wait_for_free_samples(callback, 1);
        if (!pass)
        {
            for (i = 0; i < buffer_count; ++i)
            {
                hr = IMFD3D12SynchronizationObjectCommands_SignalEventOnResourceReady(commands[i], ready_events[i]);
                ok(hr == S_OK, "Failed to check reset readiness, hr %#lx.\n", hr);
                result = WaitForSingleObject(ready_events[i], 20);
                ok(result == WAIT_TIMEOUT, "Recycled buffer retained ready state, result %#lx.\n", result);
            }
        }
next:
        /* Cleanup cannot leave a deliberately gated queue behind. */
        for (i = 0; i < buffer_count; ++i)
        {
            if (gates[i]) ID3D12Fence_Signal(gates[i], pass + 1);
            if (commands[i]) IMFD3D12SynchronizationObjectCommands_Release(commands[i]);
            if (resources[i]) ID3D12Resource_Release(resources[i]);
            commands[i] = NULL;
            resources[i] = NULL;
        }
        if (sample) IMFSample_Release(sample);
        if (new_sample) IMFSample_Release(new_sample);
        sample = new_sample = NULL;
        winetest_pop_context();
    }
done:
    for (i = 0; i < buffer_count; ++i)
    {
        if (gates[i]) ID3D12Fence_Signal(gates[i], 100);
        if (queues[i] && completions[i] && event)
        {
            ID3D12CommandQueue_Signal(queues[i], completions[i], 100);
            ID3D12Fence_SetEventOnCompletion(completions[i], 100, event);
            WaitForSingleObject(event, 5000);
        }
        if (commands[i]) IMFD3D12SynchronizationObjectCommands_Release(commands[i]);
        if (resources[i]) ID3D12Resource_Release(resources[i]);
        if (queues[i]) ID3D12CommandQueue_Release(queues[i]);
        if (gates[i]) ID3D12Fence_Release(gates[i]);
        if (completions[i]) ID3D12Fence_Release(completions[i]);
        if (ready_events[i]) CloseHandle(ready_events[i]);
        if (release_events[i]) CloseHandle(release_events[i]);
        if (destroyed_events[i]) CloseHandle(destroyed_events[i]);
    }
    IMFVideoSampleAllocatorEx_UninitializeSampleAllocator(allocator);
    if (event) CloseHandle(event);
    if (attributes) IMFAttributes_Release(attributes);
    if (callback) IMFVideoSampleAllocatorCallback_Release(callback);
}

static void check_allocator_initialization_failures(IMFVideoSampleAllocatorEx *allocator, IMFMediaType *type)
{
    IMFVideoSampleAllocatorCallback *callback;
    IMFAttributes *attributes;
    IMFSample *sample;
    BYTE clear_value = 0;
    LONG count;
    HRESULT hr;

    hr = IMFVideoSampleAllocatorEx_QueryInterface(allocator, &IID_IMFVideoSampleAllocatorCallback,
            (void **)&callback);
    ok(hr == S_OK, "Failed to get allocator callback, hr %#lx.\n", hr);
    if (FAILED(hr)) return;
    hr = MFCreateAttributes(&attributes, 1);
    ok(hr == S_OK, "Failed to create attributes, hr %#lx.\n", hr);
    if (FAILED(hr)) goto done;
    hr = IMFVideoSampleAllocatorEx_InitializeSampleAllocatorEx(allocator, 1, 1, NULL, type);
    ok(hr == S_OK, "Failed to initialize valid pool, hr %#lx.\n", hr);
    if (FAILED(hr)) goto done_attributes;

    /* A malformed candidate is rejected before replacing the current pool. */
    IMFAttributes_SetBlob(attributes, &MF_SA_D3D12_CLEAR_VALUE, &clear_value, sizeof(clear_value));
    hr = IMFVideoSampleAllocatorEx_InitializeSampleAllocatorEx(allocator, 1, 1, attributes, type);
    ok(hr == E_INVALIDARG, "Unexpected malformed clear value result %#lx.\n", hr);
    hr = IMFVideoSampleAllocatorCallback_GetFreeSampleCount(callback, &count);
    ok(hr == S_OK && count == 1, "Malformed candidate replaced valid pool, count %ld, hr %#lx.\n", count, hr);
    hr = IMFVideoSampleAllocatorEx_AllocateSample(allocator, &sample);
    ok(hr == S_OK, "Failed to allocate from preserved pool, hr %#lx.\n", hr);
    if (SUCCEEDED(hr)) IMFSample_Release(sample);
    wait_for_free_samples(callback, 1);

    /* Resource creation fails after committing the candidate configuration.
     * Failed population must leave no partly initialized sample pool. */
    IMFAttributes_DeleteItem(attributes, &MF_SA_D3D12_CLEAR_VALUE);
    IMFAttributes_SetUINT32(attributes, &MF_SA_D3D12_HEAP_TYPE, D3D12_HEAP_TYPE_UPLOAD);
    hr = IMFVideoSampleAllocatorEx_InitializeSampleAllocatorEx(allocator, 1, 1, attributes, type);
    ok(FAILED(hr), "Initialization accepted an upload-heap texture, hr %#lx.\n", hr);
    hr = IMFVideoSampleAllocatorCallback_GetFreeSampleCount(callback, &count);
    ok(hr == S_OK && !count, "Failed initialization left samples behind, count %ld, hr %#lx.\n", count, hr);
    hr = IMFVideoSampleAllocatorEx_AllocateSample(allocator, &sample);
    ok(hr == MF_E_NOT_INITIALIZED, "Unexpected failed-pool allocation result %#lx.\n", hr);
    if (SUCCEEDED(hr)) IMFSample_Release(sample);

    hr = IMFVideoSampleAllocatorEx_InitializeSampleAllocatorEx(allocator, 1, 1, NULL, type);
    ok(hr == S_OK, "Failed to recover after resource creation failure, hr %#lx.\n", hr);
    if (SUCCEEDED(hr))
    {
        hr = IMFVideoSampleAllocatorEx_AllocateSample(allocator, &sample);
        ok(hr == S_OK, "Failed to allocate after recovery, hr %#lx.\n", hr);
        if (SUCCEEDED(hr)) IMFSample_Release(sample);
        wait_for_free_samples(callback, 1);
    }
    IMFVideoSampleAllocatorEx_UninitializeSampleAllocator(allocator);
done_attributes:
    IMFAttributes_Release(attributes);
done:
    IMFVideoSampleAllocatorCallback_Release(callback);
}

static void check_external_producer_retirement(ID3D12Device *device, IMFVideoSampleAllocatorEx *allocator,
        IMFMediaType *type)
{
    IMFD3D12SynchronizationObjectCommands *commands = NULL;
    IMFD3D12SynchronizationObject *sync = NULL;
    IMFVideoSampleAllocatorCallback *callback = NULL;
    ID3D12Fence *gate = NULL, *completion = NULL;
    ID3D12Resource *resource = NULL;
    ID3D12CommandQueue *queue = NULL, *other_queue = NULL;
    IMFMediaBuffer *buffer = NULL;
    IMFDXGIBuffer *dxgi = NULL;
    IMFSample *sample = NULL, *next_sample = NULL;
    HANDLE event = NULL, ready_event = NULL, destroyed_event = NULL;
    ULONG_PTR old_resource, old_sample;
    unsigned int pass;
    DWORD result, before;
    LONG count;
    HRESULT hr;

    hr = IMFVideoSampleAllocatorEx_QueryInterface(allocator, &IID_IMFVideoSampleAllocatorCallback,
            (void **)&callback);
    ok(hr == S_OK, "Failed to get allocator callback, hr %#lx.\n", hr);
    if (FAILED(hr)) return;
    if (!(queue = create_queue(device))) goto done;
    if (!(other_queue = create_queue(device))) goto done;
    hr = ID3D12Device_CreateFence(device, 0, D3D12_FENCE_FLAG_NONE, &IID_ID3D12Fence, (void **)&gate);
    ok(hr == S_OK, "Failed to create producer gate, hr %#lx.\n", hr);
    if (FAILED(hr)) goto done;
    hr = ID3D12Device_CreateFence(device, 0, D3D12_FENCE_FLAG_NONE, &IID_ID3D12Fence, (void **)&completion);
    ok(hr == S_OK, "Failed to create producer completion fence, hr %#lx.\n", hr);
    if (FAILED(hr)) goto done;
    event = CreateEventW(NULL, FALSE, FALSE, NULL);
    ready_event = CreateEventW(NULL, TRUE, FALSE, NULL);
    ok(event && ready_event, "Failed to create producer events.\n");
    if (!event || !ready_event) goto done;
    hr = IMFVideoSampleAllocatorEx_InitializeSampleAllocatorEx(allocator, 1, 1, NULL, type);
    ok(hr == S_OK, "Failed to initialize producer pool, hr %#lx.\n", hr);
    if (FAILED(hr)) goto done;

    /* A sample never submitted to a producer has no readiness to wait for. */
    hr = IMFVideoSampleAllocatorEx_AllocateSample(allocator, &sample);
    ok(hr == S_OK, "Failed to allocate unused sample, hr %#lx.\n", hr);
    if (FAILED(hr)) goto done;
    IMFSample_Release(sample);
    sample = NULL;
    wait_for_free_samples(callback, 1);

    for (pass = 0; pass < 4; ++pass)
    {
        winetest_push_context("external producer scenario %u", pass);
        hr = IMFVideoSampleAllocatorEx_AllocateSample(allocator, &sample);
        ok(hr == S_OK, "Failed to allocate producer sample, hr %#lx.\n", hr);
        if (FAILED(hr)) goto next;
        old_sample = (ULONG_PTR)sample;
        hr = IMFSample_GetBufferByIndex(sample, 0, &buffer);
        ok(hr == S_OK, "Failed to get producer buffer, hr %#lx.\n", hr);
        if (FAILED(hr)) goto next;
        hr = IMFMediaBuffer_QueryInterface(buffer, &IID_IMFDXGIBuffer, (void **)&dxgi);
        ok(hr == S_OK, "Failed to get producer DXGI buffer, hr %#lx.\n", hr);
        if (FAILED(hr)) goto next;
        hr = IMFDXGIBuffer_GetResource(dxgi, &IID_ID3D12Resource, (void **)&resource);
        ok(hr == S_OK, "Failed to get producer texture, hr %#lx.\n", hr);
        if (FAILED(hr)) goto next;
        old_resource = (ULONG_PTR)resource;
        destroyed_event = track_resource_destruction(resource);
        ok(!!destroyed_event, "Failed to track producer texture.\n");
        if (!destroyed_event) goto next;
        hr = IMFDXGIBuffer_GetUnknown(dxgi, &MF_D3D12_SYNCHRONIZATION_OBJECT,
                &IID_IMFD3D12SynchronizationObjectCommands, (void **)&commands);
        ok(hr == S_OK, "Failed to get producer synchronization, hr %#lx.\n", hr);
        if (FAILED(hr)) goto next;
        hr = IMFD3D12SynchronizationObjectCommands_QueryInterface(commands,
                &IID_IMFD3D12SynchronizationObject, (void **)&sync);
        ok(hr == S_OK, "Failed to get producer reset interface, hr %#lx.\n", hr);
        if (FAILED(hr)) goto next;
        if (!pass && !strcmp(winetest_platform, "wine"))
        {
            /* A cancelled failed signal must not hold the sample forever or
             * consume another producer's pending-completion reference. */
            hr = IMFD3D12SynchronizationObjectCommands_EnqueueResourceReady(commands, &failing_queue);
            ok(hr == E_FAIL, "Unexpected injected producer failure %#lx.\n", hr);
        }
        ResetEvent(ready_event);
        hr = ID3D12CommandQueue_Wait(queue, gate, pass + 1);
        ok(hr == S_OK, "Failed to gate producer, hr %#lx.\n", hr);
        hr = IMFD3D12SynchronizationObjectCommands_EnqueueResourceReady(commands, queue);
        ok(hr == S_OK, "Failed to enqueue producer readiness, hr %#lx.\n", hr);
        hr = IMFD3D12SynchronizationObjectCommands_SignalEventOnResourceReady(commands, ready_event);
        ok(hr == S_OK, "Failed to register producer readiness, hr %#lx.\n", hr);
        hr = ID3D12CommandQueue_Signal(queue, completion, pass + 1);
        ok(hr == S_OK, "Failed to enqueue producer completion, hr %#lx.\n", hr);
        result = WaitForSingleObject(ready_event, 100);
        ok(result == WAIT_TIMEOUT, "Producer was not blocked, result %#lx.\n", result);
        /* Public final-release behavior is deliberately independent of Ready,
         * even though the allocator must not recycle a pending producer. */
        hr = IMFD3D12SynchronizationObject_SignalEventOnFinalResourceRelease(sync, event);
        ok(hr == S_OK, "Failed to register public final-release event, hr %#lx.\n", hr);
        result = WaitForSingleObject(event, 100);
        ok(result == WAIT_OBJECT_0, "Pending Ready changed public final release, result %#lx.\n", result);
        if (pass == 2)
        {
            hr = IMFD3D12SynchronizationObject_Reset(sync);
            ok(hr == S_OK, "Pending Ready prevented public reset, hr %#lx.\n", hr);
        }
        if (pass == 1 || pass == 2)
        {
            /* A completed newer producer cannot prove that the first queue
             * has finished, with or without a public Reset between them. */
            hr = IMFD3D12SynchronizationObjectCommands_EnqueueResourceReady(commands, other_queue);
            ok(hr == S_OK, "Failed to enqueue second producer readiness, hr %#lx.\n", hr);
            result = WaitForSingleObject(ready_event, 5000);
            ok(result == WAIT_OBJECT_0, "Second producer did not become ready, result %#lx.\n", result);
        }

        /* Drop the frame without submitting any consumer. Neither this test
         * nor an application-held interface should keep its texture alive. */
        IMFD3D12SynchronizationObjectCommands_Release(commands);
        commands = NULL;
        IMFD3D12SynchronizationObject_Release(sync);
        sync = NULL;
        ID3D12Resource_Release(resource);
        resource = NULL;
        IMFDXGIBuffer_Release(dxgi);
        dxgi = NULL;
        IMFMediaBuffer_Release(buffer);
        buffer = NULL;
        IMFSample_Release(sample);
        sample = NULL;
        Sleep(100);
        hr = IMFVideoSampleAllocatorCallback_GetFreeSampleCount(callback, &count);
        ok(hr == S_OK && !count, "Pending producer sample was recycled, count %ld, hr %#lx.\n", count, hr);
        hr = IMFVideoSampleAllocatorEx_AllocateSample(allocator, &next_sample);
        ok(hr == MF_E_SAMPLEALLOCATOR_EMPTY, "Pending producer pool was not empty, hr %#lx.\n", hr);
        if (SUCCEEDED(hr))
        {
            IMFSample_Release(next_sample);
            next_sample = NULL;
        }
        if (pass == 3)
        {
            before = GetTickCount();
            hr = IMFVideoSampleAllocatorEx_UninitializeSampleAllocator(allocator);
            ok(hr == S_OK, "Failed to uninitialize producer pool, hr %#lx.\n", hr);
            ok(GetTickCount() - before < 1000, "Uninitialization blocked on an external producer.\n");
            result = WaitForSingleObject(destroyed_event, 0);
            ok(result == WAIT_TIMEOUT, "Pending producer texture was destroyed, result %#lx.\n", result);
            hr = IMFVideoSampleAllocatorEx_InitializeSampleAllocatorEx(allocator, 1, 1, NULL, type);
            ok(hr == S_OK, "Failed to reinitialize producer pool, hr %#lx.\n", hr);
            if (FAILED(hr)) goto next;
            hr = IMFVideoSampleAllocatorEx_AllocateSample(allocator, &next_sample);
            ok(hr == S_OK, "Failed to allocate new-generation sample, hr %#lx.\n", hr);
            if (FAILED(hr)) goto next;
        }
        hr = ID3D12Fence_Signal(gate, pass + 1);
        ok(hr == S_OK, "Failed to unblock producer, hr %#lx.\n", hr);
        hr = ID3D12Fence_SetEventOnCompletion(completion, pass + 1, event);
        ok(hr == S_OK, "Failed to register producer completion, hr %#lx.\n", hr);
        result = WaitForSingleObject(event, 5000);
        ok(result == WAIT_OBJECT_0, "Producer did not complete, result %#lx.\n", result);
        result = WaitForSingleObject(ready_event, 5000);
        ok(result == WAIT_OBJECT_0, "Producer readiness was lost, result %#lx.\n", result);
        if (pass == 3)
        {
            result = WaitForSingleObject(destroyed_event, 5000);
            ok(result == WAIT_OBJECT_0, "Retired producer texture was not destroyed, result %#lx.\n", result);
            hr = IMFVideoSampleAllocatorCallback_GetFreeSampleCount(callback, &count);
            ok(hr == S_OK && !count, "Old producer entered new pool, count %ld, hr %#lx.\n", count, hr);
        }
        else
        {
            wait_for_free_samples(callback, 1);
            hr = IMFVideoSampleAllocatorEx_AllocateSample(allocator, &next_sample);
            ok(hr == S_OK, "Failed to allocate completed producer sample, hr %#lx.\n", hr);
            ok((ULONG_PTR)next_sample == old_sample, "Completed producer sample was replaced.\n");
            if (FAILED(hr)) goto next;
        }
        hr = IMFSample_GetBufferByIndex(next_sample, 0, &buffer);
        ok(hr == S_OK, "Failed to get recycled buffer, hr %#lx.\n", hr);
        if (FAILED(hr)) goto next;
        hr = IMFMediaBuffer_QueryInterface(buffer, &IID_IMFDXGIBuffer, (void **)&dxgi);
        ok(hr == S_OK, "Failed to get recycled DXGI buffer, hr %#lx.\n", hr);
        if (FAILED(hr)) goto next;
        hr = IMFDXGIBuffer_GetResource(dxgi, &IID_ID3D12Resource, (void **)&resource);
        ok(hr == S_OK, "Failed to get recycled texture, hr %#lx.\n", hr);
        if (SUCCEEDED(hr))
            ok(((ULONG_PTR)resource == old_resource) == (pass != 3), "Unexpected recycled resource %p.\n", resource);
next:
        ID3D12Fence_Signal(gate, pass + 1);
        if (commands) IMFD3D12SynchronizationObjectCommands_Release(commands);
        if (sync) IMFD3D12SynchronizationObject_Release(sync);
        if (resource) ID3D12Resource_Release(resource);
        if (dxgi) IMFDXGIBuffer_Release(dxgi);
        if (buffer) IMFMediaBuffer_Release(buffer);
        if (sample) IMFSample_Release(sample);
        if (next_sample) IMFSample_Release(next_sample);
        if (destroyed_event) CloseHandle(destroyed_event);
        commands = NULL;
        sync = NULL;
        resource = NULL;
        dxgi = NULL;
        buffer = NULL;
        sample = next_sample = NULL;
        destroyed_event = NULL;
        wait_for_free_samples(callback, 1);
        winetest_pop_context();
    }
done:
    if (gate) ID3D12Fence_Signal(gate, 100);
    if (queue && completion && event)
    {
        ID3D12CommandQueue_Signal(queue, completion, 100);
        ID3D12Fence_SetEventOnCompletion(completion, 100, event);
        WaitForSingleObject(event, 5000);
    }
    IMFVideoSampleAllocatorEx_UninitializeSampleAllocator(allocator);
    if (sample) IMFSample_Release(sample);
    if (queue) ID3D12CommandQueue_Release(queue);
    if (other_queue) ID3D12CommandQueue_Release(other_queue);
    if (gate) ID3D12Fence_Release(gate);
    if (completion) ID3D12Fence_Release(completion);
    if (event) CloseHandle(event);
    if (ready_event) CloseHandle(ready_event);
    IMFVideoSampleAllocatorCallback_Release(callback);
}

static void check_upload_uninitialization(IMFVideoSampleAllocatorEx *allocator, IMFMediaType *type)
{
    IMFMediaBuffer *buffer = NULL;
    IMF2DBuffer2 *buffer2d = NULL;
    ID3D12Resource *resource = NULL;
    IMFDXGIBuffer *dxgi = NULL;
    IMFSample *sample = NULL;
    HANDLE destroyed_event = NULL;
    BYTE *data = NULL;
    DWORD length, result;
    HRESULT hr;

    hr = IMFVideoSampleAllocatorEx_InitializeSampleAllocatorEx(allocator, 1, 1, NULL, type);
    ok(hr == S_OK, "Failed to initialize upload pool, hr %#lx.\n", hr);
    if (FAILED(hr)) return;
    hr = IMFVideoSampleAllocatorEx_AllocateSample(allocator, &sample);
    ok(hr == S_OK, "Failed to allocate upload sample, hr %#lx.\n", hr);
    if (FAILED(hr)) goto done;
    hr = IMFSample_GetBufferByIndex(sample, 0, &buffer);
    ok(hr == S_OK, "Failed to get upload buffer, hr %#lx.\n", hr);
    if (FAILED(hr)) goto done;
    hr = IMFMediaBuffer_QueryInterface(buffer, &IID_IMFDXGIBuffer, (void **)&dxgi);
    ok(hr == S_OK, "Failed to get upload DXGI buffer, hr %#lx.\n", hr);
    if (FAILED(hr)) goto done;
    hr = IMFDXGIBuffer_GetResource(dxgi, &IID_ID3D12Resource, (void **)&resource);
    ok(hr == S_OK, "Failed to get upload resource, hr %#lx.\n", hr);
    if (FAILED(hr)) goto done;
    destroyed_event = track_resource_destruction(resource);
    ok(!!destroyed_event, "Failed to track upload texture destruction.\n");
    if (!destroyed_event) goto done;
    hr = IMFMediaBuffer_QueryInterface(buffer, &IID_IMF2DBuffer2, (void **)&buffer2d);
    ok(hr == S_OK, "Failed to get upload 2D buffer, hr %#lx.\n", hr);
    if (FAILED(hr)) goto done;
    hr = IMF2DBuffer2_GetContiguousLength(buffer2d, &length);
    ok(hr == S_OK, "Failed to get upload length, hr %#lx.\n", hr);
    if (FAILED(hr)) goto done;
    if (!(data = malloc(length))) goto done;
    memset(data, 0x5a, length);
    hr = IMF2DBuffer2_ContiguousCopyFrom(buffer2d, data, length);
    ok(hr == S_OK, "Failed to submit upload, hr %#lx.\n", hr);
    /* Exercise teardown after upload without first draining it via readback.
     * The private copy queue may already have completed this small upload.
     * The external producer test covers teardown with a definitely pending queue. */
done:
    free(data);
    if (resource) ID3D12Resource_Release(resource);
    if (dxgi) IMFDXGIBuffer_Release(dxgi);
    if (buffer2d) IMF2DBuffer2_Release(buffer2d);
    if (buffer) IMFMediaBuffer_Release(buffer);
    if (sample) IMFSample_Release(sample);
    hr = IMFVideoSampleAllocatorEx_UninitializeSampleAllocator(allocator);
    ok(hr == S_OK, "Failed to uninitialize upload pool, hr %#lx.\n", hr);
    if (destroyed_event)
    {
        result = WaitForSingleObject(destroyed_event, 5000);
        ok(result == WAIT_OBJECT_0, "Upload texture was not destroyed, result %#lx.\n", result);
        CloseHandle(destroyed_event);
    }
}

static void test_sample_allocator(ID3D12Device *device)
{
    static const UINT64 sizes[] = {((UINT64)66 << 32) | 18, ((UINT64)1920 << 32) | 1088};
    IMFVideoSampleAllocatorEx *allocator;
    IMFDXGIDeviceManager *manager;
    IMFMediaType *type;
    IMFMediaBuffer *buffer;
    IMFSample *sample, *second_sample;
    unsigned int token, pass;
    HRESULT hr;

    hr = pMFCreateDXGIDeviceManager(&token, &manager);
    ok(hr == S_OK, "Failed to create device manager, hr %#lx.\n", hr);
    if (FAILED(hr)) return;
    hr = IMFDXGIDeviceManager_ResetDevice(manager, (IUnknown *)device, token);
    ok(hr == S_OK, "Failed to set D3D12 device, hr %#lx.\n", hr);
    if (FAILED(hr)) goto done;
    hr = pMFCreateVideoSampleAllocatorEx(&IID_IMFVideoSampleAllocatorEx, (void **)&allocator);
    ok(hr == S_OK, "Failed to create allocator, hr %#lx.\n", hr);
    if (FAILED(hr)) goto done;
    hr = IMFVideoSampleAllocatorEx_SetDirectXManager(allocator, (IUnknown *)manager);
    ok(hr == S_OK, "Failed to set manager, hr %#lx.\n", hr);
    if (FAILED(hr)) goto done_allocator;
    hr = MFCreateMediaType(&type);
    ok(hr == S_OK, "Failed to create media type, hr %#lx.\n", hr);
    if (FAILED(hr)) goto done_allocator;
    IMFMediaType_SetGUID(type, &MF_MT_MAJOR_TYPE, &MFMediaType_Video);
    IMFMediaType_SetGUID(type, &MF_MT_SUBTYPE, &MFVideoFormat_NV12);

    /* The game supplies a D3D12 manager without setting RESOURCE_VERSION. */
    for (pass = 0; pass < 4; ++pass)
    {
        IMFMediaType_SetUINT64(type, &MF_MT_FRAME_SIZE, sizes[pass / 2]);
        if (pass & 1) IMFMediaType_SetUINT32(type, &MF_MT_D3D_RESOURCE_VERSION, MF_D3D12_RESOURCE);
        else IMFMediaType_DeleteItem(type, &MF_MT_D3D_RESOURCE_VERSION);
        hr = IMFVideoSampleAllocatorEx_InitializeSampleAllocatorEx(allocator, 1, 2, NULL, type);
        ok(hr == S_OK, "Failed to initialize pass %u, hr %#lx.\n", pass, hr);
        if (FAILED(hr)) continue;
        hr = IMFVideoSampleAllocatorEx_AllocateSample(allocator, &sample);
        ok(hr == S_OK, "Failed to allocate sample, hr %#lx.\n", hr);
        if (FAILED(hr)) continue;
        hr = IMFSample_GetBufferByIndex(sample, 0, &buffer);
        ok(hr == S_OK, "Failed to get sample buffer, hr %#lx.\n", hr);
        if (SUCCEEDED(hr))
        {
            check_buffer_data(device, buffer, 2, 0);
            IMFMediaBuffer_Release(buffer);
        }
        check_sample_copy(device, type, sample);
        /* Exercise lazy allocation after exhausting the preallocated pool. */
        hr = IMFVideoSampleAllocatorEx_AllocateSample(allocator, &second_sample);
        ok(hr == S_OK, "Failed to allocate cold sample, hr %#lx.\n", hr);
        if (SUCCEEDED(hr))
        {
            check_sample_copy(device, type, second_sample);
            IMFSample_Release(second_sample);
        }
        IMFSample_Release(sample);
        hr = IMFVideoSampleAllocatorEx_UninitializeSampleAllocator(allocator);
        ok(hr == S_OK, "Failed to uninitialize allocator, hr %#lx.\n", hr);
    }
    /* An explicit request must not be silently replaced with another version. */
    IMFMediaType_SetUINT64(type, &MF_MT_FRAME_SIZE, sizes[0]);
    check_sample_reuse(device, allocator, type, 1);
    check_sample_reuse(device, allocator, type, 2);
    check_external_producer_retirement(device, allocator, type);
    check_allocator_initialization_failures(allocator, type);
    IMFMediaType_SetUINT64(type, &MF_MT_FRAME_SIZE, sizes[1]);
    check_upload_uninitialization(allocator, type);
    IMFMediaType_SetUINT32(type, &MF_MT_D3D_RESOURCE_VERSION, MF_D3D11_RESOURCE);
    hr = IMFVideoSampleAllocatorEx_InitializeSampleAllocatorEx(allocator, 1, 2, NULL, type);
    ok(FAILED(hr), "Explicit D3D11 request accepted a D3D12 device, hr %#lx.\n", hr);
    IMFMediaType_Release(type);
done_allocator:
    IMFVideoSampleAllocatorEx_Release(allocator);
done:
    IMFDXGIDeviceManager_Release(manager);
}

START_TEST(d3d12)
{
    ID3D12Device *device;
    HMODULE module, mf, mfplat;
    ULONG refcount;
    HRESULT hr;

    module = LoadLibraryA("d3d12.dll");
    pD3D12CreateDevice = (void *)GetProcAddress(module, "D3D12CreateDevice");
    if (!pD3D12CreateDevice)
    {
        win_skip("D3D12CreateDevice is unavailable.\n");
        if (module) FreeLibrary(module);
        return;
    }
    hr = pD3D12CreateDevice(NULL, D3D_FEATURE_LEVEL_11_0, &IID_ID3D12Device, (void **)&device);
    if (FAILED(hr))
    {
        skip("Failed to create D3D12 device, hr %#lx.\n", hr);
        FreeLibrary(module);
        return;
    }

    CoInitializeEx(NULL, COINIT_MULTITHREADED);
    mfplat = GetModuleHandleA("mfplat.dll");
    pMFCreateD3D12SynchronizationObject = (void *)GetProcAddress(mfplat, "MFCreateD3D12SynchronizationObject");
    pMFCreateDXGIDeviceManager = (void *)GetProcAddress(mfplat, "MFCreateDXGIDeviceManager");
    pMFCreateDXGISurfaceBuffer = (void *)GetProcAddress(mfplat, "MFCreateDXGISurfaceBuffer");
    pMFCreateVideoSampleAllocatorEx = (void *)GetProcAddress(mfplat, "MFCreateVideoSampleAllocatorEx");
    if (!pMFCreateD3D12SynchronizationObject)
    {
        win_skip("D3D12 Media Foundation synchronization is unavailable.\n");
        CoUninitialize();
        ID3D12Device_Release(device);
        FreeLibrary(module);
        return;
    }
    mf = LoadLibraryA("mf.dll");
    pMFCreateSampleCopierMFT = (void *)GetProcAddress(mf, "MFCreateSampleCopierMFT");
    if (!pMFCreateSampleCopierMFT) win_skip("MFCreateSampleCopierMFT is unavailable.\n");
    hr = MFStartup(MF_VERSION, MFSTARTUP_FULL);
    ok(hr == S_OK, "Failed to start Media Foundation, hr %#lx.\n", hr);
    test_failed_release_signal(device);
    if (pMFCreateDXGISurfaceBuffer)
    {
        test_producer_completion(device);
        test_surface_buffers(device);
    }
    else win_skip("MFCreateDXGISurfaceBuffer is unavailable.\n");
    if (pMFCreateDXGIDeviceManager && pMFCreateVideoSampleAllocatorEx) test_sample_allocator(device);
    else win_skip("D3D12 sample allocator exports are unavailable.\n");
    MFShutdown();
    CoUninitialize();
    if (mf) FreeLibrary(mf);
    refcount = ID3D12Device_Release(device);
    ok(!refcount, "Leaked device references, refcount %lu.\n", refcount);
    FreeLibrary(module);
}
