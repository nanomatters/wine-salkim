/*
 * Native tests for parser read caching
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
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

/* Exercise the production read_thread with deterministic COM and WG providers. */
#include <wchar.h>
#include "wine/debug.h"
#undef TRACE
#undef WARN
#undef ERR
#undef FIXME
static inline void discard_trace(const char *format, ...)
{
    (void)format;
}
#define TRACE(...) do { if (0) discard_trace(__VA_ARGS__); } while (0)
#define WARN(...) ((void)0)
#define ERR(...) ((void)0)
#define FIXME(...) ((void)0)
#include "../quartz_parser.c"

struct read_request
{
    uint64_t offset;
    uint32_t size;
    bool success;
};

struct sync_step
{
    uint64_t offset;
    LONG size;
    HRESULT result;
    LONG actual_size;
};

struct test_context
{
    struct parser parser;
    const struct read_request *requests;
    const struct sync_step *reads;
    unsigned int request_count, read_count;
    unsigned int requested, pushed, synced;
    uint64_t file_size;
};

static struct test_context *test;

static BYTE source_byte(uint64_t offset)
{
    return offset % 251;
}

static HRESULT WINAPI mock_length(IAsyncReader *reader, LONGLONG *total, LONGLONG *available)
{
    assert(reader == test->parser.reader);
    *total = *available = test->file_size;
    return S_OK;
}

static HRESULT WINAPI mock_sync_read(IAsyncReader *reader, LONGLONG offset, LONG size, BYTE *buffer)
{
    const struct sync_step *read;
    LONG i;

    assert(reader == test->parser.reader);
    assert(test->synced < test->read_count);
    read = test->reads + test->synced++;
    assert((uint64_t)offset == read->offset);
    assert(size == read->size);
    assert(read->actual_size >= 0 && read->actual_size <= size);
    for (i = 0; i < read->actual_size; ++i)
        buffer[i] = source_byte(offset + i);
    return read->result;
}

static const IAsyncReaderVtbl reader_vtbl =
{
    .SyncRead = mock_sync_read,
    .Length = mock_length,
};

bool array_reserve(void **data, size_t *capacity, size_t count, size_t size)
{
    void *next;
    size_t old_size = *capacity * size;

    if (count <= *capacity) return true;
    assert((next = malloc(count * size)));
    memcpy(next, *data, old_size);
    memset((BYTE *)next + old_size, 0xa5, count * size - old_size);
    free(*data);
    *data = next;
    *capacity = count;
    return true;
}

bool wg_parser_get_next_read_offset(wg_parser_t parser, uint64_t *offset, uint32_t *size)
{
    const struct read_request *request;

    assert(parser == (wg_parser_t)(uintptr_t)test);
    assert(test->requested == test->pushed);
    assert(test->requested < test->request_count);
    request = test->requests + test->requested++;
    *offset = request->offset;
    *size = request->size;
    return true;
}

void wg_parser_push_data(wg_parser_t parser, const void *data, uint32_t size)
{
    const struct read_request *request;
    uint64_t available;
    uint32_t expected_size, i;

    assert(parser == (wg_parser_t)(uintptr_t)test);
    assert(test->pushed < test->requested);
    request = test->requests + test->pushed++;
    available = request->offset < test->file_size ? test->file_size - request->offset : 0;
    expected_size = available < request->size ? available : request->size;
    assert(size == expected_size);
    assert((data != NULL) == request->success);
    if (data)
        for (i = 0; i < size; ++i)
            assert(((const BYTE *)data)[i] == source_byte(request->offset + i));
    if (test->pushed == test->request_count)
        test->parser.sink_connected = false;
}

static void run_test(const char *name, uint64_t file_size,
        const struct read_request *requests, unsigned int request_count,
        const struct sync_step *reads, unsigned int read_count)
{
    IAsyncReader reader = {&reader_vtbl};
    struct test_context context = {0};

    context.requests = requests;
    context.reads = reads;
    context.request_count = request_count;
    context.read_count = read_count;
    context.file_size = file_size;
    context.parser.reader = &reader;
    context.parser.wg_parser = (wg_parser_t)(uintptr_t)&context;
    context.parser.sink_connected = true;
    test = &context;
    assert(!read_thread(&context.parser));
    assert(context.synced == read_count);
    assert(context.pushed == request_count);
    printf("%s passed (%u reads for %u requests)\n", name, read_count, request_count);
}

#define RUN_TEST(name, length, requests, reads) \
    run_test(name, length, requests, ARRAY_SIZE(requests), reads, ARRAY_SIZE(reads))

int main(void)
{
    const struct read_request hits[] =
    {
        {0, 16, true}, {0, 8, true}, {4, 12, true}, {8, 16, true},
        {2, 10, true}, {40, 16, true}, {120, 16, true}, {128, 1, true},
        {UINT64_MAX, 16, true},
    };
    const struct sync_step hit_reads[] =
    {
        {0, 16, S_OK, 16}, {16, 8, S_OK, 8}, {2, 10, S_OK, 10},
        {40, 16, S_OK, 16}, {120, 8, S_OK, 8},
    };
    const struct read_request failure[] = {{0, 16, true}, {4, 16, false}, {4, 8, true}};
    const struct sync_step failed_reads[] =
    {
        {0, 16, S_OK, 16}, {16, 4, E_FAIL, 2}, {4, 8, S_OK, 8},
    };
    const struct read_request partial[] = {{0, 16, true}, {4, 16, false}, {4, 8, true}};
    const struct sync_step partial_reads[] =
    {
        {0, 16, S_OK, 16}, {16, 4, S_FALSE, 2}, {4, 8, S_OK, 8},
    };
    const struct read_request initial_partial[] = {{0, 16, false}, {0, 8, true}};
    const struct sync_step initial_partial_reads[] = {{0, 16, S_FALSE, 8}, {0, 8, S_OK, 8}};
    const struct read_request grow[] = {{0, 32, true}, {16, 8192, true}, {4096, 4096, true}};
    const struct sync_step grow_reads[] = {{0, 32, S_OK, 32}, {32, 8176, S_OK, 8176}};
    const struct read_request same_offset_fail[] = {{0, 16, true}, {0, 20, false}, {0, 8, true}};
    const struct sync_step same_offset_reads[] = {{0, 16, S_OK, 16}, {16, 4, E_FAIL, 2}, {0, 8, S_OK, 8}};

    RUN_TEST("cache hits, extension, backward seek, EOF", 128, hits, hit_reads);
    RUN_TEST("overlap extension error", 128, failure, failed_reads);
    RUN_TEST("overlap partial read", 128, partial, partial_reads);
    RUN_TEST("uncached partial read", 128, initial_partial, initial_partial_reads);
    RUN_TEST("grow buffer with overlapping cache", 10000, grow, grow_reads);
    RUN_TEST("same offset extension error", 128, same_offset_fail, same_offset_reads);
    return 0;
}
