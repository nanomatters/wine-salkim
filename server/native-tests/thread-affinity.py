#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Check the live server affinity branches with controlled host failures."""

from pathlib import Path
import subprocess
import tempfile


source = Path(__file__).resolve().parents[1].joinpath("thread.c").read_text()
start = source.index("    if (req->mask & SET_THREAD_INFO_AFFINITY)")
end = source.index("    if (req->mask & SET_THREAD_INFO_TOKEN)", start)
body = source[start:end]

harness = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#define SET_THREAD_INFO_AFFINITY 4
#define SET_THREAD_INFO_GROUP_AFFINITY 8
#define TERMINATED 1
#define STATUS_INVALID_PARAMETER 2
#define STATUS_THREAD_IS_TERMINATING 3
#define HOST_ERROR 4
typedef uint64_t affinity_t;
struct process { affinity_t affinity; };
struct thread { struct process *process; affinity_t affinity; int state; };
struct set_thread_info_request { unsigned int mask; affinity_t affinity, system_affinity; };
static int error, calls;
static bool host_failure;
static void set_error(int value) { error = value; }
static void file_set_error(void) { error = HOST_ERROR; }
static int set_thread_affinity(struct thread *thread, affinity_t affinity)
{
    ++calls;
    if (host_failure) return -1;
    thread->affinity = affinity;
    return 0;
}
static void apply(struct thread *thread, const struct set_thread_info_request *req)
{
''' + body + r'''
}
static void check(unsigned int flag, affinity_t process_mask, affinity_t system_mask,
                  affinity_t requested, bool terminated, bool fail)
{
    struct process process = {process_mask};
    struct thread thread = {&process, UINT64_C(0x12345678), terminated};
    struct set_thread_info_request req = {flag, requested, system_mask};
    affinity_t expected, process_expected = process_mask;
    int expected_error;
    bool valid;
    if (flag & SET_THREAD_INFO_AFFINITY)
    {
        expected = requested & system_mask;
        valid = expected && (expected & process_mask) == expected;
    }
    else
    {
        expected = requested ? requested : process_mask & system_mask;
        valid = expected && !(expected & ~system_mask);
        if (valid && !terminated && !fail && (expected & ~process_mask))
            process_expected = UINT64_MAX;
    }
    expected_error = !valid ? STATUS_INVALID_PARAMETER : terminated ? STATUS_THREAD_IS_TERMINATING
                     : fail ? HOST_ERROR : 0;
    error = calls = 0;
    host_failure = fail;
    apply(&thread, &req);
    assert(error == expected_error);
    assert(calls == (valid && !terminated));
    assert(process.affinity == process_expected);
    assert(thread.affinity == (expected_error ? UINT64_C(0x12345678) : expected));
}
int main(void)
{
    unsigned int p, s, r, state, flag;
    unsigned long cases = 0;
    for (p = 0; p < 16; ++p)
        for (s = 0; s < 16; ++s)
            for (r = 0; r < 32; ++r)
                for (state = 0; state < 4; ++state)
                    for (flag = 4; flag <= 12; flag += 4)
                    {
                        check(flag, p, s, r, state & 1, state & 2);
                        ++cases;
                    }
    check(8, 1, UINT64_MAX, UINT64_C(1) << 63, false, false);
    check(8, 1, UINT64_MAX, UINT64_C(1) << 63, false, true);
    check(8, UINT64_MAX, UINT64_MAX, 0, false, false);
    check(4, UINT64_MAX, UINT64_MAX, UINT64_MAX, false, false);
    printf("PASS: %lu affinity cases plus 64-bit boundaries\n", cases);
}
'''

with tempfile.TemporaryDirectory(prefix="wine-affinity-") as temp:
    path = Path(temp)
    test = path / "test.c"
    test.write_text(harness)
    binary = path / "test"
    subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                    "-fsanitize=address,undefined", "-g", str(test), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True, timeout=20)
