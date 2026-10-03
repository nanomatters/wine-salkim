/*
 * Native transactional checks for the server desktop switch handler.
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
 * The runner appends this fixture to graph.c and inserts the production
 * set_thread_desktop handler, changing only its request-handler signature.
 * Desktop handles, membership and default-desktop dependencies are stubs.
 */
#include <stdint.h>
#define STATUS_ACCESS_DENIED 3
#define STATUS_DEVICE_BUSY 4
#define STATUS_INVALID_HANDLE 5
struct winstation { struct object obj; };
struct process { unsigned int desktop; };
struct desktop_request { unsigned int handle; };
struct desktop_reply { uintptr_t locator; };
static struct winstation station, foreign_station;
static struct process process;
static struct thread *current;
static struct desktop_request request, *req = &request;
static struct desktop_reply response, *reply = &response;
static unsigned int desktop_members[2];
static unsigned int add_calls, remove_calls, default_calls;
static int station_failure;

static void clear_error(void) { error = 0; }
static struct winstation *get_process_winstation(struct process *process, unsigned int access)
{
    if (station_failure) { set_error(STATUS_ACCESS_DENIED); return NULL; }
    return grab_object(&station);
}
static struct desktop *get_desktop_obj(struct process *process, unsigned int handle, unsigned int access)
{
    if (!handle || handle > 2) { set_error(STATUS_INVALID_HANDLE); return NULL; }
    return grab_object(&desktops[handle - 1]);
}
static void add_desktop_thread(struct desktop *desktop, struct thread *thread)
{
    unsigned int index = desktop - desktops;
    unsigned int bit = 1u << (thread - owners);
    assert(index < 2 && !(desktop_members[index] & bit));
    desktop_members[index] |= bit;
    ++add_calls;
}
static void remove_desktop_thread(struct desktop *desktop, struct thread *thread)
{
    unsigned int index = desktop - desktops;
    unsigned int bit = 1u << (thread - owners);
    assert(index < 2 && (desktop_members[index] & bit));
    desktop_members[index] &= ~bit;
    ++remove_calls;
}
static uintptr_t get_shared_object_locator(desktop_shm_t *shared) { return (uintptr_t)shared; }
static void set_process_default_desktop(struct process *process, struct desktop *desktop, unsigned int handle)
{
    process->desktop = handle;
    ++default_calls;
}

/* ACTUAL_DESKTOP_HANDLER */

static void desktop_setup(int count)
{
    int i;
    setup(count);
    station.obj = foreign_station.obj = (struct object){1, 4};
    process.desktop = 0;
    current = &owners[0];
    req->handle = 2;
    reply->locator = 0;
    desktop_members[0] = (1u << count) - 1;
    desktop_members[1] = 0;
    add_calls = remove_calls = default_calls = station_failure = 0;
    for (i = 0; i < 2; ++i) desktops[i].winstation = &station;
    for (i = 0; i < count; ++i)
    {
        owners[i].desktop = 1;
        owners[i].process = &process;
    }
}
static void desktop_verify_references(void)
{
    int i, counts[2] = {1, 1};
    for (i = 0; i < live_count; ++i)
    {
        unsigned int index = live[i]->desktop - desktops;
        assert(index < 2);
        ++counts[index];
    }
    assert(desktops[0].obj.refs == counts[0] && desktops[1].obj.refs == counts[1]);
    assert(station.obj.refs == 1 && foreign_station.obj.refs == 1);
    for (i = 0; i < 8; ++i) if (owners[i].queue) assert(owners[i].obj.refs == 1);
    verify();
}
static void assert_transaction_unchanged(struct thread_input *before, unsigned int members)
{
    int i;
    assert(current->desktop == 1 && desktop_members[0] == members && !desktop_members[1]);
    assert(!add_calls && !remove_calls && !default_calls && !process.desktop);
    assert(!reply->locator);
    for (i = 0; i < 4; ++i) assert(queues[i].input == before);
    assert(list_count(&before->attachments) == 3);
    assert(before->shared->active == 2 && before->shared->focus == 2);
    desktop_verify_references();
}
static void desktop_busy_test(void)
{
    struct thread_input *before;
    desktop_setup(4); attach(0, 1); attach(0, 2); attach(3, 0);
    before = queues[0].input;
    before->shared->active = before->shared->focus = 2;
    current->desktop_users = 1;
    calls = 0;
    test_set_thread_desktop();
    assert(error == STATUS_DEVICE_BUSY && !calls);
    assert_transaction_unchanged(before, 15);
    cleanup();
}
static void desktop_rollback_test(void)
{
    int fail;
    for (fail = 1; fail <= 7; ++fail)
    {
        struct thread_input *before;
        desktop_setup(4); attach(0, 1); attach(0, 2); attach(3, 0);
        before = queues[0].input;
        before->shared->active = before->shared->focus = 2;
        calls = 0; fail_call = fail;
        test_set_thread_desktop();
        assert(error == STATUS_NO_MEMORY);
        assert_transaction_unchanged(before, 15);

        /* Retrying the same request after memory recovers commits once. */
        fail_call = error = 0;
        test_set_thread_desktop();
        assert(!error && current->desktop == 2);
        assert(desktop_members[0] == 14 && desktop_members[1] == 1);
        assert(add_calls == 1 && remove_calls == 1 && default_calls == 1 && process.desktop == 2);
        assert(reply->locator == (uintptr_t)desktops[1].shared);
        assert(queues[0].input->desktop == &desktops[1]);
        assert(queues[1].input->shared->active == 2 && queues[1].input->shared->focus == 2);
        assert(live_count == 4);
        desktop_verify_references(); cleanup();
    }
}
static void desktop_same_test(void)
{
    struct thread_input *before;
    desktop_setup(2); attach(0, 1);
    before = queues[0].input;
    current->desktop_users = 1;
    req->handle = 1;
    calls = 0;
    test_set_thread_desktop();
    assert(!error && !calls && queues[0].input == before && queues[1].input == before);
    assert(current->desktop == 1 && desktop_members[0] == 3 && !desktop_members[1]);
    assert(!add_calls && !remove_calls && default_calls == 1 && process.desktop == 1);
    assert(reply->locator == (uintptr_t)desktops[0].shared);
    desktop_verify_references(); cleanup();
}
static void desktop_validation_test(void)
{
    desktop_setup(1);
    desktops[1].winstation = &foreign_station;
    test_set_thread_desktop();
    assert(error == STATUS_ACCESS_DENIED && current->desktop == 1);
    assert(desktop_members[0] == 1 && !desktop_members[1] && !default_calls);
    desktop_verify_references(); cleanup();

    desktop_setup(1); req->handle = 3;
    test_set_thread_desktop();
    assert(error == STATUS_INVALID_HANDLE && current->desktop == 1);
    assert(desktop_members[0] == 1 && !desktop_members[1] && !default_calls);
    desktop_verify_references(); cleanup();

    desktop_setup(1); station_failure = 1;
    test_set_thread_desktop();
    assert(error == STATUS_ACCESS_DENIED && current->desktop == 1);
    desktop_verify_references(); cleanup();
}
int main(void)
{
    desktop_busy_test(); desktop_rollback_test(); desktop_same_test(); desktop_validation_test();
    puts("Actual desktop handler: busy and all 7 allocation failures preserve desktop, lists, default and input graph, successful retry, same-desktop and validation passed");
    return 0;
}
