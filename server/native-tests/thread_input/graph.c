/*
 * Native allocation-failure checks for server thread input components.
 *
 * Copyright 2026 Erhan
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
 *
 * The runner inserts the production functions at the marked locations.
 * Allocation, object, window-owner and shared-memory dependencies are
 * deterministic stubs. This is not a full wineserver integration test.
 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wine/list.h>
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define STATUS_INVALID_PARAMETER 1
#define STATUS_NO_MEMORY 2
typedef unsigned int user_handle_t;
typedef struct {
    user_handle_t active, focus, capture, menu_owner, move_size, caret, cursor;
    unsigned int foreground;
    int cursor_count;
    unsigned int keystate_lock, keystate_serial;
    unsigned char keystate[256];
} input_shm_t;
typedef struct { unsigned char keystate[256]; } desktop_shm_t;
struct object { int refs, kind; };
struct desktop {
    struct object obj;
    desktop_shm_t *shared;
    struct winstation *winstation;
};
struct thread_input {
    struct object obj;
    struct desktop *desktop;
    struct list msg_list, attachments, queues;
    int needs_split;
    unsigned int user_time;
    unsigned char desktop_keystate[256];
    struct { unsigned int pointer_id; } pointer_state;
    input_shm_t *shared;
};
struct msg_queue {
    struct thread_input *input;
    struct list input_entry;
    unsigned int input_component;
    int cursor_count, keystate_lock;
};
struct attachment { struct list entry; struct msg_queue *queue_from, *queue_to; };
struct thread {
    struct object obj;
    struct msg_queue *queue;
    unsigned int desktop, desktop_users;
    struct process *process;
};
static struct desktop desktops[2];
static desktop_shm_t desktop_shared[2];
static struct thread owners[8];
static struct msg_queue queues[8];
static struct thread_input *live[128];
static int live_count, error, calls, fail_call, invalidations;
static const int thread_input_ops;
#define SHARED_WRITE_BEGIN(ptr, type) { type *shared = (ptr);
#define SHARED_WRITE_END }
static void set_error(int value) { error = value; }
static unsigned int get_error(void) { return error; }
static void *mem_alloc(size_t size)
{
    if (++calls == fail_call) { set_error(STATUS_NO_MEMORY); return NULL; }
    return malloc(size);
}
static void *grab_object(void *ptr)
{
    struct object *obj = ptr;
    assert(obj->refs > 0);
    ++obj->refs;
    return ptr;
}
static void release_object(void *ptr)
{
    struct object *obj = ptr;
    struct thread_input *input = ptr;
    struct attachment *attach, *next;
    int i;
    assert(obj->refs > 0);
    if (--obj->refs) return;
    assert(obj->kind == 1);
    assert(list_empty(&input->queues));
    assert(list_empty(&input->msg_list));
    LIST_FOR_EACH_ENTRY_SAFE(attach, next, &input->attachments, struct attachment, entry)
    { list_remove(&attach->entry); free(attach); }
    release_object(input->desktop);
    free(input->shared);
    for (i = 0; i < live_count; ++i) if (live[i] == input) break;
    assert(i < live_count);
    live[i] = live[--live_count];
    free(input);
}
static void *alloc_object(const int *ops)
{
    struct thread_input *input = mem_alloc(sizeof(*input));
    assert(ops == &thread_input_ops);
    if (!input) return NULL;
    memset(input, 0xcc, sizeof(*input));
    input->obj = (struct object){1, 1};
    assert(live_count < ARRAY_SIZE(live));
    live[live_count++] = input;
    return input;
}
static void *alloc_shared_object(size_t size) { return mem_alloc(size); }
static void set_caret_window(struct thread_input *input, input_shm_t *shared, user_handle_t window)
{ shared->caret = window; }
static struct thread *get_window_thread(user_handle_t window)
{
    if (!window || window > 8 || !owners[window - 1].queue) return NULL;
    return grab_object(&owners[window - 1]);
}
static void lock_input_keystate(struct thread_input *input) { ++input->shared->keystate_lock; }
static void unlock_input_keystate(struct thread_input *input)
{ assert(input->shared->keystate_lock); --input->shared->keystate_lock; }
static void invalidate_shared_object(input_shm_t *shared) { ++invalidations; }

/* ACTUAL_FUNCTIONS */

