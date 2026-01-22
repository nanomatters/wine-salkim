#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Exercise the actual x86-64 syscall wrappers with unextended arguments."""

import os
from pathlib import Path
import platform
import re
import shlex
import shutil
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]


@unittest.skipUnless(platform.machine() == "x86_64" and platform.system() == "Linux",
                     "requires the native x86-64 ELF ABI")
class SyscallWrapperTests(unittest.TestCase):
    def test_register_and_stack_arguments(self):
        compiler = shlex.split(os.environ.get("CC", "clang"))
        if not shutil.which(compiler[0]):
            self.skipTest(f"compiler not found: {compiler[0]}")

        source = (ROOT / "dlls/ntdll/unix/syscall.c").read_text()
        wrappers = source[source.index("/* Clang on x86-64"):
                          source.index("\n\nstatic void stub_syscall")]
        register_wrappers = wrappers[wrappers.index("#ifdef __x86_64__"):
                                     wrappers.index("\n#else\n")]
        common_wrappers = wrappers[wrappers.index("WRAP_FUNC( NtAcceptConnectPort"):
                                   wrappers.index("#define NtAcceptConnectPort")]
        names = re.findall(r"^WRAP_FUNC\(\s*(Nt\w+),", register_wrappers + common_wrappers, re.M)
        self.assertTrue(names)
        self.assertEqual(len(names), len(set(names)))

        header = (ROOT / "include/winternl.h").read_text()
        prototypes = dict(re.findall(r"WINAPI\s+(Nt\w+)\(([^)]*)\)", header))
        spec = (ROOT / "dlls/ntdll/ntdll.spec").read_text()
        syscalls = re.findall(r"^@\s+.*?\s-syscall(?:=[^\s]+)?\s+(Nt\w+)", spec, re.M)
        required = {name for name in syscalls if name in prototypes and
                    any(value.strip() == "BOOLEAN" for value in prototypes[name].split(","))}
        self.assertEqual(set(names), required)
        boolean_masks = []
        for name in names:
            positions = [i for i, value in enumerate(prototypes[name].split(","))
                         if value.strip() == "BOOLEAN"]
            self.assertTrue(positions, name)
            boolean_masks.append(sum(1 << i for i in positions))

        # The caller and capture target are assembly. Ordinary C calls can extend
        # BOOLEAN arguments before the wrapper and would hide the original bug.
        targets = "\n".join(f'__ASM_GLOBAL_FUNC({name}, "jmp capture_arguments")'
                            for name in names)
        capture = [f'"movq %{register},captured+{i * 8}(%rip)\\n\\t"'
                   for i, register in enumerate(("rdi", "rsi", "rdx", "rcx", "r8", "r9"))]
        for i in range(6, 13):
            capture += [f'"movq {(i - 5) * 8}(%rsp),%rax\\n\\t"',
                        f'"movq %rax,captured+{i * 8}(%rip)\\n\\t"']
        caller = ['"pushq %rbx\\n\\t"', '"subq $64,%rsp\\n\\t"',
                  '"movq %rdi,%rbx\\n\\t"', '"movq %rsi,%r10\\n\\t"']
        for i in range(6, 13):
            caller += [f'"movq {i * 8}(%r10),%rax\\n\\t"',
                       f'"movq %rax,{(i - 6) * 8}(%rsp)\\n\\t"']
        caller += [f'"movq {i * 8}(%r10),%{register}\\n\\t"'
                   for i, register in enumerate(("rdi", "rsi", "rdx", "rcx", "r8", "r9"))]
        caller += ['"call *%rbx\\n\\t"', '"addq $64,%rsp\\n\\t"',
                   '"popq %rbx\\n\\t"', '"ret"']

        harness = r"""
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include "wine/asm.h"
typedef int NTSTATUS;
uint64_t captured[13];
extern void call_wrapper(void *, const uint64_t *);
""" + wrappers + "\n" + targets + "\n"
        harness += '__ASM_GLOBAL_FUNC(capture_arguments,\n' + "\n".join(capture)
        harness += '\n"xorl %eax,%eax\\n\\tret")\n'
        harness += '__ASM_GLOBAL_FUNC(call_wrapper,\n' + "\n".join(caller) + ')\n'
        harness += 'static void * const functions[] = {'
        harness += ",".join(f"wrap_{name}" for name in names) + '};\n'
        harness += 'static const unsigned int masks[] = {'
        harness += ",".join(map(str, boolean_masks)) + '};\n'
        harness += r"""
int main(void)
{
    static const uint64_t upper_bits[] =
    {
        UINT64_C(0), UINT64_C(0xffffff00), UINT64_C(0xfedcba9876543200),
    };
    uint64_t args[13], expected;
    unsigned int function, upper, low, index, checks = 0;
    for (function = 0; function < sizeof(functions) / sizeof(functions[0]); ++function)
        for (upper = 0; upper < sizeof(upper_bits) / sizeof(upper_bits[0]); ++upper)
            for (low = 0; low < 256; ++low)
            {
                for (index = 0; index < 13; ++index)
                    args[index] = upper_bits[upper] ^ ((uint64_t)index << 8) ^ low;
                call_wrapper(functions[function], args);
                for (index = 0; index < 13; ++index)
                {
                    expected = args[index];
                    if (masks[function] & (1u << index)) expected &= 0xff;
                    assert(captured[index] == expected);
                    ++checks;
                }
            }
    printf("PASS: %u argument checks across %zu actual wrappers\n", checks,
           sizeof(functions) / sizeof(functions[0]));
    return 0;
}
"""
        with tempfile.TemporaryDirectory(prefix="wine-syscall-wrappers-") as temporary:
            directory = Path(temporary)
            test, binary = directory / "test.c", directory / "test"
            test.write_text(harness)
            command = compiler + ["-std=gnu11", "-O2", "-Wall", "-Wextra", "-Werror",
                                  "-D__clang__=1", "-I" + str(ROOT / "include")]
            subprocess.run(command + [str(test), "-o", str(binary)], check=True, timeout=30)
            subprocess.run([str(binary)], check=True, timeout=30)


if __name__ == "__main__":
    unittest.main()
