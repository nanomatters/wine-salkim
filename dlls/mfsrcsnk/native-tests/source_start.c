/*
 * Native tests for media source start validation
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
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library. If not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stddef.h>

/* Execute the production function with deterministic descriptor providers. */
typedef int32_t HRESULT;
typedef int32_t NTSTATUS;
typedef uint32_t DWORD;
typedef bool BOOL;
typedef int GUID;
typedef struct descriptor IMFPresentationDescriptor;
typedef struct stream_descriptor IMFStreamDescriptor;
typedef struct { int vt; struct { int64_t QuadPart; } hVal; } PROPVARIANT;
#define S_OK 0
#define E_FAIL ((HRESULT)0x80004005)
#define MF_E_SHUTDOWN ((HRESULT)0xc00d3e85)
#define FAILED(hr) ((hr) < 0)
#define FALSE false
#define VT_EMPTY 0
#define VT_I8 20
#define SOURCE_STOPPED 0
#define SOURCE_RUNNING 1
#define SOURCE_SHUTDOWN 2
#define MESourceStarted 1
#define MESourceSeeked 2
#define TRACE(...) ((void)0)
#define WARN(...) ((void)0)
struct stream_descriptor { DWORD id; };
struct descriptor { HRESULT result; DWORD count; BOOL selected; struct stream_descriptor stream; };
struct media_stream { BOOL eos, active; };
struct media_source { int state; DWORD stream_count; struct media_stream **streams; int *stream_map;
    int winedmo_demuxer, queue; };
static unsigned int count_calls, stream_calls, event_calls;

static HRESULT IMFPresentationDescriptor_GetStreamDescriptorCount(struct descriptor *descriptor, DWORD *count)
{
    ++count_calls;
    *count = descriptor->count;
    return descriptor->result;
}

static HRESULT IMFPresentationDescriptor_GetStreamDescriptorByIndex(struct descriptor *descriptor,
        DWORD index, BOOL *selected, struct stream_descriptor **stream)
{
    assert(index < descriptor->count);
    *selected = descriptor->selected;
    *stream = &descriptor->stream;
    return S_OK;
}

static HRESULT IMFStreamDescriptor_GetStreamIdentifier(struct stream_descriptor *descriptor, DWORD *id)
{
    *id = descriptor->id;
    return S_OK;
}

static void IMFStreamDescriptor_Release(struct stream_descriptor *descriptor) { (void)descriptor; }

static void media_stream_start(struct media_stream *stream, int index, PROPVARIANT *position)
{
    (void)stream;
    (void)index;
    (void)position;
    ++stream_calls;
}

static NTSTATUS winedmo_demuxer_seek(int demuxer, int64_t position)
{
    (void)demuxer;
    (void)position;
    return 0;
}

static void queue_media_event_value(int queue, int type, PROPVARIANT *position)
{
    (void)queue;
    (void)type;
    (void)position;
    ++event_calls;
}

#include "source_start.h"

int main(void)
{
    struct media_stream stream = {true, true}, *streams[] = {&stream};
    int map[] = {0};
    struct media_source source = {SOURCE_STOPPED, 1, streams, map, 0, 0};
    struct descriptor descriptor = {E_FAIL, 1, true, {1}};
    PROPVARIANT position = {VT_EMPTY, {0}};

    assert(media_source_start(&source, &descriptor, NULL, &position) == E_FAIL);
    assert(source.state == SOURCE_STOPPED && stream.active && stream.eos);
    assert(position.vt == VT_EMPTY);
    assert(count_calls == 1 && !stream_calls && !event_calls);

    source.state = SOURCE_SHUTDOWN;
    assert(media_source_start(&source, &descriptor, NULL, &position) == MF_E_SHUTDOWN);
    assert(count_calls == 1);

    source.state = SOURCE_STOPPED;
    descriptor.result = S_OK;
    descriptor.stream.id = 0;
    assert(media_source_start(&source, &descriptor, NULL, &position) == S_OK);
    assert(!stream.active && !stream.eos && source.state == SOURCE_RUNNING);
    assert(stream_calls == 1 && event_calls == 1);

    descriptor.stream.id = 1;
    assert(media_source_start(&source, &descriptor, NULL, &position) == S_OK);
    assert(stream.active && stream_calls == 2 && event_calls == 2);
    puts("Media source failure, shutdown, zero-id and valid-id cases passed.");
}