static void verify(void)
{
    struct attachment *attach;
    struct msg_queue *queue;
    int i, j, count;
    for (i = 0; i < live_count; ++i)
    {
        struct thread_input *input = live[i];
        int cursor = 0, locks = 0;
        count = 0;
        LIST_FOR_EACH_ENTRY(queue, &input->queues, struct msg_queue, input_entry)
        {
            assert(queue->input == input);
            cursor += queue->cursor_count;
            locks += !!queue->keystate_lock;
            ++count;
        }
        assert(count == input->obj.refs);
        assert(cursor == input->shared->cursor_count);
        assert(locks == input->shared->keystate_lock);
        LIST_FOR_EACH_ENTRY(attach, &input->attachments, struct attachment, entry)
        {
            assert(attach->queue_from->input == input && attach->queue_to->input == input);
            assert(!list_empty(&attach->queue_from->input_entry));
            assert(!list_empty(&attach->queue_to->input_entry));
        }
        if (input->shared->active) assert(owners[input->shared->active - 1].queue->input == input);
        if (input->shared->focus) assert(owners[input->shared->focus - 1].queue->input == input);
    }
    for (i = 0; i < 8; ++i)
    {
        if (!owners[i].queue) continue;
        count = 0;
        for (j = 0; j < live_count; ++j)
            LIST_FOR_EACH_ENTRY(queue, &live[j]->queues, struct msg_queue, input_entry)
                count += queue == &queues[i];
        assert(count == 1);
    }
}
static void setup(int count)
{
    int i;
    assert(!live_count);
    memset(owners, 0, sizeof(owners));
    memset(queues, 0, sizeof(queues));
    calls = fail_call = error = invalidations = 0;
    for (i = 0; i < 2; ++i)
    {
        desktops[i].obj = (struct object){1, 2};
        desktops[i].shared = &desktop_shared[i];
        memset(desktop_shared[i].keystate, 0x50 + i, sizeof(desktop_shared[i].keystate));
    }
    for (i = 0; i < count; ++i)
    {
        struct thread_input *input = create_thread_input(&desktops[0]);
        assert(input && !input->needs_split && !input->user_time);
        assert(input->pointer_state.pointer_id == ~0u);
        assert(!memcmp(input->desktop_keystate, desktop_shared[0].keystate, 256));
        assert(input->shared->keystate_serial == 1 && !input->shared->foreground);
        owners[i].obj = (struct object){1, 3};
        owners[i].queue = &queues[i];
        queues[i].input = input;
        queues[i].cursor_count = i - 2;
        queues[i].keystate_lock = i & 1;
        list_add_tail(&input->queues, &queues[i].input_entry);
        input->shared->cursor_count = queues[i].cursor_count;
        input->shared->keystate_lock = queues[i].keystate_lock;
    }
    verify();
}
/* The relevant production teardown sequence, with ancillary timer/message
 * cleanup omitted. Keep the queue's owning input reference until splitting. */
