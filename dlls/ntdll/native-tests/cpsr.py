#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Check the actual ARM context merges preserve host-owned CPSR bits."""

from pathlib import Path
import re
import subprocess
import tempfile


ntdll = Path(__file__).resolve().parents[1]
signal = (ntdll / "unix/signal_arm64.c").read_text()
private = (ntdll / "unix/unix_private.h").read_text()
user = re.search(r"static const ULONG cpsr_user_mask = .*;", private).group()
host = re.search(r"static const ULONG cpsr_host_mask = .*;", signal).group()
direct = next(line.strip() for line in signal.splitlines()
              if "PSTATE_sig(sigcontext) =" in line and "context->Cpsr" in line)
queued = next(line.strip() for line in signal.splitlines()
              if "PSTATE_sig(sigcontext) =" in line and "frame->cpsr" in line)
harness = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
typedef uint32_t ULONG;
#define PSTATE_sig(context) (*(context))
''' + user + '\n' + host + r'''
static uint32_t direct_merge(uint32_t old, uint32_t requested)
{
    struct { uint32_t Cpsr; } value = {requested}, *context = &value;
    uint32_t *sigcontext = &old;
''' + direct + r'''
    return old;
}
static uint32_t queued_merge(uint32_t old, uint32_t requested)
{
    struct { uint32_t cpsr; } value = {requested & cpsr_user_mask}, *frame = &value;
    uint32_t *sigcontext = &old;
''' + queued + r'''
    return old;
}
static void check(uint32_t old, uint32_t requested)
{
    uint32_t expected = (old & cpsr_host_mask) | (requested & ~cpsr_host_mask);
    assert(direct_merge(old, requested) == expected);
    assert(queued_merge(old, requested) == expected);
}
int main(void)
{
    uint32_t old = 0x12345678, requested = 0x87654321;
    unsigned int i, j;
    check(0, 0x1000);
    check(0x1000, 0);
    for (i = 0; i < 32; ++i)
        for (j = 0; j < 32; ++j)
        {
            check(UINT32_C(1) << i, UINT32_C(1) << j);
            check(~(UINT32_C(1) << i), ~(UINT32_C(1) << j));
        }
    for (i = 0; i < 1000000; ++i)
    {
        old = old * UINT32_C(1664525) + UINT32_C(1013904223);
        requested = requested * UINT32_C(1103515245) + UINT32_C(12345);
        check(old, requested);
    }
    puts("PASS: both ARM context paths preserve host bits, including SSBS");
}
'''

with tempfile.TemporaryDirectory(prefix="wine-cpsr-") as temp:
    path = Path(temp)
    test = path / "test.c"
    test.write_text(harness)
    binary = path / "test"
    subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                    "-fsanitize=address,undefined", "-g", str(test), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True, timeout=20)
