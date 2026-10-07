#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Exercise the live DirectInput cleanup handoff without a Wine installation."""

from pathlib import Path
import subprocess
import tempfile


def function(source, signature):
    start = source.index(signature)
    opening = source.index("{", start)
    depth = 1
    end = opening + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


source = Path(__file__).resolve().parents[1].joinpath("dinput_main.c").read_text()
start = function(source, "void input_thread_start(void)")
remove = function(source, "void input_thread_remove_user(void)")
worker = function(source, "static DWORD WINAPI dinput_thread_proc( void *params )")
cleanup = worker[worker.rindex("    DestroyWindow( di_em_win );"):]

harness = r'''
#include <assert.h>
#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <stdio.h>

#define WINAPI
#define FALSE 0
#define INFINITE (~0u)
#define INPUT_THREAD_NOTIFY 1
#define NOTIFY_THREAD_STOP 0
#define TRACE(...) ((void)0)
#define ERR(...) ((void)0)
typedef unsigned int DWORD;
typedef void *HMODULE;
struct handle { pthread_mutex_t lock; pthread_cond_t cond; bool signaled, closed; };
typedef struct handle *HANDLE;
static struct handle handles[8];
static unsigned int handle_count, event_calls, fail_event, thread_calls;
static bool fail_thread, notified;
static pthread_mutex_t notify_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t notify_cond = PTHREAD_COND_INITIALIZER;
static pthread_mutex_t loader_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_t worker_thread;
static HANDLE dinput_thread, stop_event;
static unsigned int input_thread_user_count;
static int dinput_hook_crit;
static void *di_em_win;

static HANDLE new_handle(void)
{
    HANDLE h = &handles[handle_count++];
    assert(handle_count <= 8);
    pthread_mutex_init(&h->lock, NULL);
    pthread_cond_init(&h->cond, NULL);
    h->signaled = h->closed = false;
    return h;
}
static HANDLE CreateEventW(void *sa, int manual, int value, void *name)
{
    (void)sa; (void)manual; (void)value; (void)name;
    if (++event_calls == fail_event) return NULL;
    return new_handle();
}
#define GetLastError() 8u
static int CloseHandle(HANDLE h)
{
    if (!h) return 0;
    assert(!h->closed);
    h->closed = true;
    return 1;
}
static int SetEvent(HANDLE h)
{
    assert(h && !h->closed);
    pthread_mutex_lock(&h->lock);
    h->signaled = true;
    pthread_cond_signal(&h->cond);
    pthread_mutex_unlock(&h->lock);
    return 1;
}
static DWORD WaitForSingleObject(HANDLE h, DWORD timeout)
{
    (void)timeout;
    assert(h && !h->closed);
    pthread_mutex_lock(&h->lock);
    while (!h->signaled) pthread_cond_wait(&h->cond, &h->lock);
    pthread_mutex_unlock(&h->lock);
    return 0;
}
static void DestroyWindow(void *hwnd) { (void)hwnd; }
static void FreeLibraryAndExitThread(HMODULE module, DWORD code)
{
    (void)module; (void)code;
    pthread_mutex_lock(&loader_lock);
    pthread_mutex_unlock(&loader_lock);
    pthread_exit(NULL);
}
static DWORD run_cleanup(void)
{
    HMODULE this_module = (void *)1;
''' + cleanup + r'''
static void *worker_main(void *arg)
{
    (void)arg;
    pthread_mutex_lock(&notify_lock);
    while (!notified) pthread_cond_wait(&notify_cond, &notify_lock);
    pthread_mutex_unlock(&notify_lock);
    run_cleanup();
    return NULL;
}
static DWORD dinput_thread_proc(void *arg) { (void)arg; return 0; }
static HANDLE CreateThread(void *sa, size_t size, DWORD (*fn)(void *), void *arg, int flags, void *id)
{
    (void)sa; (void)size; (void)fn; (void)flags; (void)id;
    ++thread_calls;
    if (fail_thread) return NULL;
    dinput_thread = new_handle();
    di_em_win = (void *)1;
    assert(!pthread_create(&worker_thread, NULL, worker_main, NULL));
    SetEvent(arg);
    return dinput_thread;
}
static void EnterCriticalSection(int *cs) { (void)cs; }
static void LeaveCriticalSection(int *cs) { (void)cs; }
static void SendMessageW(void *hwnd, int msg, int wp, int lp)
{
    (void)hwnd; (void)msg; (void)wp; (void)lp;
    pthread_mutex_lock(&notify_lock);
    notified = true;
    pthread_cond_signal(&notify_cond);
    pthread_mutex_unlock(&notify_lock);
}
''' + start + '\n' + remove + r'''
static void reset(void)
{
    unsigned int i;
    for (i = 0; i < handle_count; ++i)
    {
        assert(handles[i].closed);
        pthread_mutex_destroy(&handles[i].lock);
        pthread_cond_destroy(&handles[i].cond);
    }
    handle_count = event_calls = thread_calls = fail_event = 0;
    fail_thread = notified = false;
    stop_event = dinput_thread = NULL;
}
int main(void)
{
    unsigned int i;
    fail_event = 1;
    input_thread_start();
    assert(!dinput_thread && !thread_calls);
    reset();
    fail_event = 2;
    input_thread_start();
    assert(!dinput_thread && !thread_calls);
    reset();
    fail_thread = true;
    input_thread_start();
    assert(!dinput_thread && !stop_event && thread_calls == 1);
    reset();
    for (i = 0; i < 100; ++i)
    {
        input_thread_user_count = 1;
        input_thread_start();
        assert(dinput_thread && stop_event && event_calls == 2);
        pthread_mutex_lock(&loader_lock);
        input_thread_remove_user();
        assert(!dinput_thread && !stop_event && !input_thread_user_count);
        pthread_mutex_unlock(&loader_lock);
        pthread_join(worker_thread, NULL);
        reset();
    }
    puts("PASS: loader-lock cleanup, repeated lifetimes and allocation failures");
}
'''

with tempfile.TemporaryDirectory(prefix="dinput-cleanup-") as temp:
    path = Path(temp)
    test = path / "test.c"
    test.write_text(harness)
    binary = path / "test"
    subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-pthread",
                    "-fsanitize=address,undefined", "-g", str(test), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True, timeout=20)