static void destroy_queue(int index)
{
    struct msg_queue *queue = owners[index].queue;
    input_shm_t *input_shm = queue->input->shared;
    owners[index].queue = NULL;
    /* ACTUAL_TEARDOWN_CORE */
    queue->input = NULL;
}
static void cleanup(void)
{
    int i;
    fail_call = 0;
    for (i = 0; i < 8; ++i) if (owners[i].queue) destroy_queue(i);
    assert(!live_count);
    assert(desktops[0].obj.refs == 1 && desktops[1].obj.refs == 1);
}
static void attach(int from, int to)
{ attach_thread_input(&queues[from], &queues[to]); verify(); }
static void detach(int from, int to)
{ detach_thread_input(&queues[from], &queues[to], &desktops[0]); verify(); }
static void duplicate_test(void)
{
    setup(3);
    attach(0, 1); attach(0, 1); attach(1, 0);
    detach(0, 1); assert(queues[0].input == queues[1].input);
    detach(1, 0); assert(queues[0].input == queues[1].input);
    detach(0, 1); assert(queues[0].input != queues[1].input);
    detach(0, 1); assert(error == STATUS_INVALID_PARAMETER);
    cleanup();
}
static void graph_test(void)
{
    setup(5);
    attach(0, 1); attach(1, 2); attach(2, 0);
    attach(3, 4); attach(0, 3);
    assert(live_count == 1);
    queues[0].input->shared->active = queues[0].input->shared->focus = 3;
    detach(0, 1);
    assert(live_count == 1);
    detach(1, 2);
    assert(queues[1].input != queues[0].input);
    assert(queues[0].input == queues[2].input && queues[3].input == queues[4].input);
    assert(queues[2].input->shared->focus == 3);
    detach(0, 3);
    assert(queues[0].input != queues[3].input);
    assert(queues[0].input == queues[2].input && queues[3].input == queues[4].input);
    cleanup();
}
static void rollback_test(void)
{
    int fail, i;
    for (fail = 1; fail <= 7; ++fail)
    {
        struct thread_input *before;
        setup(4);
        attach(0, 1); attach(0, 2); attach(0, 3);
        before = queues[0].input;
        before->shared->active = before->shared->focus = 1;
        calls = 0; fail_call = fail;
        detach_thread_input(&queues[0], NULL, &desktops[1]);
        if (error != STATUS_NO_MEMORY) fprintf(stderr, "desktop rollback fail point %d, calls %d, error %d\n", fail, calls, error);
        assert(error == STATUS_NO_MEMORY);
        for (i = 0; i < 4; ++i) assert(queues[i].input == before);
        assert(list_count(&before->attachments) == 3);
        assert(before->shared->active == 1 && before->shared->focus == 1);
        assert(live_count == 1 && desktops[1].obj.refs == 1);
        verify(); cleanup();
    }
    for (fail = 1; fail <= 3; ++fail)
    {
        struct thread_input *before;
        setup(3); attach(0, 1); attach(0, 2);
        before = queues[0].input;
        calls = 0; fail_call = fail;
        detach(0, 1);
        assert(error == STATUS_NO_MEMORY && live_count == 1);
        assert(queues[0].input == before && queues[1].input == before && queues[2].input == before);
        assert(list_count(&before->attachments) == 2);
        cleanup();
    }
    setup(2);
    calls = 0; fail_call = 1;
    attach_thread_input(&queues[0], &queues[1]);
    assert(error == STATUS_NO_MEMORY && queues[0].input != queues[1].input);
    verify(); cleanup();
}
static void teardown_test(void)
{
    int fail;
    setup(4); attach(0, 1); attach(0, 2); attach(0, 3);
    destroy_queue(0); verify();
    assert(live_count == 3);
    assert(queues[1].input != queues[2].input && queues[2].input != queues[3].input);
    cleanup();
    for (fail = 1; fail <= 5; ++fail)
    {
        struct thread_input *before;
        int old_invalidations;
        setup(4); attach(0, 1); attach(0, 2); attach(0, 3);
        before = queues[0].input;
        before->shared->active = before->shared->focus = 3;
        before->shared->foreground = 1;
        old_invalidations = invalidations;
        calls = 0; fail_call = fail; error = 99;
        destroy_queue(0); verify();
        assert(error == 99 && before->needs_split);
        assert(invalidations > old_invalidations);
        assert(live_count == 1 && list_empty(&before->attachments));
        assert(queues[1].input == before && queues[2].input == before && queues[3].input == before);

        /* A repeated repair failure keeps membership and the pending flag. */
        calls = 0; fail_call = 2; error = 0;
        assert(!repair_thread_input(before));
        assert(error == STATUS_NO_MEMORY && before->needs_split && live_count == 1);
        assert(queues[1].input == before && queues[2].input == before && queues[3].input == before);
        verify();

        fail_call = error = 0;
        assert(repair_thread_input(before));
        assert(!before->needs_split);
        verify();
        assert(live_count == 3);
        assert(queues[1].input != queues[2].input && queues[2].input != queues[3].input);
        assert(queues[2].input == before && before->shared->foreground);
        assert(before->shared->active == 3 && before->shared->focus == 3);
        assert(!queues[1].input->shared->foreground && !queues[3].input->shared->foreground);
        cleanup();
    }
}
static void deferred_attach_test(void)
{
    struct thread_input *before;
    setup(5); attach(0, 1); attach(0, 2); attach(0, 3);
    before = queues[0].input;
    calls = 0; fail_call = 1;
    destroy_queue(0);
    assert(before->needs_split);
    calls = 0; fail_call = 2; error = 0;
    attach_thread_input(&queues[1], &queues[4]);
    assert(error == STATUS_NO_MEMORY && before->needs_split);
    assert(list_empty(&before->attachments) && queues[1].input != queues[4].input);
    verify();
    fail_call = error = 0;
    attach(1, 4);
    assert(queues[1].input == queues[4].input);
    assert(queues[2].input != queues[1].input && queues[3].input != queues[1].input);
    assert(queues[2].input != queues[3].input);
    assert(live_count == 3);
    cleanup();
}
static void desktop_move_test(void)
{
    int i;
    setup(4); attach(0, 1); attach(0, 2); attach(3, 0);
    queues[0].input->shared->active = queues[0].input->shared->focus = 1;
    detach_thread_input(&queues[0], NULL, &desktops[1]);
    assert(!error && live_count == 4);
    assert(queues[0].input->desktop == &desktops[1]);
    assert(queues[0].input->shared->active == 1 && queues[0].input->shared->focus == 1);
    for (i = 1; i < 4; ++i)
    {
        assert(queues[i].input->desktop == &desktops[0]);
        assert(queues[i].input != queues[0].input && list_empty(&queues[i].input->attachments));
    }
    verify(); cleanup();

    /* An unattached queue still needs a new input on the requested desktop. */
    setup(1);
    detach_thread_input(&queues[0], NULL, &desktops[1]);
    assert(!error && queues[0].input->desktop == &desktops[1]);
    verify(); cleanup();
}
int main(void)
{
    duplicate_test(); graph_test(); rollback_test(); teardown_test();
    deferred_attach_test(); desktop_move_test();
    puts("Actual input graph functions: topology, ownership, refcounts, 11 attach/detach allocation failures, 5 exit allocation failures, repeated repair failure, deferred attach and desktop migration passed");
    return 0;
}
